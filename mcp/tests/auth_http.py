"""Test OAuth resource metadata/challenge and server-side Bearer authorization."""
import json
import os
import socket
import subprocess
import sys
import time
import http.client

exe = sys.argv[1]
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
env = dict(os.environ, MYMCP_TEST_TOKEN='test-secret-token')
p = subprocess.Popen([exe, '--http-auth', str(port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    for _ in range(60):
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2):
                break
        except OSError:
            time.sleep(.05)
    else:
        raise AssertionError('server not started')
    def send(method, path, body=None, token=None):
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
        try:
            headers = {}
            if token:
                headers['Authorization'] = 'Bearer ' + token
            if body is not None:
                headers.update({'Content-Type': 'application/json',
                                'Accept': 'application/json, text/event-stream',
                                'MCP-Protocol-Version': '2026-07-28', 'Mcp-Method': body['method']})
                body = json.dumps(body)
            c.request(method, path, body=body, headers=headers)
            r = c.getresponse()
            return r.status, dict(r.headers), json.loads(r.read())
        finally:
            c.close()
    status, _, doc = send('GET', '/.well-known/oauth-protected-resource')
    assert status == 200 and doc['authorization_servers'] == ['https://auth.example']
    payload = {'jsonrpc':'2.0', 'id':42, 'method':'server/discover',
       'params': {'_meta':{'io.modelcontextprotocol/protocolVersion':'2026-07-28',
                          'io.modelcontextprotocol/clientCapabilities':{},
                          'io.modelcontextprotocol/clientInfo':{'name':'test','version':'1'}}}}
    status, headers, doc = send('POST','/mcp',payload)
    assert status == 401 and doc['error']['code'] == -32001
    assert 'resource_metadata="http://127.0.0.1:' in headers['WWW-Authenticate']
    status, _, _ = send('POST','/mcp',payload, 'bad-secret')
    assert status == 401
    status, _, doc = send('POST','/mcp',payload, 'test-secret-token')
    assert status == 200 and doc['result']['resultType'] == 'complete', doc
    print('HTTP auth tests passed')
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
