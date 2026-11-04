"""Checks real long-lived SSE handshake, multiplexing, and HTTP concurrency."""
import http.client
import json
import socket
import subprocess
import sys
import time

exe = sys.argv[1]
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
proc = subprocess.Popen([exe, '--http', str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    for _ in range(80):
        try:
            sock = socket.create_connection(('127.0.0.1', port), timeout=0.15)
            sock.close()
            break
        except OSError:
            time.sleep(.03)
    else:
        raise RuntimeError('Cannot connect to test server')

    def body(method, extra=None):
        return json.dumps({'jsonrpc': '2.0', 'id': 41, 'method': method,
                           'params': {'_meta': {
                             'io.modelcontextprotocol/protocolVersion': '2026-07-28',
                             'io.modelcontextprotocol/clientCapabilities': {},
                                    'io.modelcontextprotocol/clientInfo': {'name': 'python-test', 'version': '1'}}, **(extra or {})}})

    def headers(method):
        return {'Mcp-Method': method, 'MCP-Protocol-Version': '2026-07-28',
                'Accept': 'application/json, text/event-stream', 'Content-Type': 'application/json'}

    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
    connection.request('POST', '/mcp', body('subscriptions/listen', {
        'notifications': {'toolsListChanged': True, 'resourceSubscriptions': ['demo://status']}}),
        headers('subscriptions/listen'))
    response = connection.getresponse()
    assert response.status == 200, response.status
    assert response.getheader('Content-Type') == 'text/event-stream'
    first = response.readline().decode('utf8')
    assert first.startswith('data: '), first
    event = json.loads(first[6:])
    assert event['method'] == 'notifications/subscriptions/acknowledged', event
    assert event['params']['_meta']['io.modelcontextprotocol/subscriptionId'] == 41
    assert event['params']['notifications']['toolsListChanged'] is True

    # A subscription must not block new independent requests on the same server.
    other = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
    other.request('POST', '/mcp', body('tools/list'), headers('tools/list'))
    listed = other.getresponse()
    assert listed.status == 200
    assert len(json.loads(listed.read())['result']['tools']) >= 2
    other.close()
    connection.close()
    output = subprocess.run([sys.argv[2], f'http://127.0.0.1:{port}/mcp'],
        capture_output=True, text=True, timeout=5)
    assert output.returncode == 0, (output.stdout, output.stderr)
    print('SSE subscription smoke tests passed')
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
