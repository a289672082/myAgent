"""Confirms subscription acknowledgement and cancellation over stdio."""
import json
import subprocess
import sys

p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True, bufsize=1)
try:
    request = {'jsonrpc': '2.0', 'id': 'sub1', 'method': 'subscriptions/listen',
               'params': {'notifications': {'toolsListChanged': True},
                          '_meta': {'io.modelcontextprotocol/protocolVersion': '2026-07-28',
                                    'io.modelcontextprotocol/clientCapabilities': {},
                                    'io.modelcontextprotocol/clientInfo': {'name': 'python-test', 'version': '1'}}}}
    p.stdin.write(json.dumps(request) + '\n')
    p.stdin.flush()
    first = json.loads(p.stdout.readline())
    assert first['method'] == 'notifications/subscriptions/acknowledged', first
    assert first['params']['_meta']['io.modelcontextprotocol/subscriptionId'] == 'sub1'
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/cancelled',
                              'params': {'requestId': 'sub1'}}) + '\n')
    p.stdin.flush()
    p.stdin.close()
    assert p.wait(timeout=4) == 0
    print('Stdio subscription test passed')
finally:
    if p.poll() is None:
        p.kill()
        p.wait()
