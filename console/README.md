# myAgent Console — 独立的 Codex 风格测试客户端

这是一个用于手动验证 myAgent SDK 的**独立应用**。终端界面依赖 MIT 许可的 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v7；控制台仅使用 SDK 的公开 API。

**不会修改 SDK 公共头、静态库源码、根 CMakeLists 或 SDK 的默认依赖。** 仅在手动配置 -S console 时才编译控制台。界面借鉴 Codex 的顶部信息、聊天滚动区、底部输入框、流式反馈、Slash 建议和工具审批；它不是 Codex 本体，也不提供 Codex 专用账号、云会话和沙箱。

## 构建

从 myAgent 仓库根目录：

~~~sh
git submodule update --init --recursive
# 如 FTXUI v7 已安装：完全离线
cmake -S console -B build-console

# 或者仅对独立控制台显式下载固定版本 FTXUI v7.0.3：
cmake -S console -B build-console -DMYAGENT_CONSOLE_FETCH_FTXUI=ON

cmake --build build-console --config Debug --parallel 2
ctest --test-dir build-console -C Debug --output-on-failure
~~~

Windows MSVC 可执行文件通常为 build-console/Debug/myagent_console.exe；Linux Ninja 为 build-console/myagent_console。可通过 CMAKE_TOOLCHAIN_FILE 指向已安装 FTXUI 的 vcpkg 工具链。原有 SDK 构建仍使用 cmake -S . -B build，不会下载或使用 FTXUI。

## 配置和直接聊天

建议通过环境变量 MYAGENT_API_KEY 或 OPENAI_API_KEY 读取密钥（也支持 MYAGENT_BASE_URL、MYAGENT_MODEL）。启动后直接输入问题并回车。未配置密钥时，在控制台输入 /apikey 会打开带字符遮挡的安全输入框。

~~~text
/help                       查看命令
/status                     显示当前模型、URL、协议、Token 与 MCP 数量（隐藏密钥）
/model gpt-4.1-mini         更改模型
/baseurl https://api.example.com
/protocol responses         默认：OpenAI Responses API
/protocol chat              Chat Completions 兼容 API
/apikey                     打开遮挡的密钥输入框
/apikey env MY_OTHER_KEY    从环境变量读取密钥
/apikey clear               清除密钥
/mcp                        列出已配置的 MCP 服务
/mcp add local stdio "C:\Tools\mcp.exe" --stdio
/mcp add remote http https://mcp.example.com/mcp MCP_BEARER_ENV
/mcp remove local
/mcp refresh                清理工具目录缓存
/permissions ask            默认：每个工具调用询问 Y/N
/permissions deny           禁止 MCP 工具调用
/permissions allow          自动批准全部工具调用，仅可信环境
/mode react                 默认 ReAct
/mode plan
/mode reflect
/mode evaluate
/mode rewoo
/new                        开始新对话，保留当前配置
/clear                      清空界面和对话
/usage                      查看模型返回的真实 Token 用量
/exit                       退出
~~~

输入 Slash 前缀显示命令建议。普通输入栏中直接输入 /apikey sk-... 会被拒绝，不接受命令行明文密钥。HTTP MCP 的 Bearer Token 只从指定环境变量读取；不会打印或保存密钥。

**重要行为**：改变 /model、/baseurl、/protocol、/apikey 或 /mcp add/remove 时重建 Agent 并清空以前的会话；程序会显示提示。/new 清理会话而不清理连接配置。/permissions、/mode 不清理历史。

## MCP、安全和测试

- stdio MCP 进程通过 SDK add_mcp_stdio 创建，不经 shell 展开。
- HTTP MCP 通过 SDK add_mcp_http 创建。远端要求 HTTPS；明文 HTTP 仅允许 localhost / 127.0.0.1 / [::1]。拒绝 URL 用户名密码、查询和 Fragment。
- 默认 /permissions ask：在屏幕底部显示工具名称及 JSON 参数，按 Y 只批准此次操作，N 拒绝，Esc / Ctrl+C 请求取消。
- /permissions allow 不应在不可信服务器或重要工作区使用。取消无法强杀不合作的用户代码。
- 需要 OAuth 浏览器认证的 MCP 服务应由完整 OAuth 客户端宿主处理，当前测试工具没有伪造这项能力。
- Execute-Verify-Repair 需要可信的编译/测试验收回调，控制台不会用模型自评冒充真实验证。
- API Key 不自动存盘，也不加入消息历史或控制台日志；进程退出后会消失。应用仍会在进程内存中临时持有密钥。
- 控制台专有 CTest 验证命令解析、敏感信息不打印、MCP 管理、HTTP 限制、协议选择。独立 console.yml 使用 Linux/Windows CI 验证构建；SDK 主 CI 继续独立运行。

这是用于开发调试的 Codex 风格 TUI，不包含 Codex 的所有功能。
