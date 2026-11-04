// AgentRouter 回归：有效/无效路由、历史隔离、异常隔离、无隐式重试与原有 API。
#include "../examples/loop_demo_support.hpp"
#include "myagent/router.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace myagent;
int main() {
  try {
    auto cfg=loop_demo::config();
    cfg.model="router-model";
    auto selector=std::make_shared<loop_demo::MockModel>();
    selector->handle=[](const Json& req,unsigned) {
      const auto text=std::string(req.at("input")[0].at("content").as_string());
      if(text=="unknown")return loop_demo::answer(R"({"agent":"not-registered"})");
      if(text=="broken")return loop_demo::answer(R"({"agent":false})");
      if(text=="throw")throw std::runtime_error("selector temporarily unavailable");
      if(text=="review")return loop_demo::answer(R"({"selected_agent":"reviewer"})");
      return loop_demo::answer("coder");
    };
    AgentRouter router(cfg,selector);
    std::shared_ptr<loop_demo::MockModel> coder_model=std::make_shared<loop_demo::MockModel>();
    coder_model->handle=[](const Json&,unsigned) {return loop_demo::answer("coded");};
    std::shared_ptr<loop_demo::MockModel> reviewer_model=std::make_shared<loop_demo::MockModel>();
    reviewer_model->handle=[](const Json&,unsigned) {return loop_demo::answer("reviewed");};
    auto coder_cfg=loop_demo::config();
    coder_cfg.agent_id="coder";
    auto reviewer_cfg=loop_demo::config();
    reviewer_cfg.agent_id="reviewer";
    auto coder=std::make_shared<Agent>(coder_cfg,coder_model);
    auto reviewer=std::make_shared<Agent>(reviewer_cfg,reviewer_model);

    router.add_agent("coder","编写 C++ 代码",coder);
    router.add_agent("reviewer","审查 C++ 代码",reviewer);
    bool duplicate=false;
    try{router.add_agent("coder","重复",coder);}catch(const std::invalid_argument&){duplicate=true;}
    loop_demo::check(duplicate,"重复 ID 未拒绝");

    std::vector<std::string> event_types;
    router.set_conversation_callback([&](const Json& ev) {
      event_types.emplace_back(ev.at("event").as_string());
      if(event_types.back()=="route_selected")throw std::runtime_error("UI callback failure");
    });
    int completed=0;
    ConversationCallbacks typed;
    typed.on_turn_completed=[&](const Json&) {++completed;};
    router.set_conversation_callbacks(typed);
    const auto review=router.ask("review");
    loop_demo::check(review.selected_agent=="reviewer" && review.answer=="reviewed",
                     "模型决策路由错误");
    loop_demo::check(completed==1 && reviewer_model->round==1 && coder_model->round==0,
                     "通用或具名回调异常 / 执行了错误 Worker");
    const auto code=router.react("code");
    loop_demo::check(code.selected_agent=="coder" && code.answer=="coded" &&
                     code.loop_result && code.loop_result->status==LoopStatus::Completed &&
                     completed==2,"路由到 ReAct 或具名事件失败");
    loop_demo::check(!coder->history().empty() &&
                     reviewer->history().size()>0,
                     "两个 Worker 未保留独立历史");

    const auto before=coder_model->round+reviewer_model->round;
    for(const std::string input:{"unknown","broken","throw"}) {
      bool failed=false;
      try {router.ask(input);}catch(const std::exception&){failed=true;}
      loop_demo::check(failed,"无效/失败选择必须拒绝");
    }
    loop_demo::check(before==coder_model->round+reviewer_model->round,
                     "Router 错误时不得执行任何 Worker");
    unsigned failure_events=0;
    for(const auto& type:event_types)
      if(type=="route_failed")++failure_events;
    loop_demo::check(failure_events==3,"路由失败事件数量不符");

    // 后注册的 Agent 也应继承原来的通用及具名回调。
    auto writer_model=std::make_shared<loop_demo::MockModel>();
    writer_model->handle=[](const Json&,unsigned) {return loop_demo::answer("written");};
    auto writer_cfg=loop_demo::config();
    writer_cfg.agent_id="writer";
    auto writer=std::make_shared<Agent>(writer_cfg,writer_model);
    router.add_agent("writer","文档编写",writer);
    auto writer_result=router.ask("write"); // mock 选 coder，证明不会因为新增而随机偏移
    loop_demo::check(writer_result.selected_agent=="coder" &&
                     writer_model->round==0,"注册新 Agent 不应改变模型确定的选择");

    // 只有一个候选时跳过模型请求；也验证 ask() 与原生 Agent 的兼容性。
    auto never=std::make_shared<loop_demo::MockModel>();
    never->handle=[](const Json&,unsigned)->Json {
      throw std::runtime_error("single-agent route called model");
    };
    AgentRouter single(cfg,never);
    single.add_agent("writer",writer); // 兼容方案 A 的简单注册接口。
    auto result=single.ask("anything");
    loop_demo::check(result.selected_agent=="writer" &&
                     result.route_method=="single_agent" &&
                     result.answer=="written" && never->round==0,
                     "单 Worker 路由出现额外模型请求");
    loop_demo::check(coder->ask("regular")=="coded","原生 Agent::ask 不兼容");

    // 任务文本不应超出路由预算；失败前不调用模型。
    bool oversized=false;
    const auto calls=selector->round;
    try {router.route(std::string(16*1024+1,'a'));}
    catch(const std::invalid_argument&){oversized=true;}
    loop_demo::check(oversized && selector->round==calls,
                     "超长路由输入未在模型调用前阻止");

    std::cout<<"myagent_router tests OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"myagent_router tests FAILED: "<<e.what()<<'\n';
    return 1;
  }
}
