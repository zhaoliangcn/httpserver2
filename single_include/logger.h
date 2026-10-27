/*
 * logger.h - Single-file cross-platform C/C++ logging library (stb-style)
 *
 * Features:
 *   - C++17 modern API + C11 C API + convenience macros for both
 *   - 6 log levels: TRACE / DEBUG / INFO / WARN / ERROR / FATAL
 *   - Console sink (≤WARN → stdout, ≥ERROR → stderr)
 *   - File sink with automatic size-based rotation
 *   - Compressed file sink with LZ4 frame compression (optional)
 *   - Thread-safe (mutex-guarded sinks and file writes)
 *   - Compile-time switch (LOGGER_ENABLE=0 → zero overhead)
 *   - Cross-platform: GCC/Clang __attribute__, MSVC compat, POSIX/Win localtime
 *
 * ─── Usage (C++) ──────────────────────────────────────────────────────
 *   #include "logger.h"
 *   int main() {
 *       log_add_console_sink();
 *       LOG_INFO("hello %s", "world");
 *   }
 *
 * ─── Usage (C) ────────────────────────────────────────────────────────
 *   #include "logger.h"
 *   int main(void) {
 *       log_add_console_sink();
 *       LOG_INFO("hello %s", "world");
 *   }
 *
 * ─── Single-file setup ────────────────────────────────────────────────
 *   #define LOGGER_IMPLEMENTATION   // in exactly ONE .c/.cpp file
 *   #include "logger.h"
 *
 *   All other files that use the library just #include "logger.h"
 *   (without the #define above).
 *
 * ─── Compile-time disable (zero overhead) ────────────────────────────
 *   #define LOGGER_ENABLE 0   // before #include "logger.h"
 *   // or pass -DLOGGER_ENABLE=0 to compiler
 *
 * ─── LZ4 compressed file sink (optional) ─────────────────────────────
 *   #define LOGGER_USE_LZ4 1  // before #include "logger.h"
 *   // link with -llz4 (or add lz4.c/lz4frame.c to your build)
 *   // Then call log_add_compressed_file_sink("app.log.lz4", ...)
 *
 * License: MIT
 */

#ifndef LOGGER_SINGLE_INCLUDE_H
#define LOGGER_SINGLE_INCLUDE_H

/* ======================================================================
 * Compile-time options
 * ====================================================================== */
#ifndef LOGGER_ENABLE
#  define LOGGER_ENABLE 1
#endif

#ifndef LOGGER_USE_LZ4
#  define LOGGER_USE_LZ4 0
#endif

/* ======================================================================
 * Cross-platform helpers
 * ====================================================================== */
#if defined(__GNUC__) || defined(__clang__)
#  define LOGGER_PRINTF_ATTR(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define LOGGER_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

#if defined(_WIN32)
#  define LOGGER_LOCALTIME(tm, t) localtime_s((tm), (t))
#else
#  define LOGGER_LOCALTIME(tm, t) localtime_r((t), (tm))
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * C-compatible types
 * ====================================================================== */
typedef uint8_t log_level_t;
#define LOG_LEVEL_TRACE ((log_level_t)0)
#define LOG_LEVEL_DEBUG ((log_level_t)1)
#define LOG_LEVEL_INFO  ((log_level_t)2)
#define LOG_LEVEL_WARN  ((log_level_t)3)
#define LOG_LEVEL_ERROR ((log_level_t)4)
#define LOG_LEVEL_FATAL ((log_level_t)5)
#define LOG_LEVEL_OFF   ((log_level_t)6)

/* -----------------------------------------------------------------------
 * C API — Lifecycle & configuration
 * ----------------------------------------------------------------------- */
int  log_init(void);
void log_shutdown(void);
void log_set_level(log_level_t level);
int  log_add_console_sink(void);
int  log_add_file_sink(const char* filename, unsigned long max_size, unsigned int max_files);
int  log_add_compressed_file_sink(const char* filename, unsigned long max_size, unsigned int max_files);
void log_clear_sinks(void);
void log_write(log_level_t level, const char* file, int line, const char* func,
               const char* fmt, ...) LOGGER_PRINTF_ATTR(5, 6);
void log_write_va(log_level_t level, const char* file, int line, const char* func,
                  const char* fmt, va_list args);
void log_flush(void);

/* -----------------------------------------------------------------------
 * C Convenience Macros (compile-time switchable)
 * ----------------------------------------------------------------------- */
