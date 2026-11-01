// P0 上下文压缩回归：保留完整工具调用对、触发自动摘要、失败不污染历史、统计摘要 Token。
#include "myagent/agent.hpp"
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace myagent;
namespace j=myagent::json;
static Json response(std::string value,int tokens=10) {
  return j::object{{"status","completed"},
    {"usage",j::object{{"input_tokens",tokens},{"output_tokens",10},{"total_tokens",tokens+10}}},
    {"output",j::array{j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",std::move(value)}}}}}}}};
}
struct Mock : Model {
  int calls=0,summaries=0;
  bool fail_summary=false;
  Json create(const Json& request) override {
    ++calls;
    const auto* instructions=j::if_contains(request,"instructions");
    if(instructions && instructions->is_string() &&
       std::string(instructions->as_string()).find("Summarize older conversation")!=std::string::npos) {
      ++summaries;
      if(fail_summary)throw std::runtime_error("summary failed");
      return response("已确认需求：编写 C++ SDK，原始任务约束已记录。",30);
    }
    return response("对话记录已完成。请继续保持目前 C++20 工程设计与中文注释规范。",20);
  }
};
static Config settings() {
  Config c;c.auto_discover_skills=false;c.auto_skill_selection=false;
  c.context.auto_compress=false;
  c.context.max_context_tokens=500;
  c.context.reserve_output_tokens=50;
  c.context.keep_recent_turns=1;
  return c;
}
static void test_manual_and_stats() {
  auto model=std::make_shared<Mock>();
  Agent agent(settings(),model);
  for(int i=0;i<4;++i)agent.ask("用户消息 "+std::to_string(i)+": C++20 项目应该有线程安全与工具权限隔离。");
  const auto old=agent.history().size();
  const auto total=agent.total_usage().total_tokens;
  const auto r=agent.compress_context();
  assert(r.compressed && r.original_messages==old && r.remaining_messages<old);
  assert(agent.history().size()==r.remaining_messages);
  assert(agent.context_stats().compression_count==1);
  assert(agent.last_usage().total_tokens==40);
  assert(agent.total_usage().total_tokens==total+40);
  assert(agent.history()[0].at("content").as_string().find("Compressed earlier context")!=std::string::npos);
  const auto temp=std::filesystem::temp_directory_path()/"myagent-context-test.json";
  agent.save_history(temp);
  Agent restored(settings(),model);
  restored.load_history(temp);
  std::filesystem::remove(temp);
  assert(j::serialize(restored.history())==j::serialize(agent.history()));
  model->fail_summary=true;
  const auto before=j::serialize(agent.history());
  try {agent.compress_context();assert(false);}catch(const std::runtime_error&){}
  assert(j::serialize(agent.history())==before);
  agent.clear_history();
  assert(agent.context_stats().compression_count==0);
}
static void test_auto_and_working_history() {
  Config c=settings();
  c.context.auto_compress=true;
  c.context.max_context_tokens=300;
  c.context.reserve_output_tokens=30;
  c.context.compress_threshold=0.35;
  auto mock=std::make_shared<Mock>();
  Agent agent(c,mock);
  for(int i=0;i<6;++i)agent.ask("对话 "+std::to_string(i)+"：测试自动压缩、计费与工具结果边界。");
  assert(mock->summaries>0);
  assert(agent.context_stats().compression_count>0);
  assert(agent.total_usage().reported_calls>6);
  // 压缩后的历史依旧能进入四种 Loop 的共同执行路径。
  auto result=agent.react("继续任务");
  // 若摘要已不足以把整个请求缩到模型预算之内，必须明确 LimitReached，
  // 不再向模型发送超限请求；这不是模型调用失败或历史损坏。
  assert(result.status==LoopStatus::Completed || result.status==LoopStatus::LimitReached);
  if (result.status==LoopStatus::LimitReached)
    assert(result.error.find("Context budget")!=std::string::npos);
}
static void test_preserve_full_tool_turn() {
  auto mock=std::make_shared<Mock>();
  Agent agent(settings(),mock);
  agent.tool("sum","求和",[](int a,int b){return a+b;},
             mymcp::arg<int>("a"),mymcp::arg<int>("b"));
  const auto first=std::make_shared<int>(0);
  struct FunctionalModel:Model {
    std::function<Json(const Json&)> fn;
    Json create(const Json& req) override {return fn(req);}
  };
  auto model=std::make_shared<FunctionalModel>();
  int round=0;
  model->fn=[&](const Json& req)->Json {
    ++round;
    const auto* instructions=j::if_contains(req,"instructions");
    if(instructions && instructions->is_string() &&
       std::string(instructions->as_string()).find("Summarize older conversation")!=std::string::npos)
      return response("摘要：已调用 sum，结果 3。",10);
    if(round==1) {
      return j::object{{"status","completed"},{"output",j::array{j::object{
        {"type","function_call"},{"name","local__sum"},{"call_id","id1"},
        {"arguments",R"({"a":1,"b":2})"}}}}};
    }
    return response("普通答复",10);
  };
  Agent with_tools(settings(),model);
  with_tools.tool("sum","求和",[](int a,int b){return a+b;},
             mymcp::arg<int>("a"),mymcp::arg<int>("b"));
  with_tools.ask("计算 1+2"); // user, function_call, function_call_output, assistant
  with_tools.ask("确认已收到结果");
  with_tools.ask("总结");
  const auto total=with_tools.history().size();
  auto r=with_tools.compress_context();
  assert(r.compressed && with_tools.history().size()<total);
  assert(with_tools.history()[0].at("content").as_string().find("摘要")!=std::string::npos);
  // 被压缩的工具轮次作为整体摘要，不能保留孤立 function_call_output。
  for(const auto& msg:with_tools.history().array_range()) {
    const auto* type=j::if_contains(msg,"type");
    if(type && type->is_string())
      assert(type->as_string()!="function_call_output");
  }
}
// 回归：摘要模型已调用却未缩短上下文，或返回不完整状态时，
// 不应修改原始历史；但实际返回的 usage 不能从压缩统计中漏记。
static void test_compaction_attempt_usage() {
  ContextOptions options;
  options.auto_compress = false;
  options.keep_recent_turns = 1;
  ContextManager context(options);
  Json history = j::array{
    j::object{{"role", "user"}, {"content", "初始需求：维护 C++ Agent SDK"}},
    j::object{{"role", "assistant"}, {"content", "已核对任务和代码"}},
    j::object{{"role", "user"}, {"content", "请继续审查"}}
  };
  const std::string original = j::serialize(history);
  unsigned calls = 0;
  const auto no_gain = context.compress(history, [&](const Json&) {
    ++calls;
    // 有效的 usage，但摘要比原始历史更长，不应误记成功压缩。
    return response(std::string(1800, 'X'), 30);
  }, true);
  assert(calls == 1);
  assert(!no_gain.compressed && no_gain.reason == "summary_not_smaller");
  assert(j::serialize(history) == original);
  assert(context.stats(history).compression_count == 0);
  assert(context.stats(history).compression_usage.total_tokens == 40);
  assert(context.stats(history).compression_usage.reported_calls == 1);

  // 服务端报告 incomplete：不替换历史，同样保存已经收到的 Token 消耗。
  try {
    context.compress(history, [&](const Json&) {
      ++calls;
      Json result = response("不能采用的摘要", 12);
      result["status"] = "incomplete";
      return result;
    }, true);
    assert(false && "Incomplete response must be rejected");
  } catch (const std::runtime_error&) {}
  assert(calls == 2 && j::serialize(history) == original);
  assert(context.stats(history).compression_usage.total_tokens == 62);
  assert(context.stats(history).compression_usage.reported_calls == 2);
  assert(context.stats(history).compression_count == 0);
}
int main() {
  test_manual_and_stats();
  test_auto_and_working_history();
  test_preserve_full_tool_turn();
  test_compaction_attempt_usage();
  std::cout<<"ContextManager offline tests OK\n";
}
