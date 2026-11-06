# myAgent 控制台——默认入口（FTXUI 7.0.3）

根 `CMakeLists.txt` 默认构建控制台；`sdk/` 是可独立编译和链接的通用库，
`examples/` 和 `tests/` 是独立模块。界面只是 Codex 风格测试 TUI，不是 Codex 本体。

## Visual Studio 2022 原生工程（推荐，无需 CMD）

1. 确保仓库的 Git 子模块 `third_party/FTXUI` 与 `third_party/yaml-cpp` 已获取，并安装 Visual Studio 2022 C++ 桌面开发工作负载、x64 OpenSSL。
2. 在文件资源管理器中**双击根目录的 `myAgent.sln`**；选择 `Debug|x64`。
3. 右键 `myAgentConsole` → **设为启动项目**；点击 **生成 → 生成解决方案**。
4. 点击 **本地 Windows 调试器 / F5**。无需先打开 CMD、执行 `cmake`、`build.cmd` 或使用 NMake。

所有原生 `.vcxproj` 随仓库维护，OpenSSL 默认位于 `C:\\Program Files\\OpenSSL`，
路径不同时在 VS 属性管理器修改 `vs2022/myAgent.Native.props` 的 `OpenSslRoot`。
选择独立 Agent/MCP Demo 后可右键**生成**，默认生成解决方案只编译核心库与控制台。
完整说明见 [VS2022 原生工程指南](../vs2022/README.md)。

## 其他平台与 CMake 构建

将 FTXUI v7.0.3 固定作为 Git 子模块下载到 `third_party/FTXUI`：

```sh
git clone --recurse-submodules https://github.com/a289672082/myAgent.git
cd myAgent
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure
```

构建过程不会调用 FetchContent、下载第三方依赖或访问网络；
若子模块未初始化，CMake 会明确提示 `git submodule update --init --recursive`。
默认 CMake 关闭 SDK 演示程序与测试，启用控制台及其离线回归测试。

仅需 SDK：

```sh
cmake -S sdk -B build-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build-sdk --config Release
```

若要运行 SDK 全套示例/测试：

```sh
cmake -S . -B build-sdk-tests -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build-sdk-tests --config Debug
ctest --test-dir build-sdk-tests -C Debug --output-on-failure
```

## 常用命令

```text
/help                      显示帮助和 Slash 前缀提示
/status                    当前状态（不显示密钥）
/listSkills                列出已发现的 Skill
/model gpt-4.1-mini        模型名
/baseurl https://api.openai.com
/protocol responses        或 /protocol chat
/apikey                   使用遮挡输入框设置 Key
/apikey YOUR_API_KEY      直接设置 Key
/apikey env MY_API_KEY
/apikey clear
/tools                     原生 23 个工具状态
/tools on                  开启 23 个原生工具（默认）
/tools off                 关闭原生工具
/mcp                       查看 MCP
/mcp add local stdio "C:\Tools\mcp.exe" --stdio
/mcp add remote http https://mcp.example.com/mcp MY_MCP_BEARER
/mcp remove local
/mcp refresh
/permissions ask           询问每次工具调用（默认），也可 deny / allow
/mode react                也可 plan / reflect / evaluate / rewoo
/new                       开始新对话
/clear                     清空当前会话与界面
/usage                     统计真实 Token 用量
/exit
```

默认使用 `MYAGENT_API_KEY` 或 `OPENAI_API_KEY` 环境变量（也支持 MYAGENT_BASE_URL、MYAGENT_MODEL）。
模型 HTTP JSON 自动追加到程序当前工作目录的 `myagent_console.log`；每行 JSON 包含 `event`（`request` 或 `response`）和 `data`（原始 JSON 正文）。流式响应按服务端 SSE JSON 事件逐条记录。请求头和 API Key 不会主动记录；日志可能包含完整上下文、工具数据和服务端返回的敏感内容，请妥善保管。
用户修改的 Key、Base URL 和模型会自动保存并在下次启动时加载；Windows 下配置位于 `%APPDATA%\myAgent\console.json`，Key 使用当前 Windows 用户的 DPAPI 加密。其他系统将配置放在用户配置目录，Key 以明文存储并限制配置目录和文件权限。直接输入 Key 时，输入框中会显示明文。远端 MCP HTTP 只能用 HTTPS；
Bearer Token 只从环境变量获取。默认每次 MCP 工具调用都需在界面确认。
斜杠命令候选可用上下方向键选择，按 Tab 或 Enter 补全。
对话区域可使用鼠标滚轮或 Page Up / Page Down 浏览；滚回底部或发送新消息后自动跟随新内容。
在 Windows 中选中文本后按 Ctrl+C 复制；没有选区时，Ctrl+C 清空输入或取消正在执行的任务，Ctrl+D 或 /exit 退出。Windows Terminal 也支持按住 Shift 选择终端文本，再使用 Ctrl+Shift+C 复制。
修改模型、URL、协议、密钥或 MCP 服务将重建 Agent 并清空旧会话。
Execute-Verify-Repair 需要真实验证器，控制台不伪造验证结果。

默认控制台会向 Agent 注册原生 DeveloperTools 工具（`local__read_file` 等），
无需 /mcp 连接。该工具库在 `tools/developer_tools/`，若已手动接入同名 MCP
服务可能出现两套相似工具；可使用 `/tools off` 防止重复。