#if LOGGER_ENABLE

#  define LOG_TRACE(fmt, ...) \
    log_write(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_DEBUG(fmt, ...) \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_INFO(fmt, ...) \
    log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_WARN(fmt, ...) \
    log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_ERROR(fmt, ...) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_FATAL(fmt, ...) \
    log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#else /* LOGGER_ENABLE == 0 */

#  define LOG_TRACE(fmt, ...) ((void)0)
#  define LOG_DEBUG(fmt, ...) ((void)0)
#  define LOG_INFO(fmt, ...)  ((void)0)
#  define LOG_WARN(fmt, ...)  ((void)0)
#  define LOG_ERROR(fmt, ...) ((void)0)
#  define LOG_FATAL(fmt, ...) ((void)0)

#endif /* LOGGER_ENABLE */

#ifdef __cplusplus
} /* extern "C" */

/* ======================================================================
 * C++ API
 * ====================================================================== */
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logger {

/* -----------------------------------------------------------------------
 * Log Level
 * ----------------------------------------------------------------------- */

// Windows SDK 的 wingdi.h 里定义了 `#define ERROR 0`，会把下面的枚举值
// 展开成 `0 = 4`，导致整片语法错误（且错误信息指向无关行）。
// 本项目没有任何地方使用该宏，这里直接取消定义。
#ifdef ERROR
#undef ERROR
#endif

enum class Level : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
    OFF   = 6
};

constexpr const char* levelToString(Level lv) noexcept {
    switch (lv) {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        case Level::ERROR: return "ERROR";
        case Level::FATAL: return "FATAL";
        default:           return "UNKNOWN";
    }
}

/* -----------------------------------------------------------------------
 * Log Record
 * ----------------------------------------------------------------------- */
struct Record {
    Level       level;
    std::string message;
    std::string file;
    int         line;
    std::string func;
    std::string threadName;
    int64_t     timestamp; // milliseconds since epoch
};

/* -----------------------------------------------------------------------
 * Sink (output target)
 * ----------------------------------------------------------------------- */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void log(const Record& rec) = 0;
    virtual void flush() = 0;
};

/* -----------------------------------------------------------------------
 * Formatter
 * ----------------------------------------------------------------------- */
class Formatter {
public:
    virtual ~Formatter() = default;
    virtual std::string format(const Record& rec) = 0;
};

/* -----------------------------------------------------------------------
 * Default Formatter
 * ----------------------------------------------------------------------- */
class DefaultFormatter final : public Formatter {
public:
    std::string format(const Record& rec) override;
};

/* -----------------------------------------------------------------------
 * Console Sink
 * ----------------------------------------------------------------------- */
class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(std::unique_ptr<Formatter> fmt = std::make_unique<DefaultFormatter>());
    void log(const Record& rec) override;
    void flush() override;

private:
    std::unique_ptr<Formatter> formatter_;
};

/* -----------------------------------------------------------------------
 * File Sink with rotation
 * ----------------------------------------------------------------------- */
class FileSink final : public Sink {
public:
    FileSink(const std::string& baseFilename,
             std::size_t         maxFileSize  = 10 * 1024 * 1024,
             unsigned int        maxFiles     = 5,
             std::unique_ptr<Formatter> fmt  = std::make_unique<DefaultFormatter>());
    ~FileSink() override;

    FileSink(const FileSink&)            = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&&)                 = delete;
    FileSink& operator=(FileSink&&)      = delete;

    void log(const Record& rec) override;
    void flush() override;

private:
    void openFile();
    void rotate();

    std::string                baseFilename_;
    std::size_t                maxFileSize_;
    unsigned int               maxFiles_;
    std::unique_ptr<Formatter> formatter_;
    std::FILE*                 file_{nullptr};
    std::size_t                currentSize_{0};
    std::mutex                 fileMutex_;
};

#if LOGGER_USE_LZ4
/* -----------------------------------------------------------------------
 * Compressed File Sink with LZ4 frame compression and rotation
 * ----------------------------------------------------------------------- */
class CompressedFileSink final : public Sink {
public:
    CompressedFileSink(const std::string& baseFilename,
                       std::size_t         maxFileSize  = 10 * 1024 * 1024,
                       unsigned int        maxFiles     = 5,
                       std::unique_ptr<Formatter> fmt  = std::make_unique<DefaultFormatter>());
    ~CompressedFileSink() override;

