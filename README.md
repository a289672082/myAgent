# myAgent — 控制台入口 + 通用 C++20 SDK

仓库根目录默认构建 **myagent_console**（Codex 风格 FTXUI 终端交互应用），
SDK 头文件和源码位于 `sdk/`，与控制台代码/依赖完全隔离。

**Visual Studio 2022：** 仓库根目录已包含 [myAgent.sln](myAgent.sln)。
直接打开解决方案即可使用控制台、Agent SDK／示例／测试，以及 MCP SDK／示例／测试共七个项目（x64，Debug／Release）；
控制台默认参与生成，并配置了 F5 调试路径。工程具体说明见 [vs2022/README.md](vs2022/README.md)。
SDK/FTXUI 依赖由 CMake 统一构建，不维护第二套 MSVC 编译配置。

## 23 个开发工具：Agent / MCP 双重注册

新增独立的 [开发工具使用指南](tools/developer_tools/README.md)。工具实现和 JSON Schema 只维护在
`tools/developer_tools/`；可分别使用 `developer_tools::RegisterToAgent(agent, context, options)`
以及 `developer_tools::RegisterToMcp(server, context, options)`，无需维护两套工具定义。
七大类别共 23 个工具可独立开关，支持只读选项。

控制台可通过 `/tools`、`/tools on`、`/tools off` 控制原生工具。独立 MCP stdio 服务端
`myagent_developer_tools_mcp` 供其他 MCP 客户端使用。二者复用 `mcp/sdk/` 内置 SDK，
不再依赖外部 myMcpTools 仓库；纯 Agent SDK (`cmake -S sdk`) 不包含这些应用级工具。
文件写入与命令操作需要宿主明确授权，工具本身不是操作系统沙箱。

## 内置 MCP SDK 与文档索引

**myMcpTools 的客户端、服务端、示例、测试及所需第三方源码已经整合到本仓库的 `mcp/` 目录**，不再通过 `third_party/myMcpTools` Git 子模块获取。Agent SDK 在 `sdk/`，MCP SDK 在 `mcp/sdk/`，两套 SDK、Demo 和测试分别维护；`yaml-cpp` 与 `FTXUI` 仍是独立子模块。

需要了解 MCP 的具体用法、协议兼容性或安全限制时，请优先查看**仓库内置文档**：

| MCP 文档 | 内容 |
| --- | --- |
| [MCP SDK 总览](mcp/README.md) | 客户端与服务端 API、传输方式、OAuth、协议能力和独立构建说明 |
| [MCP Demo 使用指南](mcp/examples/README.md) | 独立 run_xxx_demo、stdio / HTTP / OAuth 和 23 个工具综合示例 |
| [协议兼容性矩阵](mcp/COMPATIBILITY.md) | 已实现的 MCP 版本与特性、未覆盖项和测试范围 |
| [Codex / Claude 接入指南](mcp/INTEROPERABILITY.md) | stdio、Streamable HTTP、外部客户端连接与互操作注意事项 |
| [安全与生产使用限制](mcp/SECURITY.md) | 认证、授权、OAuth、远程暴露、任务与进程隔离风险 |
| [第三方依赖与许可证](mcp/third_party/README.md) | jsoncons / cpp-httplib 的来源、固定版本、离线编译与许可证 |
| [上游版本说明](mcp/UPSTREAM.md) | 本仓库内置的 myMcpTools 来源与固定提交 |

仅构建 MCP SDK（不编译 MCP Demo / 测试）：

```sh
cmake -S mcp/sdk -B build-mcp-sdk
cmake --build build-mcp-sdk --config Debug
```

编译 MCP SDK、独立 Demo，并运行 MCP 测试：

```sh
cmake -S mcp -B build-mcp
cmake --build build-mcp --config Debug
ctest --test-dir build-mcp -C Debug --output-on-failure
```

若使用根项目，可传入 `-DMYAGENT_BUILD_MCP_EXAMPLES=ON -DMYAGENT_BUILD_MCP_TESTS=ON` 开启 MCP 示例与测试；默认仍以 Agent 控制台为入口。构建要求 C++20、CMake、OpenSSL 和线程库；`mcp/third_party/` 中已内置 jsoncons、cpp-httplib 源码及许可证。

## 目录结构与快速启动

```text
myAgent/
├── CMakeLists.txt           # 根项目：默认构建控制台及 Agent / MCP SDK
├── console/                 # Codex 风格 FTXUI 终端
├── sdk/                     # Agent SDK：独立 CMake、include、src
├── examples/                # Agent Demo
├── tests/                   # Agent 回归测试
├── tools/developer_tools/      # 23 个共享工具 + Agent/MCP 注册
├── mcp_servers/developer_tools/ # 独立 MCP stdio 服务端
├── mcp/
│   ├── README.md            # MCP SDK 使用说明
│   ├── COMPATIBILITY.md     # MCP 协议兼容性
│   ├── INTEROPERABILITY.md  # Codex / Claude 互操作
│   ├── SECURITY.md          # 安全说明
│   ├── UPSTREAM.md          # 上游固定版本
│   ├── sdk/                 # MCP SDK：include、src 和 CMakeLists.txt
│   ├── examples/            # MCP Demo 和使用指南
│   ├── tests/               # MCP 测试
│   └── third_party/         # jsoncons、cpp-httplib 与许可证（源码内置）
└── third_party/
    ├── FTXUI/               # FTXUI Git 子模块
    └── yaml-cpp/            # YAML Git 子模块
```

```sh
git clone --recurse-submodules https://github.com/a289672082/myAgent.git
cd myAgent
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure
```

子模块初始化后，FTXUI 固定源代码位于 `third_party/FTXUI`，CMake 全程离线，
**不会在配置时下载 FTXUI**。更多命令见 [控制台说明](console/README.md)。

