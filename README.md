# myAgent — C++20 OpenAI Agent 框架

独立 C++20 Agent 项目，复用 [myMcpTools](https://github.com/a289672082/myMcpTools) 的 MCP 客户端、服务端、JSON、HTTP、OAuth 和其第三方源码。myMcpTools 使用固定提交的 Git submodule；Agent 源码全部在本仓库。

## 功能

- OpenAI Responses API 多轮对话、函数调用与结果回传
- MCP 客户端：stdIO / Streamable HTTP / OAuth；工具自动发现与执行
- Agent 内直接通过 Lambda 或 JSON Schema 定义 MCP 工具
- 加载 SKILL.md 并按名称启用（支持简单 YAML frontmatter）
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
#include <memory>

int main() {
  myagent::Config config;
  config.model = "gpt-4.1-mini";
  myagent::Agent agent(config);

  agent.tool("add", "计算两个整数的和", [](int a, int b) { return a + b; },
             mymcp::arg<int>("a"), mymcp::arg<int>("b"));

  auto remote = std::make_shared<mymcp::Client>(
      mymcp::Client::http("http://127.0.0.1:3000/mcp"));
  agent.add_mcp("remote", remote);
  agent.load_skills("./skills");
  agent.use_skill("brief-summary");

  std::cout << agent.ask("计算 20 + 22") << "\n";
  std::cout << agent.ask("上一个答案是多少？") << "\n";

  // agent.ask_image("请描述图片", "example.png");
  // agent.ask_audio("example.wav", "总结音频");
  // agent.speak("你好", "result.mp3");
  agent.save_history("agent_history.json");
}
~~~

## Demo 运行

先设置 OPENAI_API_KEY 环境变量；直接运行 build/run_agent_demo（Windows：build/Debug/run_agent_demo.exe），无命令行参数。可选环境变量 OPENAI_MODEL、MYAGENT_MCP_URL、MYAGENT_IMAGE、MYAGENT_AUDIO、MYAGENT_SPEECH_OUTPUT；从仓库根目录运行才能自动找到示例 Skill。

核心头文件：include/myagent/agent.hpp。
核心实现：src/agent.cpp。
测试：tests/test_agent.cpp。
示例：examples/run_agent_demo.cpp。

## 边界和安全

当前是同步的 Responses + MCP Agent，不支持流式 SSE 对话、Realtime 双向语音、直接分析音频声学特征、自动工具权限审批和上下文压缩。
API Key 默认从 OPENAI_API_KEY 读取；历史文件可能包含敏感会话内容，勿提交到 Git。
技能文件作为提示词加载，不执行脚本；请核查 Skills 和远程 MCP 服务的来源。会改变文件或运行命令的工具应由调用方额外做权限控制与隔离。
媒体上限为 20 MiB，工具返回上限为 256 KiB；不包含实际 OpenAI API 联网集成测试。