    CompressedFileSink(const CompressedFileSink&)            = delete;
    CompressedFileSink& operator=(const CompressedFileSink&) = delete;
    CompressedFileSink(CompressedFileSink&&)                 = delete;
    CompressedFileSink& operator=(CompressedFileSink&&)      = delete;

    void log(const Record& rec) override;
    void flush() override;

private:
    void openFile();
    void rotate();
    void flushBuffer();

    std::string                baseFilename_;
    std::size_t                maxFileSize_;
    unsigned int               maxFiles_;
    std::unique_ptr<Formatter> formatter_;
    std::FILE*                 file_{nullptr};
    std::size_t                fileSize_{0};
    std::mutex                 fileMutex_;

    std::string                buffer_;
    static constexpr std::size_t kBufferCapacity = 64 * 1024; // 64 KB
};
#endif // LOGGER_USE_LZ4

/* -----------------------------------------------------------------------
 * Logger Core
 * ----------------------------------------------------------------------- */
class Logger final {
public:
    static Logger& instance();

    void setLevel(Level lv) noexcept;
    Level level() const noexcept;

    void addSink(std::unique_ptr<Sink> sink);
    void clearSinks();

    void log(Level level, const char* file, int line, const char* func,
             const char* fmt, ...) LOGGER_PRINTF_ATTR(6, 7);
    void logVA(Level level, const char* file, int line, const char* func,
               const char* fmt, va_list args);
    void flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    Level                              level_{Level::TRACE};
    std::vector<std::unique_ptr<Sink>> sinks_;
    mutable std::mutex                 sinksMutex_;
    std::string                        getThreadName() const;
    int64_t                            nowMs() const noexcept;
};

/* -----------------------------------------------------------------------
 * C++ Convenience Macros
 *
 * The C macros above are also visible in C++ mode (they live inside the
 * extern "C" block, which only closes further down). Redefining them here
 * without an #undef first triggers "macro redefinition" warnings
 * (MSVC C4005 / GCC -Wmacro-redefined) in every translation unit that
 * defines LOGGER_IMPLEMENTATION.
 * ----------------------------------------------------------------------- */
#ifdef LOG_TRACE
#  undef LOG_TRACE
#endif
#ifdef LOG_DEBUG
#  undef LOG_DEBUG
#endif
#ifdef LOG_INFO
#  undef LOG_INFO
#endif
#ifdef LOG_WARN
#  undef LOG_WARN
#endif
#ifdef LOG_ERROR
#  undef LOG_ERROR
#endif
#ifdef LOG_FATAL
#  undef LOG_FATAL
#endif

#if LOGGER_ENABLE

#  define LOG_TRACE(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_DEBUG(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_INFO(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_WARN(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_ERROR(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define LOG_FATAL(fmt, ...) \
    ::logger::Logger::instance().log(::logger::Level::FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#else

#  define LOG_TRACE(fmt, ...) ((void)0)
#  define LOG_DEBUG(fmt, ...) ((void)0)
#  define LOG_INFO(fmt, ...)  ((void)0)
#  define LOG_WARN(fmt, ...)  ((void)0)
#  define LOG_ERROR(fmt, ...) ((void)0)
#  define LOG_FATAL(fmt, ...) ((void)0)

#endif /* LOGGER_ENABLE */

} // namespace logger

#endif /* __cplusplus */

/* ======================================================================
 * ─── IMPLEMENTATION ──────────────────────────────────────────────────
 * Define LOGGER_IMPLEMENTATION in exactly ONE .c/.cpp file.
 * ====================================================================== */
#ifdef LOGGER_IMPLEMENTATION

/* ======================================================================
 * C/C++ common includes for implementation
 * ====================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

namespace logger {

/* ====================================================================
 * nowMs
 * ==================================================================== */
int64_t Logger::nowMs() const noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/* ====================================================================
 * getThreadName
 * ==================================================================== */
