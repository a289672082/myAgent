// 多 Agent 对话回调 Demo：已注册及后续注册的 Worker、协调器、并行任务 ID。
// 纯离线 MockModel，不访问公网；并行事件由互斥量保护。
#include "myagent/multi_agent.hpp"
#include "advanced_demo_support.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace j=myagent::json;

int run_team_conversation_callbacks_demo() {
  try {
    auto cfg=advanced_demo::config();
    auto coordinator=std::make_shared<advanced_demo::Mock>();
    unsigned coordinator_rounds=0;
    coordinator->handler=[&](const myagent::Json&) -> myagent::Json {
      if(++coordinator_rounds==1) {
        return advanced_demo::answer(
          R"({"tasks":[{"id":"planned-1","agent":"coder","description":"回答规划任务","depends_on":[],"mode":"react"}]})");
      }
      return advanced_demo::answer("协调器已汇总所有结果");
    };
    myagent::AgentTeam team(cfg,coordinator);

    // 先注册一个 Worker，再订阅：证明 set_conversation_callback 会覆盖现有 Agent。
    auto coder_model=std::make_shared<advanced_demo::Mock>();
    coder_model->handler=[](const myagent::Json&) {
      return advanced_demo::answer("Coder 已完成");
    };
    myagent::AgentSpec coder;
    coder.name="coder";
    coder.description="处理离线 C++ 任务";
    coder.config=cfg;
    team.add_agent(std::move(coder),coder_model);

    std::mutex mu;
    std::vector<myagent::Json> events;
    team.set_conversation_callback([&](const myagent::Json& event) {
      std::lock_guard<std::mutex> lock(mu);
      events.push_back(event);
    });
    unsigned named_completions=0;
    myagent::ConversationCallbacks named;
    named.on_turn_completed=[&](const myagent::Json&) {
      std::lock_guard<std::mutex> lock(mu);
      ++named_completions;
    };
    team.set_conversation_callbacks(std::move(named));

    myagent::TeamTask task;
    task.id="task-001";
    task.agent="coder";
    task.description="执行简单任务";
    myagent::TeamOptions opts;
    opts.max_parallel_agents=2;
    const auto result=team.collaborate({task},opts);
    advanced_demo::ensure(result.status==myagent::TeamStatus::Completed,
                          "显式协作任务失败："+result.error);
    bool saw_user=false,saw_reply=false,saw_done=false;
    std::string turn_id;
    for(const auto& e:events) {
      if(!e.contains("agent_id") ||
         std::string(e.at("agent_id").as_string())!="coder")continue;
      advanced_demo::ensure(e.contains("task_id") &&
          std::string(e.at("task_id").as_string())=="task-001",
          "Worker 事件没有原始 task_id");
      const std::string type(e.at("event").as_string());
      const std::string current(e.at("turn_id").as_string());
      if(turn_id.empty())turn_id=current;
      advanced_demo::ensure(!current.empty() && turn_id==current,
                            "同一轮的事件关联 ID 不一致");
      saw_user |= type=="user_message";
      saw_reply |= type=="assistant_message";
      saw_done |= type=="turn_completed";
    }
    advanced_demo::ensure(saw_user && saw_reply && saw_done,
                          "Worker 缺少输入、输出或完成回调");

    // 再注册 Worker：应自动继承已经安装的统一回调。
    auto reviewer_model=std::make_shared<advanced_demo::Mock>();
    reviewer_model->handler=[](const myagent::Json&) {
      return advanced_demo::answer("Reviewer 已完成");
    };
    myagent::AgentSpec reviewer;
    reviewer.name="reviewer";
    reviewer.description="离线代码审查";
    reviewer.config=cfg;
    team.add_agent(std::move(reviewer),reviewer_model);
    events.clear();
    const auto delegated=team.delegate("reviewer","请执行审查");
    advanced_demo::ensure(delegated.status==myagent::LoopStatus::Completed,
                          "后续注册的 Worker 无法正常运行");
    bool saw_reviewer=false;
    for(const auto& e:events)
      if(std::string(e.at("agent_id").as_string())=="reviewer" &&
         std::string(e.at("event").as_string())=="turn_completed")
        saw_reviewer=true;
    advanced_demo::ensure(saw_reviewer,"后注册的 Worker 未继承回调");

    // 自动协作：coordinator 的规划与最终总结也通过统一回调观测。
    events.clear();
    const auto automatic=team.run("自动分配并完成一个任务",opts);
    advanced_demo::ensure(automatic.status==myagent::TeamStatus::Completed,
                          "自动协作失败："+automatic.error);
    bool saw_coordinator=false,saw_planned_worker=false;
    for(const auto& e:events) {
      const std::string id(e.at("agent_id").as_string());
      const std::string type(e.at("event").as_string());
      if(id=="coordinator" && type=="turn_completed")saw_coordinator=true;
      if(id=="coder" && e.contains("task_id") &&
         std::string(e.at("task_id").as_string())=="planned-1")
        saw_planned_worker=true;
    }
    advanced_demo::ensure(saw_coordinator && saw_planned_worker,
                          "自动协作中协调器/子任务回调不完整");
    advanced_demo::ensure(named_completions>=5,
                          "团队具名回调未覆盖已注册/后注册 Worker 或 Coordinator");

    std::cout<<"run_team_conversation_callbacks_demo: team / delegate / run OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_team_conversation_callbacks_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_team_conversation_callbacks_demo();}
