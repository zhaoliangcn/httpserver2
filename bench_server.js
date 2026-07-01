/**
 * Node.js HTTP server — performance comparison baseline
 * Usage: node bench_server.js [port] [static_dir]
 */
const http = require('http');
const fs = require('fs');
const path = require('path');
const cluster = require('cluster');
const os = require('os');

const PORT = parseInt(process.argv[2] || '9091');
const STATIC_DIR = path.resolve(process.argv[3] || process.cwd() + '/static');
const CPUS = os.cpus().length;

// Preload static files into cache
const fileCache = {};
function loadFiles(dir) {
    try {
        const files = fs.readdirSync(dir);
        for (const f of files) {
            const fp = path.join(dir, f);
            const stat = fs.statSync(fp);
            if (stat.isFile()) {
                fileCache[f] = {
                    data: fs.readFileSync(fp),
                    type: getContentType(f),
                    size: stat.size
                };
            }
        }
        console.error(`[Node] Cached ${Object.keys(fileCache).length} static files`);
    } catch (e) {
        console.error(`[Node] Static dir ${dir} not found, using / endpoint`);
    }
}

function getContentType(filename) {
    const ext = path.extname(filename).toLowerCase();
    const map = {
        '.html': 'text/html; charset=utf-8',
        '.js': 'application/javascript',
        '.css': 'text/css',
        '.json': 'application/json',
        '.txt': 'text/plain',
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.svg': 'image/svg+xml',
    };
    return map[ext] || 'application/octet-stream';
}

if (cluster.isMaster) {
    console.error(`[Node] Master ${process.pid} starting ${CPUS} workers on port ${PORT}`);
    for (let i = 0; i < CPUS; ++i) cluster.fork();
    cluster.on('exit', (worker) => {});
} else {
    loadFiles(STATIC_DIR);

    const server = http.createServer((req, res) => {
        // Parse path - support / as /index.html
        let filename = req.url === '/' ? 'index.html' : path.basename(req.url);
        let cached = fileCache[filename];

        if (cached) {
            res.writeHead(200, {
                'Content-Type': cached.type,
                'Content-Length': cached.size,
                'Connection': 'keep-alive'
            });
            res.end(cached.data);
        } else {
            // Simple text response for any path
            const body = `Hello from Node.js (worker ${process.pid})\n`;
            res.writeHead(200, {
                'Content-Type': 'text/plain',
                'Content-Length': Buffer.byteLength(body),
                'Connection': 'keep-alive'
            });
            res.end(body);
        }
    });

    server.keepAliveTimeout = 30000;
    server.headersTimeout = 31000;
    server.listen(PORT, () => {
        console.error(`[Node] Worker ${process.pid} listening on ${PORT}`);
    });
}
