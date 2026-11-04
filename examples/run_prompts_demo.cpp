// 提示词示例：注册可传参的 Prompt，并提供参数输入补全。
// No command line parameters: prompts with arguments and auto-completion.
#include "demo_common.hpp"
#include <string>

int run_prompts_demo() {
  return demo::execute("run_prompts_demo", [] {
    mymcp::Server server("prompts-demo");
    // 注册 review 提示词，声明必填 subject 参数。
    server.prompt_full("review", "Ask for a code review",
      [](const mymcp::Json& params) -> mymcp::Json {
        // 读取请求参数，构造包含 user 消息的 Prompt 结果。
        std::string subject = params.at("subject").as_string();
        return mymcp::json::object{{"messages", mymcp::json::array{mymcp::json::object{
          {"role", "user"}, {"content", mymcp::json::object{
            {"type", "text"}, {"text", "Review file " + subject}}}}}}};
      }, mymcp::json::array{mymcp::json::object{
           {"name", "subject"}, {"required", true}, {"description", "File to review"}}});
    // 注册自动补全器，返回主文件名等候选项。
    server.completion([](const mymcp::Json&) -> mymcp::Json {
      return mymcp::json::object{{"completion", mymcp::json::object{
        {"values", mymcp::json::array{"main.cpp", "model.cpp"}}, {"hasMore", false}}}};
    });
    auto client = demo::in_process(server);
    // 客户端先查询可用提示词目录。
    demo::print("prompt catalog", client.prompts());
    // 传入 subject=main.cpp，获取生成的消息。
    demo::print("prompt", client.get_prompt("review", mymcp::json::object{{"subject", "main.cpp"}}));
    // 尝试对 subject 的输入前缀 m 进行补全。
    demo::print("completion", client.complete(mymcp::json::object{{"type", "ref/prompt"}, {"name", "review"}},
                                              "subject", "m"));
  });
}
int main() { return run_prompts_demo(); }
