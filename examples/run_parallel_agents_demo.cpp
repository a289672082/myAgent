// P2 并行子 Agent 演示：两个不同 Agent 同时执行，拥有独立工作区。
// 工作区文件工具仅限制注册时附带的本地操作，不等于操作系统沙箱。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

int run_parallel_agents_demo() {
  namespace fs=std::filesystem;
  try {
    auto cfg=loop_demo::config();
    myagent::AgentTeam team(cfg,std::make_shared<loop_demo::MockModel>());
    std::atomic<int> active{0},peak{0};
    const auto root=fs::temp_directory_path()/"myagent-parallel-demo";
    for(int i=0;i<2;++i) {
      myagent::AgentSpec spec;
      spec.name="worker"+std::to_string(i);
      spec.description="独立工作线程";
      spec.config=loop_demo::config();spec.config.model=spec.name+"-model";
      spec.workspace=root/spec.name;
      auto mock=std::make_shared<loop_demo::MockModel>();
      mock->handle=[&,expected=spec.config.model](const myagent::Json& request,unsigned) {
        loop_demo::check(request.at("model").as_string()==expected,"worker model");
        const int now=++active;
        int p=peak.load();
        while(now>p && !peak.compare_exchange_weak(p,now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        --active;
        return loop_demo::answer("并行任务已完成");
      };
      team.add_agent(std::move(spec),mock);
    }
    std::vector<myagent::TeamTask> tasks;
    for(int i=0;i<2;++i) {
      myagent::TeamTask t;
      t.id="task"+std::to_string(i);t.agent="worker"+std::to_string(i);
      t.description="并行检查模块";tasks.push_back(t);
    }
    myagent::TeamOptions opts;opts.max_parallel_agents=2;
    const auto result=team.collaborate(tasks,opts);
    loop_demo::check(result.status==myagent::TeamStatus::Completed,"parallel complete");
    loop_demo::check(peak==2,"two workers should run concurrently");
    std::cout<<"Parallel peak="<<peak<<" tasks="<<result.tasks.size()<<"\n";
    fs::remove_all(root);return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_parallel_agents_demo: "<<e.what()<<'\n';return 1;
  }
}
int main(){return run_parallel_agents_demo();}
