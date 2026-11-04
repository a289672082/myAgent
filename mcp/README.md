# MCP SDK — integrated with myAgent

本目录是从独立仓库 **a289672082/myMcpTools** 的 `main` 最新提交
`3a6c8bf434ae3c1fd708ff43aab6b24f3e8dbd30` 导入的源码、示例、回归测试及文档。
该仓库不再是构建、运行或 Git 子模块依赖。MCP 公共 API 名称（`mymcp::Client`、`mymcp::Server`）保持不变。

目录：`mcp/sdk/include/mymcp`、`mcp/sdk/src`、`mcp/examples`、`mcp/tests`。
直接依赖源码位于仓库根目录的 `third_party/jsoncons` 和 `third_party/cpp-httplib`
（固定上游子模块；需首次运行 `git submodule update --init --recursive`）。

```sh
cmake -S mcp/sdk -B build-mcp-sdk
cmake --build build-mcp-sdk

cmake -S mcp -B build-mcp -DCMAKE_BUILD_TYPE=Debug
cmake --build build-mcp
ctest --test-dir build-mcp --output-on-failure
```

下面保留了上游完整功能说明，但其中从独立 myMcpTools 仓库根目录运行的历史命令
在本仓库中需将 `-S .` 改为 `-S mcp`，路径 `examples/` 改为 `mcp/examples/`。

---

# myMcpTools

A C++20 MCP client and server library with convenient typed tool registration.

> **Status: experimental prototype, NOT certified against the full MCP specification.**
> Implements a subset of the MCP **2026-07-28** wire protocol. Do not use it as a production-facing unauthenticated service.

## Example (server)

```cpp
#include "mymcp/mcp.hpp"
using namespace mymcp;

int main() {
  Server server("calculator", "1.0.0");
  server.tool("add", "Add integers", [](int a, int b) { return a + b; },
              arg<int>("a", "First number"), arg<int>("b", "Second number"));
  server.resource("app://status", "Status", [] { return std::string("OK"); });
  server.prompt("welcome", "Welcome prompt", [](const Json&) { return "Hi!"; });
  server.run_stdio();                 // or server.run_http(3000)
}
```

## Example (client)

```cpp
#include "mymcp/mcp.hpp"
using namespace mymcp;

int main() {
  auto client = Client::process("./mymcp_example_server");
  auto info = client.discover();
  auto list = client.tools();
  auto answer = client.call("add", json::object{{"a", 1}, {"b", 2}});
}
```