std::string Logger::getThreadName() const {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

/* ====================================================================
 * DefaultFormatter
 * ==================================================================== */
std::string DefaultFormatter::format(const Record& rec) {
    char timeBuf[48]{};
    auto ms   = rec.timestamp;
    auto sec  = ms / 1000;
    auto msec = ms % 1000;
    std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm{};
    LOGGER_LOCALTIME(&tm, &t);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream os;
    os << "[" << timeBuf << "." << std::setfill('0') << std::setw(3) << msec << "]"
       << " [" << std::setfill(' ') << std::setw(5) << std::left << levelToString(rec.level) << "]"
       << " [thread:" << rec.threadName << "] "
       << rec.file << ":" << rec.line << " " << rec.func << " - " << rec.message;
    return os.str();
}

/* ====================================================================
 * ConsoleSink
 * ==================================================================== */
ConsoleSink::ConsoleSink(std::unique_ptr<Formatter> fmt)
    : formatter_(std::move(fmt)) {}

void ConsoleSink::log(const Record& rec) {
#if LOGGER_ENABLE
    auto out = formatter_->format(rec);
    if (rec.level >= Level::ERROR) {
        std::fprintf(stderr, "%s\n", out.c_str());
    } else {
        std::fprintf(stdout, "%s\n", out.c_str());
    }
#else
    (void)rec;
#endif
}

void ConsoleSink::flush() {
#if LOGGER_ENABLE
    std::fflush(stdout);
    std::fflush(stderr);
#endif
}

/* ====================================================================
 * FileSink
 * ==================================================================== */
FileSink::FileSink(const std::string& baseFilename,
                   std::size_t        maxFileSize,
                   unsigned int       maxFiles,
                   std::unique_ptr<Formatter> fmt)
    : baseFilename_(baseFilename)
    , maxFileSize_(maxFileSize)
    , maxFiles_(std::max(1u, maxFiles))
    , formatter_(std::move(fmt)) {
#if LOGGER_ENABLE
    openFile();
#endif
}

FileSink::~FileSink() {
#if LOGGER_ENABLE
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
#endif
}

void FileSink::openFile() {
#if LOGGER_ENABLE
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
#if defined(_WIN32)
    fopen_s(&file_, baseFilename_.c_str(), "a");
#else
    file_ = std::fopen(baseFilename_.c_str(), "a");
#endif
    if (file_) {
        currentSize_ = static_cast<std::size_t>(std::ftell(file_));
        if (currentSize_ == static_cast<std::size_t>(-1L)) {
            currentSize_ = 0;
        }
    }
#endif
}

void FileSink::rotate() {
#if LOGGER_ENABLE
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    std::string oldest = baseFilename_ + "." + std::to_string(maxFiles_ - 1);
    std::remove(oldest.c_str());
    for (unsigned int i = maxFiles_ - 1; i > 0; --i) {
        std::string src = baseFilename_ + "." + std::to_string(i - 1);
        std::string dst = baseFilename_ + "." + std::to_string(i);
        std::rename(src.c_str(), dst.c_str());
    }
    std::string backup = baseFilename_ + ".0";
    std::rename(baseFilename_.c_str(), backup.c_str());
    openFile();
#endif
}

void FileSink::log(const Record& rec) {
#if LOGGER_ENABLE
    auto line = formatter_->format(rec) + "\n";
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (!file_) {
        openFile();
        if (!file_) return;
    }
    if (currentSize_ + line.size() > maxFileSize_) {
        rotate();
        if (!file_) return;
    }
    std::fwrite(line.data(), 1, line.size(), file_);
    currentSize_ += line.size();
#else
    (void)rec;
#endif
}

void FileSink::flush() {
#if LOGGER_ENABLE
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_) {
        std::fflush(file_);
    }
#endif
}

#if LOGGER_USE_LZ4
/* ====================================================================
 * CompressedFileSink (LZ4 frame compression + rotation)
 * ==================================================================== */
#include <lz4frame.h>

CompressedFileSink::CompressedFileSink(const std::string& baseFilename,
                                       std::size_t        maxFileSize,
                                       unsigned int       maxFiles,
                                       std::unique_ptr<Formatter> fmt)
    : baseFilename_(baseFilename)
    , maxFileSize_(maxFileSize)
    , maxFiles_(std::max(1u, maxFiles))
    , formatter_(std::move(fmt)) {
#if LOGGER_ENABLE
    buffer_.reserve(kBufferCapacity);
    openFile();
#endif
}

CompressedFileSink::~CompressedFileSink() {
#if LOGGER_ENABLE
    flushBuffer();
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
#endif
}