只构建通用 SDK（完全不引入 FTXUI）：

```sh
cmake -S sdk -B build-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build-sdk --config Release
```

仅构建/测试 SDK 示例和回归测试：

```sh
cmake -S . -B build-sdk-tests -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build-sdk-tests --config Debug
ctest --test-dir build-sdk-tests -C Debug --output-on-failure
```

---

## Agent SDK 使用指南

本项目使用**内置的 MCP SDK** 支持 MCP 客户端、服务端、JSON、HTTP 和 OAuth，MCP 源码、示例及第三方依赖都由本仓库维护。MCP 的 API 说明见 [MCP SDK README](mcp/README.md)，实际操作示例见 [MCP Demo 指南](mcp/examples/README.md)；原上游仓库仅用作版本来源，不是构建依赖。

## 功能

- OpenAI Responses API 多轮对话、函数调用与结果回传
- MCP 客户端：stdIO / Streamable HTTP / OAuth；工具自动发现与执行
- Agent 内直接通过 Lambda 或 JSON Schema 定义 MCP 工具
- 自动发现项目级和用户级 Skills；模型按任务触发、逐层读取 SKILL.md 和参考资料，也可手动启用
- 图片输入：PNG / JPEG / WebP / GIF 转 base64 Data URL
- 音频输入：转写成文字后继续对话；音频输出：合成 MP3
- 保存/加载对话历史，注入 Model mock 做离线测试
- 六种循环：ReAct / Plan-and-Execute / Reflection / Evaluator-Optimizer / ReWOO / Execute-Verify-Repair（统一预算，支持多图及音频转写）
- Token 用量统计：Responses 真实 usage、单次与累计、按循环阶段和模型分组、缓存与推理 Token
- P0/P1/P2：完整轮次上下文压缩、独立模型配置的 AgentTeam、任务委派、顺序及并行协作、工作区受限文件工具

## 构建

依赖 C++20、CMake 3.20 或更新版本、OpenSSL 和线程库。固定版本的 **yaml-cpp 0.9.0** 以 Git 子模块引入（MIT 许可证），用于标准 YAML 前置元数据解析；通过 `--recurse-submodules` 克隆后可离线编译。

~~~sh
git clone --recurse-submodules https://github.com/a289672082/myAgent.git
cd myAgent
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
~~~

已经克隆时可以执行 git submodule update --init --recursive。CMake 默认完全离线，不会在配置过程中下载依赖。MCP SDK 和依赖源码已内置于 mcp/；无需额外下载或指定 MYAGENT_MCP_DIR。

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

Agent 直接读取每次 Responses API 返回的 `usage`，**不靠文字字符数估算**。支持普通 `ask()`、图片请求、工具调用后的模型轮次，以及全部六种 Agent Loop。每个 Loop 都会统计所有规划、执行、反思、评估阶段的用量。

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
## 统一 Agent 循环（六种策略 + 图片和音频）

新增 myagent::LoopInput、LoopOptions、LoopResult、LoopMode、LoopStatus（sdk/include/myagent/loop.hpp），在不破坏原有 ask() 的基础上提供六种策略（下面先介绍原有四种，新增两种见文末 P1 章节）：
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

// 音频先转写为文字后进入六种循环，不是原始音频声学推理。
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
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
~~~

## MCP 快捷接口（推荐）

以下为 Agent 对 MCP 的快捷封装；若需要直接使用 MCP SDK 的服务端、客户端、OAuth 或通知能力，请参阅 [内置 MCP SDK API](mcp/README.md)、[MCP 独立示例](mcp/examples/README.md) 和 [互操作说明](mcp/INTEROPERABILITY.md)。

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
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
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

核心头文件：sdk/include/myagent/agent.hpp。
核心实现：sdk/src/agent.cpp。
测试：tests/test_agent.cpp。
示例：examples/run_agent_demo.cpp、examples/run_skill_yaml_demo.cpp、examples/run_react_demo.cpp、examples/run_plan_execute_demo.cpp、examples/run_reflection_demo.cpp、examples/run_evaluator_optimizer_demo.cpp。

## 边界和安全

当前是同步的 Responses + MCP Agent，不支持流式 SSE 对话、Realtime 双向语音、直接分析音频声学特征、自动工具权限审批和上下文压缩。
`Config::api_key` 和 `Config::base_url` 必须由调用方显式赋值。使用真实 API Key 时应由你的应用安全地取得并传入，**不要将真实密钥硬编码后提交到 Git**；历史文件可能包含敏感会话内容，也不要提交。
技能文件只在按需激活时作为工具内容或手动配置的提示词加载，不执行脚本；请核查 Skills 和远程 MCP 服务的来源。会改变文件或运行命令的工具应由调用方额外做权限控制与隔离。
媒体上限为 20 MiB，工具返回上限为 256 KiB；不包含实际 OpenAI API 联网集成测试。

## 2026 年 11 月 1 日代码审计加固（PR #16）

