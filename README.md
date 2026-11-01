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
- Token 用量统计：Responses 真实 usage、单次与累计、按循环阶段和模型分组、缓存与推理 Token
- P0/P1/P2：完整轮次上下文压缩、独立模型配置的 AgentTeam、任务委派、顺序及并行协作、工作区受限文件工具

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

## P0/P1/P2 工程化加固：权限、恢复、Provider、记忆

### 工具安全与截止时间（P0）

```cpp
myagent::Config cfg;
cfg.agent_id = "coder";
cfg.http_timeout_seconds = 45;
cfg.tool_policy.default_access = myagent::ToolAccess::Deny;
cfg.tool_policy.allow("local__read");
cfg.tool_policy.require_approval("git__push");
cfg.tool_policy.deny("shell__*");
cfg.tool_policy.approve = [](const myagent::ToolRequest& request) {
    // 宿主应按真实用户授权决策，不能让模型自行批准。
    return false;
};
myagent::Agent agent(cfg);
agent.ask_without_tools("仅规划，不开放工具");

myagent::LoopOptions loop_options;
loop_options.timeout = std::chrono::seconds(30);
auto answer = agent.react("分析代码", loop_options);
```

工具权限是执行器强制检查：Deny 从工具列表移除并拒绝执行，RequireApproval 在实际调用前询问宿主；通配符只支持末尾 * 前缀匹配。默认 default_access=Allow 是为兼容旧代码，生产环境建议显式切换 Deny。Coordinator 自动计划与最终汇总使用无工具请求。相同 Agent 的模型/对话/Skill 和 MCP 注册有会话互斥；不同子 Agent 仍可并发。

**限制**：Loop/Team timeout 是轮次间检查的协作式取消，不会强制中止正在阻塞的第三方 MCP；HTTP 模型有可配置网络超时。工作区写入采用临时文件及重命名以避免部分截断，但路径校验仍不是 OS 级沙箱，执行不可信程序需要进程或容器隔离。

### 任务 Checkpoint、重试、备用 Agent 和事件（P1）

```cpp
myagent::TeamTask task{"compile", "coder", "编译并测试项目"};
task.priority = 10;
task.fallback_agents = {"backup-coder"};

myagent::TeamOptions opts;
opts.max_parallel_agents = 2;
opts.max_task_retries = 1;             // 仅无工具调用的失败任务可自动重试
opts.checkpoint_path = "team-state.json";
opts.event_log_path = "team-events.jsonl";
opts.validate_task = [](const myagent::TeamTask& task,
                        const myagent::DelegationResult& result) {
    // 应检查真实文件/编译/测试证据，不可只信任模型文字。
    return myagent::TaskValidation{false, "Replace with real validator"};
};
auto first = team.collaborate({task}, opts);
// 恢复时必须使用相同任务图；检查已完成结果，拒绝回放不明 in-flight 副作用。
opts.resume_checkpoint = true;
auto restored = team.collaborate({task}, opts);
```

Checkpoint 只记录任务图、执行结果、已知 in-flight 状态和用量，不存模型凭据。恢复必须提供宿主验证器重新确认完成任务；若崩溃时任务正在执行则默认拒绝盲目重放。这里展示的是调用方式，示例验证器返回 false，须由实际项目替换。优先在固定任务图 collaborate() 上恢复；team.run() 会重新请求 Coordinator 生成任务图。失败时可以有限重试或委派给已注册的备用 Agent，但只在**未执行工具**时自动重试/重新委派。调度事件可经 TeamOptions.on_event、event_log_path 和 TeamResult.events 查看，事件不含 API Key 和工具正文。

### YAML/JSON 声明式 Agent 配置（P1/P2）

```yaml
agents:
  - name: coder
    description: C++ 开发
    config:
      base_url: https://provider.example.com
      model: coder-model
      model_protocol: responses
      api_key_ref: coder-key
      auto_discover_skills: false
      auto_skill_selection: true
    skills: [./skills/coder]
    use_skills: []
    mcp:
      - prefix: files
        transport: http
        url: http://127.0.0.1:3000/mcp
        bearer_token_ref: files-key
```

```cpp
team.add_agents_from_file("./agents.yaml",
    [](const std::string& key_ref) {
        return read_secret_from_host(key_ref); // 宿主自定义的密钥解析方法
    });
```

配置文件不允许内联 api_key 或 bearer_token；密钥由宿主回调按引用查找。AgentSpec 也支持直接声明 skill_directories、explicit_skills、mcp（HTTP、stdio、已有 OAuth Session）。YAML 的 OAuth 会话仍须由 C++ 宿主交互授权后注册。

