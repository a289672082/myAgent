// P0 上下文压缩：普通 ask、模型工具轮次和手动压缩都能使用同一 ContextManager。
// 该示例不需要外部凭据，Model Mock 返回固定 Responses usage。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>

int run_context_compression_demo() {
  try {
    auto cfg=loop_demo::config();
    cfg.context.max_context_tokens=1024;
    cfg.context.reserve_output_tokens=128;
    cfg.context.keep_recent_turns=1;
    cfg.context.auto_compress=false; // 演示先手动压缩，可按需改为 true。
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[](const myagent::Json& req, unsigned) {
      const auto* ins=myagent::json::if_contains(req,"instructions");
      if(ins && ins->is_string() &&
         std::string(ins->as_string()).find("Summarize older conversation")!=std::string::npos) {
        auto r=loop_demo::answer("既有目标：维护 C++ SDK；已完成目录检查；后续保留最新需求。");
        r["usage"]=myagent::json::object{{"input_tokens",50},{"output_tokens",20},{"total_tokens",70}};
        return r;
      }
      return loop_demo::answer("记录完成：这段历史消息较长，主要用于模拟真实 Agent 的上下文积累。");
    };
    myagent::Agent agent(cfg,model);
    for(int i=0;i<5;++i)agent.ask("请记录当前项目讨论第 "+std::to_string(i)+" 轮。");
    const auto before=agent.history().size();
    const auto result=agent.compress_context();
    loop_demo::check(result.compressed,"应压缩旧轮次");
    loop_demo::check(agent.history().size()<before,"压缩后对话应更短");
    loop_demo::check(agent.context_stats().compression_count==1,"统计压缩次数");
    loop_demo::check(agent.last_usage().total_tokens==70,"压缩的 token 必须计费");
    agent.ask("继续刚才的工作");
    loop_demo::check(agent.history().size()>=3,"保留摘要和最近轮次");
    std::cout<<"compressed "<<result.original_messages<<" -> "
             <<result.remaining_messages<<" messages\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_context_compression_demo: "<<e.what()<<'\n';return 1;
  }
}
int main(){return run_context_compression_demo();}
