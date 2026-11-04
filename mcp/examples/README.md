# Standalone C++20 MCP demos (no command-line arguments)

Every file `run_xxxx_demo.cpp` defines `int run_xxxx_demo()` plus a zero-argument `int main()` that calls it. The corresponding CMake executable is **`run_xxxx_demo`** (Windows: `.exe`). Each demo focuses on one feature; no `--http`, `--stdio`, URL, port or credential command-line options are used.

Build once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For Visual Studio multi-config builds, add `--config Debug` to the build command, and `-C Debug` to `ctest`. Run demo executables from your build output directory. Most demos complete on their own; the three servers are designed to keep listening until terminated.

## One executable = one feature

| Run directly | What it demonstrates | Prerequisite |
| --- | --- | --- |
| `run_tools_demo` | Typed tools, auto JSON Schema, validation errors, pagination, discovery | None |
| `run_resources_demo` | Static resources, templates, cached resource lists | None |
| `run_prompts_demo` | Prompt arguments and completion | None |
| `run_mrtr_demo` | Multi round-trip user elicitation on a harmless mock approval | None |
| `run_tasks_demo` | Task creation, async execution and status polling | None |
| `run_extensions_demo` | Extension capability and custom method | None |
| `run_stdio_client_demo` | Launch `run_stdio_server_demo`, discover and call a tool over stdio | Automatically finds its companion executable |
| `run_stdio_server_demo` | Stdio MCP server reading JSON-RPC from stdin | Started by stdio client (or host application) |
| `run_progress_demo` | Report progress notifications from a stdio tool | Automatically starts stdio server |
| `run_http_server_demo` | Loopback HTTP server, tools, resources and publishing | Leave running in terminal 1 |
| `run_http_client_demo` | HTTP MCP client requests | Run HTTP server first |
| `run_subscriptions_demo` | SSE subscription, acknowledgement and resource-update event | Run HTTP server first |
| `run_oauth_demo` | **No-credential mock OAuth**: discovery, native registration, PKCE S256, callback, token exchange | None |
| `run_oauth_login_demo` | Real OAuth server with browser callback and bearer-protected MCP calls | Edit config in source, use a real OAuth provider |
| `run_oauth_resource_demo` | Protected Resource Metadata, 401 challenge, local-only fixed demo Bearer verifier | Leave running in terminal; **development only** |
| `run_oauth_resource_client_demo` | Send a mock Bearer token to the local protected resource server | Run `run_oauth_resource_demo` first |

### Start two HTTP examples with no arguments

Terminal 1:

```bash
./run_http_server_demo
```

Terminal 2:

```bash
./run_http_client_demo
./run_subscriptions_demo
```

The fixed local address/port is defined in `demo_common.hpp` (`http://127.0.0.1:33867/mcp`). Change that header to use another port; both server and clients will share it.

### OAuth walkthrough

Try `./run_oauth_demo` **first**. It starts an in-memory mock authorization server on a random loopback port, then runs protected resource metadata discovery, native dynamic registration, PKCE S256 construction, validates a simulated OAuth callback and exchanges the code. It needs **no real credentials** and does not present a real authorization UI.

For a **real** provider, edit the `configuration` constants at the top of `run_oauth_login_demo.cpp`:

```cpp
constexpr const char* mcp_resource = "https://your-mcp-host.example/mcp";
constexpr const char* public_client_id = "YOUR_PUBLIC_CLIENT_ID";
constexpr bool use_dynamic_registration = false;
constexpr const char* requested_scope = "tools:read";
```

Then rebuild and run `./run_oauth_login_demo`. It opens a local callback listener on an OS-selected port, prints an authorization URL for you to open, waits up to 180 seconds, validates callback state and issuer, exchanges the code and calls MCP with the OAuth session. No tokens are printed or saved; do not commit actual private credentials. The OAuth provider must allow the selected loopback redirect URI or support native dynamic registration.

