#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
HttpServer2 并发性能测试脚本
使用 asyncio 和 aiohttp 进行高性能并发测试
"""

import asyncio
import aiohttp
import time
import argparse
import json
import statistics
from datetime import datetime
from typing import Dict, List, Tuple, Optional

class PerformanceTestResult:
    """性能测试结果类"""
    
    def __init__(self):
        self.total_requests: int = 0
        self.successful_requests: int = 0
        self.failed_requests: int = 0
        self.total_time: float = 0.0
        self.response_times: List[float] = []
        self.status_codes: Dict[str, int] = {}
        self.start_time: Optional[float] = None
        self.end_time: Optional[float] = None
    
    def add_result(self, success: bool, response_time: float, status_code: Optional[str] = None):
        """添加单个请求的测试结果"""
        self.total_requests += 1
        if success:
            self.successful_requests += 1
            self.response_times.append(response_time)
            if status_code:
                self.status_codes[status_code] = self.status_codes.get(status_code, 0) + 1
        else:
            self.failed_requests += 1
    
    def calculate_metrics(self):
        """计算性能指标"""
        metrics = {
            "total_requests": self.total_requests,
            "successful_requests": self.successful_requests,
            "failed_requests": self.failed_requests,
            "success_rate": (self.successful_requests / self.total_requests * 100) if self.total_requests > 0 else 0,
            "total_time": self.total_time,
            "rps": (self.successful_requests / self.total_time) if self.total_time > 0 else 0,
        }
        
        if self.response_times:
            metrics.update({
                "min_response_time": min(self.response_times),
                "max_response_time": max(self.response_times),
                "avg_response_time": statistics.mean(self.response_times),
                "median_response_time": statistics.median(self.response_times),
            })
            
            # 计算百分位数
            sorted_times = sorted(self.response_times)
            metrics.update({
                "p95_response_time": sorted_times[int(len(sorted_times) * 0.95)] if sorted_times else 0,
                "p99_response_time": sorted_times[int(len(sorted_times) * 0.99)] if sorted_times else 0,
            })
        
        metrics["status_codes"] = self.status_codes
        return metrics

class HttpServerPerformanceTester:
    """HTTP服务器性能测试器"""
    
    def __init__(self, base_url: str, concurrency: int, total_requests: int, 
                 method: str = "GET", headers: Optional[Dict] = None,
                 payload: Optional[str] = None, timeout: float = 30.0, verify_ssl: bool = True):
        self.base_url = base_url
        self.concurrency = concurrency
        self.total_requests = total_requests
        self.method = method.upper()
        self.headers = headers or {}
        self.payload = payload
        self.timeout = timeout
        self.verify_ssl = verify_ssl
        self.results = PerformanceTestResult()
        self.session: Optional[aiohttp.ClientSession] = None
    
    async def _make_request(self, session: aiohttp.ClientSession, request_id: int):
        """发送单个HTTP请求并记录结果"""
        start_time = time.time()
        success = False
        status_code = None
        
        try:
            async with session.request(
                self.method,
                self.base_url,
                headers=self.headers,
                data=self.payload,
                timeout=self.timeout
            ) as response:
                # 确保读取完整响应体
                await response.read()
                success = True
                status_code = str(response.status)
        except Exception as e:
            # 静默记录错误，避免输出过多
            pass
        
        end_time = time.time()
        response_time = (end_time - start_time) * 1000  # 转换为毫秒
        
        self.results.add_result(success, response_time, status_code)
        
        # 每100个请求输出进度
        if request_id % 100 == 0 or request_id == self.total_requests:
            print(f"进度: {request_id}/{self.total_requests} 请求完成")
    
    async def _worker(self, start_idx: int, batch_size: int):
        """工作协程，处理一批请求"""
        for i in range(start_idx, min(start_idx + batch_size, self.total_requests)):
            await self._make_request(self.session, i + 1)
    
    async def run_test(self):
        """运行并发性能测试"""
        print(f"开始性能测试: {self.method} {self.base_url}")
        print(f"配置: 并发数={self.concurrency}, 总请求数={self.total_requests}")
        if self.base_url.startswith('https://'):
            print(f"SSL验证: {'已启用' if self.verify_ssl else '已禁用'}")
        
        self.results.start_time = time.time()
        
        # 创建会话
        timeout = aiohttp.ClientTimeout(total=self.timeout)
        # 配置SSL选项
        ssl_context = None if self.verify_ssl else False
        self.session = aiohttp.ClientSession(timeout=timeout, connector=aiohttp.TCPConnector(ssl=ssl_context))
        
        try:
            # 计算每个工作协程处理的请求数
            batch_size = (self.total_requests + self.concurrency - 1) // self.concurrency
            
            # 创建并启动工作协程
            tasks = []
            for i in range(0, self.total_requests, batch_size):
                task = asyncio.create_task(self._worker(i, batch_size))
                tasks.append(task)
            
            # 等待所有任务完成
            await asyncio.gather(*tasks)
            
        finally:
            await self.session.close()
        
        self.results.end_time = time.time()
        self.results.total_time = self.results.end_time - self.results.start_time
        
        print("测试完成!")
        return self.results
    
    def print_report(self, metrics: Dict):
        """打印测试报告"""
        print("\n" + "="*60)
        print("性能测试报告")
        print("="*60)
        print(f"测试目标: {self.method} {self.base_url}")
        print(f"测试时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print(f"配置: 并发数={self.concurrency}, 总请求数={self.total_requests}")
        if self.base_url.startswith('https://'):
            print(f"SSL配置: {'验证证书' if self.verify_ssl else '忽略证书验证'}")
        print("\n性能指标:")
        print(f"  总请求数: {metrics['total_requests']}")
        print(f"  成功请求数: {metrics['successful_requests']}")
        print(f"  失败请求数: {metrics['failed_requests']}")
        print(f"  成功率: {metrics['success_rate']:.2f}%")
        print(f"  总耗时: {metrics['total_time']:.2f} 秒")
        print(f"  吞吐量(RPS): {metrics['rps']:.2f}")
        
        if 'min_response_time' in metrics:
            print("\n响应时间(毫秒):")
            print(f"  最小响应时间: {metrics['min_response_time']:.2f} ms")
            print(f"  最大响应时间: {metrics['max_response_time']:.2f} ms")
            print(f"  平均响应时间: {metrics['avg_response_time']:.2f} ms")
            print(f"  中位数响应时间: {metrics['median_response_time']:.2f} ms")
            print(f"  95%分位数响应时间: {metrics['p95_response_time']:.2f} ms")
            print(f"  99%分位数响应时间: {metrics['p99_response_time']:.2f} ms")
        
        print("\n状态码统计:")
        for code, count in sorted(metrics['status_codes'].items()):
            percentage = (count / metrics['total_requests'] * 100) if metrics['total_requests'] > 0 else 0
            print(f"  {code}: {count} ({percentage:.2f}%)")
        
        print("="*60)
    
    def export_report(self, metrics: Dict, filename: str):
        """导出测试报告到JSON文件"""
        report_data = {
            "test_info": {
                "url": self.base_url,
                "method": self.method,
                "concurrency": self.concurrency,
                "total_requests": self.total_requests,
                "test_start_time": datetime.fromtimestamp(self.results.start_time).isoformat() if self.results.start_time else None,
                "test_end_time": datetime.fromtimestamp(self.results.end_time).isoformat() if self.results.end_time else None,
                "headers": self.headers,
                "payload_provided": bool(self.payload)
            },
            "metrics": metrics
        }
        
        with open(filename, 'w', encoding='utf-8') as f:
            json.dump(report_data, f, ensure_ascii=False, indent=2)
        
        print(f"\n报告已导出到: {filename}")

def parse_arguments():
    """解析命令行参数"""
    parser = argparse.ArgumentParser(description='HttpServer2 并发性能测试工具')
    
    # 基本测试参数
    parser.add_argument('--url', '-u', required=True, help='测试目标URL')
    parser.add_argument('--concurrency', '-c', type=int, default=50, help='并发连接数 (默认: 50)')
    parser.add_argument('--requests', '-r', type=int, default=1000, help='总请求数 (默认: 1000)')
    parser.add_argument('--method', '-m', default='GET', choices=['GET', 'POST', 'PUT', 'DELETE'],
                        help='HTTP方法 (默认: GET)')
    
    # 请求配置
    parser.add_argument('--headers', '-H', action='append', help='HTTP请求头，格式: "Key: Value"')
    parser.add_argument('--data', '-d', help='请求体数据')
    parser.add_argument('--timeout', type=float, default=30.0, help='请求超时时间(秒) (默认: 30)')
    
    # SSL配置
    parser.add_argument('--no-verify-ssl', action='store_true', help='不验证SSL证书 (适用于自签名证书)')
    
    # 报告配置
    parser.add_argument('--export', '-e', help='导出报告文件名')
    
    return parser.parse_args()

def parse_headers(header_strings: List[str]) -> Dict[str, str]:
    """解析命令行中的请求头字符串"""
    headers = {}
    if header_strings:
        for header_str in header_strings:
            try:
                key, value = header_str.split(':', 1)
                headers[key.strip()] = value.strip()
            except ValueError:
                print(f"警告: 无效的请求头格式: {header_str}")
    return headers

async def main():
    """主函数"""
    args = parse_arguments()
    headers = parse_headers(args.headers)
    
    # 如果是POST请求且没有指定Content-Type，默认设置为application/json
    if args.method == 'POST' and args.data and 'Content-Type' not in headers:
        headers['Content-Type'] = 'application/json'
    
    # 创建性能测试器
    tester = HttpServerPerformanceTester(
        base_url=args.url,
        concurrency=args.concurrency,
        total_requests=args.requests,
        method=args.method,
        headers=headers,
        payload=args.data,
        timeout=args.timeout,
        verify_ssl=not args.no_verify_ssl
    )
    
    # 运行测试
    results = await tester.run_test()
    
    # 计算指标并生成报告
    metrics = results.calculate_metrics()
    tester.print_report(metrics)
    
    # 导出报告
    if args.export:
        tester.export_report(metrics, args.export)

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n测试被用户中断")
    except Exception as e:
        print(f"测试发生错误: {e}")