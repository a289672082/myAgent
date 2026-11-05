# myMcpTools

提供 MCP 客户端与服务端功能的 C++20 库，支持便捷的强类型工具注册。

> **项目状态：实验性原型，尚未通过完整 MCP 规范符合性认证。**
> 当前仅实现 MCP **2026-07-28** 传输协议的部分功能。请勿将其作为未经身份认证的公网生产服务。

## 服务端示例

```cpp
#include "mymcp/mcp.hpp"
using namespace mymcp;

int main() {
  Server server("calculator", "1.0.0");
  server.tool("add", "Add integers", [](int a, int b) { return a + b; },
              arg<int>("a", "First number"), arg<int>("b", "Second number"));
  server.resource("app://status", "Status", [] { return std::string("OK"); });
  server.prompt("welcome", "Welcome prompt", [](const Json&) { return "Hi!"; });
  server.run_stdio();                 // 也可使用 server.run_http(3000)
}
```

## 客户端示例

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

HTTP 客户端的创建方式：

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp");
```

## 无命令行参数的独立演示程序

参见 **[演示程序指南](examples/README.md)**。每个示例都是独立的 `run_xxxx_demo` 可执行程序，包含同名的 `run_xxxx_demo()` 函数和无参数 `main()`。例如 `run_tools_demo`、`run_resources_demo`、`run_prompts_demo`、`run_tasks_demo`、`run_mrtr_demo`、`run_stdio_client_demo`、`run_progress_demo` 和 `run_oauth_demo`（本机模拟 OAuth）。旧版带命令行参数的程序仅保留在 `tests/fixtures/` 中作为集成测试辅助程序。

## 构建

构建要求：C++20、CMake 3.20 或更新版本、OpenSSL、线程库，以及仓库内置的第三方源码（`third_party/jsoncons` 和 `third_party/cpp-httplib`）。默认离线配置与构建，不依赖 Boost；OpenSSL 仍需要由系统提供。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

请在构建输出目录运行示例（Windows 需追加 `.exe`）：

```sh
./run_tools_demo
./run_resources_demo
./run_oauth_demo  # 本地模拟，不需要外部凭据
```

测试 HTTP 与 SSE 时，在一个终端运行 `run_http_server_demo`，再在另一个终端运行 `run_http_client_demo` 和 `run_subscriptions_demo`。这些程序均不要求命令行参数。

## 协议覆盖情况（2026 年 10 月）

**目标版本：MCP 2026-07-28。本 SDK 仍属实验性实现，不代表完全符合规范。**

已实现且有本地测试覆盖的功能：

- 现代无状态请求元数据、`resultType`、`server/discover`，以及旧版 stdio 兼容回退。
- C++ 强类型工具注册、工具列表与调用，以及支持复杂结果的原始处理函数。
- 资源和 URI 模板（简单 `{variable}` 变量展开）、完整的提示词结果处理函数和补全功能。
- 工具、资源、提示词和模板的确定性列表与不透明游标分页；六项必需操作支持 `ttlMs`、`cacheScope` 缓存字段。
- 在有效操作中使用 MRTR `input_required`，客户端回调与 `requestState` 多轮往返。
- `subscriptions/listen`：stdio 确认与取消、HTTP 长连接 SSE 的确认及保活、按订阅过滤事件，并支持 `Server::publish(...)` 和 `Client::listen_http(...)`。
- 自定义方法处理函数，以及扩展能力声明接口。
- Streamable HTTP POST 路由及请求头校验、`x-mcp-header` 参数映射、来源校验和仅监听回环地址的服务端；支持单请求 SSE 进度/结果流与客户端增量事件处理，快速请求则返回 JSON。

尚未完整实现的功能（生产使用前务必了解）：

- 远程 Schema 获取、根据 Schema 校验输出、全部结构化内容类型，以及超出简单变量替换范围的 URI 模板运算符。当前 JSON Schema 校验器使用 jsoncons，支持本地 2020-12 Schema。
- OAuth 尚缺：应用内浏览器及回调监听的通用封装、安全持久化令牌、自动重新授权与渐进式权限提升、客户端 ID 元数据校验、内置 JWT/JWKS 校验。客户端已有发现、PKCE、令牌交换与刷新流程；服务端提供 Bearer 验证回调。HTTP 服务端仅绑定回环地址，远程部署应使用可信 HTTPS 代理。
- 完整的旧版 HTTP 会话及 GET 通知通道、阻塞处理函数的断线可靠取消、日志通知，以及全方位自定义超时与重试。
- 完整的 Tasks / Apps / EMA 扩展实现（虽然支持原始自定义方法分发和命名空间能力声明，但**不代表通过扩展规范验证**）。
- SSE 重放与恢复、自动重新建立流、完整的 stdio 异步客户端订阅管理、跨平台并发压力测试，以及官方 SDK 符合性与互操作测试。

详细功能矩阵请参见 [协议兼容性说明](COMPATIBILITY.md)。通过本地测试并不等同于通过官方规范认证。

## 新增 API 使用示例

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

// 客户端可以透明地合并全部分页：
auto all = client.list_all("tools/list", "tools");

// HTTP 通知：回调返回 false 时关闭 SSE 连接。
Client::listen_http("http://127.0.0.1:3000/mcp", json::object{{"toolsListChanged", true}},
  [](const Json& event) {
    std::cout << json::serialize(event) << "\n";
    return true;
  });
```