### Provider、并行调度和记忆（P2）

```cpp
myagent::Config cfg;
cfg.model_protocol = myagent::ModelProtocol::ChatCompletions;
cfg.base_url = "https://provider.example.com";
cfg.api_key = "example-key";
cfg.model = "chat-model";
myagent::Agent agent(cfg);

myagent::MemoryStore memory;
memory.remember({"coding", "Use C++20 and RAII."});
auto hits = memory.search("C++20 RAII");
memory.save("./memory.json");
```

支持 Responses 和部分 **OpenAI 兼容 Chat Completions** 的文本/图像/函数调用映射；原生 Anthropic、Gemini 等协议需注入自定义 Model，尚无全面能力探测。AgentTeam 使用固定大小线程池和优先级调度，避免反复建线程；同一 Agent 在同一批任务中不会并发运行。MemoryStore 是本地**关键词检索**，并非向量语义检索；只有配置 memory_retrieval=true 才向模型提供命中的记忆条目。记忆和 Checkpoint 文件可能含任务内容，宿主应控制访问权限。

离线示例：run_runtime_hardening_demo()、run_team_checkpoint_demo()。测试：myagent_runtime_hardening、myagent_p1_p2，覆盖审批、恢复、YAML 配置、Chat Provider、内存、备选 Agent、优先级。


## P0/P1/P2：上下文自动压缩与多 Agent 协作

### 1. 上下文压缩 ContextManager

历史增长时不应按消息条数强制截断。`ContextManager` 将完整的旧用户轮次压缩为结构化摘要，并保留最近完整轮次和所有成对的 MCP 工具调用/结果。摘要模型由调用方配置；压缩请求本身的 `usage` 会计入 Agent 的 Token 统计。压缩失败时不会破坏原历史。

```cpp
myagent::Config config;
config.base_url = "https://api.openai.com";
config.api_key = "sk-example-only"; // 非真实密钥
config.model = "gpt-4.1-mini";
config.context.max_context_tokens = 128000; // 必须按实际模型容量填写
config.context.reserve_output_tokens = 8192;
config.context.compress_threshold = 0.80;
config.context.keep_recent_turns = 4;
config.context.auto_compress = true;
myagent::Agent agent(config);

agent.ask("检查 C++ 项目");
auto manual = agent.compress_context(); // 即使未达到阈值，也尝试手动压缩
auto stats = agent.context_stats();      // estimated_tokens、compression_count、compression_usage
```

**注意**：用于触发压缩的 `estimated_tokens` 是近似值，图片、音频转写、工具定义、中文和不同模型的实际 Token 分布可能有偏差；服务端 `usage` 仍是成本统计依据。默认 `auto_compress=false`，因为不同模型上下文上限不相同，框架不会凭空猜测。`ContextOptions::summary_model` 可指定同一模型后端下的摘要模型名称。自动压缩是在已有完整轮次之间进行，不会拆断一组调用/结果；如果没有足够旧轮次可压缩，会保留历史并由原有模型上限保护继续处理。总结仍可能损失细节；关键事实应由宿主保留原始文件或独立审计记录。`save_history()` 与 `load_history()` 会保存/恢复压缩后的消息，但不会保存 API Key。

### 2. 子 Agent 独立配置与手动委派

```cpp
#include "myagent/multi_agent.hpp"

myagent::Config leader;
leader.base_url = "https://api.openai.com";
leader.api_key = "sk-example-leader";
leader.model = "leader-model";
myagent::AgentTeam team(leader);

myagent::AgentSpec coder;
coder.name = "coder";
coder.description = "C++ 代码实现";
coder.config.base_url = "https://provider-a.example.com";
coder.config.api_key = "coder-example-key";
coder.config.model = "coder-model";
team.add_agent(coder);

myagent::AgentSpec reviewer;
reviewer.name = "reviewer";
reviewer.description = "C++ 代码审查";
reviewer.config.base_url = "https://provider-b.example.com";
reviewer.config.api_key = "reviewer-example-key";
reviewer.config.model = "reviewer-model";
team.add_agent(reviewer);

// 明确指定子 Agent 与 Loop 模式，不需要命令行参数或环境变量
auto code = team.delegate("coder", "编写一个线程安全的 C++ 组件");
auto review = team.delegate("reviewer", "审查该组件的资源管理");

// 主 Agent 自动形成任务图，并选择注册过的子 Agent
auto result = team.run("开发并审查 C++ 组件");
```


