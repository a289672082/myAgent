// MRTR 多轮交互示例：缺少确认时返回 input_required，收到回答后恢复工具调用。
// No command line parameters: safe mock multi-round tool elicitation.
#include "demo_common.hpp"
#include <stdexcept>

int run_mrtr_demo() {
  return demo::execute("run_mrtr_demo", [] {
    mymcp::Server server("mrtr-demo");
    // 上下文工具能接收 inputResponses 和 requestState 等多轮请求数据。
    server.add_tool_context("confirm_demo", "Ask to confirm a non-destructive demo",
      mymcp::json::object{{"type", "object"}}, [](const mymcp::Json& params) -> mymcp::Json {
        // 首次调用还没有回答，返回 input_required 和待填写的表单。
        if (!params.contains("inputResponses"))
          return mymcp::json::object{{"resultType", "input_required"},
            // requestState 用于关联后续请求；正式服务端应校验和保护其中的状态。
            {"requestState", "demo-v1"}, {"inputRequests", mymcp::json::object{
              {"confirmation", mymcp::json::object{{"method", "elicitation/create"},
                {"params", mymcp::json::object{{"mode", "form"},
                  {"message", "Allow this harmless example to finish?"},
                  {"requestedSchema", mymcp::json::object{{"type", "object"},
                    {"properties", mymcp::json::object{{"approved", mymcp::json::object{{"type", "boolean"}}}}},
                    {"required", mymcp::json::array{"approved"}}}}}}}}}}};
        // 第二轮调用读取用户的 approved 布尔回答。
        const auto& answers = params.at("inputResponses");
        bool approved = answers.at("confirmation").at("content").at("approved").as_bool();
        return mymcp::detail::make_tool_result(approved ? "confirmed" : "rejected");
      });
    auto client = demo::in_process(server);
    // 客户端注册输入回调，SDK 会携带回复重新调用工具。
    client.on_input_required([](const mymcp::Json& input) -> mymcp::Json {
      // 这里只是为了自动测试而同意；实际产品必须向用户征求确认。
      demo::print("user-facing request (demo auto-approves ONLY here)", input);
      return mymcp::json::object{{"confirmation", mymcp::json::object{
        {"action", "accept"}, {"content", mymcp::json::object{{"approved", true}}}}}};
    });
    demo::print("MRTR result", client.call("confirm_demo"));
    // Real products must ask their user instead of automatically consenting.
  });
}
int main() { return run_mrtr_demo(); }
