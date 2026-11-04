// Execute-Verify-Repair Demo：执行 → 真实校验 → 失败反馈 → 修复 → 再校验。
// 
// 本例离线模拟 C++ 文件改写及编译器，不连接真实 API 或修改用户源码。
// 作用：把“测试通过”的裁决从模型移交给可信宿主程序。
// 流程：
//   1. Executor 通过授权工具写出初版代码（模拟 revision=1）；
//   2. verify_execution 读取真实工具状态并运行模拟编译：返回 exit_code=1；
//   3. 把错误证据反馈给模型，Repair 再次调用同一工具写入 revision=2；
//   4. 宿主重新验证 exit_code=0 后，整个 Loop 才标记 Completed。
// 每轮工具调用依然受审批、Token/工具预算和非幂等重复调用保护。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>
#include <string>

int run_execute_verify_repair_demo() {
  using namespace myagent;
  try {
    auto cfg=loop_demo::config();
    cfg.agent_id="compile-repair-demo";
    // 对有副作用的写入工具要求宿主显式审核。
    cfg.tool_policy.require_approval("local__write_code");
    unsigned policy_checks=0;
    cfg.tool_policy.approve=[&](const ToolRequest& request) {
      ++policy_checks;
      return request.tool=="local__write_code";
    };
    unsigned round=0;
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[&](const Json& request,unsigned) ->Json {
      ++round;
      if(round==1)
        return loop_demo::tool_call("local__write_code",R"({"revision":1})");
      if(round==2)
        return loop_demo::answer(R"({"status":"completed","summary":"初版代码已生成"})");
      if(round==3) {
        loop_demo::check(loop_demo::has(json::serialize(request.at("input")),
                         "simulated compiler error"),
                         "Repair 必须收到宿主真实编译错误而不是模型假设");
        return loop_demo::tool_call("local__write_code",R"({"revision":2})");
      }
      if(round==4)
        return loop_demo::answer(R"({"status":"completed","summary":"代码已修复并提交验证"})");
      throw std::runtime_error("Mock 模型收到多余的循环请求");
    };
    Agent agent(cfg,model);
    unsigned writes=0,approvals=0,checks=0;
    int revision=0;
    agent.tool("write_code","写入演示版 C++ 代码",
      [&](int next_revision) {
        ++writes;
        revision=next_revision;
        return "模拟写入代码版本 "+std::to_string(next_revision);
      },mymcp::arg<int>("revision"));
    LoopOptions options;
    options.max_model_calls=8;
    options.max_tool_calls=4;
    options.max_iterations=3;
    options.max_repairs=2;
    options.on_tool_approval=[&](const ToolApprovalRequest& request) {
      loop_demo::check(request.name=="local__write_code",
                       "审批不能允许其他工具");
      ++approvals;
      return ToolDecision::Approve;
    };
    options.verify_execution=[&](const Json& execution,const Json& tool_records,
                                  unsigned attempt) -> ExecutionVerification {
      ++checks;
      loop_demo::check(execution.at("status").as_string()=="completed",
                       "模型必须提交明确的执行状态");
      loop_demo::check(tool_records.is_array() && tool_records.size()==1,
                       "验证器应取得本轮真实 MCP 结果");
      loop_demo::check(attempt==checks,"验证轮次与宿主调用次数不一致");
      // 模拟独立编译器，以 revision 判定退出码，不相信模型自述“已通过”。
      const bool compiled=(revision==2);
      return ExecutionVerification{
        compiled,
        compiled?"compiler exit_code=0":
                 "simulated compiler error: C++ build exit_code=1",
        json::object{{"exit_code",compiled?0:1},{"revision",revision}}
      };
    };
    const auto result=agent.execute_verify_repair("编译并修复 C++ 工程",options);
    loop_demo::check(result.status==LoopStatus::Completed,
                     "执行验证修复 Loop 失败: "+result.error);
    loop_demo::check(result.answer=="代码已修复并提交验证" &&
                     result.iterations==2 && result.evaluations.size()==2,
                     "未在第二次客观验证通过后结束");
    loop_demo::check(result.evaluations[0].at("passed").as_bool()==false &&
                     result.evaluations[1].at("passed").as_bool()==true,
                     "验证结果应先失败再成功");
    loop_demo::check(writes==2 && approvals==2 && policy_checks==2 &&
                     checks==2 && result.tool_calls==2,
                     "真实工具修改、审批和验证次数异常");

    // 故障示例：没有客观验证器时立即失败，模型或工具都不会被执行。
    auto invalid_model=std::make_shared<loop_demo::MockModel>();
    invalid_model->handle=[](const Json&,unsigned)->Json {
      throw std::runtime_error("没有验证器时不应访问模型");
    };
    Agent unverified(cfg,invalid_model);
    LoopOptions unsafe;
    const auto no_verifier=unverified.execute_verify_repair("无法客观验证",unsafe);
    loop_demo::check(no_verifier.status==LoopStatus::Failed &&
                     no_verifier.model_calls==0 && no_verifier.tool_calls==0,
                     "缺失验证器时必须拒绝执行");

    std::cout<<"run_execute_verify_repair_demo: execute -> verify(fail) -> repair -> verify(pass) OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"run_execute_verify_repair_demo 失败: "<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_execute_verify_repair_demo();}