- **Chat Completions 请求路由**：保留可选的网关路径前缀及查询参数；只有 `stop` 和 `tool_calls` 被视为成功的结束原因。异常终止或最终文本为空时会报错。
- **音频网关**：默认将语音转写与合成请求发送到模型地址对应的 `/v1/audio`。如需使用独立服务，请设置 `Config::audio_base_url = "https://proxy.example.com/tenant"`，SDK 会追加 `/v1/audio/{transcriptions|speech}`；团队 YAML 配置也支持 `config.audio_base_url`。
- **取消机制**：子 Agent 的独立取消状态会与团队取消状态结合；团队超时覆盖协调器规划及汇总阶段，并将剩余时间传给各工作者。模型 HTTP 与 MCP 操作仍主要依赖协作式超时。
- **历史记录**：`save_history()` 写入包含模型及协议元数据的 `myagent-history-v2` 格式；`load_history()` 只在严格校验通过时接受旧版数组格式，并拒绝不兼容的 v2 元数据。保存与加载均强制执行 8 MiB 限额；保存的记忆文件在加载时有 32 MiB 限额。
- **幂等性保护**：`LoopOptions::non_idempotent_tools` 指定同一次循环内不能用相同参数重复执行的工具；内置 `local__workspace_write` 默认受保护。外部写入、删除或支付工具必须显式添加（例如 `options.non_idempotent_tools.insert("git__push")`）。修改参数或重启进程并不能自动保证幂等，应结合宿主验证、操作 ID 与 Checkpoint 对账。
- **磁盘安全**：Checkpoint 使用唯一临时文件写入、刷新并在同目录原子替换；Windows 工作区操作固定父目录句柄并拒绝重解析点。以上机制不构成操作系统级或租户级安全沙箱。
- **团队 YAML**：相对 `workspace` 路径以配置文件所在目录为基准解析，与 Skill 路径处理一致。批量注册时若后续子 Agent 创建失败，会回滚此前新建的工作者。
- **离线测试**：`myagent_full_audit` 覆盖协议 URL、结束原因、Token 溢出保护、空输出、历史记录安全、取消及超时、YAML 回滚和非幂等调用防重放。

生产环境应设置 `tool_policy.default_access = ToolAccess::Deny`，仅放行必要工具、审查自动发现的 Skill，并在 MCP 服务端和操作系统/容器边界落实权限控制。

### 后续代码审计：2026 年 11 月 1 日

- 恢复旧版 v2 历史时避免 jsoncons 子对象自赋值；已使用 Linux AddressSanitizer 回归测试覆盖。
- `LoopOptions.protect_unclassified_tools=true` 会阻止同一次循环内未知 MCP 工具以相同参数重复执行。已验证只读的工具可以加入 `LoopOptions.idempotent_tools`；显式 `non_idempotent_tools` 始终优先。保护只在单次运行内生效，并不提供外部系统事务账本。
- `TeamOptions.timeout` 会按请求传递给内置 Responses 与 Chat Completions HTTP 传输。第三方 `Model` 实现可以覆盖 `create_with_timeout` 以支持超时；无法安全地强行中断 MCP 工具或不合作的自定义模型。
- 历史与记忆保存使用同目录持久化原子替换；Checkpoint 还通过 `<path>.lock` 目录拒绝并发写入。进程写入途中崩溃可能留下过期锁；**确认没有写入进程运行后**才能清理。
- 媒体、历史、Checkpoint 和记忆读取会校验已打开的普通文件句柄并施加字节限额，拒绝最终路径中的符号链接、重解析点或 FIFO。
- 子 Agent 交接预算将任务描述、依赖任务结果和可选结构化上下文统一纳入总字节限额。
- YAML 批量注册回滚只删除本次新建且为空的工作区目录，绝不会递归删除原有文件或目录。

### MCP 与数据持久化的安全限制

- Agent 使用有界分页发现 MCP 工具，并拒绝过大的 Schema 或工具目录。**MCP SDK 传输层**也必须限制载荷大小；但进程内回调仍可能在返回前就分配巨大 JSON 响应。
- 历史和记忆保存与 Checkpoint 一样，使用相邻的 `*.lock` 目录拒绝并发写入。崩溃进程可能留下过期锁；**仅在确认没有活动写入者后才能删除**。加载过或已保存的文件会在锁内进行版本比对与替换，防止旧版本覆盖新数据。首次写入另一个目标位置时不存在先前版本，按设计会直接替换该目标。更新共享历史/记忆文件前，应用应先调用 `load()`，或自行协调首次写入。
- 外部用户提供的 C++ 模型或进程内 MCP 工具回调，无法通过 `std::future` 或终止线程安全地强制停止。严格截止时间应使用受监管的独立操作系统进程，并设置可执行的终止与资源限制。内置 myMcpTools HTTP 传输有网络读取超时；固定版本的 stdio 传输已经为不合作的子进程设置有限截止时间。


## P0/P1 MCP 硬截止时间与严格进程隔离（2026-11-01）

Agent 通过内置 MCP SDK 的 `RequestOptions` 在 MCP JSON 解析之前执行字节限制，并把 Loop 剩余时间传入发现、分页和工具调用。默认 mcp_timeout_seconds=30，max_mcp_response_bytes=1 MiB，后者不得超过 4 MiB。

~~~cpp
myagent::Config config;
config.api_key = "<configured-credential>";
config.base_url = "https://model-gateway.example";
config.mcp_timeout_seconds = 10;
config.max_mcp_response_bytes = 512 * 1024;
config.strict_process_isolation = true;
myagent::Agent agent(config);
agent.add_mcp_stdio("worker", "/absolute/path/to/untrusted-tool-server");
~~~

strict_process_isolation=true 会拒绝同步 Agent::tool() C++ 回调、没有 SDK 接收/截止时间能力的自定义 MCP Client（包括存在同步 Token 刷新的 http_oauth），以及未显式声明支持请求超时的 Model；内建进程内 Skill 工具也不可调用。自定义 Model 只有确实实现 create_with_timeout 才能返回 supports_request_deadline=true。

兼容模式（strict_process_isolation=false）仍允许旧的进程内回调，但无法保证恶意或死循环 C++ 代码按时间终止。严格模式解决的是拒绝不安全路径，而非自动把任意 lambda 变为可跨进程序列化的对象。

严格模式不是系统沙箱。仍需要针对外部 MCP 子进程执行最小权限、文件系统隔离、进程树限制与资源配额。

## AGENTS.md 显式加载 Demo（离线）

