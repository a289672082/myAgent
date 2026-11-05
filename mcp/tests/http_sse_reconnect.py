"""验证 MCP 2026-07-28 subscriptions/listen 的 LF/CRLF SSE 分帧及安全重订阅。"""
import http.server
import json
import subprocess
import sys
import threading

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    connections = 0

    def do_POST(self):
        if self.path != "/mcp":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        assert request["method"] == "subscriptions/listen"
        assert "Last-Event-ID" not in self.headers
        Handler.connections += 1
        event = {"jsonrpc": "2.0", "method": "notifications/tools/list_changed",
                 "params": {}}
        payload = ("data: " + json.dumps(event) + "\r\n\r\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()
        self.close_connection = True

    def log_message(self, *_args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
    url = f"http://127.0.0.1:{server.server_address[1]}/mcp"
    subprocess.run([sys.argv[1], url], check=True, timeout=15)
    if Handler.connections != 2:
        raise AssertionError(f"Expected two subscriptions, got {Handler.connections}")
finally:
    server.shutdown()
    server.server_close()
