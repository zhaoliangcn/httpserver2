#!/usr/bin/env python3
"""
HttpServer2 Performance Comparison Benchmark
Compares: HttpServer2 vs Node.js vs Python asyncio
"""
import subprocess, time, json, os, sys, signal

BASE = '/home/zhaoliang/testhttpserver2'
STRESS = f'{BASE}/build/test_stress'
SERVER = f'{BASE}/build/httpserver2'

PORTS = {'httpserver2': 9090, 'nodejs': 9091, 'python': 9092}

# Test scenarios: (name, concurrency, requests, keep_alive, warmup)
SCENARIOS = [
    ("低并发 短连接",   10,   5000,  False, 500),
    ("中并发 短连接",   50,   10000, False, 1000),
    ("高并发 短连接",   200,  20000, False, 2000),
    ("中并发 Keep-Alive", 50, 10000, True,  1000),
]

results = {}

def start_httpserver2():
    """Start HttpServer2 on port 9090 with no-auth config"""
    port = PORTS['httpserver2']
    proc = subprocess.Popen(
        [SERVER, str(port), '--config', 'test_config_no_auth.yaml'],
        cwd=BASE,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        stdin=subprocess.PIPE
    )
    # Feed newline after 120s to trigger server shutdown
    import threading
    def _feed():
        import time
        time.sleep(120)
        try:
            proc.stdin.write(b'\n')
            proc.stdin.flush()
        except:
            pass
    threading.Thread(target=_feed, daemon=True).start()
    time.sleep(2)
    return proc

