#!/usr/bin/env python3
"""
Python asyncio HTTP server — performance comparison baseline
Usage: python3 bench_server.py [port] [static_dir]
"""
import asyncio, os, sys, mimetypes, signal

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9092
STATIC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.getcwd(), 'static')

HTML_RESP = b'<html><body><h1>Hello from Python asyncio</h1></body></html>'

# Preload static files
file_cache = {}
if os.path.isdir(STATIC):
    for f in os.listdir(STATIC):
        fp = os.path.join(STATIC, f)
        if os.path.isfile(fp):
            with open(fp, 'rb') as fh:
                data = fh.read()
            ctype, _ = mimetypes.guess_type(f)
            file_cache[f] = (data, ctype or 'application/octet-stream')
    print(f'[Python] Cached {len(file_cache)} static files', file=sys.stderr)

async def handle(reader, writer):
    try:
        req = b''
        while b'\r\n\r\n' not in req:
            chunk = await asyncio.wait_for(reader.read(4096), timeout=10)
            if not chunk:
                break
            req += chunk
        if not req:
            writer.close()
            return

        # Parse request line
        line = req.split(b'\r\n')[0].decode()
        parts = line.split()
        path = parts[1] if len(parts) > 1 else '/'

        # Serve static or default
        fname = 'index.html' if path == '/' else os.path.basename(path)
        if fname in file_cache:
            data, ctype = file_cache[fname]
            resp = (
                f'HTTP/1.1 200 OK\r\n'
                f'Content-Type: {ctype}\r\n'
                f'Content-Length: {len(data)}\r\n'
                f'Connection: keep-alive\r\n'
                f'\r\n'
            ).encode() + data
        else:
            resp = (
                f'HTTP/1.1 200 OK\r\n'
                f'Content-Type: text/html\r\n'
                f'Content-Length: {len(HTML_RESP)}\r\n'
                f'Connection: keep-alive\r\n'
                f'\r\n'
            ).encode() + HTML_RESP

        writer.write(resp)
        await writer.drain()
    except:
        pass
    finally:
        try:
            writer.close()
        except:
            pass

async def main():
    server = await asyncio.start_server(handle, '0.0.0.0', PORT)
    print(f'[Python] Listening on {PORT}', file=sys.stderr)
    async with server:
        await server.serve_forever()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
