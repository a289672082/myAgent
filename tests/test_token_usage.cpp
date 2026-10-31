// Token 消耗统计的离线回归测试：直接模拟 Responses API usage 字段。
// 验证累计、缓存/推理子项、Tool Calling、分阶段、缺失 usage 和异常后保留消耗。
#include "myagent/agent.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace myagent;
namespace j = myagent::json;

static Json reply(const std::string& text) {
  return j::object{{"status", "completed"}, {"output", j::array{
    j::object{{"type", "message"}, {"role", "assistant"},
              {"content", j::array{j::object{{"type", "output_text"}, {"text", text}}}}}}}};
}
static Json with_usage(Json response, int input, int output,
                       int cached = 0, int reasoning = 0) {
  response["usage"] = j::object{
    {"input_tokens", input}, {"output_tokens", output},
    {"total_tokens", input + output},
    {"input_tokens_details", j::object{{"cached_tokens", cached}}},
    {"output_tokens_details", j::object{{"reasoning_tokens", reasoning}}}
  };
  return response;
}
static Json call(const char* name) {
  return j::object{{"status", "completed"}, {"output", j::array{
    j::object{{"type", "function_call"}, {"name", name}, {"call_id", "call-1"},
              {"arguments", R"({"a":2,"b":3})"}}}}};
}
struct Mock : Model {
  unsigned calls = 0;
  std::function<Json(const Json&, unsigned)> handle;
  Json create(const Json& request) override { return handle(request, ++calls); }
};
static Config cfg() {
  Config c;
  c.auto_discover_skills = false;
  c.auto_skill_selection = false;
  return c;
}
static void test_parser() {
  // 1. 响应带完整 usage，缓存和推理 token 属于 input/output 子集。
  auto t = parse_token_usage(with_usage(reply("ok"), 150, 30, 60, 15));
  assert(t.input_tokens == 150 && t.output_tokens == 30 && t.total_tokens == 180);
  assert(t.cached_input_tokens == 60 && t.reasoning_output_tokens == 15);
  assert(t.reported_calls == 1 && t.unreported_calls == 0 && t.complete());

  // 2. usage 缺失时不使用字符数估算，也不会把 0 当成确定没消耗。
  t = parse_token_usage(reply("response without usage"));
  assert(!t.complete() && t.reported_calls == 0 && t.unreported_calls == 1);
  assert(t.total_tokens == 0);
  // 3. 输入输出有统计、总数缺省时可从两部分求和。
  auto r = reply("missing total");
  r["usage"] = j::object{{"input_tokens", 10}, {"output_tokens", 3}};
  t = parse_token_usage(r);
  assert(t.reported_calls == 1 && t.total_tokens == 13);
  // 4. 非法类型/负值不是可用的 Token 统计。
  r["usage"]["input_tokens"] = -2;
  t = parse_token_usage(r);
  assert(t.unreported_calls == 1 && t.reported_calls == 0);
  r["usage"]["input_tokens"] = "123";
  t = parse_token_usage(r);
  assert(t.unreported_calls == 1);
  r["usage"]["input_tokens"] = 6;
  r["usage"]["input_tokens_details"] = j::object{{"cached_tokens", 100}};
  t = parse_token_usage(r);
  assert(t.unreported_calls == 1);
}
static void test_ask_tool_rounds_and_counters() {
  auto model = std::make_shared<Mock>();
  Agent agent(cfg(), model);
  agent.tool("add", "Add", [](int a, int b) { return a + b; },
             mymcp::arg<int>("a"), mymcp::arg<int>("b"));
  model->handle = [](const Json&, unsigned round) -> Json {
    if (round == 1) return with_usage(call("local__add"), 100, 20, 10, 2);
    if (round == 2) return with_usage(reply("5"), 200, 50, 30, 15);
    return reply("no usage from gateway");
  };
  assert(agent.ask("2+3") == "5");
  assert(agent.last_usage().input_tokens == 300);
  assert(agent.last_usage().output_tokens == 70);
  assert(agent.last_usage().cached_input_tokens == 40);
  assert(agent.last_usage().reasoning_output_tokens == 17);
  assert(agent.last_usage().total_tokens == 370);
  assert(agent.last_usage().reported_calls == 2);
  assert(agent.total_usage().total_tokens == 370);
  // 新的 ask 只覆盖 last_usage，累积 total_usage 不会归零。
  assert(agent.ask("没有 usage 的消息") == "no usage from gateway");
  assert(agent.last_usage().total_tokens == 0 && agent.last_usage().unreported_calls == 1);
  assert(agent.total_usage().reported_calls == 2);
  assert(agent.total_usage().unreported_calls == 1);
  agent.clear_history();
  assert(agent.total_usage().total_tokens == 370);
  agent.reset_usage();
  assert(agent.last_usage().total_tokens == 0 && agent.total_usage().total_tokens == 0);
  assert(agent.total_usage().reported_calls == 0 && agent.total_usage().unreported_calls == 0);
}
static void test_all_loop_modes() {
  for (const LoopMode mode : {LoopMode::ReAct, LoopMode::PlanAndExecute,
                              LoopMode::Reflection, LoopMode::EvaluatorOptimizer}) {
    auto model = std::make_shared<Mock>();
    Agent agent(cfg(), model);
    model->handle = [mode](const Json& request, unsigned round) -> Json {
      if (mode == LoopMode::ReAct)
        return with_usage(reply("ReAct done"), 40, 10, 4, 2);
      if (mode == LoopMode::PlanAndExecute) {
        if (round == 1) {
          assert(!j::if_contains(request, "tools"));
          return with_usage(reply(R"({"steps":[{"id":"one","description":"检查文件"}]})"),
                            100, 20, 20, 3);
        }
        if (round == 2)
          return with_usage(reply(R"({"status":"completed","summary":"已检查"})"),
                            200, 40, 15, 7);
        return with_usage(reply("Plan final"), 60, 15, 10, 3);
      }
      if (mode == LoopMode::Reflection) {
        if (round == 1) return with_usage(reply("draft"), 70, 20, 7, 3);
        return with_usage(reply(R"({"needs_revision":false,"feedback":"无需修改"})"),
                          80, 25, 8, 5);
      }
      if (round == 1) return with_usage(reply("candidate"), 80, 15, 5, 4);
      assert(request.at("model").as_string() == "reviewer-name");
      return with_usage(reply(R"({"score":0.95,"passed":true,"feedback":"OK","issues":[]})"),
                        90, 20, 9, 2);
    };
    LoopOptions options;
    options.evaluator_model = "reviewer-name";
    options.planner_model = "planner-name";
    options.commit_to_history = false;
    const auto res = agent.run_loop("测试循环统计", mode, options);
    assert(res.status == LoopStatus::Completed && agent.history().empty());
    assert(res.usage.reported_calls == res.model_calls);
    assert(res.usage.unreported_calls == 0);
    assert(agent.last_usage().total_tokens == res.usage.total_tokens);
    assert(agent.total_usage().total_tokens == res.usage.total_tokens);
    assert(res.usage_by_phase.size() == (mode == LoopMode::ReAct ? 1 :
       mode == LoopMode::PlanAndExecute ? 3 : 2));
    if (mode == LoopMode::PlanAndExecute) {
      assert(res.model_calls == 3 && res.usage.total_tokens == 435);
      assert(res.usage_by_phase.at("plan").total_tokens == 120);
      assert(res.usage_by_phase.at("execute").total_tokens == 240);
      assert(res.usage_by_phase.at("synthesize").total_tokens == 75);
      assert(res.usage_by_model.at("planner-name").total_tokens == 120);
      assert(res.usage_by_model.at("gpt-4.1-mini").total_tokens == 315);
    }
    if (mode == LoopMode::Reflection) {
      assert(res.usage_by_phase.at("reflect").reported_calls == 1);
      assert(res.usage_by_model.at("reviewer-name").total_tokens == 105);
    }
    if (mode == LoopMode::EvaluatorOptimizer)
      assert(res.usage_by_phase.at("evaluate").reported_calls == 1);
  }
}
static void test_failure_does_not_drop_usage() {
  // 即使模型返回未完成状态，usage 也已经是实际消耗，应保留在统计中。
  auto model = std::make_shared<Mock>();
  Agent agent(cfg(), model);
  model->handle = [](const Json&, unsigned) {
    auto res = with_usage(reply("not completed"), 45, 13);
    res["status"] = "incomplete";
    return res;
  };
  auto r = agent.react("模型未完成");
  assert(r.status == LoopStatus::Failed);
  assert(r.usage.input_tokens == 45 && r.usage.total_tokens == 58);
  assert(agent.last_usage().total_tokens == 58 && agent.total_usage().total_tokens == 58);
  assert(agent.history().empty());

  // 编排失败也不能让既有的 usage 消失。
  model->calls = 0;
  model->handle = [](const Json&, unsigned round) {
    if (round == 1)
      return with_usage(reply(R"({"steps":[{"id":"one","description":"执行"}]})"), 25, 10);
    return with_usage(reply("Executor error: not JSON"), 35, 8);
  };
  r = agent.plan_and_execute("错误格式");
  assert(r.status == LoopStatus::Failed);
  assert(r.usage.total_tokens == 78 && r.usage.reported_calls == 2);
  assert(agent.total_usage().total_tokens == 136);
  assert(agent.last_usage().total_tokens == 78);
  assert(agent.history().empty());

  // 传统 ask 抛错时，last_usage 和 total_usage 仍包含已收到的 usage。
  model->calls = 0;
  model->handle = [](const Json&, unsigned) {
    auto r = with_usage(reply("failed"), 12, 8);
    r["status"] = "incomplete";
    return r;
  };
  try { agent.ask("失败也应记账"); assert(false); }
  catch (const std::runtime_error&) {}
  assert(agent.last_usage().total_tokens == 20);
  assert(agent.total_usage().total_tokens == 156);
  assert(agent.history().empty());
}
int main() {
  test_parser();
  test_ask_tool_rounds_and_counters();
  test_all_loop_modes();
  test_failure_does_not_drop_usage();
  std::cout << "Token usage accounting tests OK\n";
}
