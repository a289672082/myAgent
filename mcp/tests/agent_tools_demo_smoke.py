"""通过真实 stdio JSON-RPC 验证全部十组智能体工具与安全拒绝路径。"""
import json
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading

EXECUTABLE = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='mymcp-agent-tools-') as tmp:
    p = subprocess.Popen([str(EXECUTABLE)], cwd=tmp, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, encoding='utf-8', bufsize=1)
    events = queue.Queue()
    threading.Thread(target=lambda: [events.put(line) for line in p.stdout], daemon=True).start()
    next_id = 0

    def request(method, params=None):
        global next_id
        next_id += 1
        params = dict(params or {})
        params['_meta'] = {
            'io.modelcontextprotocol/protocolVersion': '2026-07-28',
            'io.modelcontextprotocol/clientCapabilities': {},
            'io.modelcontextprotocol/clientInfo': {'name': 'agent-demo-smoke', 'version': '1.0'}
        }
        p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': next_id,
                                  'method': method, 'params': params}) + '\n')
        p.stdin.flush()
        answer = json.loads(events.get(timeout=25))
        assert answer.get('id') == next_id, answer
        if 'error' in answer:
            raise AssertionError(f'Unexpected JSON-RPC error {method}: {answer}')
        return answer['result']

    def call(name, **args):
        result = request('tools/call', {'name': name, 'arguments': args})
        assert result.get('resultType') in (None, 'complete'), result
        return result

    def succeeded(name, **args):
        r = call(name, **args)
        assert not r.get('isError'), (name, r)
        return r.get('structuredContent') or r

    def denied(name, **args):
        r = call(name, **args)
        assert r.get('isError'), (name, r)

    try:
        tools = request('tools/list')['tools']
        names = {t['name'] for t in tools}
        expected = {
            'read_file', 'get_file_info', 'write_file', 'append_file',
            'list_directory', 'create_directory', 'directory_tree',
            'search_files', 'find_by_extension', 'search_text', 'grep_regex',
            'replace_text', 'apply_patch', 'git_status', 'git_diff', 'git_log',
            'run_command', 'run_build', 'http_get', 'download_file',
            'project_info', 'detect_build_system', 'find_symbols'
        }
        assert expected <= names, names
        assert len(names) == 23, len(names)

        # 1-2: 文件读取/写入，默认拒绝覆盖，追加受控。
        succeeded('create_directory', path='src')
        succeeded('write_file', path='src/main.cpp', content='int magicNumber = 12;\n')
        denied('write_file', path='src/main.cpp', content='overwrite attempt')
        succeeded('append_file', path='src/main.cpp', content='// helper\n')
        assert 'magicNumber' in succeeded('read_file', path='src/main.cpp')['content']
        assert succeeded('get_file_info', path='src/main.cpp')['bytes'] > 0

        # 3-5: 目录树、文件通配符与跨文件文本搜索。
        assert succeeded('list_directory', path='src')['items']
        assert succeeded('directory_tree', path='.')['items']
        assert succeeded('search_files', directory='.', pattern='*.cpp')['files']
        assert succeeded('find_by_extension', directory='.', extension='.cpp')['files']
        assert succeeded('search_text', directory='.', text='magicNumber')['matches']
        assert succeeded('grep_regex', directory='.', pattern=r'magicNumber\s*=\s*12')['matches']

        # 6: 只替换唯一匹配；严格验证统一补丁上下文。
        succeeded('replace_text', path='src/main.cpp', old_text='12', new_text='42')
        denied('replace_text', path='src/main.cpp', old_text='not found', new_text='x')
        succeeded('apply_patch', path='src/main.cpp',
                  patch='@@ -1,2 +1,2 @@\n-int magicNumber = 42;\n+int magicNumber = 100;\n // helper\n')
        assert '100' in succeeded('read_file', path='src/main.cpp')['content']
        denied('apply_patch', path='src/main.cpp',
               patch='@@ -1 +1 @@\n-bad old text\n+something\n')

        # 路径穿越、绝对路径、工作区符号链接和任意覆盖全部拒绝。
        denied('read_file', path='../outside')
        denied('write_file', path='../escaped.txt', content='bad')
        denied('write_file', path=str(pathlib.Path(tmp).resolve() / 'absolute.txt'), content='bad')
        if os.name != 'nt':
            outside = pathlib.Path(tmp) / 'outside.txt'
            outside.write_text('SECRET')
            (pathlib.Path(tmp) / 'demo_workspace' / 'link').symlink_to(outside)
            denied('read_file', path='link')
        assert not (pathlib.Path(tmp) / 'escaped.txt').exists()

        # 7: Git 命令使用固定可执行程序与固定参数。
        git_version = succeeded('run_command', command='git_version')
        if git_version['exitCode'] == 0:
            subprocess.run(['git', '-C', str(pathlib.Path(tmp)/'demo_workspace'), 'init', '-q'], check=True)
            assert succeeded('git_status')['exitCode'] == 0
            assert succeeded('git_diff')['exitCode'] == 0
            assert 'exitCode' in succeeded('git_log')  # 无提交时可以返回非零状态
        denied('run_command', command='rm -rf .')
        assert succeeded('run_command', command='cmake_version')['exitCode'] == 0

        # 8: 构建只使用内部固定源码且需要显式确认。
        denied('run_build', confirm=False)
        if shutil.which('cmake'):
            output = succeeded('run_build', confirm=True)
            assert output['configure']['exitCode'] == 0, output
            assert output['build']['exitCode'] == 0, output

        # 9: HTTP 和下载对未授权域名立即拒绝，无需联网。
        denied('http_get', url='http://127.0.0.1:12345/admin')
        denied('download_file', url='https://attacker.example/file', path='x.txt')
        assert not (pathlib.Path(tmp) / 'demo_workspace' / 'x.txt').exists()

        # 10: C++ 工程与符号识别。
        found = succeeded('find_symbols', symbol='magicNumber')
        assert found['matches'], found
        assert succeeded('project_info')['sourceFiles'] >= 1
        assert 'buildFiles' in succeeded('detect_build_system')
        print('All 23 MCP tools / 10 groups and path, command, patch, network safeguards passed')
    finally:
        p.stdin.close()
        try:
            exit_code = p.wait(timeout=5)
            assert exit_code == 0, p.stderr.read()
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
