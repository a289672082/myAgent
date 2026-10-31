# myAgent — C++20 OpenAI Agent 框架

独立 C++20 Agent 项目，复用 [myMcpTools](https://github.com/a289672082/myMcpTools) 的 MCP 客户端、服务端、JSON、HTTP、OAuth 和其第三方源码。myMcpTools 使用固定提交的 Git submodule；Agent 源码全部在本仓库。

## 功能

- OpenAI Responses API 多轮对话、函数调用与结果回传
- MCP 客户端：stdIO / Streamable HTTP / OAuth；工具自动发现与执行
- Agent 内直接通过 Lambda 或 JSON Schema 定义 MCP 工具
- 自动发现项目级和用户级 Skills；模型按任务触发、逐层读取 SKILL.md 和参考资料，也可手动启用
- 图片输入：PNG / JPEG / WebP / GIF 转 base64 Data URL
- 音频输入：转写成文字后继续对话；音频输出：合成 MP3
- 保存/加载对话历史，注入 Model mock 做离线测试

## 构建

依赖 C++20、CMake >= 3.20、OpenSSL 和 Threads。子模块及其中的第三方依赖均保留原 LICENSE 文件。

~~~sh
git clone --recurse-submodules https://github.com/a289672082/myAgent.git
cd myAgent
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
~~~

已经克隆时可以执行 git submodule update --init --recursive。CMake 默认完全离线，不会在配置过程中下载依赖。也可以用 -DMYAGENT_MCP_DIR=/path/to/myMcpTools 指向已有 SDK 源码。

## 快速示例

~~~cpp
#include "myagent/agent.hpp"
#include <iostream>
#include <string>

int main() {
  myagent::Config config;
  // 模型地址和密钥都由应用代码显式传入，框架不会读取环境变量。
  const std::string modelBaseUrl = "https://api.openai.com";
  const std::string modelApiKey = "sk-example-replace-with-your-api-key"; // 示例密钥，不可用
  config.base_url = modelBaseUrl;
  config.api_key = modelApiKey;
  config.model = "gpt-4.1-mini";
  myagent::Agent agent(config);

  agent.tool("add", "计算两个整数的和", [](int a, int b) { return a + b; },
             mymcp::arg<int>("a"), mymcp::arg<int>("b"));

  // 可选：运行外部 MCP 服务后再取消注释
  // agent.add_mcp_http("remote", "http://127.0.0.1:3000/mcp");
  // Skill 自动发现 + 模型自动选择，无需手动注册。
  // 如需指定：agent.use_skill("brief-summary");

  std::cout << agent.ask("计算 20 + 22") << "\n";
  std::cout << agent.ask("上一个答案是多少？") << "\n";

  // agent.ask_image("请描述图片", "example.png");
  // agent.ask_audio("example.wav", "总结音频");
  // agent.speak("你好", "result.mp3");
  agent.save_history("agent_history.json");
}
~~~

## 模型 baseUrl：自动补全或使用完整地址

两种模式都使用 `Config::base_url`，通过 `Config::append_responses_path` 切换，默认 `true`。如果自动模式下地址已以 `/v1/responses` 结尾，则不会重复追加。已有网关路径前缀与 URL 查询参数会保留。

~~~cpp
myagent::Config config;
config.api_key = "sk-example-replace-with-your-api-key"; // 非真实密钥
config.model = "gpt-4.1-mini";

// 模式 1：自动追加 /v1/responses（默认）
config.base_url = "https://api.openai.com";
config.append_responses_path = true;
// 请求目标：https://api.openai.com/v1/responses

// 模式 2：使用完整请求地址，不追加任何路径
config.base_url = "https://proxy.example.com/my/custom/responses?version=1";
config.append_responses_path = false;
// 请求目标：https://proxy.example.com/my/custom/responses?version=1

myagent::Agent agent(config);
~~~

完整地址必须提供 **Responses API 兼容接口**；仅取消自动拼接并不意味着能够调用 Chat Completions 格式的 `/v1/chat/completions`。
注意：`ask_audio` 和 `speak` 是独立 API，仍向相同域名下的 `/v1/audio/transcriptions` 和 `/v1/audio/speech` 请求，不使用自定义 Responses 路径。若第三方网关不支持音频接口，这些功能不可用。

## Skills 自动发现与按需加载

默认启用渐进式加载，行为接近 Codex / Claude Code：

1. 构造 `Agent` 时扫描当前目录以及父级 Git 仓库范围的 `.agents/skills/`（Codex）与 `.claude/skills/`（Claude），另兼容当前目录 `skills/`，以及用户级 `~/.agents/skills/` 和 `~/.claude/skills/`。
2. **发现阶段只读取 `SKILL.md` 的 YAML frontmatter 名称、描述**；不立即加载指令正文，也不会自动执行脚本。
3. 每轮请求只向模型公开有限长度的 Skill 名称/描述目录。模型认为某项 Skill 适用时调用内建 `agent_skill_load`；返回该 Skill 的完整 SKILL.md 及辅助文件列表。
4. 若需要额外资料，再调用 `agent_skill_read_file` 读取选定 Skill 的 `references/`、`scripts/`、`assets/` 子目录中的**文本文件**；不读取二进制文件，不执行脚本。
5. 自动激活仅作用于当前轮及其工具调用，避免跨无关问题滥用 Skill；对话历史记录该轮的工具结果。`clear_history()` 清除历史，但不移除已发现的 Skill。项目级同名 Skill 优先。

~~~cpp
myagent::Config config;
config.base_url = "https://api.openai.com";
config.api_key = "sk-example-replace-with-your-api-key"; // 非真实密钥
config.auto_discover_skills = true; // 默认：发现项目级 / 用户级 Skill
config.auto_skill_selection = true; // 默认：允许模型按需选用
myagent::Agent agent(config);

// 自动识别 code-review / brief-summary 等 Skill 的描述并按需调用。
std::string answer = agent.ask("帮我审查这段 C++ 代码");

// 可选：加载非标准目录（同名 Skill 会报错，避免隐式覆盖）
// agent.load_skills("./my-extra-skills");
// 可选：显式激活，立即读取并在后续轮次注入完整说明
// agent.use_skill("brief-summary");
~~~

Skill 目录应为 `name/SKILL.md` 格式，frontmatter 至少包含 `name` 与 `description`；支持简单的单行 YAML 键值写法，并不是完整的 YAML 解析器。每个 SKILL.md 最大 128 KiB；总数最多 64，目录提示信息总长度限制为 8,000 字节。同名 Skill 自动发现以先发现的项目级为准。使用项目 Skill 前应审查其来源；本项目尚不提供自动执行脚本、shell 执行隔离、Skill 文件变更监视、完整 Codex/Claude 技能规范或外部技能市场同步。

## MCP 快捷接口（推荐）

Agent 负责创建和保存客户端，无需显式构造 mymcp::Client、std::shared_ptr，也不必自行管理传输对象的生命周期。

~~~cpp
// Streamable HTTP（无需认证）
agent.add_mcp_http("github", "https://example.com/mcp");

// HTTP Bearer Token（由调用方直接传入，真实凭据勿提交到仓库）
agent.add_mcp_http("private", "https://example.com/private/mcp", token);

// stdio（可选的 argv 数组直接传递给进程；不使用 shell 拼接）
agent.add_mcp_stdio("files", "./filesystem-mcp-server");
agent.add_mcp_stdio("git", "./git-mcp-server", {"--workspace", "./project"});

// OAuth：Session 由宿主程序先完成 OAuth 授权及令牌获取
// auto session = std::make_shared<mymcp::oauth::Session>(oauth_url);
// ... session->discover(), begin_authorization(), exchange_callback_url() ...
agent.add_mcp_oauth("secured", oauth_url, session);
~~~

需要自定义通知处理或进阶 MCP API 时，仍保留 `agent.add_mcp(prefix, std::shared_ptr<mymcp::Client>)`，供高级用户使用。`add_mcp_oauth` 不会自动打开浏览器进行 OAuth 登录，Session 必须能提供访问令牌。MCP 工具名自动添加 `prefix__` 前缀以避免冲突。HTTP 连接在 `ask()` 时发现，stdio 子进程在注册时启动；不要运行来源不可信的 MCP 可执行文件。

## Demo 运行

`examples/run_agent_demo.cpp` 已将所有配置集中在局部变量中，不读取任何环境变量，也不使用命令行参数。

首先将 `modelBaseUrl`、`modelApiKey`、`modelName` 改为你的实际接口配置；示例中的 API Key 只是演示占位符，不能用于真实调用。

HTTP / stdio MCP、图片、音频示例使用可读的变量和 `enable...` 开关，默认关闭依赖外部服务或本地样例文件的功能。示例 Skill 由 Agent 自动发现，无需手动 `use_skill()`。

从仓库根目录运行 `build/run_agent_demo`（Windows：`build/Debug/run_agent_demo.exe`），即可加载示例 Skill。

核心头文件：include/myagent/agent.hpp。
核心实现：src/agent.cpp。
测试：tests/test_agent.cpp。
示例：examples/run_agent_demo.cpp。

## 边界和安全

当前是同步的 Responses + MCP Agent，不支持流式 SSE 对话、Realtime 双向语音、直接分析音频声学特征、自动工具权限审批和上下文压缩。
`Config::api_key` 和 `Config::base_url` 必须由调用方显式赋值。使用真实 API Key 时应由你的应用安全地取得并传入，**不要将真实密钥硬编码后提交到 Git**；历史文件可能包含敏感会话内容，也不要提交。
技能文件只在按需激活时作为工具内容或手动配置的提示词加载，不执行脚本；请核查 Skills 和远程 MCP 服务的来源。会改变文件或运行命令的工具应由调用方额外做权限控制与隔离。
媒体上限为 20 MiB，工具返回上限为 256 KiB；不包含实际 OpenAI API 联网集成测试。
