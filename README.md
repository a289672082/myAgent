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
- 四种循环 ReAct / Plan-and-Execute / Reflection / Evaluator-Optimizer（统一工具预算，支持多图及音频转写）

## 构建

依赖 C++20、CMake >= 3.20、OpenSSL 和 Threads。新增固定版本 **yaml-cpp 0.9.0** Git submodule（MIT），用于标准 YAML frontmatter 解析；子模块源码和 LICENSE 随 `--recurse-submodules` 克隆后可离线编译。

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

Skill 目录应为 `name/SKILL.md` 格式，`SKILL.md` 通过 yaml-cpp 完整解析 YAML frontmatter（含多行 `|`/`>`、引号、注释、嵌套 `metadata` 等），并严格校验 Agent Skills 规范：`name`（1–64，ASCII 小写字母/数字/单连字符，和文件夹同名），`description`（1–1024 Unicode 字符）、可选 `license`、`compatibility`（1–500 字符）、`metadata`（字符串映射）及 `allowed-tools`（字符串）。会拒绝无效 YAML、字段类型错误、重复 YAML 键及缺少 frontmatter，错误包含文件与字段/行号。每个 SKILL.md 最大 128 KiB；总数最多 64，目录提示信息总长度限制为 8,000 字节。同名 Skill 自动发现以先发现的项目级为准。支持 Claude `disable-model-invocation: true` 和 Codex `agents/openai.yaml` 的 `policy.allow_implicit_invocation: false`，这种 Skill 不会被模型自动激活，但仍可用 `use_skill()` 显式启用。**`allowed-tools` 仅作元数据，不代表授予执行权限。** 其他厂商扩展字段不会阻止加载。使用项目 Skill 前应审查其来源；本项目尚不提供自动执行脚本、shell 执行隔离、Skill 文件变更监视、完整 Codex/Claude 技能运行时行为或外部技能市场同步。

## 统一 Agent 循环（四种策略 + 图片和音频）

新增 myagent::LoopInput、LoopOptions、LoopResult、LoopMode、LoopStatus（include/myagent/loop.hpp），在不破坏原有 ask() 的基础上提供四种策略：
- ReAct：决策 → 工具调用 → 观察 → 最终回答。
- Plan-and-Execute：严格 JSON 规划 → 单步执行 → 阻塞时受限重规划 → 汇总。
- Reflection：草稿 → JSON 自评 → 修订 → 检查完成。
- Evaluator-Optimizer：生成 → JSON 评分与问题清单 → 优化 → 评估通过。

~~~cpp
myagent::Config config;
config.base_url = "https://api.openai.com";
config.api_key = "sk-example-replace-with-your-key"; // 非真实密钥
config.model = "gpt-4.1-mini";
myagent::Agent agent(config);

myagent::LoopOptions options;
options.max_iterations = 6;
options.max_model_calls = 24;
options.max_tool_calls = 20;
options.max_replans = 1;
options.max_revisions = 3;

auto r1 = agent.react("调用工具查找信息", options);
auto r2 = agent.plan_and_execute("分析 C++ 项目并给出实施步骤", options);
auto r3 = agent.reflect("审查这段 C++ 实现", options);
auto r4 = agent.evaluate_and_optimize("优化 C++ 代码", options);

// 一个任务可以混合文本与多张图片；模型需要具备视觉输入能力。
myagent::LoopInput image_task;
image_task.text = "比较两张图片";
image_task.images = {"before.png", "after.png"};
auto image_result = agent.evaluate_and_optimize(image_task, options);

// 音频先转写为文字后进入四种循环，不是原始音频声学推理。
// 真实转写/语音合成需要兼容的独立 API 端点。
myagent::LoopInput audio_task;
audio_task.text = "总结录音";
audio_task.audio = "meeting.wav";
options.speech_output = "answer.mp3"; // 可选：把最终答案合成为音频文件
auto audio_result = agent.reflect(audio_task, options);
~~~