示例源文件为 `examples/run_agents_md_demo.cpp`，样例规则位于 `examples/agents_md_demo/AGENTS.md`。
独立入口 `run_agents_md_demo()` 使用 Mock Model 验证以下行为，无需 API Key、命令行参数或网络：
- `agent.LoadAgentsMdFile("AGENTS.md")`：从真实文件加载规则。
- `agent.LoadAgentsMdFileFormString(markdown)`：用字符串规则替换之前加载的规则。
- `agent.LoadAgentsMdFileFormString("")`：清空 AGENTS.md 规则，但保留 `Config::instructions`。

构建和运行：

```bash
cmake --build build --target run_agents_md_demo --config Debug
./build/run_agents_md_demo
# Windows: .\\build\\Debug\\run_agents_md_demo.exe
ctest --test-dir build -R myagent_agents_md_demo --output-on-failure
```

CMake 自动指定样例资源目录，因此 Demo 不依赖当前工作目录。该示例只展示**手动指定规则文件**，不会自动搜索多级 AGENTS.md。

## 可恢复 Agent Loop、暂停审批与 SSE 流式文本

普通 `Agent::run_loop()` 现可使用 `LoopOptions::checkpoint_path` 持久化模型响应与工具执行日志。
记录采用写前日志：**开始执行工具前**先持久化 `pending` 意图，完成后再持久化结果。
恢复时已经记录完成的模型响应/工具输出会被回放，不会再次调用工具；如果崩溃发生在
`pending` 状态，则**绝不自动重试**，而是返回 `LoopStatus::Paused`，
直到宿主在 `reconcile_tool` 中核实外部状态并提供已确认的结果。

```cpp
myagent::LoopOptions options;
options.checkpoint_path = "./task-checkpoint.json";
options.on_tool_approval = [](const myagent::ToolApprovalRequest& request) {
    // GUI 可显示 request.name / request.arguments。
    // 未收到人工批准时返回 Pause，立即交还控制权，不阻塞执行线程。
    return myagent::ToolDecision::Pause;
};
auto result = agent.react("检查并修改 C++ 代码", options);
if (result.status == myagent::LoopStatus::Paused) {
    // 用户点击“继续”后，重新提供明确的批准决策。
    myagent::LoopOptions next;
    next.on_tool_approval = [](const myagent::ToolApprovalRequest&) {
        return myagent::ToolDecision::Approve;
    };
    next.reconcile_tool = [](const myagent::ToolApprovalRequest& call)
        -> std::optional<std::string> {
        // 只针对已经核实结果的不确定工具返回输出。
        // 无法证实时返回 std::nullopt，继续保持暂停，绝不自动重放。
        return std::nullopt;
    };
    result = agent.resume("./task-checkpoint.json", next);
}
```

应用还可从其他线程调用 `agent.pause()`，在下一安全边界暂停。
此操作不是抢占式终止：正在执行的非合作 C++ 回调或网络请求仍需由各自的取消机制处理。
`resume()` 需要相同的模型配置、工具注册、初始会话历史和项目指令；
将任务迁移到新进程时应先重新配置这些依赖。已完成的 Checkpoint 禁止重复恢复。

> 安全限制：第一版 Checkpoint 仅支持**纯文本** `LoopInput`，单文件 16 MiB 上限。
> 快照会包含模型请求、提示词、历史和工具输出，可能含有项目机密；
> 应放在受控目录，设置文件访问权限，不应提交到 Git。
> 不能把 `reconcile_tool` 用作“总是批准重试”；它必须核实真实副作用状态。

内置 Responses 和 Chat Completions HTTP 客户端支持真实 SSE 文本增量回调：

```cpp
myagent::LoopOptions options;
options.on_text_delta = [](const std::string& phase, const std::string& delta) {
    // phase 表示 react / plan / execute / evaluate 等阶段；
    // delta 只包含模型提供的公开输出文本，不含隐藏推理内容。
    std::cout << "[" << phase << "] " << delta << std::flush;
};
const auto result = agent.react("解释这段代码", options);
```

`Model::create_stream` 可由自定义 Provider 重写；默认实现只返回完整结果，
不会伪造 token 流。具体内容为 Responses `response.output_text.delta`、
Chat `choices[].delta.content`，由服务端真正逐段推送。
离线回归包含 `myagent_streaming`（本机模拟 SSE HTTP）和
`myagent_full_audit`（审批暂停、日志恢复、崩溃未确认副作用不自动重放）。


## 可恢复执行与 SSE 的两个独立 Demo

新增两个独立的 C++20 可执行程序：`run_agent_checkpoint_demo()` 和 `run_agent_streaming_demo()`。无需真实 API Key、命令行参数或外网。

### 1. run_agent_checkpoint_demo

源码：`examples/run_agent_checkpoint_demo.cpp`，覆盖功能 1、2、4：

- Checkpoint：`LoopOptions::checkpoint_path` 持久化普通 ReAct Loop。
- 人工审批：`on_tool_approval` 返回 `ToolDecision::Pause`，演示 GUI 非阻塞等待人工确认。
- 恢复：`agent.resume()` 读取 Checkpoint，已记录的模型响应不会重复请求。
- 副作用保护：工具只执行一次；已完成的 Checkpoint 拒绝二次恢复。
- 崩溃故障注入：在临时快照中模拟尚未确认结果的 `pending` 工具；必须经宿主通过 `reconcile_tool` 提供经真实证据核实的结果后才能继续。

故障注入仅用于演示，生产环境不能直接修改 Checkpoint JSON，也不得未经核实就返回成功；示例使用 RAII 清理临时文件，不会修改用户项目。

### 2. run_agent_streaming_demo

源码：`examples/run_agent_streaming_demo.cpp`，覆盖功能 5：