For an HTTP client:

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp");
```

## Standalone no-argument demos

See **[examples/README.md](examples/README.md)**. Every demonstration is a standalone executable named `run_xxxx_demo`, containing a matching `run_xxxx_demo()` function and a zero-argument `main()`. Examples include `run_tools_demo`, `run_resources_demo`, `run_prompts_demo`, `run_tasks_demo`, `run_mrtr_demo`, `run_stdio_client_demo`, `run_progress_demo` and `run_oauth_demo` (local mock OAuth). The old CLI-driven examples are retained only as integration test fixtures in `tests/fixtures/`.

## Build

Requirements (migration): C++20, CMake >= 3.20, OpenSSL, Threads, and the checked-in third-party sources (`third_party/jsoncons` and `third_party/cpp-httplib`). Configuration and builds are offline by default. No Boost dependency; OpenSSL remains a system prerequisite.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run from the build output directory (Windows: add `.exe`):

```sh
./run_tools_demo
./run_resources_demo
./run_oauth_demo  # local, no external credentials
```

To exercise HTTP and SSE, run `run_http_server_demo` in one terminal, then `run_http_client_demo` and `run_subscriptions_demo` in another. Nothing requires command-line parameters.

## Protocol coverage — October 2026

**Target: MCP specification 2026-07-28. Experimental SDK, NOT full conformance.**

Implemented and covered by local tests:

- Modern stateless request metadata and `resultType`, `server/discover`, legacy stdio fallback.
- Typed C++ tool registration, tool listing/calling, raw handlers for rich tool results.
- Resources and URI templates (simple `{variable}` expansion), full prompt-result handlers and completion.
- Deterministic listing and opaque cursor pagination for tools/resources/prompts/templates; caching `ttlMs` and `cacheScope` on all six required operations.
- MRTR `input_required` on valid operations, client-side callback and `requestState` round trips.
- `subscriptions/listen`: stdio acknowledgement and cancellation, HTTP long-lived SSE with acknowledgement/keepalive, per-subscription filters and explicit `Server::publish(...)`, `Client::listen_http(...)`.
- Custom method handler and extension-capability advertisement APIs.
- Streamable HTTP POST routing/header verification, `x-mcp-header` projection, origin checks, loopback-only listener. Request-scoped SSE progress/response streaming and incremental client event delivery; JSON is used for fast server responses.

Still incomplete (important before production use):

- Remote schema retrieval and schema-derived output validation, typed content types and URI template operators beyond simple variables. The JSON Schema validator now uses jsoncons with 2020-12 support for local schemas.
- OAuth gaps: an in-app browser/callback listener, secure persistent token storage, automatic authorization retry and incremental scope escalation, Client ID Metadata validation, and built-in JWT/JWKS verification. The client has discovery + PKCE + token/refresh flows; the server offers a Bearer verifier callback. The HTTP server binds loopback only; use a trusted HTTPS proxy for remote deployment.
- Full legacy HTTP sessions/GET notification channels, reliable disconnect cancellation of blocked handlers, log notifications, and custom timeout/retry configuration.
- Complete Tasks / Apps / EMA extension implementations (raw custom method dispatch and namespaced capability advertisement are available, but **do not** imply extension conformance).
- SSE replay/resumption and automatic stream restart, full stdio asynchronous client subscription management, cross-platform concurrency load testing, full official SDK conformance/interop tests.

See [COMPATIBILITY.md](COMPATIBILITY.md) for a conservative per-feature matrix. Passing local tests is not an official compliance certificate.

## New API examples

```cpp
Server server("books", "1.0");
server.cache(60000, "private");
server.page_size(50);
server.resource_template("books://{id}", "Book",
  [](const std::string& uri, const Json& vars) -> Json {
    return json::object{{"contents", json::array{json::object{{"uri", uri},
      {"text", std::string(vars.at("id").as_string())}}}}};
  });
server.completion([](const Json&) -> Json {
  return json::object{{"completion", json::object{{"values", json::array{"cpp", "c++"}}, {"hasMore", false}}}};
});

// Consumers can transparently aggregate all pages:
auto all = client.list_all("tools/list", "tools");

// HTTP notifications: callback returns false to close the SSE connection.
Client::listen_http("http://127.0.0.1:3000/mcp", json::object{{"toolsListChanged", true}},
  [](const Json& event) {
    std::cout << json::serialize(event) << "\n";
    return true;
  });
```

For stdio/HTTP subscriptions, event producers call `server.publish("notifications/tools/list_changed")` or `server.publish("notifications/resources/updated", json::object{{"uri", "..."}})`. The SDK enforces the requested subscription filter and sends an acknowledgment first. Tool catalogs should be registered before starting the server; dynamic mutation of handler maps is not currently thread safe.

## Spec references

- [MCP 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28)
- [Discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)

## Expanded APIs (October 2026)

Tasks are opt-in and are advertised to clients explicitly:

```cpp
server.enable_tasks();
server.add_task_tool("background", "Long-running work", {{"type", "object"}},
    [](const mymcp::Json& responses, std::atomic_bool& cancelled) -> mymcp::Json {
      if (cancelled) return {{"content", mymcp::json::array{}}};
      return mymcp::detail::make_tool_result("finished");
    });

