# myAgent API / 设计文档

## API

- myagent::Config：model、api_key、base_url、instructions、max_tool_rounds、max_history_items
- myagent::Model：模型抽象接口，可注入 Mock，避免测试时调用付费 API
- myagent::OpenAIResponses：调用 OpenAI Responses 和音频转写/合成 API
- myagent::Agent：ask、ask_parts、ask_image、ask_audio、speak、add_mcp、tool、load_skills、use_skill、save_history、load_history、clear_history。默认自动发现并按需调用 Skills。
- myagent::SkillLibrary：加载指定目录下的 SKILL.md（name/description 基础 frontmatter），按名称选取

## 模型地址 / API Key 配置

OpenAIResponses 仅使用应用显式传入的 `Config::base_url` 和 `Config::api_key`，不再读取 OPENAI_API_KEY 等环境变量。
两者都为必填，创建真实 OpenAIResponses 后端时缺失会立即抛出 `std::invalid_argument`；base_url 必须以 http:// 或 https:// 开头。
对于注入自定义 Mock Model 的单元测试，可不设置真实模型连接信息。

~~~cpp
const std::string modelBaseUrl = "https://api.openai.com";
const std::string modelApiKey = "sk-example-replace-with-your-api-key"; // 占位符
myagent::Config config;
config.base_url = modelBaseUrl;
config.append_responses_path = true; // 默认添加 /v1/responses
// 完整 Responses URL 时设置为 false，不进行任何路径拼接：
// config.base_url = "https://proxy.example.com/custom/responses";
// config.append_responses_path = false;
config.api_key = modelApiKey;
config.model = "gpt-4.1-mini";
myagent::Agent agent(config);
~~~

`append_responses_path=true`（默认）在基础 URL 末尾补 `/v1/responses`，已有该后缀时不会重复补全；设置为 `false` 时使用整个 `base_url` 作为 Responses HTTP POST 目标，保留查询参数。
这是 Responses 请求 URL 的控制开关，不改变独立的音频 API：转写/合成仍向同域名的 `/v1/audio/...` 发送请求。

真实密钥可由调用方自己的配置或安全存储获取后传入，**不要把真实 API Key 提交到仓库**。所有 Demo 的连接地址、路径和选项都以源码局部变量表示，不需要环境变量。

## Skill 自动发现与渐进式披露

- 默认自动扫描项目 `.agents/skills/`、`.claude/skills/`、当前 `skills/` 及用户的 `~/.agents/skills/`、`~/.claude/skills/`（`Config::auto_discover_skills` 可以关闭）。
- 发现阶段使用本地固定的 yaml-cpp 0.9.0 解析标准 YAML frontmatter，校验 name/description/license/compatibility/metadata/allowed-tools 类型及长度与目录名；`Skill.content` 在目录阶段留空，完整正文按需从 SKILL.md 读取。
- 模型收到 Skill 元数据而不是正文；可调用内部工具 `agent_skill_load(name)` 获取完整指令，必要时再调用 `agent_skill_read_file(name,path)` 读取选中 Skill 的 references/scripts/assets 文本材料。
- 自动 Skill 激活与辅助文件读取权限是本轮调用范围，避免无关任务之间持续注入。显式 `use_skill(name)` 则在该 Agent 会话内持续生效。
- 支持 `Config::auto_skill_selection = false` 只用显式激活机制；`clear_history()` 保留目录注册和手动激活状态。
- 遵循 `disable-model-invocation: true`（Claude）和 `agents/openai.yaml` 的 `policy.allow_implicit_invocation: false`（Codex）禁用自动激活，仍允许显式 use_skill；忽略不认识的扩展 YAML 字段，不提供完整 Claude/Codex 执行语义。

## P0 Agent Skills YAML 专项示例

- 独立源文件：`examples/run_skill_yaml_demo.cpp`，入口 `run_skill_yaml_demo()`，单独的可执行目标 `run_skill_yaml_demo`，不依赖 `run_agent_demo()`。
- 示例资源：`examples/skill_yaml_demo/skills` 中的多行 YAML、完整元数据、Codex/Claude 禁用自动激活策略和参考文件；`invalid-skills` 中有预期失败的 YAML 文件。
- 离线 Mock Model 演示 `agent_skill_load` 与 `agent_skill_read_file` 的往返结果、手动 `use_skill()`、`clear_history()`。**不需要模型凭据、命令行参数或任何网络请求**。
- `MYAGENT_SKILL_YAML_DEMO_DIR` 在 CMake 构建时定位示例资源根目录，避免依赖执行时的 cwd；`ctest` 中注册 `myagent_skill_yaml_demo`。
- `allowed-tools` 仅作元数据展示；`scripts/check.py` 不会运行。