对于 stdio/HTTP 订阅，事件发布方可调用 `server.publish("notifications/tools/list_changed")` 或 `server.publish("notifications/resources/updated", json::object{{"uri", "..."}})`。SDK 会应用订阅过滤规则，并首先发送确认消息。应在启动服务端之前注册工具目录；当前不保证运行时动态修改处理函数映射的线程安全。

## 协议规范参考

- [MCP 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28)
- [服务发现](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Streamable HTTP 传输](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
- [工具](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)

## 扩展 API（2026 年 10 月）

Tasks 任务功能需要主动启用，服务端会明确向客户端声明相关能力：

```cpp
server.enable_tasks();
server.add_task_tool("background", "Long-running work", {{"type", "object"}},
    [](const mymcp::Json& responses, std::atomic_bool& cancelled) -> mymcp::Json {
      if (cancelled) return {{"content", mymcp::json::array{}}};
      return mymcp::detail::make_tool_result("finished");
    });

mymcp::Client client = mymcp::Client::process("./my_server");
client.enable_tasks(); // 必须在 discover() 之前调用
auto handle = client.call("background");
auto state = client.get_task(std::string(handle.at("taskId").as_string()));
// client.update_task(taskId, inputResponses);  client.cancel_task(taskId);
```

服务端任务注册表仅保存在当前进程内，**重启后不会保留**。HTTP 任务调用还会核对 `Mcp-Name` 请求头与 `taskId` 是否一致。

stdio 服务端可以在运行中的工具处理函数内调用 `server.progress(value, total, "message")` 报告进度，使用 `server.is_cancelled()` 响应取消；客户端可使用 `request_with_progress` 和 `Client::process` 通知回调。HTTP 请求包含 `progressToken` 时，进度信息会通过当前请求的 SSE 响应流增量返回；但阻塞处理函数的取消仍依赖 SSE 写入失败后的检测。

对于需要保护的 HTTP 服务，`server.protect_http(protected_resource_metadata, token_verifier)` 会安装受保护资源元数据端点与授权回调。验证器必须检查令牌签发者、受众、到期时间与操作权限范围。测试服务仅通过 `MYMCP_TEST_TOKEN` 演示本机身份认证，新的无参数程序见 `examples/`。`Client::http("https://host/mcp", token)` 校验 TLS 证书和主机名，并携带 Bearer 令牌。`mymcp/oauth_client.hpp` 提供 OAuth 元数据发现、PKCE S256、注册、令牌交换与刷新，但**不会**独立完成浏览器交互登录或安全持久化存储令牌。

所有原始工具处理函数均使用 jsoncons JSON Schema 进行输入校验（见 `include/mymcp/schema.hpp`）。Schema 违规会返回 JSON-RPC 错误 `-32602`，而不是工具级 `isError`。远程 Schema 引用与官方 MCP 符合性测试尚待完善，参见 [兼容性说明](COMPATIBILITY.md)和[安全说明](SECURITY.md)。

## 第三方依赖迁移（C++20 / cpp-httplib / jsoncons）

此次实验性迁移移除了 SDK 对 Boost 头文件与库的依赖。
`cpp-httplib` 与 `jsoncons` 的固定版本源码已经随仓库内置，
CMake 配置时不需要从网络获取它们；TLS 与 PKCE 仍依赖系统 OpenSSL。JSON 值类型为
`mymcp::Json`（即 `jsoncons::json`），此前直接使用 `boost::json::value` 的调用方
需要修改源码。包含内置依赖的 CMake 包导出机制仍在完善。

OAuth 客户端接口示例：

```cpp
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("https://client.example/oauth-metadata.json");
auto authorization = session->begin_authorization("http://127.0.0.1:8900/callback", {"tools:read"});
// 在浏览器打开 authorization.url，由应用接收重定向中的 code/state/iss。
session->exchange_code(authorization, code, returned_state, returned_issuer,
                       "http://127.0.0.1:8900/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();
```

SDK **不负责**托管 OAuth 授权服务器或控制浏览器；OAuth
重定向处理必须由宿主应用实现。服务端的令牌校验仍可通过
`Server::protect_http(metadata, verifier)` 注入自定义验证器，
`verifier` 必须以密码学手段验证令牌，并检查
签发者、受众、有效期、主体身份及操作级权限范围。

只有 Linux 与 Windows CI 验证通过后，才可将此迁移版本视作可发布候选。

## OAuth 资源隔离与 HTTP x-mcp-header 缓存

**OAuth 安全边界**：调用 Client::http_oauth(mcp_url, session) 时，mcp_url 必须与创建 oauth::Session 时指定的 MCP Resource URI 完全一致。不同主机、端口、路径或协议，即使使用同一个 OAuth Server，也不能共享 Bearer Token。公网 MCP Resource 必须使用 HTTPS；仅 127.0.0.1、::1、localhost 可以使用 HTTP。检查在构造客户端时完成，尚未发送任何网络请求。该检查不会自动跟随 HTTP 重定向。

**HTTP 参数头**：新版 MCP 工具可以在 tools/list 的 inputSchema 属性上标注 x-mcp-header。Client::call() 首次调用前会完整读取 tools/list 的全部分页，将带标注的 JSON Schema 合并缓存；工具调用时根据参数路径生成对应 Mcp-Param-* 请求头，支持嵌套属性、字符串、布尔值和安全范围内的整数。特殊字符按已有编码规则处理。非法或重复（大小写不敏感）的头名会提前拒绝，不能由调用者绕开 Schema 任意加入自定义头。

缓存只在全部分页成功后发布；重新查询目录及收到 HTTP SSE notifications/tools/list_changed 时会失效并重建。对旧式无注解的 MCP 工具不增加额外参数头。服务端仍须校验权限、参数头和传入的 JSON Schema，客户端映射不替代认证。

离线回归测试：

~~~sh
ctest --test-dir build -R "(oauth_resource_binding|http_header_routing)" --output-on-failure
~~~

tests/test_oauth_resource_binding.cpp 验证跨资源令牌拒绝及本机 HTTP 兼容；tests/test_http_header_routing.cpp 使用真实 loopback HTTP Mock 验证两页 Schema、请求头映射、刷新、SSE 失效和重复注解拒绝。

## 桌面应用 OAuth 回调辅助接口

```cpp
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("your-registered-client-id");
auto flow = session->begin_authorization("http://127.0.0.1:8765/callback");
// 使用系统浏览器打开 flow.url，再由应用接收重定向。
// 辅助接口校验完整重定向 URI、state、issuer 和百分号编码。
session->exchange_callback_url(flow, received_callback_url,
                               "http://127.0.0.1:8765/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();  // 自动刷新即将过期的令牌
```

桌面或命令行应用动态注册时默认使用 `application_type=native`，
以便兼容部分 OpenID Connect 服务端。Web 应用
可以显式调用 `register_dynamic(redirects, name, "web")` 传入 `"web"`。
浏览器跳转、回调监听和凭据安全存储仍由宿主应用负责；
这里**不提供**内置授权服务器。

## 独立、无参数的演示程序

`examples/` 包含以 `run_xxxx_demo.cpp` 命名的独立示例程序，
每个程序包含 `run_xxxx_demo()` 和无参数 `main()`，例如：

```sh
./run_tools_demo
./run_resources_demo
./run_prompts_demo
./run_tasks_demo
./run_mrtr_demo
./run_oauth_demo             # 本地模拟提供方，不需要凭据
```

要演示 HTTP 与 SSE，先在另一终端启动 `run_http_server_demo`，
再运行 `run_http_client_demo` 或 `run_subscriptions_demo`。连接真实 OAuth 服务时，
需要先编辑 `examples/run_oauth_login_demo.cpp` 中的配置常量，再运行该示例。
详情参见 [演示程序指南](examples/README.md)。旧版带命令行参数的可执行程序
仅在 `tests/fixtures/` 中保留，供回归测试使用。


## Codex / Claude Code 与 Streamable HTTP

网络传输优先使用 Streamable HTTP；**SSE 是其中的响应格式，而非独立传输方式**。HTTP SDK 支持现代 `2026-07-28` 请求、基本的旧版 Streamable HTTP 兼容适配（2025-03-26 至 2025-11-25）以及请求级 SSE 工具进度流。连接命令、兼容限制和安全要求参见 [互操作说明](INTEROPERABILITY.md)。

无论工具使用 stdio 还是 HTTP，都可以通过相同接口报告进度：

```cpp
server.add_tool("count", "Report progress", mymcp::json::object{{"type", "object"}},
  [&server](const mymcp::Json&) -> mymcp::Json {
    for (int i = 1; i <= 5 && !server.is_cancelled(); ++i) server.progress(i, 5, "Working");
    return mymcp::detail::make_tool_result("done");
  });
```

HTTP 客户端需要注册进度事件回调，并提供 `progressToken`：

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp", {},
  [](const mymcp::Json& event) { /* 处理 notifications/progress 事件 */ });
