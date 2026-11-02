// P1-9：HTTP 429 和暂时性服务端错误指数退避 + 总重试预算（离线）。
#include "advanced_demo_support.hpp"
#include <iostream>
#include <chrono>
using namespace myagent;
using namespace advanced_demo;
int run_model_retry_demo() {
 try {
  auto model=std::make_shared<Mock>();
  // 避免 Mock::handler 通过 shared_ptr 再次拥有自身，形成循环引用；
  // Sanitizers 会检测到这种测试代码中的资源泄漏。
  std::weak_ptr<Mock> weak_model=model;
  model->handler=[weak_model](const Json& request)->Json {
    (void)request;
    const auto locked=weak_model.lock();
    if(!locked)throw std::runtime_error("Mock model unexpectedly expired");
    if(locked->calls<=2)throw std::runtime_error("Chat HTTP 429");
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
  ensure(model->calls==3 && result.model_calls==3,
         "实际模型尝试次数没有纳入 Loop 总预算");
  // 单次请求只允许重试一次，不能突破总预算或重放已执行的工具。
  auto fail_model=std::make_shared<Mock>();
  fail_model->handler=[](const Json&)->Json{throw std::runtime_error("Model HTTP 503");};
  cfg.retry.max_retries_per_request=1;
  cfg.retry.max_total_retries=1;
  Agent fail_agent(cfg,fail_model);
  const auto failed=fail_agent.react("temporary failure");
  ensure(failed.status==LoopStatus::Failed,"重试耗尽应返回失败");
  ensure(fail_model->calls==2,"重试上限没有生效");
  // RPM 配额为 1，首次调用返回工具请求，下一模型轮次应因 50ms 截止时间
  // 无法等待 60 秒令牌恢复而直接失败；工具绝不能被重新调用。
  auto rate_model=std::make_shared<Mock>();
  rate_model->handler=[](const Json& request) {
    if(has_tool_result(request))return answer("should not be reached");
    return tool_call("local__count_once","rate-test-call");
  };
  cfg.retry.max_retries_per_request=0;
  cfg.retry.max_total_retries=0;
  cfg.retry.requests_per_minute=1;
  Agent rate_agent(cfg,rate_model);
  unsigned side_effects=0;
  rate_agent.tool("count_once","count side effects",[&](){++side_effects;return 1;});
  LoopOptions timeout;
  timeout.timeout=std::chrono::milliseconds(50);
  const auto limited=rate_agent.react("rate limit test",timeout);
  ensure(limited.status!=LoopStatus::Completed &&
         rate_model->calls==1 && side_effects==1,
         "RPM 限流未阻止第二次请求，或重复执行工具");
  // 第二次模型请求被 RPM 限流时应可中断，而不是无响应地休眠 60 秒。
  auto cancel_model=std::make_shared<Mock>();
  auto first_request_at=std::chrono::steady_clock::time_point{};
  cancel_model->handler=[&](const Json& request) {
    if(has_tool_result(request))return answer("unexpected second request");
    first_request_at=std::chrono::steady_clock::now();
    return tool_call("local__single_effect","cancel-tool");
  };
  Agent cancel_agent(cfg,cancel_model);
  unsigned cancel_effects=0;
  cancel_agent.tool("single_effect","one side effect",
                    [&](){++cancel_effects;return 1;});
  LoopOptions cancel_options;
  cancel_options.timeout=std::chrono::seconds(2);
  cancel_options.cancelled=[&] {
    return first_request_at!=std::chrono::steady_clock::time_point{} &&
           std::chrono::steady_clock::now()-first_request_at>
               std::chrono::milliseconds(80);
  };
  const auto cancelled=cancel_agent.react("cancel rpm wait",cancel_options);
  ensure(cancelled.status==LoopStatus::Cancelled &&
         cancel_model->calls==1 && cancel_effects==1,
         "RPM 等待未及时响应取消，或重复调用已执行的工具");
  std::cout<<"run_model_retry_demo: 429、总重试预算和限流取消通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_model_retry_demo();}
