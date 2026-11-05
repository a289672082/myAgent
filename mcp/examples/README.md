# 独立 C++20 MCP 演示程序（不需要命令行参数）

每个 `run_xxxx_demo.cpp` 文件均定义 `int run_xxxx_demo()` 和调用它的无参数 `int main()`。对应的 CMake 可执行文件名为 **`run_xxxx_demo`**（Windows 下添加 `.exe`）。每个示例聚焦一项功能，不接受 `--http`、`--stdio`、URL、端口或凭据等命令行参数。

在仓库的 MCP 工程目录完成一次构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

使用 Visual Studio 等多配置生成器时，构建命令需添加 `--config Debug`，`ctest` 需添加 `-C Debug`。请从构建输出目录运行演示程序。大多数示例会自行结束；三个服务端示例将持续监听，直到手动终止。

## 每个可执行程序展示一项功能

| 直接运行的程序 | 演示内容 | 前置条件 |
| --- | --- | --- |
| `run_tools_demo` | 强类型工具、自动 JSON Schema、校验错误、分页及服务发现 | 无 |
| `run_resources_demo` | 静态资源、资源模板、带缓存的资源列表 | 无 |
| `run_prompts_demo` | 提示词参数与自动补全 | 无 |
| `run_mrtr_demo` | 使用无害的模拟审批演示多轮用户信息征询 | 无 |
| `run_tasks_demo` | 任务创建、异步执行和状态轮询 | 无 |
| `run_extensions_demo` | 扩展能力声明与自定义方法 | 无 |
| `run_stdio_client_demo` | 启动 `run_stdio_server_demo`，通过 stdio 发现并调用工具 | 自动寻找配套可执行程序 |
| `run_stdio_server_demo` | 从标准输入读取 JSON-RPC 的 stdio MCP 服务端 | 由 stdio 客户端或宿主应用启动 |
| `run_progress_demo` | 接收 stdio 工具的进度通知 | 自动启动 stdio 服务端 |
| `run_http_server_demo` | 回环 HTTP 服务端、工具、资源及事件发布 | 在第一个终端保持运行 |
| `run_http_client_demo` | HTTP MCP 客户端请求 | 先启动 HTTP 服务端 |
| `run_subscriptions_demo` | SSE 订阅、确认与资源更新事件 | 先启动 HTTP 服务端 |
| `run_oauth_demo` | **不需要凭据的模拟 OAuth**：发现、原生客户端注册、PKCE S256、回调与令牌交换 | 无 |
| `run_oauth_login_demo` | 使用真实 OAuth 服务提供方的浏览器回调和 Bearer 保护 MCP 调用 | 编辑源码配置，并使用真实 OAuth 提供方 |
| `run_oauth_resource_demo` | 受保护资源元数据、401 认证质询和仅用于本机的固定 Bearer 令牌验证 | 在终端持续运行；**仅限开发** |
| `run_oauth_resource_client_demo` | 向本地受保护资源服务端发送模拟 Bearer 令牌 | 先启动 `run_oauth_resource_demo` |

### 无参数运行一组 HTTP 示例

终端一：

```bash
./run_http_server_demo
```

终端二：

```bash
./run_http_client_demo
./run_subscriptions_demo
```

固定本地地址与端口定义在 `demo_common.hpp` 中（`http://127.0.0.1:33867/mcp`）。如需更换端口，请修改这个头文件，服务端和客户端将共用新的配置。

### OAuth 使用流程

建议先运行 `./run_oauth_demo`。它会在随机回环端口上启动内存模拟授权服务器，随后完成受保护资源元数据发现、原生动态注册、PKCE S256 初始化、模拟 OAuth 回调校验及授权码交换。整个过程**无需真实凭据**，也不会打开真正的授权页面。

如需连接**真实**提供方，请修改 `run_oauth_login_demo.cpp` 开头的 `configuration` 常量：

```cpp
constexpr const char* mcp_resource = "https://your-mcp-host.example/mcp";
constexpr const char* public_client_id = "YOUR_PUBLIC_CLIENT_ID";
constexpr bool use_dynamic_registration = false;
constexpr const char* requested_scope = "tools:read";
```

重新编译并执行 `./run_oauth_login_demo`。程序会在系统分配的端口上启动本地回调监听器，输出需要手动打开的授权 URL，最多等待 180 秒，验证回调的 state 与 issuer，然后交换授权码并使用 OAuth 会话调用 MCP。程序不会打印或保存令牌；切勿提交真实私密凭据。OAuth 提供方必须允许所选择的回环重定向 URI，或支持原生动态注册。

`run_oauth_resource_demo` **不是 OAuth 授权服务器**，不能用于 `run_oauth_login_demo` 的登录。它只演示 SDK 的受保护资源元数据和 Bearer 验证接口。固定令牌仅用于本地安全形态测试，**不能用作生产令牌验证器**。真实服务必须验证令牌签名或可信内省结果、签发者、受众、有效期和权限范围，并使用 HTTPS 部署。

## 旧版集成测试辅助程序

旧版通过命令行参数驱动的 `showcase_*`、`oauth_login.cpp` 与 `oauth_resource_server.cpp` 已移至 `tests/fixtures/`，仅作为集成测试辅助程序进行编译，避免继续公开旧版参数式示例。`examples/` 下所有程序目前均采用独立 `run_xxxx_demo` 入口函数约定。

SDK 仍是实验性实现；这些示例**不能证明**其完全符合 MCP 2026-07-28 规范。请参见 [兼容性矩阵](../COMPATIBILITY.md)。


### Streamable HTTP 与单请求 SSE

先启动 `run_http_server_demo`，再运行无参数的 `run_http_progress_demo`。工具会实时发送五条 `notifications/progress` SSE 事件和最终 JSON-RPC 响应；普通快速工具仍返回 `application/json`。Codex 与 Claude Code 的配置方法见 [互操作指南](../INTEROPERABILITY.md)。

## 一个示例包含十组智能体常用工具（中文注释）

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
