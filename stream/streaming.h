#ifndef HTTPSERVER2_STREAMING_H
#define HTTPSERVER2_STREAMING_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <fstream>
#include "../httpserver2.h"

namespace http2 { namespace stream {

// 上传文件流 — 将大文件写入临时文件而非内存
class UploadedStream {
private:
    std::string temp_path_;
    std::fstream file_;
    size_t total_size_ = 0;
    std::string mime_type_;
    std::string original_filename_;
    bool finalized_ = false;

public:
    UploadedStream(const std::string& mime_type = "", 
                   const std::string& original_filename = "");
    ~UploadedStream();
    
    // 写入数据块
    bool write(const char* data, size_t size);
    bool write(const std::vector<char>& data);
    
    // 完成写入
    bool finalize();
    
    // 保存到指定路径
    bool saveTo(const std::string& path);
    
    // 获取信息
    std::string tempPath() const { return temp_path_; }
    size_t totalSize() const { return total_size_; }
    std::string mimeType() const { return mime_type_; }
    std::string originalFilename() const { return original_filename_; }
    
    // 读取内容（小文件使用）
    std::vector<char> readAll() const;
};

// 流式响应构建器 — 用于大响应体分段发送
class StreamResponse {
private:
    HttpResponse* response_;
    bool chunked_ = false;
    std::function<bool(const std::string&)> send_callback_;

public:
    StreamResponse(HttpResponse* response, 
                   std::function<bool(const std::string&)> send_callback)
        : response_(response), send_callback_(std::move(send_callback)) {
        response_->headers["Transfer-Encoding"] = "chunked";
        chunked_ = true;
    }
    
    // 发送数据块
    bool write(const std::string& chunk) {
        if (send_callback_) {
            return send_callback_(chunk);
        }
        return false;
    }
    
    // 结束流式响应
    void end() {
        response_->response_sent = true;
    }
};

// 创建临时文件的辅助函数
std::string createTempFile(const std::string& prefix = "upload_");

}} // namespace http2::stream

#endif // HTTPSERVER2_STREAMING_H