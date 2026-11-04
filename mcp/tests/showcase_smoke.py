"""End-to-end examples test: stdio, HTTP and resource-update SSE."""
import socket
import subprocess
import sys
import time

server_exe, client_exe, subscriber_exe = sys.argv[1:4]


def check_run(command, timeout=24):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode or not ("SHOWCASE_OK" in result.stdout or "SUBSCRIPTION_OK" in result.stdout):
        raise AssertionError(f"{command!r} failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result.stdout


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


print("Checking stdio showcase...")
stdio = check_run([client_exe, "--stdio", server_exe])
assert "MRTR:" in stdio and "task:" in stdio and "progress:" in stdio

port = pick_port()
url = f"http://127.0.0.1:{port}/mcp"
process = subprocess.Popen([server_exe, "--http", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError(f"HTTP server stopped: {process.stderr.read().decode(errors='replace')}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                break
        except OSError:
            time.sleep(.05)
    else:
        raise RuntimeError("HTTP showcase server startup timeout")
    print("Checking HTTP showcase...")
    check_run([client_exe, "--http", url])
    print("Checking SSE subscription showcase...")
    check_run([subscriber_exe, url], timeout=15)
finally:
    process.terminate()
    try:
        process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate(timeout=5)
print("SHOWCASE_EXAMPLES_TEST_OK")
