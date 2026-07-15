#include "websocket.h"
#include <random>
#include <sstream>

// 确保 Boost.Beast 命名空间可用
namespace beast = boost::beast;

namespace http2 { namespace ws {

// 全局 WebSocket 管理器实例
WebSocketManager g_ws_manager;

// ============================================================
// WebSocketSession 实现
// ============================================================
WebSocketSession::WebSocketSession(
    std::shared_ptr<websocket::stream<tcp::socket>> stream,
    const std::string& id,
    const std::unordered_map<std::string, std::string>& headers,
    const std::unordered_map<std::string, std::string>& query_params,
    const std::string& remote_ip,
    WebSocketHandler handler)
    : ws_stream_(std::move(stream))
    , id_(id)
    , headers_(headers)
    , query_params_(query_params)
    , remote_ip_(remote_ip)
    , handler_(std::move(handler))
    , open_(true)
{
    if (id_.empty()) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::stringstream ss;
        ss << std::hex << gen();
        id_ = ss.str();
    }
}

WebSocketSession::~WebSocketSession() {
    close();
}

void WebSocketSession::start() {
    if (handler_.on_open) {
        try {
            handler_.on_open(shared_from_this());
        } catch (const std::exception& e) {
            LOG_ERROR("WebSocket on_open error: " + std::string(e.what()));
        }
    }
    doRead();
}

void WebSocketSession::send(const std::string& text) {
    send(WebSocketMessage::text(text));
}

void WebSocketSession::send(const std::vector<char>& binary) {
    send(WebSocketMessage::binary(binary));
}

void WebSocketSession::send(const WebSocketMessage& msg) {
    if (!open_) return;
    auto msg_ptr = std::make_shared<WebSocketMessage>(msg);
    doWrite(msg_ptr);
}

void WebSocketSession::doWrite(std::shared_ptr<const WebSocketMessage> msg) {
    if (!ws_stream_ || !ws_stream_->is_open()) return;
    
    std::lock_guard<std::mutex> lock(write_mutex_);
    
    try {
        if (msg->opcode == WebSocketMessage::TEXT) {
            ws_stream_->write(net::buffer(msg->textData()));
        } else if (msg->opcode == WebSocketMessage::BINARY) {
            ws_stream_->write(net::buffer(msg->data));
        } else if (msg->opcode == WebSocketMessage::CLOSE) {
            ws_stream_->close(websocket::close_code(
                static_cast<int>(static_cast<unsigned char>(msg->data[0]) << 8 | 
                                 static_cast<unsigned char>(msg->data[1]))));
            open_ = false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket write error: " + std::string(e.what()));
        open_ = false;
    }
}

void WebSocketSession::close(uint16_t code, const std::string& reason) {
    if (!open_ || !ws_stream_ || !ws_stream_->is_open()) return;
    
    try {
        ws_stream_->close(websocket::close_code(code));
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket close error: " + std::string(e.what()));
    }
    
    open_ = false;
    g_ws_manager.remove(id_);
    
    if (handler_.on_close) {
        try {
            handler_.on_close(shared_from_this(), code, reason);
        } catch (...) {}
    }
}

void WebSocketSession::doRead() {
    if (!open_ || !ws_stream_ || !ws_stream_->is_open()) return;
    
    auto self = shared_from_this();
    auto buffer = std::make_shared<beast::flat_buffer>();
    
    ws_stream_->async_read(*buffer,
        [self, this, buffer](beast::error_code ec, std::size_t bytes_transferred) {
            boost::ignore_unused(bytes_transferred);
            
            if (ec == websocket::error::closed) {
                open_ = false;
                g_ws_manager.remove(id_);
                if (handler_.on_close) {
                    handler_.on_close(shared_from_this(), 1000, "");
                }
                return;
            }
            
            if (ec) {
                LOG_ERROR("WebSocket read error: " + ec.message());
                open_ = false;
                g_ws_manager.remove(id_);
                if (handler_.on_error) {
                    handler_.on_error(shared_from_this(), ec.message());
                }
                if (handler_.on_close) {
                    handler_.on_close(shared_from_this(), 1006, "Abnormal closure");
                }
                return;
            }
            
            // 构建消息
            WebSocketMessage msg;
            if (ws_stream_->got_text()) {
                msg.opcode = WebSocketMessage::TEXT;
            } else {
                msg.opcode = WebSocketMessage::BINARY;
            }
            msg.fin = true;
            
            auto data_ptr = reinterpret_cast<const char*>(buffer->data().data());
            auto data_size = buffer->data().size();
            msg.data.assign(data_ptr, data_ptr + data_size);
            
            // 处理控制帧
            if (msg.opcode == WebSocketMessage::PING) {
                send(WebSocketMessage::fromData(WebSocketMessage::PONG, msg.data));
                doRead();
                return;
            }
            if (msg.opcode == WebSocketMessage::PONG) {
                doRead();
                return;
            }
            if (msg.opcode == WebSocketMessage::CLOSE) {
                uint16_t code = 1000;
                if (msg.data.size() >= 2) {
                    code = static_cast<uint16_t>(
                        (static_cast<unsigned char>(msg.data[0]) << 8) | 
                        static_cast<unsigned char>(msg.data[1]));
                }
                std::string reason;
                if (msg.data.size() > 2) {
                    reason.assign(msg.data.begin() + 2, msg.data.end());
                }
                close(code, reason);
                return;
            }
            
            // 调用 on_message 回调
            if (handler_.on_message) {
                try {
                    handler_.on_message(shared_from_this(), msg);
                } catch (const std::exception& e) {
                    LOG_ERROR("WebSocket on_message error: " + std::string(e.what()));
                }
            }
            
            doRead();
        }
    );
}

// ============================================================
// WebSocketManager 实现
// ============================================================
void WebSocketManager::add(const std::shared_ptr<WebSocketSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->id()] = session;
}

void WebSocketManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

std::shared_ptr<WebSocketSession> WebSocketManager::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        return it->second.lock();
    }
    return nullptr;
}

std::vector<std::shared_ptr<WebSocketSession>> WebSocketManager::getAll() {
    std::vector<std::shared_ptr<WebSocketSession>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto session = it->second.lock();
        if (session) {
            result.push_back(session);
            ++it;
        } else {
            it = sessions_.erase(it);
        }
    }
    return result;
}

void WebSocketManager::broadcast(const std::string& text) {
    auto sessions = getAll();
    for (auto& session : sessions) {
        if (session->isOpen()) {
            session->send(text);
        }
    }
}

size_t WebSocketManager::count() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second.expired()) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    return sessions_.size();
}

}} // namespace http2::ws