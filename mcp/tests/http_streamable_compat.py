"""Wire-level Streamable HTTP tests for 2026 and legacy clients (no external packages)."""
import http.client
import json
import socket
import subprocess
import sys
import time

with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]

proc = subprocess.Popen([sys.argv[1], "--http", str(port)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

try:
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.04)
    else:
        raise AssertionError("MCP HTTP server failed to start")

    def post(method, params=None, *, modern=False, version="2025-11-25", id=1):
        if params is None:
            params = {}
        params = dict(params)
        if modern:
            params["_meta"] = dict(params.get("_meta", {}), **{
                "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {},
                "io.modelcontextprotocol/clientInfo": {"name": "compat-test", "version": "1"}})
        body = {"jsonrpc": "2.0", "id": id, "method": method, "params": params}
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream"}
        if modern:
            headers.update({"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": method})
            if method == "tools/call":
                headers["Mcp-Name"] = params["name"]
        elif method != "initialize":
            headers["MCP-Protocol-Version"] = version
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("POST", "/mcp", json.dumps(body), headers)
        return conn, conn.getresponse()

    # Legacy initialization and normal Tools calls match Codex/Claude MCP
    # clients that still offer the 2025-11-25 or 2025-03-26 protocol.
    for version in ("2025-11-25", "2025-03-26"):
        conn, result = post("initialize", {
            "protocolVersion": version, "capabilities": {},
            "clientInfo": {"name": "legacy-client", "version": "1"}})
        doc = json.loads(result.read())
        assert result.status == 200 and doc["result"]["protocolVersion"] == version, doc
        assert "resultType" not in doc["result"], doc
        conn.close()
        conn, result = post("tools/list", version=version)
        doc = json.loads(result.read())
        assert result.status == 200 and any(t["name"] == "add" for t in doc["result"]["tools"]), doc
        assert "resultType" not in doc["result"], doc
        conn.close()
        conn, result = post("tools/call", {"name": "add", "arguments": {"a": 3, "b": 4}}, version=version)
        doc = json.loads(result.read())
        assert result.status == 200 and doc["result"]["content"][0]["text"] == "7", doc
        conn.close()

    # The pre-2026 GET notification stream is optional; 405 is a valid refusal.
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    conn.request("GET", "/mcp", headers={"Accept": "text/event-stream"})
    result = conn.getresponse()
    assert result.status == 405, result.status
    result.read()
    conn.close()

    # 2026: ordinary requests are one JSON response, but a progressToken
    # chooses one SSE stream: monotonic notifications followed by a final result.
    conn, result = post("tools/call", {"name": "add", "arguments": {"a": 4, "b": 9}}, modern=True)
    doc = json.loads(result.read())
    assert result.status == 200 and result.getheader("Content-Type").startswith("application/json")
    assert doc["result"]["content"][0]["text"] == "13", doc
    conn.close()

    conn, result = post("tools/call", {"name": "count_with_progress", "arguments": {},
        "_meta": {"progressToken": "stream-123"}}, modern=True, id=44)
    assert result.status == 200 and result.getheader("Content-Type").startswith("text/event-stream")
    # http.client receives chunked stream incrementally rather than buffering it all.
    events = []
    for raw in result:
        text = raw.decode("utf-8").strip()
        if not text or text.startswith(":"):
            continue
        if text.startswith("data:"):
            events.append(json.loads(text[5:].strip()))
    conn.close()
    progress = [e["params"]["progress"] for e in events if e.get("method") == "notifications/progress"]
    final = [e for e in events if e.get("id") == 44]
    assert progress == list(range(1, 51)), (len(progress), progress[:4], progress[-4:])
    assert len(final) == 1 and final[0]["result"]["resultType"] == "complete", events
    print("Modern/legacy HTTP and request-scoped SSE progress passed")
finally:
    proc.terminate()
    try:
        proc.communicate(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate(timeout=4)
