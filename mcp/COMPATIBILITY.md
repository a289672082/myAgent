# MCP 2026-07-28 协议兼容性矩阵

更新于 2026 年 10 月 31 日。**本 SDK 正在开发中，只实现规范的部分功能，尚未达到完整符合性要求。** CTest 通过仅代表当前本地测试覆盖的行为正常，不代表取得官方协议符合性认证。

| 协议领域 | 当前实现 | 待完成工作 |
|---|---|---|
| 核心 JSON-RPC | 请求 ID、错误、强类型工具及基础通知 | 严格的线上消息 Schema 测试与畸形数据模糊测试 |
| 现代无状态生命周期 | `server/discover`、请求 `_meta` 中要求的协议版本、客户端身份与能力，以及服务端响应元数据、`resultType` | 官方符合性测试及与 TS v2/Kotlin/C# SDK 的互操作验证 |
| 旧版 2025-11-25 | stdio 与基础 Streamable HTTP `initialize` 回退、无状态工具调用 | 基于 GET 的通知、会话恢复及完整旧版行为兼容 |
| 工具 | 强类型注册、jsoncons JSON Schema 验证、上下文工具、异步任务工具、HTTP 请求头注解 | 远程 Schema 引用、结构化输出及 `outputSchema` 验证、图标和注解 |
| 资源 | 纯文本资源、简单 URI 模板变量、更新通知 | 二进制资源及完整 RFC6570 URI 模板 |
| 提示词与补全 | 丰富的提示词回调、参数、补全回调 | Schema 校验及全面字段覆盖 |
| 列表分页与缓存 | 确定性内存分页、`ttlMs`、`cacheScope`；已认证 HTTP 强制使用私有缓存范围 | 快照游标和缓存重新验证 |
| MRTR | 基础 `input_required` 与携带 `requestState` 的重试 | 采样与用户信息征询的完整校验、状态安全和全部交互行为 |
| Tasks 扩展 | 显式能力开关、加密随机任务 ID、轮询、请求输入及更新、协作式取消、TTL 过期、HTTP 路由头 | 重启后持久化、`taskIds` 订阅和 `notifications/tasks`、外部任务后端及互操作符合性 |
| 请求进度与取消 | 并发 stdio、`notifications/progress`、协作式取消，以及 HTTP 单请求 SSE 进度和增量客户端回调 | HTTP 阻塞工具断线时及时取消、重试与超时完善、健壮的 stdio 流式 API |
| 订阅 | 显式 `subscriptions/listen`、确认、过滤、HTTP SSE 和 stdio 取消 | 任务状态订阅、重连及恢复、优雅关闭接口集成 |
| HTTP 传输 | 2026 请求头校验、4 MiB 请求限制、回环地址监听、元数据及 Bearer 验证回调 | 可对公网监听的原生 HTTPS 服务、认证反向代理部署指南、负载及拒绝服务防护调优、旧版 HTTP 会话 |
| HTTPS 客户端 | OpenSSL TLS 证书与主机名验证、可配置 Bearer 令牌 | HTTPS SSE 订阅路径、超时/重试/证书固定选项 |
| OAuth 资源服务端 | 类 RFC9728 元数据端点及 Bearer 认证质询；由调用者提供令牌验证回调（**必须**校验令牌、权限范围、受众及签发者） | 内置 JWT 校验与 JWKS 发现、OAuth 增量提权、与授权服务器互操作 |
| OAuth 客户端 | 受保护资源及授权服务器发现、PKCE S256、状态和签发者校验、DCR 注册、授权码交换、刷新与 Bearer 令牌附加 | CIMD 内容验证、浏览器交互及重定向监听、安全持久化凭据、权限提升与重新授权 |
| 扩展 | 命名空间能力声明、自定义方法注册表、部分 Tasks 实现 | Apps、其他非核心扩展、完整协商及独立扩展符合性测试 |
| 官方符合性 | Linux 与 Windows 上的 CTest 测试集 | 官方符合性测试集、TS v2/Kotlin 集成、更多操作系统 CI 验证 |

## 构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

依赖：C++20、仓库内置的 cpp-httplib 与 jsoncons、OpenSSL（SSL 和 Crypto）及线程库。CMake 直接使用固定提交对应的内置源码，不依赖配置时联网下载。

## 安全与适用范围

不得宣传本仓库完全兼容 MCP 2026-07-28。OAuth **仅部分实现**：令牌验证回调是宿主集成接口，不能替代可信的 JWT 或内省验证器。内置 HTTP 监听器只绑定回环地址；生产环境资源服务端必须部署在带身份认证的 TLS 网关后并接受安全审查。长时间运行的任务仅存于内存，保留时间有限，不能跨进程持久化。

## cpp-httplib / jsoncons 迁移说明

- 使用 cpp-httplib 替换 Boost.Asio/Beast，使用 Windows/POSIX 原生进程 API 替换 Boost.Process。
- 使用 jsoncons 替换 Boost.JSON，并基于 jsoncons 2020-12 实现 JSON Schema 校验。
- 新增 OAuth Session：受保护资源发现、授权服务器（AS）元数据发现（RFC 8414 和 OIDC）、
  授权码与 PKCE S256、回调 state/issuer 校验、令牌交换、在授权服务器声明支持时进行
  动态客户端注册以及令牌刷新；客户端可通过 `Client::http_oauth` 绑定 Session。
- OAuth 资源服务端不等于 OAuth 授权服务器；宿主应用需要分别提供令牌
  验证器和授权服务器端点。
- 当前不声明已通过官方 MCP 符合性认证，也不宣称支持原生 TLS 托管、持久化 Tasks、
  SSE 重放及恢复或安全的浏览器自动回调接收器。


## Streamable HTTP 兼容性补充

- **新增：** HTTP `tools/call` 带 `progressToken` 时，会以请求级 `text/event-stream` 返回进度通知和最终 JSON-RPC 结果；`Client::http` 可设置事件回调并增量解析。
- **新增：** 基础旧版 HTTP 初始化、列表/调用/读取兼容（`2025-03-26`、`2025-06-18`、`2025-11-25`），不会由服务端签发会话 ID。旧版 GET `/mcp` 明确返回 405（不提供 GET SSE 通知流）。
- **剩余限制：** 部分旧版客户端需要服务端主动采样/征询、GET 资源变更流、会话恢复或完整 2025 协议功能，本 SDK 尚未提供。HTTP 断线后，协作式工具只有在 SSE 写入失败时才会收到取消标记。CI 没有真实启动 Codex/Claude Code 进程进行应用层集成测试。