- 使用 `httplib::Server` 在 `127.0.0.1` 随机端口提供本机 SSE 测试响应。
- 分别调用内置 OpenAI Responses 和 Chat Completions 模型客户端。
- 通过 `LoopOptions::on_text_delta(phase, delta)` 显示服务端逐段发送的文本增量。
- 验证增量拼接与最终完整回答一致，并检查 Chat 的 usage 统计。
- 不读取模型隐藏推理，也不会把完整回答拆分成伪造的网络流式增量。

### 构建与运行

```bash
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build --target run_agent_checkpoint_demo run_agent_streaming_demo --config Debug

# Linux
./build/run_agent_checkpoint_demo
./build/run_agent_streaming_demo

# Windows (PowerShell)
# .\build\Debug\run_agent_checkpoint_demo.exe
# .\build\Debug\run_agent_streaming_demo.exe

# Linux: 单独运行两个示例的 CTest
ctest --test-dir build -R 'myagent_agent_(checkpoint|streaming)_demo' --output-on-failure
# Windows: 额外指定 -C Debug
```

`MYAGENT_BUILD_EXAMPLES=OFF` 时，这两个示例和对应的 CTest 不会构建或注册。


## Agent 高级扩展：多模型路由、重试限流、MCP 缓存、多级委派、Trace 和费用

这六项功能默认关闭或不改变旧的行为。**仅配置所需项**，不需要迁移现有 Demo。

### 8. 按阶段切换模型、端点与密钥

在 `myagent::Config::phase_routes` 中按阶段名填入完整路由，例如 `plan`、
`execute`、`synthesize`、`reflect`、`evaluate`、`context_compression`。
每条路由都有独立的 model / base_url / api_key / protocol / append_path；
未匹配的阶段使用默认 Model。实际密钥由程序从安全渠道提供，**不要提交到仓库**。
还可以使用 `agent.set_phase_model("evaluate", custom_model, "review-model")`
为一个阶段注入自定义 C++ Provider。

### 9. 模型 HTTP 退避重试与限流

`Config::retry` 支持 `max_retries_per_request`、`max_total_retries`、
`base_delay_ms`、`max_delay_ms` 和 `requests_per_minute`（0 为关闭）。
只针对 429、500、502、503、504 和暂时性传输异常，使用指数退避与随机抖动；
已发送 SSE 文本增量的流不自动重试。Retry 仅包裹**模型请求**，绝不重放工具调用。
RPM 限流按 Agent 实例共享全部模型端点，不是跨程序全局云端配额。
模型重试与原有多 Agent 的任务级重试是独立的策略。

### 10. MCP 工具 Schema 缓存

`Config::mcp_catalog_ttl_seconds` 默认 0（关闭）。
非零时第一次发现后缓存 `tools/list` 的受限结果；同一实例内下一轮复用，
到期或收到 `notifications/tools/list_changed` 会刷新。
还可通过 `agent.invalidate_mcp_catalog()` 全部失效，或传入某个 MCP prefix
按需失效。每次命中仍重新执行当前 Schema/数量/总大小/权限/名称冲突校验。
对于没有提供变更通知的服务，采用 TTL 与显式失效；不承诺即时一致性。

### 11. 多级 Agent 委派

`AgentSpec::allowed_children` 指定子 Agent 名称白名单。父 Agent 可调用受限工具
`local__agent_delegate(child, task)` 继续委派。使用 `AgentTeam::delegate_tree(root,task,
max_depth,max_model_calls,max_tool_calls)` 启动自主树形委派；`TeamOptions::max_depth`
可设置 1～8 层（旧值 1 默认不变）。
所有实际模型请求、重试尝试和工具执行共享线程安全计数配额，支持白名单与循环检查。
工具由本进程回调驱动，不支持 `strict_process_isolation=true`；若需要强隔离须使用 MCP
stdio 子进程。团队账本包含嵌套子任务的 reported usage。

### 12. Trace / Span / OTEL JSONL

设置 `LoopOptions::on_span` 即可获得模型 Span 的 `traceId`、`spanId`、
状态、开始/结束纳秒时间和模型名；`LoopResult::trace_id` 用于关联任务。
`LoopOptions::otel_jsonl_path` 可离线导出兼容 OTEL 字段的 JSONL，
**不自动上传**，不包含提示词、密钥、工具参数与回答正文。
可由应用自行桥接为 OTLP。需要对产生的追踪文件配置权限和保留周期。

### 13. 模型价格与费用软预算

`Config::model_prices["model-name"] = {input_per_million, output_per_million,
cached_input_per_million}`，价格由宿主自行维护（单位 USD/百万 Token）。
`Config::max_estimated_cost_usd` 非零时，在每次获得 reported usage 后
更新预算；到达阈值阻止下一次模型请求，**不能提前阻止本次账单超过预算**。
未知价格或没有 reported usage 时选择拒绝继续预算模式，而不是估算为零。
`LoopResult::estimated_cost_usd` 与 `estimated_cost_by_model` 用于报告；
缓存输入 Token 是输入 Token 的子集，按配置的缓存价格替代而不是重复收费。

### 构建与运行六个独立 Demo

```bash
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build --config Debug --target run_model_routing_demo run_model_retry_demo run_mcp_catalog_cache_demo run_nested_agents_demo run_agent_trace_demo run_agent_cost_demo

# Linux:
./build/run_model_routing_demo
./build/run_model_retry_demo
./build/run_mcp_catalog_cache_demo
./build/run_nested_agents_demo
./build/run_agent_trace_demo
./build/run_agent_cost_demo

# Windows: .\build\Debug\run_model_routing_demo.exe 等
ctest --test-dir build -R "myagent_(model_routing|model_retry|mcp_catalog_cache|nested_agents|agent_trace|agent_cost)_demo" --output-on-failure
```

每个示例都有独立 `run_xxx_demo()` 入口及详细中文注释。运行时不会使用真实
API Key、外部 MCP 服务或联网模型；所有示例都被注册为 CTest。