def start_nodejs():
    proc = subprocess.Popen(
        ['node', f'{BASE}/bench_server.js', str(PORTS['nodejs']), f'{BASE}/static'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)
    return proc

def start_python():
    proc = subprocess.Popen(
        ['python3', f'{BASE}/bench_server.py', str(PORTS['python']), f'{BASE}/static'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)
    return proc

def stop(proc, name):
    if proc:
        os.kill(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except:
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait()
    # Clean up any leftover processes
    subprocess.run(['pkill', '-f', 'bench_server'], capture_output=True)
    subprocess.run(['pkill', '-f', 'httpserver2'], capture_output=True)
    time.sleep(1)

def run_stress(host, port, concurrency, requests, keep_alive, warmup, label):
    """Run a single stress test and return parsed JSON result"""
    json_out = f'/tmp/bench_{label.replace(" ","_").lower()}.json'
    cmd = [STRESS,
           '--host', host,
           '--port', str(port),
           '--concurrency', str(concurrency),
           '--requests', str(requests),
           '--warmup', str(warmup),
           '--json', json_out,
           '--timeout', '10']
    if keep_alive:
        cmd.append('--keep-alive')

    start = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    elapsed = time.time() - start

    try:
        with open(json_out) as f:
            data = json.load(f)
        data['wall_clock_sec'] = round(elapsed, 2)
        return data
    except:
        return {'error': result.stderr[:200], 'wall_clock_sec': round(elapsed, 2)}

def print_progress(msg):
    print(f"  [{time.strftime('%H:%M:%S')}] {msg}", flush=True)

# =========================================
# Main benchmark
# =========================================
print("=" * 60)
print("  HttpServer2 性能对比测试")
print("=" * 60)
print()

servers = {
    'httpserver2': ('HttpServer2 (C++)', start_httpserver2, '127.0.0.1', PORTS['httpserver2']),
    'nodejs':      ('Node.js 25 (cluster)', start_nodejs, '127.0.0.1', PORTS['nodejs']),
    'python':      ('Python 3.13 (asyncio)', start_python, '127.0.0.1', PORTS['python']),
}

# Warm up: verify all servers start correctly
print("--- 启动服务器验证 ---")
for sid, (sname, starter, host, port) in servers.items():
    print_progress(f"启动 {sname}...")
    # Kill leftovers first
    subprocess.run(['pkill', '-f', 'bench_server'], capture_output=True)
    subprocess.run(['pkill', '-f', 'httpserver2'], capture_output=True)
    time.sleep(1)

    proc = starter()
    # Quick health check
    import socket
    s = socket.socket()
    s.settimeout(3)
    try:
        s.connect((host, port))
        s.send(b'GET / HTTP/1.0\r\n\r\n')
        resp = s.recv(100)
        print(f"  ✓ {sname} -> {resp.split(b'\\r\\n')[0].decode()}")
    except Exception as e:
        print(f"  ✗ {sname} -> {e}")
    finally:
        s.close()
    stop(proc, sid)
    time.sleep(1)

print()
print("--- 性能测试 ---")

for sid, (sname, starter, host, port) in servers.items():
    print()
    print(f"  ═══ {sname} ═══")

    proc = starter()
    results[sid] = {}

    for scenario in SCENARIOS:
        sname_sc, conc, reqs, ka, warm = scenario
        label = f"{sid}_{sname_sc}"
        ka_label = "Keep-Alive" if ka else "短连接"

        print_progress(f"  {sname_sc} ({conc}并发/{reqs}请求/{ka_label})...")
        data = run_stress(host, port, conc, reqs, ka, warm, label)
        results[sid][sname_sc] = data

        if 'error' in data:
            print(f"    ✗ 错误: {data['error']}")
        else:
            s = data['summary']
            l = data.get('latency', {})
            print(f"    ✓ OK={s['ok']} Fail={s['failed']} RPS={s['rps']:.0f} "
                  f"P50={l.get('p50_ms',0):.2f}ms P95={l.get('p95_ms',0):.2f}ms "
                  f"速率={s.get('mbps',0):.2f}MB/s")

    stop(proc, sid)

# =========================================
# Generate comparison report
# =========================================
print()
print("=" * 60)
print("  对比结果汇总")
print("=" * 60)
print()

# Table header
header = f"{'测试场景':<18} {'指标':<8}"
for sid, (sname, _, _, _) in servers.items():
    header += f" {sname:<22}"
print(header)
print("-" * len(header))

for scenario in SCENARIOS:
    sname_sc, conc, reqs, ka, warm = scenario
    ka_label = "短连接" if not ka else "KA"

    # RPS row
    row_rps = f"{sname_sc:<18} {'RPS':<8}"
    row_lat = f"{'':<18} {'P50/P95':<8}"
    row_ok  = f"{'':<18} {'OK率':<8}"

    for sid, (_, _, _, _) in servers.items():
        data = results.get(sid, {}).get(sname_sc, {})
        if 'error' in data:
            row_rps += f" {'ERROR':>22}"
            row_lat += f" {'ERROR':>22}"
            row_ok  += f" {'ERROR':>22}"
        else:
            s = data.get('summary', {})
            l = data.get('latency', {})
            rps = s.get('rps', 0)
            ok_rate = s.get('success_rate', 0)
            p50 = l.get('p50_ms', 0)
            p95 = l.get('p95_ms', 0)
            row_rps  += f" {rps:>8.0f} req/s       "
            row_lat  += f" {p50:>5.2f}/{p95:<5.2f}ms       "
            row_ok   += f" {ok_rate:>7.1f}%         "

    print(row_rps)
    print(row_lat)
    print(row_ok)
    print()

# Speedup ratio (HttpServer2 vs Node.js)
print("--- HttpServer2 相对加速比 ---")
print(f"{'测试场景':<18} {'vs Node.js':<14} {'vs Python':<14}")
for scenario in SCENARIOS:
    sname_sc, _, _, _, _ = scenario
    h2_rps = results.get('httpserver2', {}).get(sname_sc, {}).get('summary', {}).get('rps', 0)
    nd_rps = results.get('nodejs', {}).get(sname_sc, {}).get('summary', {}).get('rps', 0)
    py_rps = results.get('python', {}).get(sname_sc, {}).get('summary', {}).get('rps', 0)

    nd_ratio = h2_rps / nd_rps if nd_rps > 0 else 0
    py_ratio = h2_rps / py_rps if py_rps > 0 else 0
    print(f"  {sname_sc:<18} {nd_ratio:>5.1f}x           {py_ratio:>5.1f}x")

# Save full results to JSON
report = {
    'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
    'servers': {sid: sname for sid, (sname, _, _, _) in servers.items()},
    'scenarios': [{'name': s[0], 'concurrency': s[1], 'requests': s[2],
                    'keep_alive': s[3], 'warmup': s[4]} for s in SCENARIOS],
    'results': results
}
report_path = '/tmp/benchmark_comparison.json'
with open(report_path, 'w') as f:
    json.dump(report, f, indent=2, default=str)

print()
print(f"完整数据: {report_path}")
print("=" * 60)