#### 子 Agent Skill 与 MCP 如何配置（可运行 Demo）

子 Agent 创建后通过 `team.agent("名称")` 获取其独立的 `Agent` 对象；不需要额外的 `AgentTeam` MCP/Skill API。建议在执行 `delegate()`、`run()`、`collaborate()` **之前**注册对应工具和 Skill。

```cpp
myagent::AgentSpec coder;
coder.name = "coder";
coder.description = "负责 C++ 开发";
coder.config.base_url = "https://model-a.example.com";
coder.config.api_key = "example-coder-key";
coder.config.model = "coder-model";
coder.config.auto_discover_skills = false; // 不扫描全局项目/用户 Skill
coder.config.auto_skill_selection = true; // 模型可通过 agent_skill_load 按需加载
team.add_agent(coder);

auto& coding = team.agent("coder");
coding.load_skills("./skills/coder");  // 仅显式加载 Coder 的 Skill 目录
coding.add_mcp_http("files", "https://files.example.com/mcp", "mcp-bearer-token");
coding.add_mcp_stdio("git", "./git-mcp-server");
// 也可调用 coding.add_mcp_oauth("github", url, authorized_session);

myagent::AgentSpec reviewer;
reviewer.name = "reviewer";
reviewer.description = "只负责审查";
reviewer.config.base_url = "https://model-b.example.com";
reviewer.config.api_key = "example-reviewer-key";
reviewer.config.model = "reviewer-model";
reviewer.config.auto_discover_skills = false;
reviewer.config.auto_skill_selection = false; // 禁止模型自主加载 Skill
team.add_agent(reviewer);
auto& review = team.agent("reviewer");
review.load_skills("./skills/reviewer");
review.use_skill("cpp-review");       // 显式固定某个 Skill 的完整指令
review.add_mcp_http("readonly", "https://readonly.example.com/mcp", "readonly-token");
```

`auto_discover_skills=true` 会在 **Agent 构造时**扫描当前项目路径的 `.agents/skills`、`.claude/skills`、当前目录 `skills`，以及用户主目录中的 Skill；`false` 不自动扫描。`auto_skill_selection=true` 会向模型公开已注册的可自动调用 Skill 元数据与 `agent_skill_load`/`agent_skill_read_file` 工具，**不代表自动加载全部正文**；设为 `false` 后仍可手动 `use_skill()`。

可直接运行 `run_agent_delegation_demo()`：里面包括三个真实示范场景（Coder 手动目录+自动 Skill 选择；Reviewer 手动目录+手动 Skill；第三个子 Agent 构造时自动发现项目 Skill），并使用内存 MCP Server/Client 模拟发现、工具调用、结果回传，验证 Coder/Reviewer 不互相继承工具。示例资源位于 `examples/agent_team_demo_skills/`，**离线运行且不连接示意 HTTP/MCP 地址**。如需实际网络 MCP，按需将演示的 `add_mcp()` 替换为 `add_mcp_http()`、`add_mcp_stdio()` 或 `add_mcp_oauth()`。

**权限提示：** `auto_discover_skills` 控制自动发现，`auto_skill_selection` 控制向模型提供 Skill 调用能力，但都不是操作系统安全机制；Skill 的 `allowed-tools` 仅作为元数据，不会自动授权。真正的 MCP 读写权限应由服务端令牌、受限工具注册或工作区权限落实。不同子 Agent 需要真正隔离的 Skill 时推荐关闭自动发现，并分别调用 `load_skills()`。
每个子 Agent 使用独立 `Agent`、模型 URL、API Key、模型名、MCP/Skill/上下文、历史和 Token 用量，不继承主 Agent 的凭据。现有实现使用 **OpenAI Responses 兼容协议**：不同 URL 可以独立配置，但不代表原生支持各家厂商不兼容的 Chat Completions 协议。建议对子 Agent 仅授予完成其任务所必需的工具权限。

### 3. 显式任务图、并行调度和团队预算

```cpp
std::vector<myagent::TeamTask> tasks = {
    {"read", "coder", "阅读并分析文件"},
    {"check", "reviewer", "独立审查安全性"},
    {"combine", "coder", "整合分析结论", {"read", "check"}}
};
myagent::TeamOptions options;
options.max_parallel_agents = 2;
options.max_delegations = 8;
options.max_model_calls = 64;
options.max_tool_calls = 64;
options.max_depth = 1;
auto result = team.collaborate(tasks, options);
// result.usage、result.usage_by_agent、result.tasks 分别有用量、归属和任务状态。
```

