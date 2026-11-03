// 固定关键上下文 Demo：手动压缩多轮对话后，重要项目约束仍原文保留。
// 本示例通过 C++ MockModel 完全离线运行，不读取环境变量、不需要 API Key。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using myagent::Json;
namespace j=myagent::json;

Json with_usage(Json value, int input, int output) {
  // 模拟真实 Responses 的 usage，压缩模型也会消耗 Token。
  value["usage"]=j::object{
    {"input_tokens",input},{"output_tokens",output},
    {"total_tokens",input+output}};
  return value;
}

bool contains(const std::string& text,const std::string& fragment) {
  return text.find(fragment)!=std::string::npos;
}
} // namespace

int run_agent_pinned_context_demo() {
  try {
    auto cfg=loop_demo::config();
    cfg.context.auto_compress=false;       // 本例用显式压缩展示前后对照。
    cfg.context.max_context_tokens=4096;  // 仅作预算上限，不宣称精确 Token。
    cfg.context.reserve_output_tokens=256;
    cfg.context.keep_recent_turns=1;      // 最近一轮原样保留，其余归纳摘要。
    cfg.context.pinned_notes={
      "项目必须使用 C++20",
      "所有第三方依赖必须支持离线编译",
      "执行非幂等工具之前必须经过宿主审批"
    };

    unsigned normal_calls=0,summary_calls=0;
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[&](const Json& request,unsigned)->Json {
      const std::string instructions(request.at("instructions").as_string());
      if(contains(instructions,"Summarize older conversation")) {
        // 注意：摘要故意不包含固定要求，用于证明固定事实不依赖摘要质量。
        ++summary_calls;
        return with_usage(loop_demo::answer("历史摘要：已讨论目录结构和测试方案。"),35,12);
      }
      // Agent 每次发送请求时，固定信息都应独立于历史摘要注入。
      for(const auto& note:cfg.context.pinned_notes)
        loop_demo::check(contains(instructions,note),
                         "固定项目约束未注入模型请求："+note);
      ++normal_calls;
      return with_usage(loop_demo::answer("任务已记录；后续仍保留项目编译和审批约束。"),10,6);
    };

    myagent::Agent agent(cfg,model);
    for(int i=0;i<5;++i) {
      const auto answer=agent.ask(
        "第 "+std::to_string(i+1)+" 轮：请讨论 C++ SDK 的错误处理和测试策略。");
      loop_demo::check(!answer.empty(),"离线会话未获得回复");
    }

    const auto original_size=agent.history().size();
    const auto compressed=agent.compress_context();
    loop_demo::check(compressed.compressed,"旧轮次未被压缩");
    loop_demo::check(agent.history().size()<original_size,"压缩没有减少消息数量");
    loop_demo::check(summary_calls==1,"压缩模型应且仅应被调用一次");
    loop_demo::check(agent.context_stats().compression_usage.total_tokens==47,
                     "摘要模型的 Token 消耗未记录");
    // 验证摘要存在，但并未偷偷把固定约束写回历史来伪造保留效果。
    const std::string history=j::serialize(agent.history());
    loop_demo::check(contains(history,"历史摘要"),"没有留下压缩摘要");
    loop_demo::check(!contains(history,"所有第三方依赖必须支持离线编译"),
                     "固定约束应由 pinned_notes 而不是历史摘要维护");

    const auto final_answer=agent.ask("压缩后继续：现在还要遵守哪些工程约束？");
    loop_demo::check(!final_answer.empty(),"压缩后会话不能继续");
    loop_demo::check(normal_calls==6,"普通模型请求次数不正确");

    std::cout<<"run_agent_pinned_context_demo: 历史消息 "
             <<original_size<<" -> "<<compressed.remaining_messages
             <<"；摘要调用 "<<summary_calls
             <<" 次；固定约束 "<<cfg.context.pinned_notes.size()
             <<" 条均在压缩后保留。\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_pinned_context_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main() {return run_agent_pinned_context_demo();}
