// example_lz4.cpp - LZ4 compressed file sink demo
//
// Build:
//   g++ -std=c++17 -DLOGGER_USE_LZ4=1 example_lz4.cpp -llz4 -o example_lz4
//
// Run:
//   ./example_lz4
//   # Compressed output is in test_lz4.log.lz4 (LZ4 frame format)
//   # Decompress with: lz4 -d test_lz4.log.lz4
//   # Or:              lz4cat test_lz4.log.lz4

#define LOGGER_IMPLEMENTATION
#define LOGGER_USE_LZ4 1
#include "logger.h"
#include <cstdio>

int main() {
    // Add an LZ4 compressed file sink: 1 MB max size, 3 rotated files
    // Output will be test.lz4, test.lz4.0, test.lz4.1, test.lz4.2
    if (log_add_compressed_file_sink("test.lz4", 1 * 1024 * 1024, 3) != 0) {
        std::fprintf(stderr, "Failed to add compressed file sink (is lz4 linked?)\n");
        return 1;
    }

    // Also output to console so we can see live
    log_add_console_sink();

    log_set_level(LOG_LEVEL_TRACE);

    // Generate enough log data to test compression and rotation
    for (int i = 0; i < 1000; ++i) {
        LOG_INFO("Iteration %04d - The quick brown fox jumps over the lazy dog. "
                 "This is a test log message that will compress well with LZ4.", i);
    }

    LOG_INFO("LZ4 compressed file sink demo complete.");

    log_flush();
    log_shutdown();

    std::printf("Done. Compressed log written to test.lz4\n");
    std::printf("Decompress with: lz4 -d test.lz4\n");
    return 0;
}
