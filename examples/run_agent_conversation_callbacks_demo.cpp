// 统一对话回调 Demo：ask、ReAct、工具事件、失败及回调异常隔离。
// 完全离线 MockModel；不使用 API Key。子 Agent 可逐个注册同一回调。
#include "loop_demo_support.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace j=myagent::json;
int run_agent_conversation_callbacks_demo() {
  try {
    auto model=std::make_shared<loop_demo::MockModel>();
    auto cfg=loop_demo::config();
    cfg.agent_id="callback-demo";
    myagent::Agent agent(cfg,model);
    std::vector<std::string> events;
    // 默认不记录工具参数/结果，避免在 UI 日志中意外泄露秘密。
    agent.set_conversation_callback([&](const myagent::Json& e) {
      const std::string type(e.at("event").as_string());
      events.push_back(type);
      if(type=="tool_call" || type=="tool_result")
        loop_demo::check(!e.contains("arguments") && !e.contains("output"),
                         "工具敏感数据不应默认暴露");
      // 故意抛出异常：观察者不能让 Agent 执行失败。
      if(type=="assistant_delta")throw std::runtime_error("UI observer failed");
    });
    model->handle=[](const myagent::Json&,unsigned)->myagent::Json {
      return loop_demo::answer("你好，回调！");
    };
    loop_demo::check(agent.ask("你好")=="你好，回调！","ask 返回错误");
    loop_demo::check(events.size()>=3 && events.front()=="user_message" &&
                     events.back()=="turn_completed","ask 生命周期事件不完整");
    events.clear();
    const auto result=agent.react("再回答一次");
    loop_demo::check(result.status==myagent::LoopStatus::Completed,"ReAct 未完成");
    loop_demo::check(events.front()=="user_message" &&
                     events.back()=="turn_completed","ReAct 生命周期事件不完整");

    // 工具事件：先报告调用意图，再报告执行结果；默认仅提供元数据。
    agent.tool("sum","计算两个整数之和",[](int a,int b){return a+b;},
               mymcp::arg<int>("a"),mymcp::arg<int>("b"));
    events.clear();
    unsigned tool_round=0;
    model->handle=[&](const myagent::Json&,unsigned)->myagent::Json {
      if(++tool_round==1)
        return j::object{{"status","completed"},{"output",j::array{
          j::object{{"type","function_call"},{"name","local__sum"},
                    {"call_id","demo-call"},{"arguments",R"({"a":20,"b":22})"}}}}};
      return loop_demo::answer("计算结果为 42");
    };
    loop_demo::check(agent.ask("20+22 等于多少？")=="计算结果为 42",
                     "工具轮次没有完成");
    loop_demo::check(std::find(events.begin(),events.end(),"tool_call")!=events.end() &&
                     std::find(events.begin(),events.end(),"tool_result")!=events.end(),
                     "工具调用或结果事件缺失");

    // 失败也必须通知宿主；失败的会话不会提交到历史。
    events.clear();
    model->handle=[](const myagent::Json&,unsigned)->myagent::Json {
      throw std::runtime_error("模拟模型失败");
    };
    const auto failed=agent.react("触发错误");
    loop_demo::check(failed.status==myagent::LoopStatus::Failed,
                     "失败状态不正确");
    loop_demo::check(events.back()=="turn_failed","缺少失败回调");

    // 子 Agent 同样使用 set_conversation_callback；Team 的每个 worker
    // 可通过 team.agent("worker").set_conversation_callback(...) 单独订阅。
    std::cout<<"run_agent_conversation_callbacks_demo: ask / ReAct / failure OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_conversation_callbacks_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_agent_conversation_callbacks_demo();}
