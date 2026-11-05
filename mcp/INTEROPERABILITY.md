# Codex、Claude Code 与 MCP 客户端互操作指南

远程客户端推荐使用 **Streamable HTTP**（`POST /mcp`），本地启动的服务端推荐 **stdio**。SDK **不会**创建独立的旧版 HTTP+SSE 端点。MCP `2026-07-28` 采用无状态请求：每次 HTTP POST 携带 `_meta`、`MCP-Protocol-Version`、`Mcp-Method` 及其他命名请求头。快速调用返回 JSON；带 `progressToken` 的调用会通过 SSE 传输进度通知，最后发送匹配的 JSON-RPC 响应。`subscriptions/listen` 则建立长时间运行的 SSE 连接。

## Codex 命令行工具

先启动 `run_http_server_demo`（默认仅监听回环地址），再打开另一个终端：

```sh
codex mcp add myMcpTools --url http://127.0.0.1:33867/mcp
codex mcp list
```

或者注册本地 **stdio** 可执行程序：

```sh
codex mcp add myMcpTools-local -- /absolute/path/to/run_stdio_server_demo
```

使用 HTTPS 部署时，请替换为真实服务端地址，并正确配置 OAuth 2.1 受保护资源元数据；**绝不可**把仅用于本地回环的明文 HTTP 演示服务直接暴露到互联网。

## Claude Code

```sh
claude mcp add --transport http myMcpTools http://127.0.0.1:33867/mcp
claude mcp list
```

如果需要在所有项目中安装（且有权限），可添加 `--scope user`。本地服务端则使用：

```sh
claude mcp add --transport stdio myMcpTools-local /absolute/path/to/run_stdio_server_demo
```

从另一台计算机连接 HTTP 服务时，`127.0.0.1` 指向**客户端自身**，并不是 MCP 服务主机。远程部署应使用带可信访问控制的 HTTPS 反向代理；本库会有意拒绝面向公网的明文 HTTP 监听。

## 协议兼容性

| 功能 | 行为 |
| --- | --- |
| MCP `2026-07-28` | 原生无状态请求、单请求 SSE、`subscriptions/listen` 订阅 |
| MCP `2025-11-25` / `2025-06-18` / `2025-03-26` | 对 Streamable HTTP 的 `initialize`、`notifications/initialized` 和基础工具/资源/提示词提供兼容适配；不创建协议级会话 |
| 旧版 GET `/mcp` SSE 流 | 返回 **405**；服务端不提供旧式通知流时，这是合法响应 |
| 旧版 HTTP+SSE（2024-11-05） | 未实现；它与 Streamable HTTP 不是同一传输协议 |
| OAuth | 参见 `SECURITY.md`；演示用令牌绝不能作为生产凭据 |

## 测试

- `http_streamable_compat.py`：验证 2026 JSON 与单请求 SSE、旧版 HTTP 握手与工具调用、GET 405。
- `legacy_http_client.py`：验证服务器不支持 `server/discover` 时，C++ `Client::http` 的自动回退行为。
- `run_http_progress_demo`：无命令行参数的 C++ 客户端，演示实时 HTTP SSE 进度和最终工具结果。
- 既有的 stdio、分页、MRTR、订阅、OAuth 和安全测试仍保持启用。

旧版测试仅**模拟协议行为**，并不真正启动 Codex/Claude 进程。本仓库**不承诺**与每一版 Codex/Claude 完全兼容，也不表示通过了官方 MCP 符合性认证。复杂的旧版服务端主动请求、恢复与 `Last-Event-ID`、不同 OAuth 签发者的兼容差异，以及阻塞工具断线取消，均不在当前测试范围内。

参考资料：[MCP 2026-07-28 HTTP 传输规范](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)。
