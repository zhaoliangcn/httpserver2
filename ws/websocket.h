#ifndef HTTPSERVER2_WEBSOCKET_H
#define HTTPSERVER2_WEBSOCKET_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <set>
#include "../httpserver2.h"
#include <boost/beast/websocket.hpp>

namespace http2 { namespace ws {

namespace websocket = boost::beast::websocket;

// WebSocket 消息
struct WebSocketMessage {
    enum OpCode {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };
    
    OpCode opcode = TEXT;
    std::vector<char> data;
    bool fin = true;
    
    // 便捷构造
    static WebSocketMessage text(const std::string& text) {
        WebSocketMessage msg;
        msg.opcode = TEXT;
        msg.data.assign(text.begin(), text.end());
        msg.fin = true;
        return msg;
    }
    
    static WebSocketMessage binary(const std::vector<char>& bin) {
        WebSocketMessage msg;
        msg.opcode = BINARY;
        msg.data = bin;
        msg.fin = true;
        return msg;
    }
    
    static WebSocketMessage close(uint16_t code = 1000, const std::string& reason = "") {
        WebSocketMessage msg;
        msg.opcode = CLOSE;
        msg.data.resize(2);
        msg.data[0] = static_cast<char>((code >> 8) & 0xFF);
        msg.data[1] = static_cast<char>(code & 0xFF);
        msg.data.insert(msg.data.end(), reason.begin(), reason.end());
        msg.fin = true;
        return msg;
    }
    
    static WebSocketMessage fromData(OpCode op, const std::vector<char>& data) {
        WebSocketMessage msg;
        msg.opcode = op;
        msg.data = data;
        msg.fin = true;
        return msg;
    }
    
    std::string textData() const {
        return std::string(data.data(), data.size());
    }
};

// WebSocket 会话（前向声明）
class WebSocketSession;

// WebSocket 事件处理器
struct WebSocketHandler {
    std::function<void(std::shared_ptr<WebSocketSession>)> on_open;
    std::function<void(std::shared_ptr<WebSocketSession>, const WebSocketMessage&)> on_message;
    std::function<void(std::shared_ptr<WebSocketSession>, uint16_t, const std::string&)> on_close;
    std::function<void(std::shared_ptr<WebSocketSession>, const std::string&)> on_error;
};

// WebSocket 会话
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
private:
    std::string id_;
    std::shared_ptr<websocket::stream<boost::asio::ip::tcp::socket>> ws_stream_;
    std::unordered_map<std::string, std::string> headers_;
    std::unordered_map<std::string, std::string> query_params_;
    std::string remote_ip_;
    WebSocketHandler handler_;
    bool open_ = false;
    std::mutex write_mutex_;

public:
    WebSocketSession(std::shared_ptr<websocket::stream<boost::asio::ip::tcp::socket>> stream,
                     const std::string& id,
                     const std::unordered_map<std::string, std::string>& headers,
                     const std::unordered_map<std::string, std::string>& query_params,
                     const std::string& remote_ip,
                     WebSocketHandler handler);
    
    ~WebSocketSession();
    
    const std::string& id() const { return id_; }
    bool isOpen() const { return open_; }
    std::string remoteIp() const { return remote_ip_; }
    std::unordered_map<std::string, std::string> headers() const { return headers_; }
    std::unordered_map<std::string, std::string> queryParams() const { return query_params_; }
    
    // 发送消息
    void send(const std::string& text);
    void send(const std::vector<char>& binary);
    void send(const WebSocketMessage& msg);
    
    // 关闭连接
    void close(uint16_t code = 1000, const std::string& reason = "");
    
    // 启动读取循环
    void start();
    
    // 获取底层流（用于 PROXY 场景）
    std::shared_ptr<websocket::stream<boost::asio::ip::tcp::socket>> stream() { return ws_stream_; }

private:
    void doRead();
    void doWrite(std::shared_ptr<const WebSocketMessage> msg);
};

// WebSocket 会话管理器
class WebSocketManager {
private:
    std::unordered_map<std::string, std::weak_ptr<WebSocketSession>> sessions_;
    std::mutex mutex_;
    
public:
    void add(const std::shared_ptr<WebSocketSession>& session);
    void remove(const std::string& id);
    std::shared_ptr<WebSocketSession> get(const std::string& id);
    std::vector<std::shared_ptr<WebSocketSession>> getAll();
    void broadcast(const std::string& text);
    size_t count();
};

// 全局 WebSocket 管理器实例
extern WebSocketManager g_ws_manager;

}} // namespace http2::ws

#endif // HTTPSERVER2_WEBSOCKET_H