严格隔离模式不安装额外的 MCP 通知回调（SDK 认为任意进程内回调不能保证硬截止时间）；此模式下请采用 TTL 或 `invalidate_mcp_catalog()` / `set_mcp_catalog_version(prefix,version)` 显式刷新。

## PR #25：稳定性优化的三个完整离线 Demo

这三个新增的独立可执行程序配合原有 `run_model_retry_demo`、
`run_mcp_catalog_cache_demo` 和 `run_team_checkpoint_demo`，
覆盖新增的重要 API，并在 CTest 中自动运行。所有示例都有
`int run_xxx_demo()` 入口、中文注释、无需命令行参数、真实 API Key 或联网。

| Demo 源码 | 演示场景 |
| --- | --- |
| `examples/run_agent_pinned_context_demo.cpp` | `Config::context.pinned_notes` 将宿主指定的重要事实独立于摘要注入；多轮对话压缩后仍保留；区分估算 Token 和 Responses 的 reported usage |
| `examples/run_agent_skills_refresh_demo.cpp` | 显式加载目录、按需调用 `agent_skill_load`、修改/新增/删除 SKILL.md、调用 `refresh_skills()`、无效 YAML 原子回滚 |
| `examples/run_team_budget_recovery_demo.cpp` | 首选 Agent 输出不完整后交由备用 Agent；失败请求费用不丢；团队软预算中止后从 Checkpoint 续跑；提高预算后只运行尚未完成的验证任务 |

### 构建与运行

```bash
git submodule update --init --recursive
cmake -S . -B build -DMYAGENT_BUILD_CONSOLE=OFF -DMYAGENT_BUILD_EXAMPLES=ON -DMYAGENT_BUILD_TESTS=ON
cmake --build build --config Debug --target run_agent_pinned_context_demo run_agent_skills_refresh_demo run_team_budget_recovery_demo

# Linux / 单配置生成器
./build/run_agent_pinned_context_demo
./build/run_agent_skills_refresh_demo
./build/run_team_budget_recovery_demo

# Windows 多配置生成器：build/Debug/run_agent_pinned_context_demo.exe 等
ctest --test-dir build -C Debug --output-on-failure -R "myagent_(agent_pinned_context|agent_skills_refresh|team_budget_recovery)_demo"
```

**注意：** `pinned_notes` 不是向量记忆，不会自动从历史抽取事实；需要可信宿主显式填写。
`refresh_skills()` 重新加载目录元数据，不会执行 Skill 下的脚本。
团队费用采用模拟的整数价格便于核算，实际费用仍取决于模型返回的 usage
与宿主配置的价格；成本上限是软预算，并不保证本次请求恰好不超额。
Checkpoint 是为崩溃恢复提供的审计日志；对已启动但结果未知的外部副作用，
仍需宿主核实，不能盲目重放。


## 统一对话回调（C++）

`Agent::set_conversation_callback(callback, include_tool_payloads=false)` 可监听普通
`ask()`、`ask_parts()`、`react()`、其他 Loop，以及各个独立的子 Agent。
事件是 JSON，`event` 字段包括 `user_message`、`assistant_delta`、
`assistant_message`、`tool_call`、`tool_result`、`turn_completed`
和 `turn_failed`。事件包含 `agent_id`、`turn_id` 和 `phase`；
Loop 的开始/结束事件还包含 `trace_id`，团队 Worker 额外包含
`task_id`（对应 `TeamTask::id`）。`turn_id` 在同一调用内保持一致；
每次 ask/Loop 都生成新 ID，不写入会话历史。完整回复由
`assistant_message.content` 或 `turn_completed.answer` 获取，
`turn_completed` 还带有请求数、已报告 Token、估算费用和耗时。
当后端支持 SSE 或自定义 Model 实际产生文本片段时，才会收到
`assistant_delta`；普通非流式 Mock 不会伪造增量。

```cpp
agent.set_conversation_callback([](const myagent::Json& event) {
  std::cout << myagent::json::serialize(event) << '\\n';
});
auto answer = agent.ask("你好");
```

若只关心部分事件，可以使用具名的 `ConversationCallbacks` 回调组；
它与上述统一 JSON 事件回调可以同时使用，互不影响：

```cpp
myagent::ConversationCallbacks hooks;
hooks.on_user_message = [](const myagent::Json& e) {
  std::cout << "提交内容: " << myagent::json::serialize(e.at("content")) << '\\n';
};
hooks.on_assistant_delta = [](const myagent::Json& e) {
  std::cout << e.at("delta").as_string() << std::flush;
};
hooks.on_assistant_message = [](const myagent::Json& e) {
  std::cout << "最终回复: " << e.at("content").as_string() << '\\n';
};
hooks.on_tool_call = [](const myagent::Json& e) {
  std::cout << "工具: " << e.at("name").as_string() << '\\n';
};
hooks.on_tool_result = [](const myagent::Json& e) {
  std::cout << "工具完成: " << e.at("call_id").as_string() << '\\n';
};
hooks.on_turn_completed = [](const myagent::Json& e) {
  std::cout << "Token: " << e.at("total_tokens").as_uint64() << '\\n';
};
hooks.on_turn_failed = [](const myagent::Json& e) {
  std::cerr << "执行未成功: " << e.at("error").as_string() << '\\n';
};
// 若要在工具回调中读取完整 arguments/output，显式设置：
// hooks.include_tool_payloads = true;
agent.set_conversation_callbacks(std::move(hooks));
```

工具参数和工具结果默认不放入回调；显式传入 `true` 才会包含
`arguments` / `output`。对于 `ask_image()` 和 `ask_parts()`，
`user_message` 只携带原始文字与媒体类型，不复制图片 Data URL/base64；
原始多媒体内容仍按正常流程发给模型。

