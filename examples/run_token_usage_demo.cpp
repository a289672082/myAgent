// Token Usage 独立 Demo：无需 API Key / 网络请求 / 命令行参数。
// 演示 ask()、Plan-and-Execute、MCP 工具调用后的真实 usage 聚合。
// Token 数由 Mock Model 模拟真实 Responses API 的 usage 返回，不是估算。
#include "myagent/agent.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using myagent::Json;
namespace json = myagent::json;
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
Json reply(const std::string& output, int input_tokens, int output_tokens,
           int cached = 0, int reasoning = 0) {
  return json::object{
    {"status", "completed"},
    {"usage", json::object{
      {"input_tokens", input_tokens}, {"output_tokens", output_tokens},
      {"total_tokens", input_tokens + output_tokens},
      {"input_tokens_details", json::object{{"cached_tokens", cached}}},
      {"output_tokens_details", json::object{{"reasoning_tokens", reasoning}}}
    }},
    {"output", json::array{json::object{
      {"type", "message"}, {"role", "assistant"},
      {"content", json::array{json::object{{"type", "output_text"}, {"text", output}}}}
    }}}
  };
}
Json tool_call(int input_tokens, int output_tokens) {
  return json::object{
    {"status", "completed"},
    {"usage", json::object{
      {"input_tokens", input_tokens}, {"output_tokens", output_tokens},
      {"total_tokens", input_tokens + output_tokens}
    }},
    {"output", json::array{json::object{
      {"type", "function_call"}, {"name", "local__add"},
      {"call_id", "add-1"}, {"arguments", R"({"a":20,"b":22})"}
    }}}
  };
}
class MockUsageModel : public myagent::Model {
 public:
  unsigned calls = 0;
  Json create(const Json& request) override {
    switch (++calls) {
      case 1: return reply("普通 ask 已完成", 100, 20, 12, 3);
      case 2: // Planner：不能执行工具
        check(!json::if_contains(request, "tools"), "Planner 不应有工具权限");
        return reply(R"({"steps":[{"id":"calc","description":"计算两个整数"}]})",
                     60, 30, 10, 5);
      case 3: // Executor 首次返回 tool_call；此次 usage 也必须计入总量
        return tool_call(80, 12);
      case 4:
        check(request.at("input").size() >= 3, "工具结果必须回传给模型");
        return reply(R"({"status":"completed","summary":"工具计算得出 42"})",
                     140, 30, 20, 4);
      case 5: // Planner 之后的最后总结
        check(!json::if_contains(request, "tools"), "最终总结不应有工具权限");
        return reply("已按计划完成，结果为 42。", 90, 20, 16, 2);
      default: throw std::runtime_error("Mock 请求次数超出示例范围");
    }
  }
};
void print_usage(const std::string& label, const myagent::TokenUsage& u) {
  std::cout << label
            << " input=" << u.input_tokens
            << " output=" << u.output_tokens
            << " total=" << u.total_tokens
            << " cached_input=" << u.cached_input_tokens
            << " reasoning_output=" << u.reasoning_output_tokens
            << " reported_calls=" << u.reported_calls
            << " unreported_calls=" << u.unreported_calls << '\n';
}
} // namespace

int run_token_usage_demo() {
  try {
    myagent::Config cfg;
    cfg.auto_discover_skills = false;
    cfg.auto_skill_selection = false;
    // Mock Model 不进行联网，以下地址与模型仅用于演示明确的配置参数。
    cfg.base_url = "https://api.openai.com";
    cfg.api_key = "sk-example-not-a-real-key";
    cfg.model = "gpt-4.1-mini";

    auto model = std::make_shared<MockUsageModel>();
    myagent::Agent agent(cfg, model);
    agent.tool("add", "求和", [](int a, int b) { return a + b; },
               mymcp::arg<int>("a"), mymcp::arg<int>("b"));

    std::cout << agent.ask("你好") << '\n';
    print_usage("本次 ask", agent.last_usage());

    myagent::LoopOptions options;
    options.max_model_calls = 10;
    options.max_tool_calls = 5;
    auto run = agent.plan_and_execute("计算 20 + 22", options);
    check(run.status == myagent::LoopStatus::Completed, "Plan Loop 未完成");
    check(run.model_calls == 4 && run.tool_calls == 1, "模型/工具调用次数错误");
    check(run.usage.reported_calls == 4 && run.usage.total_tokens == 462,
          "Loop Token 累计错误");
    check(run.usage_by_phase.at("execute").reported_calls == 2,
          "执行阶段应计入两次 Responses 调用");
    check(agent.total_usage().total_tokens == 582,
          "累计 Token 应包含之前的 ask");
    check(agent.last_usage().total_tokens == run.usage.total_tokens,
          "last_usage 应是本次 Loop 用量");
    print_usage("本次 Plan Loop", run.usage);
    for (const auto& [phase, usage] : run.usage_by_phase)
      print_usage("  阶段 " + phase, usage);
    for (const auto& [name, usage] : run.usage_by_model)
      print_usage("  模型 " + name, usage);
    print_usage("Agent 累计", agent.total_usage());

    agent.clear_history();
    check(agent.total_usage().total_tokens == 582, "清空历史不应清除账单统计");
    agent.reset_usage();
    check(agent.total_usage().total_tokens == 0, "reset_usage 应清空累计统计");
    std::cout << "run_token_usage_demo: 全部检查通过。\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_token_usage_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_token_usage_demo(); }