auto result = client.request_with_progress("tools/call",
  mymcp::json::object{{"name", "count"}, {"arguments", mymcp::json::object{}}}, "my-token");
```

完整运行无参数的客户端/服务端示例时，先启动 `run_http_server_demo`，再在另一个终端运行 `run_http_progress_demo`。

## 一体化智能体工具箱示例

**`examples/run_agent_tools_demo.cpp`** 是不需要命令行参数的 stdio MCP 服务端示例，共注册 10 组 23 个工具：文件读写、目录操作、文件名和内容搜索、精确替换与补丁、Git 查询、白名单命令及固定编译样例、受限 HTTPS 获取/下载和 C++ 工程分析。源码附有中文注释，会自动创建 `demo_workspace/` 工作区。本程序仅适合本地 Codex/Claude MCP 实验，**不能当作安全沙箱**。完整工具列表、安全边界与运行方法见 [演示程序指南](examples/README.md)。


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

## 五类 MCP 基础能力增强（2026-11-05）

本节描述新增的基础 API 和明确限制。基础支持不等于完整官方符合性，仍需通过生产环境安全审查与跨 SDK 互操作测试。

### Resources：二进制内容与 RFC 6570

使用 Server::resource_binary(uri, name, reader, mime_type) 注册原始二进制字节，resources/read 将其编码为 Base64 blob，并保留 mimeType（单个资源最多 4 MiB）。
包含 mymcp/uri_template.hpp 后，可使用 expand_uri_template(pattern, variables) 展开 URI。支持 RFC 6570 的 + # . / ; ? & 运算符、:prefix、* 以及数组和对象变量。读取模板资源时只安全地反向解析可明确匹配的标量变量，不猜测复杂对象的反向类型。

### Prompts：参数与返回结果校验

Server::prompt_full(name, description, handler, arguments, result_schema) 的第五个参数可选，默认为空 JSON Schema。注册时验证参数描述列表和重复名称，调用时验证必填字符串参数及未知参数；响应须包含 messages 数组，且角色及消息类型必须有效。非空 result_schema 用于校验完整返回结果。

### Streamable HTTP：断线取消及安全重试

Client::request_readonly_retry(method, params, max_attempts, options) 显式对 resources/read、列表、server/discover 等允许的只读方法重试（最多 4 次），每次使用新的 JSON-RPC ID；不重放 tools/call 或 prompts/get。
Client::listen_http(url, filters, on_event, ListenOptions) 支持宿主取消、订阅超时和单帧大小限制；增量 SSE 解析支持 CRLF、LF、CR、心跳及重复响应拒绝。MCP 2026-07-28 已删除 Last-Event-ID 重放，因此不实现旧版会话恢复。进程内不合作的 C++ 回调仍无法强制抢占。

### OAuth：JWT/JWKS 资源服务端验证

包含 mymcp/jwt.hpp，调用 oauth::fetch_jwks_https(trusted_url) 获取可信 HTTPS 上的 JWKS（证书验证、无重定向、1 MiB 上限）；构造 oauth::JwtVerifier(jwks, policy) 并通过 Server::protect_http 的验证回调调用 verifier.verify(token, required_scopes)。
目前只允许 RS256 签名和至少 2048 位的 RSA 公钥。校验 kid、签发者 iss、资源受众 aud、到期时间 exp、可选 nbf/iat 和 scope。JWKS URL 必须由宿主信任，宿主自行实现密钥轮换、令牌撤销、浏览器授权、OAuth 客户端注册策略及凭据安全存储。现有 OAuth Session 的发现、PKCE、授权码与刷新流程保持不变。

### MRTR：Elicitation、Sampling 与 Roots

Client::on_input_required(callback) 默认仅声明表单征询；宿主确实实现 URL 征询、模型采样或根目录读取后，可调用 enable_mrtr_capabilities(url, sampling, roots) 明确启用。Server 校验 inputRequests 的方法、参数与声明能力，Client 校验 inputResponses 的 ID 和类型，并原样携带不透明 requestState 重试原请求（上限 9 轮）。
采样与 Roots 在 2026 规范中已弃用，但为互操作保留；切勿在未征得用户同意时自动同意征询。

新增回归测试：baseline_features、jwt_jwks、http_resilience；具体实现边界见 COMPATIBILITY.md 与 SECURITY.md。
