// example.c - Single-file logger usage demo (C version)
//
// Build:
//   gcc -std=c11 -x c++ example.c -lstdc++ -o example
//   # (C API requires C++ linker for the single-file implementation)
//
// Or with C++ main:
//   g++ -std=c++17 example_noimp.cpp -o example

#define LOGGER_IMPLEMENTATION
#include "logger.h"

int main(void) {
    // Initialize
    log_init();

    // Add console output
    log_add_console_sink();

    // Set minimum level
    log_set_level(LOG_LEVEL_TRACE);

    // Log messages using macros
    LOG_TRACE("This is a TRACE message");
    LOG_DEBUG("This is DEBUG, value=%d", 42);
    LOG_INFO("Application %s v%d started", "MyApp", 1);
    LOG_WARN("Warning: %.1f%% disk full", 87.5);
    LOG_ERROR("Error #%d: %s", 404, "Not Found");
    LOG_FATAL("Fatal: system HALT");

    // Flush and shutdown
    log_flush();
    log_shutdown();

    return 0;
}
