#!/bin/bash
# Build script for single-file logger examples
set -euo pipefail

echo "=== C++17 example ==="
g++ -std=c++17 -Wall -Wextra -o example_cpp example_cpp.cpp
echo "  -> example_cpp"
echo ""

echo "=== C++17 example (LOGGER_ENABLE=0) ==="
g++ -std=c++17 -DLOGGER_ENABLE=0 -Wall -Wextra -o example_cpp_disabled example_cpp.cpp
echo "  -> example_cpp_disabled"
echo ""

echo "=== C example (requires g++ for linking) ==="
gcc -std=c11 -x c++ example.c -lstdc++ -Wall -Wextra -o example_c
echo "  -> example_c"
echo ""

echo "=== LZ4 compressed file sink example ==="
echo "  (requires liblz4-dev / lz4 library)"
if g++ -std=c++17 -DLOGGER_USE_LZ4=1 -Wall -Wextra -o example_lz4 example_lz4.cpp -llz4 2>/dev/null; then
    echo "  -> example_lz4"
else
    echo "  [SKIPPED] - install liblz4-dev to build this example"
    echo "    Debian/Ubuntu: sudo apt install liblz4-dev"
    echo "    macOS:         brew install lz4"
    echo "    Fedora:        sudo dnf install lz4-devel"
fi
echo ""

echo "Done. Run: ./example_cpp  or  ./example_c  or  ./example_lz4"
