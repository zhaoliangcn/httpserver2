// 页面加载完成后执行
window.addEventListener('DOMContentLoaded', function() {
    console.log('静态文件服务测试 - JavaScript已加载');
    
    // 添加页面加载时间显示
    const loadTime = (performance.now() / 1000).toFixed(2);
    console.log(`页面加载耗时: ${loadTime} 秒`);
    
    // 创建一个隐藏的信息元素并添加到页面
    const infoDiv = document.createElement('div');
    infoDiv.style.position = 'fixed';
    infoDiv.style.bottom = '10px';
    infoDiv.style.right = '10px';
    infoDiv.style.backgroundColor = 'rgba(0, 0, 0, 0.7)';
    infoDiv.style.color = 'white';
    infoDiv.style.padding = '10px';
    infoDiv.style.borderRadius = '5px';
    infoDiv.style.fontSize = '12px';
    infoDiv.textContent = `页面加载耗时: ${loadTime} 秒`;
    document.body.appendChild(infoDiv);
    
    // 添加点击链接的事件监听
    const links = document.querySelectorAll('a');
    links.forEach(link => {
        link.addEventListener('click', function(e) {
            const href = this.getAttribute('href');
            console.log(`正在导航到: ${href}`);
            
            // 对于API链接，阻止默认行为并显示提示
            if (href.startsWith('/api/')) {
                e.preventDefault();
                alert(`API链接: ${href}\n点击F12查看网络请求`);
            }
        });
    });
    
    // 检测是否支持缓存控制
    console.log('测试缓存控制支持...');
});

// 简单的性能监控函数
function monitorPerformance() {
    if ('performance' in window) {
        const timing = performance.timing;
        const domLoad = timing.domComplete - timing.domLoading;
        const resourcesLoad = timing.loadEventEnd - timing.loadEventStart;
        
        console.log(`DOM加载时间: ${domLoad}ms`);
        console.log(`资源加载时间: ${resourcesLoad}ms`);
    }
}