`run_oauth_resource_demo` is **not an OAuth authorization server** and cannot be used to log in with `run_oauth_login_demo`. It demonstrates only the SDK's protected-resource metadata and Bearer validation hook. Its fixed token is for local security-shape testing, **not a production token verifier**. In real services, verify token signature/introspection, issuer, audience, expiry and scopes; deploy with HTTPS.

## Legacy integration test fixtures

The previous command-line-driven `showcase_*`, `oauth_login.cpp` and `oauth_resource_server.cpp` files have moved to `tests/fixtures/` to keep existing integration tests without exposing old parameter-driven examples. They are compiled only as test fixtures. All files in `examples/` now use the independent `run_xxxx_demo` entrypoint convention.

The SDK is still experimental; these demos do **not** establish full MCP 2026-07-28 conformance. See [`COMPATIBILITY.md`](../COMPATIBILITY.md).


### Streamable HTTP + request-scoped SSE

Start `run_http_server_demo`, then run `run_http_progress_demo` (no flags). The tool sends five real-time `notifications/progress` SSE events and a final JSON-RPC response; ordinary fast tools still return `application/json`. For Codex and Claude Code configuration see [`../INTEROPERABILITY.md`](../INTEROPERABILITY.md).

## 一个 Demo：10 组智能体必备工具（中文注释）

新增 **`run_agent_tools_demo.cpp`**：单个 C++20 stdio MCP Server 提供 10 组共 23 个真实工具，直接运行 `run_agent_tools_demo`，不接受任何命令行参数。

| 类别 | MCP 工具 |
| --- | --- |
| 文件读取 | `read_file`, `get_file_info` |
| 文件写入 | `write_file`, `append_file` |
| 目录 | `list_directory`, `create_directory`, `directory_tree` |
| 文件查找 | `search_files`, `find_by_extension` |
| 内容搜索 | `search_text`, `grep_regex` |
| 文件编辑 | `replace_text`, `apply_patch` |
| Git 查询 | `git_status`, `git_diff`, `git_log` |
| 命令/编译 | `run_command`, `run_build` |
| HTTP 获取 | `http_get`, `download_file` |
| C++ 工程分析 | `project_info`, `detect_build_system`, `find_symbols` |

### 直接接入智能体（stdio）

通过 CMake 构建后，使用编译出的 `run_agent_tools_demo` 可执行文件路径配置 Codex、Claude Code 等 MCP 客户端。程序不解析 argv，也不需要额外的配置文件。标准输出只写 MCP JSON-RPC，诊断信息写到标准错误。

- **Codex**：`codex mcp add agent-tools -- /ABSOLUTE/PATH/run_agent_tools_demo`。
- **Claude Code**：`claude mcp add agent-tools -- /ABSOLUTE/PATH/run_agent_tools_demo`。

**安全限制**：启动时在进程当前工作目录创建 `demo_workspace/`，只允许访问其内部的相对路径；拒绝绝对路径、`..`、符号链接、超大文件（1 MiB）、超量遍历。写文件默认不覆盖；补丁验证上下文；命令仅允许版本查询，构建仅允许 SDK 自动创建且校验原文的 CMake 样例；HTTPS 仅允许 `https://example.com/`，不跳转、不接受任意 URL。HTTP 工具需要联网，其余工具可离线运行。工作区中不存在 Git 仓库时，Git 工具返回子进程的非零退出码，可在工作区自行 `git init`。

`run_build(confirm=true)` 是代码执行操作，即便仅允许固定样例，也应经由智能体产品取得用户的明确授权。**本 Demo 的文件系统防护不能抵御拥有同一操作系统账户、可同时恶意修改工作区的攻击者（存在检查与使用之间的竞态）；不要将它直接当作公共网络的安全沙箱或多用户隔离设施。**

运行 `ctest -R agent_tools_demo --output-on-failure` 会用真实 stdio JSON-RPC 测试全部工具与常见拒绝路径。
