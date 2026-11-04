// P1-8：Planner / Executor / Reviewer 按阶段选择独立 Model Provider（离线）。
// 注意：这是同一个 Agent 的阶段级模型/端点路由，不是 AgentRouter 的语义选人。
// 语义路由请运行 run_agent_router_demo；两者用途不同，均需要保留。
#include "advanced_demo_support.hpp"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
using namespace myagent;
using namespace advanced_demo;

static void verify_distinct_endpoint_credentials() {
  // 本机网关使用各自的 URL、模型名与 API Key 验证真实 HTTP 路由。
  httplib::Server server;
  std::atomic<unsigned> hits{0};
  const auto register_route=[&](const std::string& phase,
      const std::string& key,const std::string& model,const std::string& reply) {
    server.Post("/"+phase+"/v1/responses",
      [&,key,model,reply](const httplib::Request& req,httplib::Response& res) {
        const auto body=j::parse(req.body);
        if(req.get_header_value("Authorization")!="Bearer "+key ||
           body.at("model").as_string()!=model) {
          res.status=403;
          res.set_content("Wrong route authentication","text/plain");
          return;
        }
        ++hits;
        res.set_content(j::serialize(answer(reply)),"application/json");
      });
  };
  register_route("planner","local-key-plan","route-plan",
     R"({"steps":[{"id":"p1","description":"execute","depends_on":[],"success_criteria":"done"}]})");
  register_route("executor","local-key-exec","route-exec",
     R"({"status":"completed","summary":"done","evidence":{}})");
  register_route("summary","local-key-summary","route-summary","routed http completed");
  const int port=server.bind_to_any_port("127.0.0.1");
  ensure(port>0,"本机多端点 HTTP 模拟网关无法绑定");
  std::thread thread([&]{server.listen_after_bind();});
  try {
    for(int i=0;i<200&&!server.is_running();++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ensure(server.is_running(),"本机 HTTP 网关未启动");
    auto cfg=config();
    cfg.phase_routes["plan"]=ModelRoute{"route-plan",
      "http://127.0.0.1:"+std::to_string(port)+"/planner/v1/responses",
      "local-key-plan"};
    cfg.phase_routes["execute"]=ModelRoute{"route-exec",
      "http://127.0.0.1:"+std::to_string(port)+"/executor/v1/responses",
      "local-key-exec"};
    cfg.phase_routes["synthesize"]=ModelRoute{"route-summary",
      "http://127.0.0.1:"+std::to_string(port)+"/summary/v1/responses",
      "local-key-summary"};
    auto never_default=std::make_shared<Mock>();
    never_default->handler=[](const Json&)->Json {
      throw std::runtime_error("Unexpected default model");
    };
    Agent agent(cfg,never_default);
    const auto result=agent.plan_and_execute("validate route credentials");
    ensure(result.status==LoopStatus::Completed,
           "独立 Base URL/API Key 路由失败："+result.error);
    ensure(result.answer=="routed http completed" && hits==3,
           "三个独立 HTTP 端点未全部被正确调用");
  }catch(...){server.stop();thread.join();throw;}
  server.stop();
  thread.join();
}

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
  verify_distinct_endpoint_credentials();
  std::cout<<"run_model_routing_demo: Planner/Executor/Reviewer 模型及不同 HTTP 端点/API Key 通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_model_routing_demo();}
