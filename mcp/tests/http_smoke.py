"""Small transport smoke test; no third-party Python dependencies."""
import http.client
import json
import socket
import subprocess
import sys
import time

exe = sys.argv[1]
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]

proc = subprocess.Popen([exe, "--http", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(60):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError("HTTP server did not start")

    def request(method="server/discover", params=None, headers=None):
        if params is None:
            params = {}
        params["_meta"] = {
            "io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientCapabilities": {},
            "io.modelcontextprotocol/clientInfo": {"name": "python-test", "version": "1"},
        }
        body = {"jsonrpc": "2.0", "id": 8, "method": method, "params": params}
        hs = {
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2026-07-28", "Mcp-Method": method,
        }
        if method == "tools/call":
            hs["Mcp-Name"] = params["name"]
        hs.update(headers or {})
        con = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        try:
            con.request("POST", "/mcp", json.dumps(body), hs)
            r = con.getresponse()
            return r.status, json.loads(r.read())
        finally:
            con.close()

    code, doc = request()
    assert code == 200, doc
    assert doc["result"]["supportedVersions"] == ["2026-07-28"]
    assert doc["result"]["resultType"] == "complete"

    code, doc = request("tools/call", {"name": "add", "arguments": {"a": 2, "b": 7}})
    assert code == 200 and doc["result"]["content"][0]["text"] == "9", doc

    code, doc = request("tools/call", {"name": "add", "arguments": {"a": 2, "b": 7}}, {"Mcp-Name": "wrong"})
    assert code == 400 and doc["error"]["code"] == -32020, doc

    code, doc = request(headers={"Origin": "https://attacker.example"})
    assert code == 403, doc

    code, doc = request(headers={"MCP-Protocol-Version": "2025-11-25"})
    assert code == 400 and doc["error"]["code"] == -32020, doc

    print("HTTP smoke tests passed")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
