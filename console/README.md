# myAgent Console — 默认入口（FTXUI 7.0.3）

根 `CMakeLists.txt` 默认构建控制台；`sdk/` 是可独立编译和链接的通用库，
`examples/` 和 `tests/` 是独立模块。界面只是 Codex 风格测试 TUI，不是 Codex 本体。

## 构建

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
默认 CMake 关闭 SDK Demo/测试，启用控制台和控制台离线回归。

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
/model gpt-4.1-mini        模型名
/baseurl https://api.openai.com
/protocol responses        或 /protocol chat
/apikey                   使用遮挡输入框设置 Key
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
API Key 在内存中暂存，不写文件，不允许明文 /apikey key。远端 MCP HTTP 只能用 HTTPS；
Bearer Token 只从环境变量获取。默认每次 MCP 工具调用都需在界面确认。
控制台目前只有 Slash 前缀候选，不支持 Tab 选择或候选列表方向键切换。
修改模型、URL、协议、密钥或 MCP 服务将重建 Agent 并清空旧会话。
Execute-Verify-Repair 需要真实验证器，控制台不伪造验证结果。

默认控制台会向 Agent 注册原生 DeveloperTools 工具（`local__read_file` 等），
无需 /mcp 连接。该工具库在 `tools/developer_tools/`，若已手动接入同名 MCP
服务可能出现两套相似工具；可使用 `/tools off` 防止重复。
