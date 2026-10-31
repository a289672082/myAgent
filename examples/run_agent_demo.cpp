// myAgent 综合示例：所有参数均在源码中显式填写，无需命令行参数或环境变量。
// 注意：API Key 是示意字符串，真实调用前请替换。不要将真实密钥提交到公开仓库。
#include "myagent/agent.hpp"
#include <filesystem>
#include <iostream>
#include <string>

int run_agent_demo() {
  try {
    // 1. 大模型配置：直接传入 baseUrl / apiKey / 模型名称。
    // 模式 1（默认）：自动追加 /v1/responses。
    const std::string modelBaseUrl = "https://api.openai.com";
    const bool appendResponsesPath = true;
    // 模式 2：使用完整 URL，不自动拼接路径：
    // const std::string modelBaseUrl = "https://example.com/custom/responses";
    // const bool appendResponsesPath = false;
    const std::string modelApiKey = "sk-example-replace-with-your-api-key"; // 非真实密钥
    const std::string modelName = "gpt-4.1-mini";

    myagent::Config config;
    config.base_url = modelBaseUrl;
    config.append_responses_path = appendResponsesPath;
    config.api_key = modelApiKey;
    config.model = modelName;
    config.instructions = "You are a helpful coding assistant. Prefer concise answers.";
    myagent::Agent agent(config);

    // 2. Agent 内直接定义 MCP 工具：自动生成参数 JSON Schema。
    agent.tool("add", "Add two integers",
               [](int a, int b) { return a + b; },
               mymcp::arg<int>("a", "First integer"),
               mymcp::arg<int>("b", "Second integer"));

    // 3. 外部 MCP 服务配置。未启动对应服务时将开关保持为 false。
    const bool enableHttpMcp = false;
    const std::string mcpHttpUrl = "http://127.0.0.1:3000/mcp";
    if (enableHttpMcp)
      agent.add_mcp_http("external", mcpHttpUrl);

    const bool enableStdioMcp = false;
    const std::string mcpServerExecutable = "./mcp_server"; // Windows: ./mcp_server.exe
    if (enableStdioMcp)
      agent.add_mcp_stdio("stdio", mcpServerExecutable);

    // OAuth MCP：完成浏览器授权后，传入已认证的 OAuth Session：
    // agent.add_mcp_oauth("private", "https://example.com/mcp", authenticatedSession);

    // 4. Skill 自动发现 / 选择：无需手动调用 load_skills() 或 use_skill()。
    // Agent 构造时会搜索 ./skills、.agents/skills、.claude/skills 和用户级目录。
    // 初始只把名称/描述发送给模型；模型决定是否按需读取 SKILL.md。
    // 如需强制指定，可保留使用：agent.use_skill("brief-summary");

    // 5. 多轮对话：第二次请求复用第一轮的会话历史。
    std::cout << agent.ask("What is 12 + 30? Please use a tool if available.") << "\n";
    std::cout << agent.ask("Repeat the numerical result in Chinese.") << "\n";

    // 6. 图片：文件内容自动转成 input_image Data URL。
    const bool enableImageExample = false;
    const std::filesystem::path exampleImagePath = "./assets/example.png";
    if (enableImageExample)
      std::cout << agent.ask_image("What is in this picture?", exampleImagePath) << "\n";

    // 7. 音频输入：先转写为文字，再加入 Agent 会话。
    const bool enableAudioInputExample = false;
    const std::filesystem::path exampleAudioPath = "./assets/example.wav";
    if (enableAudioInputExample)
      std::cout << agent.ask_audio(exampleAudioPath, "Summarize the audio:") << "\n";

    // 8. 语音合成：将文字输出为 MP3 文件。
    const bool enableAudioOutputExample = false;
    const std::filesystem::path synthesizedAudioPath = "./generated_speech.mp3";
    if (enableAudioOutputExample)
      agent.speak("The C++ agent demonstration completed.", synthesizedAudioPath);

    // 9. 保存多轮对话历史。请注意历史可能含有敏感对话内容。
    const std::filesystem::path historyPath = "./agent_history.json";
    agent.save_history(historyPath);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_agent_demo failed: " << e.what() << '\n';
    return 1;
  }
}

int main() { return run_agent_demo(); }