## ContextManager / AgentTeam （P0/P1/P2）

- `Config::context` / `ContextManager`：自动或手动压缩旧的完整用户轮次，摘要替换后保留最近完整轮次，摘要失败不修改历史；触发预算是近似 Token 数，不视作真实 usage。`Agent::compress_context()` / `context_stats()` 提供手动 API 与统计。
- 压缩逻辑放在 `run_stage()` 请求边界（所有 Loop 与 `ask()` 共用），摘要请求本身计入 `last_usage`/`total_usage`，Loop 计入 `usage_by_phase["context_compression"]`。没有可压缩的完整旧轮次时不破坏工具调用结果。
- `AgentTeam` / `AgentSpec`：每个子 Agent 独立 Config（base_url/api_key/model），独立 MCP、Skills、历史和用量，不向协调器模型显示其他 Agent 的密钥或 URL；仍要求 Responses API 兼容。
- 子 Agent 的 Skill/MCP 在 `team.add_agent(spec)` 后，通过 `team.agent(name)` 注册；`config.auto_discover_skills` 控制构造时自动搜索，`config.auto_skill_selection` 控制是否提供 `agent_skill_load`，`load_skills()` 明确限制目录，`use_skill()` 可手动固定指令。
- 示例 `run_agent_delegation_demo.cpp` 通过真实 Skill fixture、离线 MCP Server/Client 演示 Coder 自动按需加载、Reviewer 手动启用、第三个 Agent 自动发现；检查各 Agent 的工具列表和调用隔离，附中文逐步说明。

- `delegate` 显式委派任务；`collaborate` 基于任务 DAG（只依赖已声明的前序任务）运行；`run` 先由协调器生成严格 JSON 任务图再执行、完成后总结。
- `TeamOptions` 包含任务数量限制、并行数量、深度=1、模型/工具调用预算、软 Token 上限和取消回调；子任务上下文只附必要的上游摘要，避免复制主 Agent 完整历史。
- `TaskContext` 支持传目标、上下文、产物引用与约束；`DelegationResult::details` 包含 worker Loop 的 plan/evaluations；`TeamOptions::validate_task` 可检查真实证据并拒绝未通过的工作，阻断依赖任务。
- 并发由 `std::async` 执行，不允许同一个非线程安全 Agent 在同一波次处理两个任务；子 Agent 的 workspace 路径不能重叠。
- 工作区内建 `workspace_read`、`workspace_list`、可选 `workspace_write` 仅约束这些本地工具的路径和大小，其他 MCP/外部命令及目录竞态无法由此强制沙箱隔离。
- 四个独立中文 Demo，离线 `test_context` / `test_multi_agent` 覆盖基本模型、预算、并行、依赖与隔离；所有依赖沿用本地 third_party 构建。
## Token 消耗统计

- `include/myagent/token_usage.hpp` / `src/token_usage.cpp`：解析 Responses API 的 input/output/total、cached input 与 reasoning output。后二者属于前两者的子集，不能重复求和。
- `Agent::run_stage()` 每次收到模型响应后马上累计用量（先于状态检查），所以 MCP 工具轮次、四种 Loop 以及最终失败的请求只要返回 usage 都可以统计。
- `Agent::last_usage()` 为最近一次 ask 或 Loop 的统计，`Agent::total_usage()` 为构造以来的累计用量；`reset_usage()` 明确清零，`clear_history()` 不清零。
- `LoopResult::usage`、`usage_by_phase`、`usage_by_model` 分别记录本次循环总量、按执行阶段和模型名称的用量；`trace` 和 `on_event` 会发出不含原始请求的 `model_usage` 事件。
- 缺少 `usage` 记为 `unreported_calls`，不估算；失败/超时无响应时，无法核算的调用也不能假装为零成本。
- 音频转写/合成是不同的 API，当前未纳入 Responses Token 统计；没有内置动态价格估算。独立 Demo `run_token_usage_demo()` 和 CTest `myagent_token_usage` 完全离线。
## 统一 Agent 循环

- include/myagent/loop.hpp 定义 LoopInput(text/images/audio)、LoopOptions、LoopResult 与四种策略类型；src/loop_engine.cpp 负责跨阶段调度。
- Agent::run_stage 是原有 ask() 与四种 Loop 共用的唯一模型/MCP/Skill 工具执行器；支持工具隔离、全局调用计数、模型名称覆盖与取消回调。
- Plan-and-Execute 使用可验证的 JSON steps，阻塞时可以有限重新规划；Reflection 使用 needs_revision/feedback；Evaluator-Optimizer 使用 score/passed/feedback/issues 和用户定义阈值。
- 规划、反思、评估、优化等阶段不提供工具；执行/草稿阶段可以使用已有 MCP/Skill，任何外部工具副作用无法自动回滚。
- 图片继续使用现有 Data URL；音频先转写文本，语音输出通过现有 speak()；多模态的后端限制依然存在。
- Loop 维护隔离的 working conversation，只有成功的最终结果提交普通 Agent 历史。取消仅在阶段/调用边界生效，不会中断 HTTP 调用。
- 四个独立离线 Demo 与 tests/test_loops.cpp 注册进 CTest；Demo 明确写出示例模型参数，不依赖环境变量和命令行参数。

