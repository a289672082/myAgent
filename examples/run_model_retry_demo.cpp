// P1-9：HTTP 429 和暂时性服务端错误指数退避 + 总重试预算（离线）。
#include "advanced_demo_support.hpp"
#include <iostream>
using namespace myagent;
using namespace advanced_demo;
int run_model_retry_demo() {
 try {
  auto model=std::make_shared<Mock>();
  model->handler=[model](const Json& request)->Json {
    (void)request;
    if(model->calls<=2)throw std::runtime_error("Chat HTTP 429");
    return answer("retry succeeded");
  };
  auto cfg=config();
  cfg.retry.max_retries_per_request=2;
  cfg.retry.max_total_retries=2;
  cfg.retry.base_delay_ms=1;
  cfg.retry.max_delay_ms=2;
  cfg.retry.requests_per_minute=120;
  Agent agent(cfg,model);
  auto result=agent.react("test retry");
  ensure(result.status==LoopStatus::Completed,"重试后任务未完成："+result.error);
  ensure(model->calls==3,"重试次数错误");
  // 单次请求只允许重试一次，不能突破总预算或重放已执行的工具。
  auto fail_model=std::make_shared<Mock>();
  fail_model->handler=[](const Json&)->Json{throw std::runtime_error("Model HTTP 503");};
  cfg.retry.max_retries_per_request=1;
  cfg.retry.max_total_retries=1;
  Agent fail_agent(cfg,fail_model);
  const auto failed=fail_agent.react("temporary failure");
  ensure(failed.status==LoopStatus::Failed,"重试耗尽应返回失败");
  ensure(fail_model->calls==2,"重试上限没有生效");
  std::cout<<"run_model_retry_demo: 429 退避与总重试预算通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_model_retry_demo();}