对于更复杂的协作，可以给某个 `TeamTask` 的 `context` 传入结构化的 `goal`、`relevant_context`、`artifacts` 和 `constraints`；任务委派只读取这些筛选过的资料及前置任务摘要，而非主 Agent 全部历史。`TeamOptions::validate_task` 是可选的宿主验收回调：接收 `TeamTask` 与 `DelegationResult`，返回 `TaskValidation{passed,reason,evidence}`。如果验收失败，任务变为 Failed，依赖它的下游任务会被 Skipped。没有设置时 `DelegationResult::verification` 为 `unverified`，模型完成声明不会自动视作真实编译/测试通过。
没有依赖且属于不同子 Agent 的任务可以通过 `std::async` 同时运行；同一 Agent 的任务绝不会同时操作其会话，因为 `Agent` 本身不是线程安全的。依赖失败的后续任务将标记为跳过。`max_model_calls/max_tool_calls` 会在子任务阶段分配调用预算；协调器本身不拥有业务工具，不会向模型提供其他 Agent 的 API Key。`max_reported_tokens` 是服务端响应后才能确认的**软阈值**，不是零超额保证。子 Agent 不允许递归创建更多子 Agent：当前 `max_depth=1`；未来可以扩展为多级树，但需要更严格的递归预算与权限管理。

### 4. P2 工作区文件工具（受路径约束，不是沙箱）

```cpp
myagent::AgentSpec worker;
worker.name = "worker";
worker.description = "分析与修改独立工作树";
worker.config = coder.config;
worker.workspace = "./worktrees/worker";
worker.allow_workspace_write = true;
team.add_agent(worker);
```

指定工作区后，框架只注册 `workspace_read`、`workspace_list`，可选 `workspace_write`；操作路径必须位于该子 Agent 的规范化工作区下，拒绝明显的越界与符号链接逃逸。不同 Agent 不允许配置重叠工作区。这是**内建文件工具的约束**，不提供 OS 容器级隔离、强制隔离任意外部 MCP 工具、也不能避免文件系统 TOCTOU 竞争。执行不可信代码、使用任意 shell 或需要安全租户隔离时仍需独立进程/容器、权限限制和专门工作树。

### Demo 与测试

`run_context_compression_demo()`、`run_agent_delegation_demo()`、`run_agent_collaboration_demo()`、`run_parallel_agents_demo()` 均为独立文件与独立 CMake 目标，包含中文说明、离线 Mock Model；`tests/test_context.cpp`、`tests/test_multi_agent.cpp` 集成到 CTest，验证上下文压缩、Token 统计、不同模型、自动委派、依赖、并发、预算和越界路径。

## 源码审计修复：上下文、Checkpoint、工具与安全边界

- **团队 Checkpoint v2**：分别保存已完成任务和失败/转派的旧 Agent 尝试记录，跨进程恢复后把旧 Token、模型调用、工具调用记回预算并跳过已经失败的备用 Agent。格式新增 `prior_attempts`，**不直接兼容旧 v1 文件**；旧文件必须先由宿主核对后迁移。
- **Loop 压缩提交**：自动压缩后，会提交压缩过的原始会话前缀以及本次任务和最终答案，不把内部工具日志混入普通历史；压缩成功后仍检查包含阶段指令、检索记忆、Skill 与工具 Schema 的最终上下文预算。
- **Chat Completions**：多个 tool_calls 被合并成同一 assistant 消息，并转发 `parallel_tool_calls` 控制。
- **Coordinator 预算**：无工具请求可限定模型调用总数，自动压缩请求同样消耗预算；请求失败也计入调用次数。
- **工作区安全**：POSIX 使用目录 FD、`openat`、`O_NOFOLLOW`、排他临时文件与 `renameat`；Windows 使用重解析点检查、排他 `CreateFileW` 与 `MoveFileExW`。**仍不是 OS 沙箱**。特别是 Windows 上父目录可被其他进程并发替换时，应借助 ACL、独立进程或容器隔离。任何外部 MCP 的权限仍需由服务端及 ToolPolicy 管理。
- **验证扩展**：新增 `myagent_audit` 测试；CI 增加 Linux/Windows Release 与 Linux ASan/UBSan；Release 版测试保持 assert 检查有效。
## Token 消耗统计（Responses usage）

Agent 直接读取每次 Responses API 返回的 `usage`，**不靠文字字符数估算**。支持普通 `ask()`、图片请求、工具调用后的模型轮次，以及全部四种 Agent Loop。每个 Loop 都会统计所有规划、执行、反思、评估阶段的用量。

