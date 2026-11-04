"""Exercise example protected-resource metadata and Bearer verifier (demo only)."""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

exe = sys.argv[1]
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
origin = f"http://127.0.0.1:{port}"
env = os.environ.copy()
env["MYMCP_DEMO_TOKEN"] = "local-test-secret"
proc = subprocess.Popen([exe, "https://issuer.example", str(port)], env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError(proc.stderr.read().decode(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                break
        except OSError:
            time.sleep(.05)
    else:
        raise RuntimeError("OAuth protected server startup timeout")
    metadata = json.load(urllib.request.urlopen(origin + "/.well-known/oauth-protected-resource", timeout=4))
    assert metadata["resource"] == origin + "/mcp"
    assert metadata["authorization_servers"] == ["https://issuer.example"]
    req_body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {"_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientInfo": {"name": "example-test", "version": "1"},
        "io.modelcontextprotocol/clientCapabilities": {}
    }}}).encode()
    headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
               "MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "tools/list"}
    def post(bearer=None):
        h = headers.copy()
        if bearer is not None:
            h["Authorization"] = "Bearer " + bearer
        try:
            with urllib.request.urlopen(urllib.request.Request(origin + "/mcp", req_body, h), timeout=5) as res:
                return res.status, json.loads(res.read())
        except urllib.error.HTTPError as err:
            return err.code, err.read()
    assert post()[0] == 401
    assert post("wrong-token")[0] == 401
    status, response = post("local-test-secret")
    assert status == 200 and response["result"]["tools"][0]["name"] == "private_hello", response
finally:
    proc.terminate()
    try:
        proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate(timeout=5)
print("OAUTH_RESOURCE_EXAMPLE_OK")
