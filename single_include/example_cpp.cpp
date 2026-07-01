// example_cpp.cpp - Single-file logger usage demo (C++17 version)
//
// Build (basic):
//   g++ -std=c++17 example_cpp.cpp -o example_cpp
//
// Build with LZ4 compressed file sink:
//   g++ -std=c++17 -DLOGGER_USE_LZ4=1 example_cpp.cpp -llz4 -o example_cpp_lz4
//
// With LOGGER_ENABLE=0 (zero overhead):
//   g++ -std=c++17 -DLOGGER_ENABLE=0 example_cpp.cpp -o example_cpp_disabled

#define LOGGER_IMPLEMENTATION
#include "logger.h"
#include <cstdio>

int main() {
    // ── Using C++ API ──
    using namespace logger;

    Logger::instance().addSink(std::make_unique<ConsoleSink>());
    Logger::instance().setLevel(Level::TRACE);

    LOG_TRACE("C++ TRACE message");
    LOG_DEBUG("C++ DEBUG value=%d", 123);
    LOG_INFO("C++ Hello %s, count=%d", "World", 7);
    LOG_WARN("C++ Warning: temp=%.1f°C", 98.6);
    LOG_ERROR("C++ Error #%d occurred", 500);
    LOG_FATAL("C++ Fatal error!");

    Logger::instance().flush();

    // ── Also using C API via extern "C" ──
    log_add_console_sink();
    LOG_INFO("C macro works from C++ too");

    log_flush();
    log_shutdown();

    std::printf("Done.\n");
    return 0;
}