所有策略复用 Agent 的 Model/MCP/Skill 工具通道和统一模型/工具调用预算。Planner、Reflector、Evaluator 和 Optimizer 阶段默认没有工具调用能力，只有执行/初稿阶段可以调用工具；planner_model、evaluator_model 可指定阶段模型名称，但仍使用同一个 baseUrl/API Key。

LoopResult 包含 status、answer、error、audio_transcript、plan、evaluations、trace、model_calls、tool_calls、iterations。默认成功时主历史只增加原始用户任务和最终答案，内部草稿与反馈不进入主历史；设置 commit_to_history=false 不提交结果。取消通过同步 cancelled 回调在每次模型/工具调用前检查，on_event 可观察事件。失败、取消、调用预算耗尽不提交主历史；但已执行工具的外部副作用不能自动回滚。

图片使用现有 PNG/JPEG/WebP/GIF Data URL 处理；音频调用现有转写接口，合成使用 speak()。这一版没有原生音频声学理解、流式输出、供应商间不同的 API Key 路由、任务断点续跑或任意时刻中断进行中的 HTTP 请求。对模型的计划/评估 JSON 进行结构校验，不宣称单凭自评分即可证明外部测试成功。

### P1 Plan-and-Execute：结构化验收、安全重试与真正替换剩余计划

此模式的真实运行流程是：**规划步骤及依赖 → 调用工具执行一个步骤 → 验证器检查执行结果 → 成功执行下一步；失败/阻塞则受控重试或替换未执行的计划 → 汇总审计记录**。例如在 C++ 项目中执行“读取文件 → 修改源码 → 编译 → 单元测试”：当编译失败时，不能因为模型说“完成”就把步骤设为成功；应根据编译工具的退出码决定是否重规划或修复。

Planner 只返回严格 JSON，包含步骤 `id`、`description` 和可选 `depends_on`（只能依赖已完成或之前的步骤）、`success_criteria`；Executor **必须**返回如下格式，否则本次循环会失败（而不是默认为已完成）：

```json
{"status":"completed","summary":"编译完成","evidence":{"reported_exit_code":0}}
```

`status` 必须为 `completed`、`failed` 或 `blocked`，`summary` 必须是非空字符串。模型生成的 `evidence` 只是声明，不代表已验证；工具如果报告 `isError=true`，框架强制将步骤标记失败。可选宿主验证器由 C++ 应用根据真实工具证据判定通过，并将验证状态写入 `result.plan`：

```cpp
myagent::LoopOptions options;
options.max_plan_steps = 10;
options.max_replans = 2;
options.max_step_retries = 0; // 默认不自动重复工具操作
options.validate_step = [](const myagent::Json& step,
                           const myagent::Json& execution,
                           const myagent::Json& tool_results) {
    myagent::StepValidation check;
    // 演示：这里应根据自己的编译/测试工具返回内容检查真实退出码，
    // 而不是信任 execution 中模型报告的 "completed"。
    check.passed = !tool_results.empty();
    check.reason = check.passed ? "已拿到工具执行记录" : "缺少工具执行记录";
    check.evidence = myagent::json::object{{"tool_count", tool_results.size()}};
    return check;
};
auto result = agent.plan_and_execute("编译项目并运行测试", options);
```

上例只展示回调 API：**存在工具记录并不等于编译通过**。生产环境中必须读取该工具结果的明确退出码、测试统计或由独立编译/测试流程验证，并在失败时返回 `passed=false`。未配置宿主验证器的成功步骤会被标记为 `verification: "unverified"`，不会伪称实际测试已通过。

重新规划保留已完成/失败步骤的审计信息，旧的未执行步骤被标记为 `superseded`，新步骤替代它们，不再自动运行过时计划。可通过 `attempts`、`status`、`verification`、`evidence` 查看执行结果。默认**不会重试已经调用过工具的步骤**，避免重复写文件、提交或执行其他有副作用的操作；只有明确设置 `retry_steps_with_tools=true` 才允许此类重试。所有模型与工具调用仍受共享预算约束。

