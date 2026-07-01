#!/usr/bin/env python3
"""
HttpServer2 统一测试运行器

自动发现并运行所有 test_*.py 测试模块，
自动管理服务器生命周期，汇总测试结果。

用法:
  python3 run_tests.py                     # 运行所有测试
  python3 run_tests.py --list              # 列出可用测试
  python3 run_tests.py error_handling      # 运行指定测试
  python3 run_tests.py --skip-start        # 跳过服务器启动（手动启动）
  python3 run_tests.py --port 9090         # 指定端口
"""

import os
import sys
import time
import signal
import subprocess
import importlib
import argparse
from pathlib import Path

# ============================================================
# 配置
# ============================================================
TEST_DIR = Path(__file__).parent.absolute()
SERVER_BINARY = str(TEST_DIR / "build" / "httpserver2")
DEFAULT_PORT = 8080
SERVER_START_TIMEOUT = 5  # 等待服务器启动的秒数

# 需要排除的测试模块（非测试类或需要特殊处理的）
EXCLUDE_MODULES = {
    'run_tests',
    'test_config',
    'test_reverse_proxy',
    'test_reverse_proxy_functional',
    'test_proxy_client',
    'test_proxy_stability',
    'performance_test',
}

# ============================================================
# 工具函数
# ============================================================

def discover_tests():
    """发现所有 test_*.py 中的测试类"""
    tests = []
    for f in sorted(TEST_DIR.glob("test_*.py")):
        module_name = f.stem
        if module_name in EXCLUDE_MODULES:
            continue
        tests.append(module_name)
    return tests


def print_colored(text, color="white", bold=False):
    """带颜色的终端输出"""
    colors = {
        "red": "\033[91m",
        "green": "\033[92m",
        "yellow": "\033[93m",
        "blue": "\033[94m",
        "magenta": "\033[95m",
        "cyan": "\033[96m",
        "white": "\033[97m",
        "bold": "\033[1m",
        "reset": "\033[0m",
    }
    prefix = ""
    if bold:
        prefix += colors["bold"]
    if color in colors:
        prefix += colors[color]
    print(f"{prefix}{text}{colors['reset']}")


def start_server(port=DEFAULT_PORT):
    """启动服务器进程"""
    if not os.path.exists(SERVER_BINARY):
        print_colored(f"[ERROR] 服务器二进制文件未找到: {SERVER_BINARY}", "red", bold=True)
        print("请先编译项目: cd build && make httpserver2")
        return None

    print_colored(f"正在启动服务器 (端口 {port})...", "cyan")
    proc = subprocess.Popen(
        [SERVER_BINARY, str(port)],
        cwd=TEST_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=subprocess.PIPE,
    )

    # 等待服务器启动
    import requests
    for i in range(SERVER_START_TIMEOUT * 2):
        time.sleep(0.5)
        try:
            r = requests.get(f"http://localhost:{port}/health", timeout=1)
            if r.status_code == 200:
                print_colored("服务器已启动 ✓", "green")
                return proc
        except Exception:
            continue

    # 检查进程是否还在运行
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        print_colored(f"[ERROR] 服务器启动失败", "red", bold=True)
        print(f"stdout: {stdout.decode()[:500]}")
        print(f"stderr: {stderr.decode()[:500]}")
        return None

    print_colored("服务器已启动（健康检查未响应，但进程仍在运行）", "yellow")
    return proc