多 Agent 推荐先调用
`team.set_conversation_callback(callback)` 或
`team.set_conversation_callbacks(hooks)`，它们会自动给协调器、已经
注册以及之后通过 `add_agent` 注册的 Worker 设置相同监听器。
也可以通过 `team.agent("worker").set_conversation_callback(...)`
或 `team.set_coordinator_conversation_callback(...)` 分别覆盖。
`TeamOptions::worker_loop` 之外，显式委派可使用
`LoopOptions::task_id` 自定义关联 ID。

回调同步执行且不允许在回调里重入同一 Agent；宿主回调抛出的异常会被捕获，
避免影响执行。多 Agent 并行时，同一观察者可能被不同 Worker 线程并发调用，
共享数据要加锁；GUI 应将事件转发至 UI 线程。不要在 callback 中记录
API Key 或未经处理的完整工具输出。断点回放的已完成工具不会重复发工具事件；
审批暂停前的工具意图事件可能在恢复时再次出现，应该使用
`turn_id` / `call_id` 做关联并根据任务恢复策略去重。

离线 Demo 与测试：

```sh
cmake --build build --target run_agent_conversation_callbacks_demo run_team_conversation_callbacks_demo
ctest --test-dir build --output-on-failure -R "myagent_(conversation_callbacks|team_conversation_callbacks)_demo"
```

分别查看 `examples/run_agent_conversation_callbacks_demo.cpp`（输入、
回复、流式、工具、失败、多模态脱敏），以及
`examples/run_team_conversation_callbacks_demo.cpp`（团队广播、任务 ID、
后注册 Worker、协调器自动协作）。


## 智能 Agent 路由（独立 AgentRouter）

- **AgentRouter** 负责对用户任务做一次分类，然后选择一个预先注册的 Agent 执行。它不拆解任务，也不会自动在失败后改派给另一个 Agent。
- 原有 Agent::ask()、Agent::react()、AgentTeam::run() 和 ConversationCallbacks 接口不变，旧 Demo 不需要迁移。
- **run_model_routing_demo 是阶段级模型路由**（Planner/Executor/Reviewer 的 Model、Base URL 和 API Key），**不是**语义 Agent 路由；两者都是有效且互补的示例。
- **run_agent_router_demo** 专门演示语义路由、多 Agent 模型隔离、ask/react、统一回调、非法选择拒绝与单 Agent 免额外模型调用；全部使用 Mock、无需网络。
- 默认只将任务正文、候选 Agent 的**名称与描述**发送到路由模型，不发送各执行者的 API Key、完整历史或工具 Schema；模型返回的 ID 必须与注册名精确一致，否则拒绝路由，**不自动选择替代 Agent**。路由器本身不执行工具。
- Router 与所持有的 Agent 为同步单会话对象，不能在多个线程上共享并发调用。注册时使用 std::shared_ptr<Agent> 管理生命周期。

~~~cpp
#include "myagent/router.hpp"
using namespace myagent;

Config router_cfg; // 路由模型的独立 API Key / base_url / model
router_cfg.api_key = "ROUTER_API_KEY";
router_cfg.base_url = "https://your-model-gateway.example.com";
router_cfg.model = "router-model";

Config coder_cfg = router_cfg;
coder_cfg.agent_id = "coder";
coder_cfg.model = "coding-model";
Config reviewer_cfg = router_cfg;
reviewer_cfg.agent_id = "reviewer";
reviewer_cfg.model = "review-model";
// 生产应用可为每个子 Agent 配置完全不同的 base_url、api_key、MCP、Skills。

auto coder = std::make_shared<Agent>(coder_cfg);
auto reviewer = std::make_shared<Agent>(reviewer_cfg);
AgentRouter router(router_cfg);
router.add_agent("coder", "实现和修改 C++ 代码", coder);
router.add_agent("reviewer", "检查代码质量、安全问题和测试覆盖", reviewer);

// Router 发出 route_selected/route_failed 事件，并转发 Worker 对话回调。
router.set_conversation_callback([](const Json& event) {
    // event["event"] 可是 route_selected、user_message、assistant_delta 等。
});

auto response = router.ask("审查这段 C++ 代码");
std::cout << response.selected_agent << ": " << response.answer << '\n';

// 原生 Loop 状态仍完整返回，失败时应检查 status/error。
auto execution = router.react("实现一个 C++ 函数");
if (execution.loop_result && execution.loop_result->status == LoopStatus::Completed)
    std::cout << execution.answer << '\n';

// 旧代码仍然正常。
std::cout << coder->ask("你好") << '\n';
~~~

路由器注册两个及以上候选 Agent 时，route() 会调用一次独立模型（额外消耗 Token），返回 RouteDecision{selected_agent, method, routing_usage}。只有一个候选 Agent 时直接选择它，不进行额外模型调用。ask() 返回 RouterResult，react() 返回同样结构并额外附带 loop_result。

回调属于旁路观察功能：Router 的 set_conversation_callback / set_conversation_callbacks 会覆盖子 Agent 已有的同类型监听器（可使用 Router 级统一订阅），不会修改工具审批配置。并行调度依然交给 AgentTeam，而不是 AgentRouter。

~~~sh
cmake --build build --target run_agent_router_demo myagent_router_tests
ctest --test-dir build -R 'myagent_(router|router_demo)$' --output-on-failure
~~~


## P1 新增两种 Loop：ReWOO / Execute-Verify-Repair（中文源码注释）

现有 4 种 Loop：react、plan_and_execute、reflect、evaluate_and_optimize 保持兼容。新增 LoopMode::ReWOO 和 LoopMode::ExecuteVerifyRepair（尾部追加，不改变旧枚举值），并为多媒体 LoopInput 和文本任务分别提供同名入口。新 Loop 仍受原有模型/工具调用上限、超时/取消、MCP ToolPolicy、人工审批、对话事件、Token/费用统计和 Checkpoint 控制。

