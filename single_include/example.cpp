// example.cpp - Single-file logger usage demo
//
// Build:
//   g++ -std=c++17 example.cpp -o example
//   or (disable logging):
//   g++ -std=c++17 -DLOGGER_ENABLE=0 example.cpp -o example

#define LOGGER_IMPLEMENTATION
#include "logger.h"

int main() {
    // Add a console sink
    log_add_console_sink();

    // C-style logging macros
    LOG_INFO("Application started (C macro)");
    LOG_WARN("Disk usage at %.1f%%", 87.3);
    LOG_ERROR("Failed to open file: %s", "config.ini");

    log_flush();

    // C++ style (works in C++ mode)
    using namespace logger;
    Logger::instance().addSink(std::make_unique<ConsoleSink>());

    LOG_INFO("C++ style logging works too");
    LOG_DEBUG("Debug value=%d", 42);

    Logger::instance().flush();
    return 0;
}
