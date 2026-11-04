// P1 Loop 安全回归：ReWOO 断点审批/非法计划/非幂等去重，
// Execute-Verify-Repair 必须由宿主验证且受失败修复预算约束。
#include "../examples/loop_demo_support.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace fs=std::filesystem;
using namespace myagent;

int main() {
  try {
    auto cfg=loop_demo::config();
    cfg.agent_id="p1-test";

    // ① ReWOO：工具审批 Pause → Checkpoint → Resume；不得提前执行工具。
    // Planner 已返回的计划必须通过日志回放，不能在恢复时重复计费或修改计划。
    int executions=0;
    auto mock=std::make_shared<loop_demo::MockModel>();
    mock->handle=[](const Json&,unsigned round) {
      if(round==1)return loop_demo::answer(R"({"steps":[
        {"id":"read","tool":"local__safe_read",
         "arguments":{"file":"test.cpp"},"depends_on":[]}
      ]})");
      return loop_demo::answer("文件已经读取");
    };
    Agent agent(cfg,mock);
    agent.tool("safe_read","只读模拟工具",
      [&](const std::string& file) {
        ++executions;
        return "source: "+file;
      },mymcp::arg<std::string>("file"));
    const auto checkpoint=fs::temp_directory_path()/
      ("myagent-p1-rewoo-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    struct Cleanup {
      fs::path path;
      ~Cleanup(){std::error_code ec;fs::remove(path,ec);}
    } cleanup{checkpoint};
    LoopOptions opts;
    opts.max_tool_calls=1;
    opts.max_plan_steps=1;
    opts.checkpoint_path=checkpoint;
    opts.on_tool_approval=[](const ToolApprovalRequest&) {
      return ToolDecision::Pause;
    };
    auto paused=agent.rewoo("读取 C++ 文件",opts);
    loop_demo::check(paused.status==LoopStatus::Paused &&
                     executions==0 && fs::exists(checkpoint) &&
                     mock->round==1,"暂停前执行了工具或未写入 Checkpoint");
    LoopOptions resumed_options=opts;
    resumed_options.on_tool_approval=[](const ToolApprovalRequest&) {
      return ToolDecision::Approve;
    };
    const auto resumed=agent.resume(checkpoint,resumed_options);
    loop_demo::check(resumed.status==LoopStatus::Completed &&
                     resumed.answer=="文件已经读取" &&
                     resumed.tool_calls==1 && resumed.model_calls==2 &&
                     mock->round==2 && executions==1,
                     "ReWOO 恢复没有正确回放规划/仅执行一次工具");

    // ② ReWOO：第二个步骤若无效，则在第一步执行前整体拒绝计划。
    int bad_effects=0;
    auto malformed=std::make_shared<loop_demo::MockModel>();
    malformed->handle=[](const Json&,unsigned) {
      return loop_demo::answer(R"({"steps":[
        {"id":"first","tool":"local__safe_read",
         "arguments":{"file":"a"},"depends_on":[]},
        {"id":"second","tool":"local__safe_read",
         "arguments":{"file":"b"},"depends_on":["future"]}
      ]})");
    };
    Agent bad(cfg,malformed);
    bad.tool("safe_read","只读模拟工具",
      [&](const std::string& file){++bad_effects;return file;},
      mymcp::arg<std::string>("file"));
    LoopOptions invalid_opts;
    invalid_opts.max_tool_calls=2;
    auto invalid=bad.rewoo("非法依赖",invalid_opts);
    loop_demo::check(invalid.status==LoopStatus::Failed && bad_effects==0,
                     "非法依赖导致部分工具执行");

    // ③ ReWOO：连续两步相同的未知副作用意图不得重复调用。
    int writes=0;
    auto duplicate_model=std::make_shared<loop_demo::MockModel>();
    duplicate_model->handle=[](const Json&,unsigned) {
      return loop_demo::answer(R"({"steps":[
        {"id":"w1","tool":"local__change","arguments":{"value":"same"},"depends_on":[]},
        {"id":"w2","tool":"local__change","arguments":{"value":"same"},"depends_on":["w1"]}
      ]})");
    };
    Agent duplicate(cfg,duplicate_model);
    duplicate.tool("change","模拟写操作",
      [&](const std::string& value){++writes;return value;},
      mymcp::arg<std::string>("value"));
    LoopOptions limited;
    limited.max_tool_calls=2;
    auto blocked=duplicate.rewoo("拒绝重复修改",limited);
    loop_demo::check(blocked.status==LoopStatus::Failed && writes==1,
                     "非幂等计划中重复调用没有被拦截");

    // ④ Execute-Verify-Repair：没有验证器不得启动模型；验证一直失败时
    //     最多执行 max_repairs + 1 轮，不允许模型自行宣称成功。
    auto verifier_model=std::make_shared<loop_demo::MockModel>();
    verifier_model->handle=[](const Json&,unsigned) {
      return loop_demo::answer(R"({"status":"completed","summary":"model says OK"})");
    };
    Agent unverified(cfg,verifier_model);
    const auto no_callback=unverified.execute_verify_repair("写代码");
    loop_demo::check(no_callback.status==LoopStatus::Failed &&
                     verifier_model->round==0,"缺少可信验证器时模型被调用");

    unsigned verifies=0;
    LoopOptions fail_verifier;
    fail_verifier.max_iterations=2;
    fail_verifier.max_repairs=1;
    fail_verifier.verify_execution=[&](const Json&,const Json&,unsigned attempt) {
      ++verifies;
      return ExecutionVerification{false,"compile failed",
        json::object{{"exit_code",1},{"attempt",attempt}}};
    };
    const auto failed=unverified.execute_verify_repair("代码无法编译",fail_verifier);
    loop_demo::check(failed.status==LoopStatus::LimitReached &&
                     verifies==2 && failed.iterations==2 &&
                     failed.evaluations.size()==2 &&
                     verifier_model->round==2,
                     "持续验证失败未遵守修复上限");

    std::cout<<"myagent_p1_loop_modes: checkpoint / approval / dependency / dedupe / objective verification OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"myagent_p1_loop_modes 失败: "<<e.what()<<'\n';
    return 1;
  }
}