### 1. ReWOO：一次计划 → C++ 直接调用工具 → 最终汇总

- **用途**：适用于多个工具调用参数可以提前确定的任务，例如查询文件、收集项目结构、检查多个独立资源。与 ReAct 的工具观察后再决定下一步不同，ReWOO 在工具执行期间不需要额外的模型推理。
- **规划**：Planner（只允许文本，无工具权限）输出 JSON：{"steps":[{"id":"read","tool":"local__read_file","arguments":{"path":"src/main.cpp"},"depends_on":[]}]}。计划提交前由 C++ 完整校验 ID 唯一性、已注册 MCP 工具、JSON 参数、前序依赖和预算。
- **执行**：依次调用 MCP；每次执行都经过 ToolPolicy、人工 on_tool_approval、全局 allow_tool_execution、接收限额、Checkpoint 及非幂等重复意图拦截。工具失败**立即停止**，不会继续下一步并声称成功。
- **汇总**：Solver 在无工具权限阶段读取真实工具观察结果并回答。默认不会在 conversation callback 中暴露工具参数/结果，只有显式开启 include_tool_payloads 才会发出。
- **限制**：不支持在运行时修改既定计划，也不支持把前一个工具输出动态插入后一个工具参数。单条观察有上下文大小上限；如需要根据工具观察重新规划，应使用 ReAct。

~~~cpp
#include "myagent/agent.hpp"
using namespace myagent;
Agent agent(config);
// 事先注册 read_file、inspect 等已审计工具（参见 run_rewoo_demo.cpp）。
LoopOptions opts;
opts.max_plan_steps = 4;
opts.max_model_calls = 4; // 至少 2 次：Planner + Solver。
opts.max_tool_calls = 4;
opts.on_tool_approval = [](const ToolApprovalRequest& request) {
    // 用户/宿主有权限时返回 Approve；不能由模型直接批准。
    return ToolDecision::Approve;
};
const auto result = agent.rewoo("读取源码并汇总测试目标", opts);
if (result.status == LoopStatus::Completed)
    std::cout << result.answer << '\n';
~~~

### 2. Execute-Verify-Repair：执行 → 宿主验证 → 按错误修复 → 再验证

- **用途**：面向 C++ 编译、单元测试、代码检查等需要**客观证据**的任务。
- **执行**：Executor 使用授权 MCP 工具完成任务，返回结构化 JSON {"status":"completed","summary":"..."}；它的状态不是成功凭据。
- **验证**：必须提供 LoopOptions::verify_execution(execution, tool_results, attempt)。宿主运行真正的构建或测试并返回 ExecutionVerification{passed, reason, evidence}。缺少验证器会在首次模型请求前失败。
- **修复**：验证失败时将错误摘要和证据反馈给模型，执行新一轮修复，并重新调用宿主验证器。只有执行器状态 completed 且宿主 passed=true 才结束。最多执行 max_repairs+1 轮，仍受 max_iterations/max_model_calls/max_tool_calls 限制。
- **安全**：不要让不可信模型任意选择 verifier 命令；真实编译、测试由可信宿主实现并设置独立资源限额。evidence 可能包含内部路径、日志和机密，宿主需要裁剪和脱敏。失败不自动重放相同非幂等工具；如果工具执行结果未知，恢复前需要人工核实。

~~~cpp
#include "myagent/agent.hpp"
using namespace myagent;
Agent agent(config);
LoopOptions opts;
opts.max_repairs = 2;
opts.max_iterations = 3;
opts.verify_execution = [](const Json& execution,
                           const Json& tool_results,
                           unsigned attempt) -> ExecutionVerification {
    // 在可信宿主运行真正的编译和测试；本例仅展示结果的返回形式。
    const bool passed = run_build_and_tests_in_sandbox(); // 由宿主自行实现
    return {passed, passed ? "build/test passed" : "build/test failed",
            mymcp::json::object{{"attempt",attempt},{"passed",passed}}};
};
const auto result = agent.execute_verify_repair("实现功能并确保测试通过", opts);
if (result.status == LoopStatus::Completed)
    std::cout << result.answer << '\n';
else
    std::cerr << result.error << '\n';
~~~

上面的 run_build_and_tests_in_sandbox() 是宿主接口占位符，不是 myAgent 内置函数。可运行的 run_execute_verify_repair_demo.cpp 使用离线模拟编译器展示先失败、再根据退出码修复、再次成功的完整过程，无外部服务依赖。

### Demo、CTest 与团队 Worker

~~~sh
cmake --build build --target run_rewoo_demo run_execute_verify_repair_demo myagent_p1_loop_modes_tests
ctest --test-dir build -R "myagent_(rewoo_demo|execute_verify_repair_demo|p1_loop_modes)$" --output-on-failure
~~~

- run_rewoo_demo：双工具依赖、Planner/Solver 调用次数、工具审批和非法计划拒绝。
- run_execute_verify_repair_demo：写代码、宿主模拟编译失败、模型修复、再次验证通过，以及无验证器时拒绝运行。
- myagent_p1_loop_modes_tests：Checkpoint 暂停恢复、非法依赖、重复非幂等调用拦截，以及宿主验证失败时的修复次数上限。
- AgentTeam 中可设置 TeamTask.mode 为 LoopMode::ReWOO 或 LoopMode::ExecuteVerifyRepair。后一种的 Worker 必须通过 TeamOptions::worker_loop.verify_execution 提供宿主验证器，否则按失败处理。

**Checkpoint 注意**：恢复需使用相同的 Agent 配置、工具目录和可信批准/核实回调。ReWOO 回归测试覆盖暂停前工具未执行、恢复后工具只执行一次。验证器通常应该是只读检查；恢复时可能需要重新执行验证，不要让它产生不可重放的副作用。
