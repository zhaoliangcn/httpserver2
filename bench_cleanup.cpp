/*
 * Benchmark helper: kill any leftover servers on test ports
 */
#include <cstdlib>
int main() {
    system("pkill -f 'node.*bench_server' 2>/dev/null; "
           "pkill -f 'python3.*bench_server' 2>/dev/null; "
           "pkill -f 'httpserver2' 2>/dev/null; "
           "sleep 1");
    return 0;
}
