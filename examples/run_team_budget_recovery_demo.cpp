// 多 Agent 预算恢复 Demo：失败重试、备用 Agent、费用累计和 Checkpoint 续跑。
// 完全离线 Mock：绝不访问真实模型；无需 API Key，也不会执行有副作用的工具。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace j=myagent::json;

struct ScopedDirectory {
  fs::path path=fs::temp_directory_path()/
      ("myagent-team-budget-recovery-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
  ScopedDirectory(){fs::create_directories(path);}
  ~ScopedDirectory(){std::error_code ec;fs::remove_all(path,ec);}
};

myagent::Json priced_reply(const std::string& text,bool completed) {
  auto reply=loop_demo::answer(text);
  if(!completed)reply["status"]="incomplete";
  // 每次模型请求消耗 8 输入 + 12 输出 Token。
  reply["usage"]=j::object{
    {"input_tokens",8},{"output_tokens",12},{"total_tokens",20}};
  return reply;
}

myagent::Config priced_config() {
  auto cfg=loop_demo::config();
  // 仅为演示整数费用：输入/输出每百万 Token 各 1,000,000 美元，
  // 因此一个请求 20 Token 刚好对应 20 美元。不是现实服务商价格。
  cfg.model_prices[cfg.model]=myagent::ModelPrice{1000000.0,1000000.0,1000000.0};
  return cfg;
}

// 每个子 Agent 的模型、会话互不共享；Mock 可以精确统计真实模型尝试次数。
struct Scenario {
  myagent::AgentTeam team;
  std::shared_ptr<loop_demo::MockModel> primary;
  std::shared_ptr<loop_demo::MockModel> backup;
  std::shared_ptr<loop_demo::MockModel> auditor;

  explicit Scenario(const myagent::Config& cfg)
      :team(cfg,std::make_shared<loop_demo::MockModel>()),
       primary(std::make_shared<loop_demo::MockModel>()),
       backup(std::make_shared<loop_demo::MockModel>()),
       auditor(std::make_shared<loop_demo::MockModel>()) {
    primary->handle=[](const myagent::Json&,unsigned) {
      // 首选 Agent 返回 incomplete：确实产生 Token 费用，但没有工具副作用。
      return priced_reply("首选模型本轮输出不完整",false);
    };
    backup->handle=[](const myagent::Json&,unsigned) {
      return priced_reply("备用 Agent 完成实现",true);
    };
    auditor->handle=[](const myagent::Json&,unsigned) {
      return priced_reply("验证步骤完成",true);
    };
    myagent::AgentSpec spec;
    spec.config=cfg;
    spec.name="primary";spec.description="首选执行 Agent";
    team.add_agent(spec,primary);
    spec.name="backup";spec.description="失败后备用 Agent";
    team.add_agent(spec,backup);
    spec.name="auditor";spec.description="后续独立验证 Agent";
    team.add_agent(spec,auditor);
  }
};

bool same_cost(double value,double expected) {
  return std::abs(value-expected)<1e-6;
}
} // namespace

int run_team_budget_recovery_demo() {
  try {
    ScopedDirectory directory;
    const auto checkpoint=directory.path/"team-state.json";
    auto cfg=priced_config();

    myagent::TeamTask implement{"implement","primary","实现本地编译脚本"};
    implement.fallback_agents={"backup"};
    myagent::TeamTask verify{
      "verify","auditor","检查编译脚本的结果",{"implement"}};
    const std::vector<myagent::TeamTask> tasks{implement,verify};

    myagent::TeamOptions options;
    options.checkpoint_path=checkpoint;
    options.max_parallel_agents=2;
    options.max_model_calls=3;
    options.max_estimated_cost_usd=30.0; // 软预算：记录实际返回 usage 后再拦截下一任务。
    options.validate_task=[](const myagent::TeamTask& task,
                             const myagent::DelegationResult& result) {
      // 这里只验证 Mock 的明确输出，生产中必须使用真实测试退出码等证据。
      const bool passed=result.status==myagent::LoopStatus::Completed &&
        ((task.id=="implement" && result.answer=="备用 Agent 完成实现") ||
         (task.id=="verify" && result.answer=="验证步骤完成"));
      return myagent::TaskValidation{passed,"离线 Mock 验收",
                                    j::object{{"mock_verified",passed}}};
    };

    // 阶段 1：首选失败花费 $20，备用成功再花费 $20。
    // 预算 $30 已达到，所以依赖任务 verify 不得启动，状态必须可恢复。
    Scenario initial(cfg);
    const auto first=initial.team.collaborate(tasks,options);
    loop_demo::check(first.status==myagent::TeamStatus::LimitReached,
                     "成本软预算未阻止后续任务");
    loop_demo::check(first.model_calls==2 && same_cost(first.estimated_cost_usd,40),
                     "首选失败的模型费用没有计入团队总账");
    loop_demo::check(initial.primary->round==1 &&
                     initial.backup->round==1 && initial.auditor->round==0,
                     "失败转移或依赖任务调度出现异常");
    loop_demo::check(first.usage.total_tokens==40,"Token 总账不准确");
    loop_demo::check(fs::exists(checkpoint),"未写出可恢复的团队 Checkpoint");

    // 阶段 2：模拟进程重启，使用全新 Agent 实例加载同一路径。
    // 原预算不变时，不应再次调用已完成的子 Agent，也不应越过软预算。
    Scenario restarted(cfg);
    options.resume_checkpoint=true;
    const auto bounded=restarted.team.collaborate(tasks,options);
    loop_demo::check(bounded.status==myagent::TeamStatus::LimitReached &&
                     bounded.model_calls==2 &&
                     same_cost(bounded.estimated_cost_usd,40),
                     "恢复后遗失历史费用或越过预算");
    loop_demo::check(restarted.primary->round==0 &&
                     restarted.backup->round==0 &&
                     restarted.auditor->round==0,
                     "恢复不应重做首选、备用或后续任务");

    // 阶段 3：由宿主显式提高预算至 $100，再从相同 Checkpoint 继续。
    // 已验收的 implement 不重新执行，仅启动尚未完成的 verify。
    options.max_estimated_cost_usd=100.0;
    const auto resumed=restarted.team.collaborate(tasks,options);
    loop_demo::check(resumed.status==myagent::TeamStatus::Completed,
                     "提高预算后未能继续剩余任务");
    loop_demo::check(resumed.model_calls==3 &&
                     same_cost(resumed.estimated_cost_usd,60),
                     "恢复后总调用数或费用未正确累计");
    loop_demo::check(restarted.primary->round==0 &&
                     restarted.backup->round==0 &&
                     restarted.auditor->round==1,
                     "续跑应该只执行尚未完成的验证任务");

    // 阶段 4：再恢复一次已经完成的任务图，验证不会重复产生费用。
    const auto again=restarted.team.collaborate(tasks,options);
    loop_demo::check(again.status==myagent::TeamStatus::Completed &&
                     same_cost(again.estimated_cost_usd,60) &&
                     restarted.auditor->round==1,
                     "完成后重复执行或遗失持久化费用");

    std::cout<<"run_team_budget_recovery_demo: 首选失败/备用成功 $40；"
                "预算 $30 阻断；恢复时 0 次重复调用；"
                "提高预算后新增验证 $20，总费用 $60。\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_team_budget_recovery_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main() {return run_team_budget_recovery_demo();}