def stop_server(proc):
    """停止服务器进程"""
    if proc is None or proc.poll() is not None:
        return
    print_colored("正在停止服务器...", "cyan")
    # 先发送 Enter 键触发优雅关闭
    try:
        proc.communicate(input=b"\n", timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    print_colored("服务器已停止 ✓", "green")


def run_test_module(module_name, port):
    """运行单个测试模块，返回 (passed, failed, total)"""
    print()
    print_colored(f"{'='*60}", "blue")
    print_colored(f"  运行测试模块: {module_name}", "blue", bold=True)
    print_colored(f"{'='*60}", "blue")

    try:
        # 通过子进程运行，传递端口环境变量
        env = os.environ.copy()
        env["TEST_PORT"] = str(port)
        result = subprocess.run(
            [sys.executable, f"{module_name}.py"],
            cwd=TEST_DIR,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )

        # 输出测试结果
        if result.stdout:
            print(result.stdout)

        if result.stderr:
            # 过滤掉 SSL 警告等非关键信息
            for line in result.stderr.splitlines():
                if "warning" not in line.lower() and "deprecat" not in line.lower():
                    print(line, file=sys.stderr)

        # 从输出中解析结果
        passed = failed = 0
        for line in result.stdout.splitlines():
            if "passed," in line and "failed," in line:
                parts = line.split(",")
                for part in parts:
                    if "passed" in part:
                        passed += int(part.strip().split()[0])
                    if "failed" in part:
                        failed += int(part.strip().split()[0])

        if result.returncode != 0:
            print_colored(f"⚠️  模块 {module_name} 返回退出码 {result.returncode}", "yellow")

        return passed, failed

    except subprocess.TimeoutExpired:
        print_colored(f"⏱️  模块 {module_name} 超时", "red", bold=True)
        return 0, 1
    except FileNotFoundError:
        print_colored(f"❌ 模块 {module_name} 未找到", "red")
        return 0, 1
    except Exception as e:
        print_colored(f"❌ 运行 {module_name} 时出错: {e}", "red")
        return 0, 1


# ============================================================
# 主流程
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="HttpServer2 统一测试运行器")
    parser.add_argument("test_modules", nargs="*", help="指定要运行的测试模块名（默认全部）")
    parser.add_argument("--list", action="store_true", help="列出可用测试模块")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"服务器端口（默认 {DEFAULT_PORT}）")
    parser.add_argument("--skip-start", action="store_true", help="跳过自动启动服务器（手动启动后使用）")
    args = parser.parse_args()

    PORT = args.port

    if args.list:
        print_colored("可用测试模块:", "cyan", bold=True)
        for t in discover_tests():
            print(f"  - {t}")
        return

    # 确定要运行的测试
    if args.test_modules:
        modules = args.test_modules
    else:
        modules = discover_tests()

    if not modules:
        print_colored("未找到测试模块！", "red", bold=True)
        sys.exit(1)

    print_colored(f"HttpServer2 测试套件", "magenta", bold=True)
    print_colored(f"{'='*60}", "magenta")
    print_colored(f"测试目录: {TEST_DIR}", "cyan")
    print_colored(f"服务器: {SERVER_BINARY}", "cyan")
    print_colored(f"端口: {PORT}", "cyan")
    if args.skip_start:
        print_colored("模式: 使用已有服务器（跳过自动启动）", "yellow")
    print()

    # 启动服务器
    server_proc = None
    if not args.skip_start:
        server_proc = start_server(PORT)
        if server_proc is None:
            print_colored("服务器启动失败，终止测试", "red", bold=True)
            sys.exit(1)
    else:
        # 检查服务器是否可访问
        import requests
        try:
            r = requests.get(f"http://localhost:{PORT}/health", timeout=2)
            print_colored(f"服务器可访问 (HTTP {r.status_code}) ✓", "green")
        except Exception:
            print_colored(f"⚠️  无法连接到 localhost:{PORT}，请确保服务器已手动启动", "yellow")

    # 运行测试
    print_colored(f"\n开始运行 {len(modules)} 个测试模块...", "cyan", bold=True)
    total_passed = 0
    total_failed = 0

    for module in modules:
        passed, failed = run_test_module(module, PORT)
        total_passed += passed
        total_failed += failed

    # 停止服务器
    if not args.skip_start:
        stop_server(server_proc)

    # 输出汇总
    print()
    print_colored(f"{'='*60}", "magenta")
    print_colored(f"  测试汇总", "magenta", bold=True)
    print_colored(f"{'='*60}", "magenta")
    print_colored(f"  运行模块: {len(modules)}", "cyan")
    print_colored(f"  通过: {total_passed}", "green", bold=True)
    print_colored(f"  失败: {total_failed}", "red" if total_failed > 0 else "green", bold=True)
    print_colored(f"  总计: {total_passed + total_failed}", "cyan")
    print()

    if total_failed == 0:
        print_colored("🎉 全部测试通过！", "green", bold=True)
        sys.exit(0)
    else:
        print_colored("💥 部分测试失败，请检查上面的日志。", "red", bold=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
