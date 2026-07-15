#include "streaming.h"
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <random>

namespace http2 { namespace stream {

// ============================================================
// 创建临时文件
// ============================================================
std::string createTempFile(const std::string& prefix) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::stringstream ss;
    ss << prefix << std::hex << gen() << ".tmp";
    
    std::string path = std::filesystem::temp_directory_path().string() + "/" + ss.str();
    return path;
}

// ============================================================
// UploadedStream 实现
// ============================================================
UploadedStream::UploadedStream(const std::string& mime_type, 
                                const std::string& original_filename)
    : mime_type_(mime_type), original_filename_(original_filename) {
    temp_path_ = createTempFile("upload_");
    file_.open(temp_path_, std::ios::binary | std::ios::out);
    if (!file_.is_open()) {
        LOG_ERROR("Failed to create temp file: " + temp_path_);
    }
}

UploadedStream::~UploadedStream() {
    if (file_.is_open()) {
        file_.close();
    }
    // 清理临时文件（如果未 finalize，即未保存）
    if (!finalized_) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
    }
}

bool UploadedStream::write(const char* data, size_t size) {
    if (!file_.is_open()) return false;
    file_.write(data, static_cast<std::streamsize>(size));
    if (file_.fail()) {
        LOG_ERROR("Failed to write to temp file: " + temp_path_);
        return false;
    }
    total_size_ += size;
    return true;
}

bool UploadedStream::write(const std::vector<char>& data) {
    return write(data.data(), data.size());
}

bool UploadedStream::finalize() {
    if (file_.is_open()) {
        file_.close();
    }
    finalized_ = true;
    return true;
}

bool UploadedStream::saveTo(const std::string& path) {
    if (!finalize()) return false;
    std::error_code ec;
    std::filesystem::copy_file(temp_path_, path, ec);
    if (ec) {
        LOG_ERROR("Failed to save upload to: " + path + " - " + ec.message());
        return false;
    }
    return true;
}

std::vector<char> UploadedStream::readAll() const {
    std::ifstream file(temp_path_, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<char> buffer(static_cast<size_t>(size));
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

}} // namespace http2::stream