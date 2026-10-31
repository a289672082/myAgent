// P1/P2 多 Agent：Coordinator 自动拆解任务，Coder 与 Reviewer 使用不同模型。
// 任务依赖确保 Reviewer 在 Coder 完成后才执行；实际工作可改为并行的独立子任务。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <iostream>
#include <memory>

int run_agent_collaboration_demo() {
  try {
    auto leader_cfg=loop_demo::config();
    leader_cfg.model="team-leader";
    auto leader=std::make_shared<loop_demo::MockModel>();
    leader->handle=[](const myagent::Json& request,unsigned round) {
      loop_demo::check(request.at("model").as_string()=="team-leader","coordinator config");
      if(round==1)return loop_demo::answer(R"JSON({"tasks":[
       {"id":"implement","agent":"coder","description":"实现目标函数","depends_on":[],"mode":"react"},
       {"id":"review","agent":"reviewer","description":"代码审查","depends_on":["implement"],"mode":"react"}
      ]})JSON");
      return loop_demo::answer("代码已经实现，审查已经完成。");
    };
    myagent::AgentTeam team(leader_cfg,leader);
    myagent::AgentSpec coder;
    coder.name="coder";coder.description="专注 C++ 实现";
    coder.config=loop_demo::config();coder.config.model="code-model";
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[](const myagent::Json&,unsigned){return loop_demo::answer("功能已经完成");};
    team.add_agent(coder,model);
    coder.name="reviewer";coder.description="专注 C++ 审查";
    coder.config.model="review-model";
    auto review=std::make_shared<loop_demo::MockModel>();
    review->handle=[](const myagent::Json& request,unsigned) {
      loop_demo::check(myagent::json::serialize(request).find("功能已经完成")!=std::string::npos,
                       "Reviewer 必须收到前序完成结果");
      return loop_demo::answer("审查通过");
    };
    team.add_agent(coder,review);
    const auto result=team.run("实现并审查一个 C++ 函数");
    loop_demo::check(result.status==myagent::TeamStatus::Completed,"team task complete");
    loop_demo::check(result.tasks.size()==2,"two tasks");
    std::cout<<result.answer<<"\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_collaboration_demo: "<<e.what()<<'\n';return 1;
  }
}
int main(){return run_agent_collaboration_demo();}
