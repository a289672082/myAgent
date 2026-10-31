// P1 Checkpoint 示例：断点保存、验收后恢复，不重复已经执行的任务。
// 持久化的是任务状态/结果，不存模型密钥；验证器必须重新确认外部工作结果。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <filesystem>
#include <iostream>
#include <memory>

int run_team_checkpoint_demo() {
  namespace fs=std::filesystem;
  const auto path=fs::temp_directory_path()/"myagent-checkpoint-demo.json";
  try {
    fs::remove(path);
    auto cfg=loop_demo::config();
    myagent::AgentTeam team(cfg,std::make_shared<loop_demo::MockModel>());
    auto mock=std::make_shared<loop_demo::MockModel>();
    mock->handle=[](const myagent::Json&,unsigned){
      return loop_demo::answer("源码已确认，测试通过（离线模拟）。");
    };
    myagent::AgentSpec worker;
    worker.name="tester";worker.description="测试 C++ 源码";worker.config=cfg;
    team.add_agent(worker,mock);
    std::vector<myagent::TeamTask> tasks{
      {"inspect","tester","检查代码"},
      {"test","tester","运行测试",{"inspect"}}
    };
    myagent::TeamOptions options;
    options.checkpoint_path=path;
    options.validate_task=[](const myagent::TeamTask&,const myagent::DelegationResult& r){
      // 生产中这里要检查真实编译/测试退出码；不能仅凭模型回答。
      return myagent::TaskValidation{r.status==myagent::LoopStatus::Completed,
        "Offline demo validator"};
    };
    const auto first=team.collaborate(tasks,options);
    loop_demo::check(first.status==myagent::TeamStatus::Completed,"first run");
    const auto calls=mock->round;
    options.resume_checkpoint=true;
    const auto restored=team.collaborate(tasks,options);
    loop_demo::check(restored.status==myagent::TeamStatus::Completed,"restored");
    loop_demo::check(mock->round==calls,"恢复不得重复执行完成步骤");
    std::cout<<"恢复已完成任务，任务数="<<restored.tasks.size()
             <<"; 模型调用次数未增加。\n";
    fs::remove(path);
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_team_checkpoint_demo: "<<e.what()<<'\n';
    std::error_code error;fs::remove(path,error);return 1;
  }
}
int main(){return run_team_checkpoint_demo();}