```cpp
myagent::Agent agent(config);
agent.ask("查找项目资料");
auto last = agent.last_usage();     // 上一次 ask() 的所有模型轮次
auto total = agent.total_usage();   // 当前 Agent 创建以来的累计用量

auto result = agent.plan_and_execute("分析并修改 C++ 项目");
auto usage = result.usage;                          // 本次 Loop 的总统计
auto planner = result.usage_by_phase.at("plan");    // 规划阶段
auto executor = result.usage_by_phase.at("execute"); // 执行阶段（含工具调用轮次）
// result.usage_by_model 可按实际请求的模型名称汇总。
std::cout << usage.input_tokens << ", " << usage.output_tokens
          << ", " << usage.total_tokens << "\n";
std::cout << usage.cached_input_tokens << ", "
          << usage.reasoning_output_tokens << "\n";
std::cout << usage.reported_calls << ", " << usage.unreported_calls << "\n";

agent.clear_history(); // 不清除 Token 用量
agent.reset_usage();   // 显式清空最近一次与累计统计（不会影响服务端账单）
```

| 字段 | 含义 |
| --- | --- |
| `input_tokens` / `output_tokens` / `total_tokens` | 输入、输出、总 Token |
| `cached_input_tokens` | 输入缓存命中 Token，已包含在 input_tokens 内 |
| `reasoning_output_tokens` | 推理 Token，已包含在 output_tokens 内 |
| `reported_calls` | 包含有效 usage 的模型响应次数 |
| `unreported_calls` | 未提供有效 usage 的模型响应次数 |

若某次调用缺少 `usage` 或格式不合法，`unreported_calls` 会加一，**不会假装消耗为零，也不会按字数推算**。累计 Token 只能代表模型已报告的用量；即使任务最终失败，之前已收到的有效 usage 仍计入统计。`TokenUsage::complete()` 表示没有缺失统计的**已收到响应**，不保证网络失败后能取得服务端计费结果。

本功能只计 Responses 模型调用。图片若进入 Responses 模型计费会出现在该模型的 usage 中；独立的音频转写/语音合成接口不在此统计范围内，也不自动计算 API 费用。不同模型和供应商的价格不相同，缓存和推理 Token 不得重复计数。

独立的离线示例 `examples/run_token_usage_demo.cpp` 提供 `run_token_usage_demo()`；测试 `tests/test_token_usage.cpp` 模拟真实 usage、工具轮次、模型缺失统计以及失败任务的账本保留。
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

## 2026-11-01 audit hardening (PR #16)

- **Chat Completions routing**: retains optional gateway prefixes and query strings; only `stop` and `tool_calls` are successful finish reasons. Unexpected termination and empty final text raise errors.
- **Audio gateway**: by default transcription/speech use `/v1/audio` on the model origin. To route them elsewhere, set `Config::audio_base_url = "https://proxy.example.com/tenant"`; it appends `/v1/audio/{transcriptions|speech}`. The team YAML `config.audio_base_url` field is also supported.
- **Cancellation**: worker-specific cancellation is combined with team cancellation; team timeout includes coordinator planning/summary and passes remaining time to workers. Model HTTP and MCP operations still use cooperative timeouts.
- **History**: `save_history()` writes `myagent-history-v2` with model/protocol metadata; `load_history()` accepts old array files only after strict validation and rejects incompatible v2 metadata. Both save and load enforce the 8 MiB limit; saved memory files enforce their 32 MiB load limit.
- **Idempotence**: `LoopOptions::non_idempotent_tools` identifies tool names that must not be replayed with identical arguments inside one loop run; the built-in `local__workspace_write` starts protected. Add external write/delete/payment tools explicitly (e.g., `options.non_idempotent_tools.insert("git__push")`). A different argument value or a process restart is not automatically idempotent; use host validations, operation IDs and checkpoint reconciliation.
- **Disk safety**: checkpoint writes use a unique temporary file, flush and atomic same-directory replacement; Windows workspace operations pin parent directories and reject reparse points. These mechanisms are not an OS or tenant sandbox.
- **Team YAML**: relative `workspace` paths resolve beside the configuration file, like Skill paths. Bulk registration rolls back already created workers if a later registration fails.
- **Offline tests**: `myagent_full_audit` checks protocol URLs, stop reasons, token overflow guarantees, empty output, history safety, cancellation/timeout, YAML rollback, and non-idempotent replay guards.

For production, configure `tool_policy.default_access = ToolAccess::Deny`, allow only necessary tools, review discovered Skills, and enforce permissions at MCP servers and OS/container boundaries.