四个独立 Demo 均使用带显式示例配置的离线 Mock Model，无需环境变量/命令行参数和付费 API：
- examples/run_react_demo.cpp：run_react_demo()（图片 + MCP 工具）
- examples/run_plan_execute_demo.cpp：run_plan_execute_demo()（规划 + 执行）
- examples/run_reflection_demo.cpp：run_reflection_demo()（草稿 + 自检）
- examples/run_evaluator_optimizer_demo.cpp：run_evaluator_optimizer_demo()（评分 + 优化）

~~~sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
~~~

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

## P0 Agent Skills YAML 专项 Demo（完全离线）

新增独立可执行文件 **`examples/run_skill_yaml_demo.cpp`**，入口函数为 **`int run_skill_yaml_demo()`**，不修改原来的 `run_agent_demo()`。示例使用内置 Mock Model，**无需 API Key、环境变量、命令行参数或联网调用**。所有 Skill 文件均随仓库提交，路径由 CMake 固定，无需手动复制。

示例结构：

```text
examples/
├── run_skill_yaml_demo.cpp
└── skill_yaml_demo/
    ├── skills/
    │   ├── cpp-code-review/
    │   │   ├── SKILL.md
    │   │   ├── references/review-checklist.md
    │   │   └── scripts/check.py
    │   ├── claude-manual/SKILL.md
    │   └── codex-manual/
    │       ├── SKILL.md
    │       └── agents/openai.yaml
    └── invalid-skills/
        └── invalid-skill/SKILL.md
```

运行示例：

```sh
cmake -S . -B build
cmake --build build --target run_skill_yaml_demo --config Debug
# Linux / macOS:
./build/run_skill_yaml_demo
# Windows（Visual Studio 多配置生成器）:
# .\build\Debug\run_skill_yaml_demo.exe

ctest --test-dir build --output-on-failure -C Debug
```

`run_skill_yaml_demo()` 实际依次演示：**SKILL.md 中的多行 YAML 与 license/compatibility/metadata/allowed-tools 解析；错误 frontmatter 校验；Claude/Codex 禁止自动激活策略；Mock Model 自动选择 Skill、按需读取 `SKILL.md` 和 `references`；显式 `use_skill()`；以及 `clear_history()` 后手动 Skill 保留**。例子中的 `scripts/check.py` 仅为资源文件，绝不会自动执行。CMake 同时将 Demo 注册为 `myagent_skill_yaml_demo` 离线回归测试。

## Demo 运行

`examples/run_agent_demo.cpp` 已将所有配置集中在局部变量中，不读取任何环境变量，也不使用命令行参数。

首先将 `modelBaseUrl`、`modelApiKey`、`modelName` 改为你的实际接口配置；示例中的 API Key 只是演示占位符，不能用于真实调用。

HTTP / stdio MCP、图片、音频示例使用可读的变量和 `enable...` 开关，默认关闭依赖外部服务或本地样例文件的功能。示例 Skill 由 Agent 自动发现，无需手动 `use_skill()`。

从仓库根目录运行 `build/run_agent_demo`（Windows：`build/Debug/run_agent_demo.exe`），即可加载示例 Skill。

核心头文件：include/myagent/agent.hpp。
核心实现：src/agent.cpp。
测试：tests/test_agent.cpp。
示例：examples/run_agent_demo.cpp、examples/run_skill_yaml_demo.cpp、examples/run_react_demo.cpp、examples/run_plan_execute_demo.cpp、examples/run_reflection_demo.cpp、examples/run_evaluator_optimizer_demo.cpp。

## 边界和安全

当前是同步的 Responses + MCP Agent，不支持流式 SSE 对话、Realtime 双向语音、直接分析音频声学特征、自动工具权限审批和上下文压缩。
`Config::api_key` 和 `Config::base_url` 必须由调用方显式赋值。使用真实 API Key 时应由你的应用安全地取得并传入，**不要将真实密钥硬编码后提交到 Git**；历史文件可能包含敏感会话内容，也不要提交。
技能文件只在按需激活时作为工具内容或手动配置的提示词加载，不执行脚本；请核查 Skills 和远程 MCP 服务的来源。会改变文件或运行命令的工具应由调用方额外做权限控制与隔离。
媒体上限为 20 MiB，工具返回上限为 256 KiB；不包含实际 OpenAI API 联网集成测试。
