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
