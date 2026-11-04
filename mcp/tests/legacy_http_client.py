"""Mock 2025 HTTP MCP peer: prove Client::http discovers legacy and sends initialize."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
import json
import subprocess
import sys

class OldPeer(BaseHTTPRequestHandler):
    seen = []

    def do_POST(self):
        assert self.path == "/mcp"
        payload = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        method = payload["method"]
        self.seen.append(method)
        if method == "server/discover":
            assert self.headers.get("Mcp-Method") == "server/discover"
            assert self.headers.get("MCP-Protocol-Version") == "2026-07-28"
        else:
            assert "Mcp-Method" not in self.headers, (method, dict(self.headers))
        if method == "server/discover":
            result = {"jsonrpc": "2.0", "id": payload["id"],
                      "error": {"code": -32601, "message": "Method not found"}}
            code = 404
        elif method == "initialize":
            assert "MCP-Protocol-Version" not in self.headers
            result = {"jsonrpc": "2.0", "id": payload["id"], "result": {
                "protocolVersion": "2025-11-25",
                "capabilities": {"tools": {}}, "serverInfo": {"name": "old", "version": "1"}}}
            code = 200
        elif method == "notifications/initialized":
            self.send_response(202)
            self.end_headers()
            return
        elif method == "tools/list":
            assert self.headers.get("MCP-Protocol-Version") == "2025-11-25"
            result = {"jsonrpc": "2.0", "id": payload["id"],
                      "result": {"tools": [{"name": "ping", "inputSchema": {"type": "object"}}]}}
            code = 200
        elif method == "tools/call":
            assert self.headers.get("MCP-Protocol-Version") == "2025-11-25"
            result = {"jsonrpc": "2.0", "id": payload["id"],
                      "result": {"content": [{"type": "text", "text": "pong"}]}}
            code = 200
        else:
            raise AssertionError(method)
        body = json.dumps(result).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = ThreadingHTTPServer(("127.0.0.1", 0), OldPeer)
worker = Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
    res = subprocess.run([sys.argv[1], "http://127.0.0.1:" + str(server.server_port) + "/mcp"],
                         timeout=10, capture_output=True, text=True)
    assert res.returncode == 0, (res.returncode, res.stdout, res.stderr, OldPeer.seen)
    assert OldPeer.seen == ["server/discover", "initialize", "notifications/initialized", "tools/list", "tools/call"]
    print("Legacy HTTP client fallback passed")
finally:
    server.shutdown()
    server.server_close()