void CompressedFileSink::openFile() {
#if LOGGER_ENABLE
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
#if defined(_WIN32)
    fopen_s(&file_, baseFilename_.c_str(), "ab");
#else
    file_ = std::fopen(baseFilename_.c_str(), "ab");
#endif
    if (file_) {
        fileSize_ = static_cast<std::size_t>(std::ftell(file_));
        if (fileSize_ == static_cast<std::size_t>(-1L)) {
            fileSize_ = 0;
        }
    }
#endif
}

void CompressedFileSink::rotate() {
#if LOGGER_ENABLE
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    std::string oldest = baseFilename_ + "." + std::to_string(maxFiles_ - 1);
    std::remove(oldest.c_str());
    for (unsigned int i = maxFiles_ - 1; i > 0; --i) {
        std::string src = baseFilename_ + "." + std::to_string(i - 1);
        std::string dst = baseFilename_ + "." + std::to_string(i);
        std::rename(src.c_str(), dst.c_str());
    }
    std::string backup = baseFilename_ + ".0";
    std::rename(baseFilename_.c_str(), backup.c_str());
    openFile();
#endif
}

void CompressedFileSink::flushBuffer() {
#if LOGGER_ENABLE
    if (buffer_.empty()) return;

    /* Compress the accumulated buffer into an LZ4 frame */
    LZ4F_preferences_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.compressionLevel    = 1;

    std::size_t dstCapacity = LZ4F_compressFrameBound(buffer_.size(), &prefs);
    if (dstCapacity == 0) { buffer_.clear(); return; }

    auto dst = std::make_unique<char[]>(dstCapacity);
    std::size_t compressedSize = LZ4F_compressFrame(dst.get(), dstCapacity,
                                                     buffer_.data(), buffer_.size(),
                                                     &prefs);
    if (LZ4F_isError(compressedSize)) {
        buffer_.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(fileMutex_);
    if (!file_) {
        openFile();
        if (!file_) { buffer_.clear(); return; }
    }
    if (fileSize_ + compressedSize > maxFileSize_) {
        rotate();
        if (!file_) { buffer_.clear(); return; }
    }

    std::fwrite(dst.get(), 1, compressedSize, file_);
    fileSize_ += compressedSize;
    buffer_.clear();
#endif
}

void CompressedFileSink::log(const Record& rec) {
#if LOGGER_ENABLE
    auto line = formatter_->format(rec) + "\n";
    std::lock_guard<std::mutex> lock(fileMutex_);
    buffer_.append(line);

    if (buffer_.size() >= kBufferCapacity) {
        /* Temporarily release the lock so compression doesn't block other sinks.
         * Use a manual unlock/lock pair since we need to hold the buffer_ invariant. */
        fileMutex_.unlock();
        try {
            flushBuffer();
        } catch (...) {
            fileMutex_.lock();
            buffer_.clear();
            throw;
        }
        fileMutex_.lock();
    }
#else
    (void)rec;
#endif
}

void CompressedFileSink::flush() {
#if LOGGER_ENABLE
    flushBuffer();
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_) {
        std::fflush(file_);
    }
#endif
}
#endif // LOGGER_USE_LZ4

/* ====================================================================
 * Logger
 * ==================================================================== */
Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    flush();
}

void Logger::setLevel(Level lv) noexcept {
#if LOGGER_ENABLE
    level_ = lv;
#else
    (void)lv;
#endif
}

Level Logger::level() const noexcept {
    return level_;
}

void Logger::addSink(std::unique_ptr<Sink> sink) {
#if LOGGER_ENABLE
    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.push_back(std::move(sink));
#else
    (void)sink;
#endif
}

void Logger::clearSinks() {
#if LOGGER_ENABLE
    std::lock_guard<std::mutex> lock(sinksMutex_);
    sinks_.clear();
#endif
}

void Logger::log(Level level, const char* file, int line, const char* func,
                 const char* fmt, ...) {
#if LOGGER_ENABLE
    va_list args;
    va_start(args, fmt);
    logVA(level, file, line, func, fmt, args);
    va_end(args);
#else
    (void)level; (void)file; (void)line; (void)func; (void)fmt;
#endif
}

void Logger::logVA(Level level, const char* file, int line, const char* func,
                   const char* fmt, va_list args) {
#if LOGGER_ENABLE
    if (level < level_) return;
    char buf[4096];
    int needed = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (needed < 0) {
        buf[0] = '\0';
    }
    Record rec;
    rec.level      = level;
    rec.message    = buf;
    rec.file       = file ? file : "";
    rec.line       = line;
    rec.func       = func ? func : "";
    rec.threadName = getThreadName();
    rec.timestamp  = nowMs();
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        sink->log(rec);
    }
