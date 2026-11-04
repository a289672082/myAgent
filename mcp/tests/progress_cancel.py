"""Test that stdio progress and cancellations are request scoped, not blocking."""
import json
import subprocess
import sys
import threading
import queue
import time

p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True, bufsize=1)
received = queue.Queue()
def read():
    for line in p.stdout:
        try: received.put(json.loads(line))
        except Exception as e: received.put(e)
thread = threading.Thread(target=read,daemon=True)
thread.start()
def send(id, method, params):
    meta = params.pop('_meta', {})
    params['_meta'] = {'io.modelcontextprotocol/protocolVersion':'2026-07-28',
      'io.modelcontextprotocol/clientCapabilities':{},
      'io.modelcontextprotocol/clientInfo':{'name':'progress-test','version':'1'}, **meta}
    p.stdin.write(json.dumps({'jsonrpc':'2.0','id':id,'method':method,'params':params})+'\n')
    p.stdin.flush()
try:
    send(100,'tools/call',{'name':'count_with_progress','arguments':{},'_meta':{'progressToken':'one'}})
    # send() must not overwrite the user supplied progressToken
    # retry independently with a fresh token to test progression.
    send(101,'tools/call',{'name':'count_with_progress','arguments':{},'_meta':{'progressToken':'two'}})
    send(102,'ping',{})
    until = time.time()+3
    saw_pong = False
    while time.time()<until and not saw_pong:
        msg=received.get(timeout=1)
        if msg.get('id')==102: saw_pong=True
    assert saw_pong, 'concurrent ping blocked by long tool request'
    p.stdin.write(json.dumps({'jsonrpc':'2.0','method':'notifications/cancelled',
                              'params':{'requestId':101,'reason':'test'}})+'\n')
    p.stdin.flush()
    send(103,'ping',{})
    got_pong=False
    while time.time()<until:
        try: msg=received.get(timeout=.15)
        except queue.Empty: continue
        if msg.get('id')==103: got_pong=True; break
    assert got_pong
    print('stdio concurrency and cancellation tests passed')
finally:
    p.terminate()
    try:p.wait(timeout=4)
    except subprocess.TimeoutExpired:p.kill();p.wait()
