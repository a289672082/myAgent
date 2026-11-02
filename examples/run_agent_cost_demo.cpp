// P1-13：模型价格表、费用估算和软预算（离线 usage）。
#include "advanced_demo_support.hpp"
#include <cmath>
#include <iostream>
using namespace myagent;
using namespace advanced_demo;
int run_agent_cost_demo() {
 try {
  auto model=std::make_shared<Mock>();
  const Json usage=j::object{{"input_tokens",1000000},
                              {"output_tokens",500000},
                              {"total_tokens",1500000}};
  model->handler=[usage](const Json& request)->Json {
    if(has_tool_result(request))return answer("done",usage);
    auto response=tool_call("local__noop","cost-tool");
    response["usage"]=usage;
    return response;
  };
  auto cfg=config();
  cfg.model_prices["offline-model"]=ModelPrice{2.0,4.0,1.0};
  cfg.max_estimated_cost_usd=3.0;
  Agent agent(cfg,model);
  unsigned effects=0;
  agent.tool("noop","count once",[&](){++effects;return "ok";});
  const auto result=agent.react("budget test");
  ensure(result.status==LoopStatus::LimitReached,
         "超出费用软阈值后仍启动下一次模型请求");
  ensure(model->calls==1 && effects==1,
         "软预算不应重放副作用或再发第二次模型请求");
  ensure(std::abs(result.estimated_cost_usd-4.0)<1e-9,
         "按输入/输出价格计算的预算不正确");
  ensure(std::abs(agent.last_estimated_cost_usd()-4.0)<1e-9,
         "Agent 预算账本不正确");
  std::cout<<"run_agent_cost_demo: 本轮估算 USD "<<result.estimated_cost_usd
           <<"，已达到软预算，停止后续模型请求。\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_agent_cost_demo();}