#else
    (void)level; (void)file; (void)line; (void)func; (void)fmt; (void)args;
#endif
}

void Logger::flush() {
#if LOGGER_ENABLE
    std::lock_guard<std::mutex> lock(sinksMutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
#endif
}

} // namespace logger

#endif /* __cplusplus */

/* ======================================================================
 * C API bridge functions
 *
 * These are compiled in both C and C++ modes.
 * They delegate to the C++ logger::Logger singleton.
 * In C mode we provide extern declarations (will link with C++ object).
 * ====================================================================== */
#if defined(__cplusplus)
extern "C" {
#endif

#if LOGGER_ENABLE

int log_init(void) {
    logger::Logger::instance();
    return 0;
}

void log_shutdown(void) {
    logger::Logger::instance().flush();
    logger::Logger::instance().clearSinks();
}

void log_set_level(log_level_t level) {
    logger::Logger::instance().setLevel(static_cast<logger::Level>(level));
}

int log_add_console_sink(void) {
    try {
        auto sink = std::make_unique<logger::ConsoleSink>();
        logger::Logger::instance().addSink(std::move(sink));
        return 0;
    } catch (...) {
        return -1;
    }
}

int log_add_file_sink(const char* filename, unsigned long max_size, unsigned int max_files) {
    if (!filename || filename[0] == '\0') return -1;
    try {
        std::size_t sz = (max_size == 0) ? (10 * 1024 * 1024) : static_cast<std::size_t>(max_size);
        unsigned int mf = (max_files == 0) ? 5 : max_files;
        auto sink = std::make_unique<logger::FileSink>(filename, sz, mf);
        logger::Logger::instance().addSink(std::move(sink));
        return 0;
    } catch (...) {
        return -1;
    }
}

int log_add_compressed_file_sink(const char* filename, unsigned long max_size, unsigned int max_files) {
#if LOGGER_USE_LZ4
    if (!filename || filename[0] == '\0') return -1;
    try {
        std::size_t sz = (max_size == 0) ? (10 * 1024 * 1024) : static_cast<std::size_t>(max_size);
        unsigned int mf = (max_files == 0) ? 5 : max_files;
        auto sink = std::make_unique<logger::CompressedFileSink>(filename, sz, mf);
        logger::Logger::instance().addSink(std::move(sink));
        return 0;
    } catch (...) {
        return -1;
    }
#else
    (void)filename; (void)max_size; (void)max_files;
    return -1; /* LZ4 support not compiled in */
#endif
}

void log_clear_sinks(void) {
    logger::Logger::instance().clearSinks();
}

void log_write(log_level_t level, const char* file, int line, const char* func,
               const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write_va(level, file, line, func, fmt, args);
    va_end(args);
}

void log_write_va(log_level_t level, const char* file, int line, const char* func,
                  const char* fmt, va_list args) {
    logger::Logger::instance().logVA(static_cast<logger::Level>(level),
                                     file, line, func, fmt, args);
}

void log_flush(void) {
    logger::Logger::instance().flush();
}

#else /* LOGGER_ENABLE == 0 */

int  log_init(void)           { return 0; }
void log_shutdown(void)       {}
void log_set_level(log_level_t level) { (void)level; }
int  log_add_console_sink(void)       { return 0; }
int  log_add_file_sink(const char* filename, unsigned long max_size, unsigned int max_files) {
    (void)filename; (void)max_size; (void)max_files;
    return 0;
}
int  log_add_compressed_file_sink(const char* filename, unsigned long max_size, unsigned int max_files) {
    (void)filename; (void)max_size; (void)max_files;
    return 0;
}
void log_clear_sinks(void)    {}
void log_write(log_level_t level, const char* file, int line, const char* func,
               const char* fmt, ...) {
    (void)level; (void)file; (void)line; (void)func; (void)fmt;
}
void log_write_va(log_level_t level, const char* file, int line, const char* func,
                  const char* fmt, va_list args) {
    (void)level; (void)file; (void)line; (void)func; (void)fmt; (void)args;
}
void log_flush(void)          {}

#endif /* LOGGER_ENABLE */

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* LOGGER_IMPLEMENTATION */

#endif /* LOGGER_SINGLE_INCLUDE_H */
