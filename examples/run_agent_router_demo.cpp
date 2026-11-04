// 智能 Agent 路由 Demo：完全离线，不需要 API Key 或外部网络。
// 与 run_model_routing_demo 不同：该 Demo 选择执行任务的 Agent，
// 而 run_model_routing_demo 只负责同一 Agent 各阶段的模型/端点路由。
#include "loop_demo_support.hpp"
#include "myagent/router.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace myagent;
namespace {
std::shared_ptr<Agent> make_worker(const std::string& name,
                                    const std::string& answer_text,
                                    std::shared_ptr<loop_demo::MockModel>& model) {
  auto cfg=loop_demo::config();
  cfg.agent_id=name;
  cfg.model=name+"-model"; // 子 Agent 的模型、历史、工具与凭据互相独立。
  model=std::make_shared<loop_demo::MockModel>();
  model->handle=[=](const Json& req,unsigned) {
    loop_demo::check(std::string(req.at("model").as_string())==name+"-model",
                     "子 Agent 使用错误的模型");
    return loop_demo::answer(answer_text);
  };
  return std::make_shared<Agent>(cfg,model);
}
} // namespace

int run_agent_router_demo() {
  try {
    auto routing_cfg=loop_demo::config();
    routing_cfg.model="selector-model";
    routing_cfg.agent_id="router";
    auto selector=std::make_shared<loop_demo::MockModel>();
    selector->handle=[](const Json& request,unsigned) {
      loop_demo::check(request.at("model").as_string()=="selector-model",
                       "路由模型名称错误");
      loop_demo::check(!request.contains("tools"),
                       "路由模型不应获得工具权限");
      const auto instructions=std::string(request.at("instructions").as_string());
      loop_demo::check(loop_demo::has(instructions,"coder") &&
                       loop_demo::has(instructions,"reviewer"),
                       "路由模型未接收到候选 Agent 注册表");
      const auto task=std::string(request.at("input")[0].at("content").as_string());
      if(loop_demo::has(task,"非法"))return loop_demo::answer(R"({"agent":"admin"})");
      if(loop_demo::has(task,"代码审查"))return loop_demo::answer(R"({"agent":"reviewer"})");
      if(loop_demo::has(task,"文档"))return loop_demo::answer(R"({"agent":"writer"})");
      return loop_demo::answer(R"({"agent":"coder"})");
    };

    // 一、注册真实的执行 Agent；Router 不共享子 Agent 的 API Key、MCP 或历史。
    AgentRouter router(routing_cfg,selector);
    std::shared_ptr<loop_demo::MockModel> coder_model,review_model,writer_model;
    const auto coder=make_worker("coder","已完成代码实现",coder_model);
    const auto reviewer=make_worker("reviewer","代码审查完成",review_model);
    router.add_agent("coder","C++ 代码实现和构建",coder);
    router.add_agent("reviewer","C++ 静态检查与代码审查",reviewer);

    // 二、路由事件 + 现有统一对话生命周期回调。
    std::vector<Json> events;
    router.set_conversation_callback([&](const Json& e) {
      events.push_back(e);
      if(e.at("event").as_string()=="route_selected")
        throw std::runtime_error("模拟 UI 监听器抛错：应被隔离");
    });
    int named_messages=0;
    ConversationCallbacks typed;
    typed.on_assistant_message=[&](const Json&) {++named_messages;};
    router.set_conversation_callbacks(typed);
    const auto review=router.ask("请对这段 C++ 代码审查");
    loop_demo::check(review.selected_agent=="reviewer" &&
                     review.route_method=="model" &&
                     review.answer=="代码审查完成","Reviewer 路由失败");
    loop_demo::check(review_model->round==1 && coder_model->round==0,
                     "任务被发送给了错误的 Agent");
    loop_demo::check(events.size()>=4 &&
                     events.front().at("event").as_string()=="route_selected",
                     "没有在子 Agent 运行前通知路由结果");
    loop_demo::check(named_messages==1,"具名回调未传递到 Worker");

    // 三、同一 Router 第二轮选择不同的 Agent。
    const auto code=router.ask("实现一个 C++20 函数");
    loop_demo::check(code.selected_agent=="coder" &&
                     code.answer=="已完成代码实现" && coder_model->round==1,
                     "Coder 路由失败");

    // 四、回调安装之后注册的 Agent 也自动继承；ReAct 复用原生 Agent Loop。
    const auto writer=make_worker("writer","文档已经生成",writer_model);
    router.add_agent("writer","撰写 API 文档和示例",writer);
    const auto doc=router.react("给新增接口编写文档");
    loop_demo::check(doc.selected_agent=="writer" &&
                     doc.loop_result.has_value() &&
                     doc.loop_result->status==LoopStatus::Completed &&
                     doc.answer=="文档已经生成","ReAct 路由失败");
    loop_demo::check(named_messages==3,"后注册 Worker 没有继承回调");

    // 五、错误的候选 ID 一律拒绝；不会默默换一个 Agent 重试。
    const auto before=coder_model->round+review_model->round+writer_model->round;
    bool rejected=false;
    try {router.ask("非法选择测试");}
    catch(const std::runtime_error&){rejected=true;}
    loop_demo::check(rejected && before==
        coder_model->round+review_model->round+writer_model->round,
        "非法路由结果导致了 Worker 被错误执行");
    loop_demo::check(events.back().at("event").as_string()=="route_failed",
                     "非法路由时未通知 route_failed");

    // 六、直接调用原生 Agent 仍然兼容，不需要经过 Router。
    loop_demo::check(coder->ask("直接请求 coder")=="已完成代码实现",
                     "Router 改坏了原生 Agent::ask");

    // 七、只有一个候选 Agent 时不耗费额外的路由模型请求。
    auto unused_selector=std::make_shared<loop_demo::MockModel>();
    unused_selector->handle=[](const Json&,unsigned)->Json {
      throw std::runtime_error("单 Worker 不应该调用路由模型");
    };
    AgentRouter one(routing_cfg,unused_selector);
    one.add_agent("coder","代码实现",coder);
    const auto selected=one.route("任何任务");
    loop_demo::check(selected.selected_agent=="coder" &&
                     selected.method=="single_agent" &&
                     unused_selector->round==0,
                     "单一 Agent 快速路由失败");

    std::cout<<"run_agent_router_demo: model route / ask / react / callbacks / invalid route / single agent OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_router_demo 失败: "<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_agent_router_demo();}
