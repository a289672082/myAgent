// OpenAI + MCP + Skills + 多轮 / 图片 / 音频 Demo：不使用命令行参数。
// Requires OPENAI_API_KEY. Optional: OPENAI_MODEL, MYAGENT_MCP_URL,
// MYAGENT_MCP_STDIO, MYAGENT_IMAGE, MYAGENT_AUDIO, MYAGENT_SPEECH_OUTPUT.
#include "myagent/agent.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int run_agent_demo() {
  try {
    if (!std::getenv("OPENAI_API_KEY")) {
      std::cerr << "Set OPENAI_API_KEY before running this demo.\n";
      return 2;
    }
    myagent::Config config;
    config.instructions = "You are a helpful coding assistant. Prefer concise answers.";
    if (const char* model = std::getenv("OPENAI_MODEL")) config.model = model;

    myagent::Agent agent(config);
    // 直接把 C++ Lambda 注册为 MCP 工具；自动产生 JSON Schema。
    agent.tool("add", "Add two integers",
               [](int a, int b) { return a + b; },
               mymcp::arg<int>("a", "First integer"),
               mymcp::arg<int>("b", "Second integer"));

    // Agent 自动创建 MCP 客户端并管理生命周期，不需要 shared_ptr。
    if (const char* url = std::getenv("MYAGENT_MCP_URL"))
      agent.add_mcp_http("external", url);
    // stdio 会启动指定程序，无需 shell 字符串。
    if (const char* executable = std::getenv("MYAGENT_MCP_STDIO"))
      agent.add_mcp_stdio("stdio", executable);
    // OAuth：宿主程序先完成授权，再传入已登录的 Session：
    // agent.add_mcp_oauth("oauth", url, authenticated_session);
    if (std::filesystem::is_directory("skills")) {
      agent.load_skills("skills");
      agent.use_skill("brief-summary");
    }

    // 第一次请求可以调用 add 工具；第二次使用保留的历史进行多轮追问。
    std::cout << agent.ask("What is 12 + 30? Please use a tool if available.") << "\n";
    std::cout << agent.ask("Repeat the numerical result in Chinese.") << "\n";

    // 图片：文件会转为 input_image Data URL，支持 png/jpeg/webp/gif。
    if (const char* image = std::getenv("MYAGENT_IMAGE"))
      std::cout << agent.ask_image("What is in this picture?", image) << "\n";

    // 音频输入先调用 transcription，再加入本次会话。
    if (const char* audio = std::getenv("MYAGENT_AUDIO"))
      std::cout << agent.ask_audio(audio, "Summarize the audio:") << "\n";

    // 音频输出单独调用 speech API，生成 mp3 文件。
    if (const char* output = std::getenv("MYAGENT_SPEECH_OUTPUT"))
      agent.speak("The C++ agent demonstration completed.", output);

    agent.save_history("agent_history.json");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_agent_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_agent_demo(); }
