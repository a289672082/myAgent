# myAgent API / 设计文档

## API

- myagent::Config：model、api_key、base_url、instructions、max_tool_rounds、max_history_items
- myagent::Model：模型抽象接口，可注入 Mock，避免测试时调用付费 API
- myagent::OpenAIResponses：调用 OpenAI Responses 和音频转写/合成 API
- myagent::Agent：ask、ask_parts、ask_image、ask_audio、speak、add_mcp、tool、load_skills、use_skill、save_history、load_history、clear_history
- myagent::SkillLibrary：加载指定目录下的 SKILL.md（name/description 基础 frontmatter），按名称选取

## MCP 设计

myMcpTools 作为独立 SDK 通过 third_party/myMcpTools Git submodule 使用。
项目直接编译 SDK 的现有 mcp.cpp、schema.cpp、oauth.cpp 和 oauth_client.cpp，复用 jsoncons、cpp-httplib；不复制或维护第二套 MCP 协议实现。
通过 mymcp::Client::http、Client::process、Client::http_oauth 接入外部 MCP；
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
- Skill 不是代码执行器；仅把加载的 SKILL.md 内容追加到 instructions。加载的内容可能包含不可信指令，应进行信任审查。

## 生产化差距

未实现 SSE/Realtime 流式支持、自动 API 重试、工具审核界面、强权限隔离、上下文智能剪裁、异步取消和更完整的 YAML/Skill 规范解析。联网 OpenAI 集成需用户凭据，离线 CI 仅验证请求、工具调用和会话编排。