mymcp::Client client = mymcp::Client::process("./my_server");
client.enable_tasks(); // must be called before discover()
auto handle = client.call("background");
auto state = client.get_task(std::string(handle.at("taskId").as_string()));
// client.update_task(taskId, inputResponses);  client.cancel_task(taskId);
```

The server task registry is process-local, **not durable across restarts**. HTTP tasks calls also validate the `Mcp-Name` header against `taskId`.

A stdio server may report progress from inside an active tool handler using `server.progress(value, total, "message")`, and honor cancellation using `server.is_cancelled()`; the client can use `request_with_progress` and a `Client::process` notification callback. HTTP progress uses a request-scoped SSE response when `progressToken` is present; the SDK delivers these notifications incrementally, but cancellation of a blocked handler still depends on a failed SSE write.

For protected HTTP, `server.protect_http(protected_resource_metadata, token_verifier)` installs the metadata endpoint and authorization callback. The verifier must validate issuer, audience, expiry and required scopes. The test fixture server supports a local-only authentication test using `MYMCP_TEST_TOKEN`; new no-argument demo programs live in `examples/`. `Client::http("https://host/mcp", token)` validates TLS certificates/hostnames and includes a Bearer token. `mymcp/oauth_client.hpp` provides OAuth metadata discovery, PKCE S256, registration, token exchange and refresh. It does **not** independently implement interactive browser login or durable token storage.

All raw tool handlers are checked with jsoncons JSON Schema validation (see `include/mymcp/schema.hpp`). Schema violations return JSON-RPC `-32602`, not tool `isError`. Remote schema reference handling and official MCP conformance testing remain outstanding. See [COMPATIBILITY.md](COMPATIBILITY.md) and [SECURITY.md](SECURITY.md).

## Dependency migration (C++20 / cpp-httplib / jsoncons)

This experimental migration removes Boost headers/libraries from the SDK. CMake fetches
pinned `cpp-httplib` and `jsoncons` revisions at configuration time (network access required
on first configure); OpenSSL remains required for TLS and PKCE. JSON values are
`mymcp::Json` (`jsoncons::json`), so callers directly using `boost::json::value`
need to update their source. CMake package exporting vendored dependencies is still being refined.

OAuth client-side interface:

```cpp
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("https://client.example/oauth-metadata.json");
auto authorization = session->begin_authorization("http://127.0.0.1:8900/callback", {"tools:read"});
// Open authorization.url in a browser, receive redirect code/state/iss through the app.
session->exchange_code(authorization, code, returned_state, returned_issuer,
                       "http://127.0.0.1:8900/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();
```

The SDK does **not** host an OAuth Authorization Server or control the browser; OAuth
redirect handling must be implemented by the application. Token validation on the
server remains pluggable through `Server::protect_http(metadata, verifier)`, where
`verifier` is responsible for cryptographically validating the credential and enforcing
issuer, audience, expiration, subject and operation-level scopes.

The migration must pass Linux + Windows CI before it is treated as a release.

## OAuth 资源隔离与 HTTP x-mcp-header 缓存

**OAuth 安全边界**：调用 Client::http_oauth(mcp_url, session) 时，mcp_url 必须与创建 oauth::Session 时指定的 MCP Resource URI 完全一致。不同主机、端口、路径或协议，即使使用同一个 OAuth Server，也不能共享 Bearer Token。公网 MCP Resource 必须使用 HTTPS；仅 127.0.0.1、::1、localhost 可以使用 HTTP。检查在构造客户端时完成，尚未发送任何网络请求。该检查不会自动跟随 HTTP 重定向。

**HTTP 参数头**：新版 MCP 工具可以在 tools/list 的 inputSchema 属性上标注 x-mcp-header。Client::call() 首次调用前会完整读取 tools/list 的全部分页，将带标注的 JSON Schema 合并缓存；工具调用时根据参数路径生成对应 Mcp-Param-* 请求头，支持嵌套属性、字符串、布尔值和安全范围内的整数。特殊字符按已有编码规则处理。非法或重复（大小写不敏感）的头名会提前拒绝，不能由调用者绕开 Schema 任意加入自定义头。

缓存只在全部分页成功后发布；重新查询目录及收到 HTTP SSE notifications/tools/list_changed 时会失效并重建。对旧式无注解的 MCP 工具不增加额外参数头。服务端仍须校验权限、参数头和传入的 JSON Schema，客户端映射不替代认证。

离线回归测试：

~~~sh
ctest --test-dir build -R "(oauth_resource_binding|http_header_routing)" --output-on-failure
~~~

tests/test_oauth_resource_binding.cpp 验证跨资源令牌拒绝及本机 HTTP 兼容；tests/test_http_header_routing.cpp 使用真实 loopback HTTP Mock 验证两页 Schema、请求头映射、刷新、SSE 失效和重复注解拒绝。

## Desktop OAuth callback helper

```cpp
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("your-registered-client-id");
auto flow = session->begin_authorization("http://127.0.0.1:8765/callback");
// Open flow.url in the OS browser, then receive the redirect in your app.
// The helper checks the full redirect URI, state, issuer and percent-encoding.
session->exchange_callback_url(flow, received_callback_url,
                               "http://127.0.0.1:8765/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();  // refreshes expiring tokens automatically
```

Dynamic registration for a desktop/CLI app uses `application_type=native` by default,
which is required for interoperability with some OpenID Connect servers. Web apps
can pass `"web"` explicitly to `register_dynamic(redirects, name, "web")`.
The host application still owns browser navigation, redirect listener and safe
credential storage; this is **not** a built-in authorization server.

## Independent, no-argument demos

`examples/` now contains single-purpose programs named `run_xxxx_demo.cpp`,
each with `run_xxxx_demo()` and a zero-argument `main()`. For example:

```sh
./run_tools_demo
./run_resources_demo
./run_prompts_demo
./run_tasks_demo
./run_mrtr_demo
./run_oauth_demo             # local mock provider, no credentials
```

For HTTP and SSE, launch `run_http_server_demo` in another terminal, then run
`run_http_client_demo` or `run_subscriptions_demo`. For a real provider, edit the
configuration constants in `examples/run_oauth_login_demo.cpp` and run it.
See [the demo guide](examples/README.md). Old parameterized executables are
retained only in `tests/fixtures/` for regression testing.


## Codex / Claude Code and Streamable HTTP

Use Streamable HTTP as the preferred network transport; **SSE is a response format inside it**, not a separate transport. The HTTP SDK now supports modern `2026-07-28` requests, a basic legacy Streamable HTTP adapter (2025-03-26 to 2025-11-25), and tool progress over request-scoped SSE. See [INTEROPERABILITY.md](INTEROPERABILITY.md) for connection commands, compatibility limits and security instructions.

A tool can report progress without caring whether it runs on stdio or HTTP:

```cpp
server.add_tool("count", "Report progress", mymcp::json::object{{"type", "object"}},
  [&server](const mymcp::Json&) -> mymcp::Json {
    for (int i = 1; i <= 5 && !server.is_cancelled(); ++i) server.progress(i, 5, "Working");
    return mymcp::detail::make_tool_result("done");
  });
```

In an HTTP client, install a progress event callback and include a `progressToken`:

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp", {},
  [](const mymcp::Json& event) { /* handle notifications/progress */ });
auto result = client.request_with_progress("tools/call",
  mymcp::json::object{{"name", "count"}, {"arguments", mymcp::json::object{}}}, "my-token");
```

To run a complete no-argument client/server pair, start `run_http_server_demo`, then `run_http_progress_demo` in another terminal.

## All-in-one agent toolbox demo

**`examples/run_agent_tools_demo.cpp`** is a single no-argument stdio MCP server that registers 23 useful tools across 10 groups: file reads/writes, directories, filename/content search, precise edits/patches, Git, allowlisted commands and a fixed build example, restricted HTTPS fetch/download, and C++ project inspection. The implementation has Chinese inline comments and runs within an automatically created `demo_workspace/` directory. It is intended for local Codex/Claude MCP experiments, **not** a hardened sandbox. See [examples/README.md](examples/README.md) for the complete tool matrix, security limits, and usage.


## MCP 传输截止时间与资源限额（P0/P1）

SDK 自带 Client::process / Client::http / Client::http_oauth 支持 RequestOptions（OAuth 刷新凭据阶段另有同步 I/O 限制）。默认 30 秒请求截止时间；超时或取消后，stdio 子进程被终止并回收。HTTP/SSE 响应在追加数据前检查大小。

~~~cpp
mymcp::RequestOptions limits;
limits.timeout = std::chrono::seconds(2);
limits.max_response_bytes = 256 * 1024;
limits.cancelled = [&] { return stopped.load(); };
auto client = mymcp::Client::process("/path/to/mcp-server");
auto response = client.request("tools/list", mymcp::json::object{}, limits);
~~~

stdio 默认上限 8 MiB，HTTP SSE 帧默认上限 4 MiB。指定 max_response_bytes 时，也限制累计响应字节。显式 timeout=0 表示关闭截止时间，不建议用于不可信 MCP。

注意：自定义 Client 回调、宿主 C++ 函数、OAuth 提供方的同步凭据处理及不合作的回调代码无法通过强杀线程安全中断。需要真正可终止的工具时，请使用独立 MCP stdio 子进程，并在 OS 层增加权限、资源和进程树隔离。
