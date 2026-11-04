"""Launch the no-argument network demos. No arguments are passed to demo executables."""
import subprocess
import json
import sys
import socket
import time
import urllib.request
import urllib.error

http_server, http_client, subscriber, http_progress, protected_server, protected_client = sys.argv[1:7]

def started(port):
    for _ in range(160):
        try:
            with socket.create_connection(("127.0.0.1", port), .15):
                return
        except OSError:
            time.sleep(.05)
    raise AssertionError(f"demo server did not start on port {port}")

def execute(path, marker):
    r = subprocess.run([path], capture_output=True, text=True, timeout=12)
    assert r.returncode == 0 and marker in r.stdout, (path, r.returncode, r.stdout, r.stderr)

def stop(process):
    process.terminate()
    try: process.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate(timeout=3)

server = subprocess.Popen([http_server], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    started(33867)
    execute(http_client, "run_http_client_demo OK")
    execute(subscriber, "run_subscriptions_demo OK")
    execute(http_progress, "run_http_progress_demo OK")
finally:
    stop(server)

server = subprocess.Popen([protected_server], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    started(33868)
    url = 'http://127.0.0.1:33868/.well-known/oauth-protected-resource'
    with urllib.request.urlopen(url, timeout=3) as response:
        metadata = response.read().decode()
        assert 'authorization_servers' in metadata
    execute(protected_client, "run_oauth_resource_client_demo OK")
    request_body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {"_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientInfo": {"name": "demo-test", "version": "1.0"},
        "io.modelcontextprotocol/clientCapabilities": {}
    }}}).encode("utf-8")
    request = urllib.request.Request('http://127.0.0.1:33868/mcp', method='POST',
       data=request_body, headers={"Content-Type": "application/json",
         "Accept": "application/json, text/event-stream",
         "MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "tools/list"})
    try:
        urllib.request.urlopen(request, timeout=3)
        raise AssertionError('Unprotected HTTP MCP request was accepted')
    except urllib.error.HTTPError as e:
        assert e.code == 401, e.code
finally:
    stop(server)

print('NO_ARGUMENT_NETWORK_DEMOS_OK')