## Plan-and-Execute P1 行为与验收接口

- Planner 创建可验证的顺序计划，步骤 ID 全局唯一、可以声明 depends_on（只允许指向更早的新步骤或已经完成的步骤）和 success_criteria。
- Executor 每一步都返回 JSON：status=completed/failed/blocked、summary，以及可选 evidence。任意纯文本不再默认为成功，返回格式无效则失败。
- src/plan_manager.hpp / src/plan_manager.cpp 负责结构校验、计划状态、依赖、尝试次数和重规划；已完成的步骤只保留历史，过时 pending/ready 改为 superseded，后续不再执行。
- 可使用 LoopOptions::validate_step(step,execution,tool_results) 给出 StepValidation(passed/reason/evidence)，让应用根据真实编译退出码、单测结果、文件状态等完成客观验收；不配置时 completed 只是 unverified。
- 工具返回 isError=true 会覆盖模型自述的 completed，步骤被标记 failed。
- 默认 max_step_retries=0 且 retry_steps_with_tools=false；即使设置重试次数，有工具操作时也不会自动执行第二遍，除非宿主明确允许。
- ReAct 的观察驱动决策、Reflection 的自检修订、Evaluator-Optimizer 的独立评分优化继续复用 run_stage()；四类 Loop 都附完整中文执行流程注释。

## MCP 设计

myMcpTools 作为独立 SDK 通过 third_party/myMcpTools Git submodule 使用。
项目直接编译 SDK 的现有 mcp.cpp、schema.cpp、oauth.cpp 和 oauth_client.cpp，复用 jsoncons、cpp-httplib；不复制或维护第二套 MCP 协议实现。
普通调用方使用 Agent 快捷接口，Client 生命周期由 Agent 管理：

- add_mcp_http(prefix, url, bearer_token = {})：复用 Client::http，HTTP Server 通过 HTTPS 验证证书。
- add_mcp_stdio(prefix, executable, args = {})：复用 Client::process，启动子进程并传递 args（不使用 shell）。
- add_mcp_oauth(prefix, url, session)：复用 Client::http_oauth，授权/换取 Token 由应用事先通过 mymcp::oauth::Session 完成。
- add_mcp(prefix, shared_ptr<Client>)：保留给需自定义 Client 行为、通知或扩展的高级用户。

连接名称（prefix）在创建客户端前校验，避免重复 stdio 进程启动。HTTP / OAuth 注册时不做网络请求，首次 ask() 会执行工具发现。

底层通过 mymcp::Client::http、Client::process、Client::http_oauth 接入外部 MCP；
Agent 自定义 tool 注册到底层 mymcp::Server 并通过内部 Client 调用，沿用相同 JSON Schema 参数校验。
OpenAI function names 是 prefix__native_name，并通过路由表映射回原 MCP 工具名。

## 会话语义

- 请求携带完整 Responses 输入项目（工具调用、工具输出、普通消息），store=false。
- 成功的模型最终回答才提交新历史；失败不会提交半轮上下文，但先前已执行的工具副作用无法回滚。
- max_tool_rounds 防止无限自动调用；max_history_items 限制输入项数，超限报错而非静默删除。
- save_history 保存对话项目，没有 API Key，但内容可能敏感，需要独立保密。
- 不承诺 Agent 类多线程安全、无需锁定时序复用。

## 媒体与 Skills

- ask_image：读本地图片（png/jpg/jpeg/webp/gif），转为 input_image data URL。
- ask_audio：通过 Transcriptions API 转写后作为文本消息执行 ask，并非实时音频、声学理解或直接语音模型推理。
- speak：调用音频语音合成 API 并写入 MP3 文件。
- Skill 不是代码执行器。手动激活内容加到 instructions；自动激活内容通过受控工具结果提供给模型。加载的内容可能包含不可信指令，应进行信任审查。

## 生产化差距

未实现 SSE/Realtime 流式支持、自动 API 重试、工具审核界面、强权限隔离、上下文智能剪裁、异步取消和更完整的 YAML/Skill 规范解析。联网 OpenAI 集成需用户凭据，离线 CI 仅验证请求、工具调用和会话编排。
