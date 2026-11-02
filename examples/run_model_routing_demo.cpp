// P1-8：Planner / Executor / Reviewer 按阶段选择独立 Model Provider（离线）。
#include "advanced_demo_support.hpp"
#include <iostream>
using namespace myagent;
using namespace advanced_demo;
int run_model_routing_demo() {
 try {
  auto planner=std::make_shared<Mock>();
  auto executor=std::make_shared<Mock>();
  auto reviewer=std::make_shared<Mock>();
  auto synthesis=std::make_shared<Mock>();
  planner->handler=[](const Json& request) {
    ensure(request.at("model").as_string()=="planner-model","planner model 路由失败");
    return answer(R"({"steps":[{"id":"step-1","description":"validate","depends_on":[],"success_criteria":"verified"}]})");
  };
  executor->handler=[](const Json& request) {
    ensure(request.at("model").as_string()=="executor-model","executor model 路由失败");
    return answer(R"({"status":"completed","summary":"verified","evidence":{}})");
  };
  reviewer->handler=[](const Json& request) {
    ensure(request.at("model").as_string()=="reviewer-model","reviewer model 路由失败");
    return answer(R"({"score":1,"passed":true,"feedback":"ok","issues":[]})");
  };
  synthesis->handler=[](const Json& request) {
    ensure(request.at("model").as_string()=="summary-model","summary model 路由失败");
    return answer("completed");
  };
  auto base=std::make_shared<Mock>();
  base->handler=[](const Json&){throw std::runtime_error("Unexpected default route");return answer("");};
  Agent agent(config(),base);
  // 在真实项目中，Config::phase_routes[phase] 可指定各自的 base_url/api_key/model。
  // 这里注入离线 Model，精确检验路由，无需泄露或请求任何真实密钥。
  agent.set_phase_model("plan",planner,"planner-model");
  agent.set_phase_model("execute",executor,"executor-model");
  agent.set_phase_model("synthesize",synthesis,"summary-model");
  LoopOptions opts;
  const auto planned=agent.plan_and_execute("test routing",opts);
  ensure(planned.status==LoopStatus::Completed,"规划执行失败："+planned.error);
  ensure(planner->calls==1 && executor->calls==1 && synthesis->calls==1,
         "Planner/Executor/Synthesizer 路由次数异常");
  Agent review_agent(config(),base);
  auto draft=std::make_shared<Mock>();
  draft->handler=[](const Json&){return answer("draft");};
  review_agent.set_phase_model("generate",draft,"draft-model");
  review_agent.set_phase_model("evaluate",reviewer,"reviewer-model");
  auto evaluation=review_agent.evaluate_and_optimize("review this");
  ensure(evaluation.status==LoopStatus::Completed,
         "Reviewer 阶段失败："+evaluation.error);
  ensure(reviewer->calls==1,"Reviewer 未使用专用模型");
  std::cout<<"run_model_routing_demo: Planner / Executor / Reviewer 独立路由通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_model_routing_demo();}
