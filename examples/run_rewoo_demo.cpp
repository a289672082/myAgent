// ReWOO Demo：一次规划 → 按确定依赖执行工具 → 统一汇总（完全离线）。
//
// 作用：一次向 Planner 获取结构化工具计划，后续由 C++ 调用 MCP，不在每步
// 都请求模型决策；最后由 Solver 阅读真实工具结果，给出有依据的回答。
// 流程：
//   1. Planner 给出两个步骤及依赖（文件读取 → 检查测试目标）；
//   2. 执行器验证工具名/JSON 参数/依赖和预算，再逐个调用本地 MCP；
//   3. 工具调用受 ToolPolicy 和宿主 on_tool_approval 审批；
//   4. Solver 仅根据真实观察结果汇总，禁止编造已经运行的工具。
// 失败分支：非法工具名必须在任何副作用发生前拒绝；不执行下一步也不总结为成功。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>
#include <string>

int run_rewoo_demo() {
  using namespace myagent;
  try {
    auto cfg=loop_demo::config();
    cfg.agent_id="rewoo-demo";
    int read_count=0,check_count=0;
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[&](const Json& request,unsigned round) {
      const std::string instructions(request.at("instructions").as_string());
      if(round==1) {
        loop_demo::check(loop_demo::has(instructions,"deterministic tool planner"),
                         "Planner 应先于工具执行");
        loop_demo::check(!request.contains("tools"),
                         "ReWOO Planner 不能直接调用工具");
        loop_demo::check(loop_demo::has(instructions,"local__read_source") &&
                         loop_demo::has(instructions,"local__inspect_test"),
                         "Planner 必须看到可用工具目录");
        return loop_demo::answer(R"({"steps":[
          {"id":"read","tool":"local__read_source",
           "arguments":{"path":"src/main.cpp"},"depends_on":[]},
          {"id":"inspect","tool":"local__inspect_test",
           "arguments":{"target":"unit"},"depends_on":["read"]}
        ]})");
      }
      loop_demo::check(round==2,"ReWOO 必须只调用 Planner 和 Solver 两次模型");
      loop_demo::check(!request.contains("tools"),"Solver 不得执行工具");
      const std::string payload=json::serialize(request.at("input"));
      loop_demo::check(loop_demo::has(payload,"src/main.cpp") &&
                       loop_demo::has(payload,"unit") &&
                       loop_demo::has(payload,"inspect_test"),
                       "Solver 未获取真实的两项工具观察");
      return loop_demo::answer("已读取源文件，并检查了 unit 测试目标。");
    };
    Agent agent(cfg,model);
    agent.tool("read_source","从演示仓库读取文件内容",
      [&](const std::string& path) {
        ++read_count;
        return "源文件: "+path;
      },mymcp::arg<std::string>("path"));
    agent.tool("inspect_test","查询测试目标（不运行外部命令）",
      [&](const std::string& target) {
        ++check_count;
        loop_demo::check(read_count==1,"依赖步骤必须先于后继工具执行");
        return "测试目标: "+target;
      },mymcp::arg<std::string>("target"));
    // 工具策略与原有 ReAct 一样由可信宿主配置，不由模型改写。
    unsigned approvals=0,tool_events=0;
    LoopOptions options;
    options.max_model_calls=4;
    options.max_tool_calls=2;
    options.max_plan_steps=2;
    options.on_tool_approval=[&](const ToolApprovalRequest& request) {
      ++approvals;
      loop_demo::check(request.call_id.starts_with("rewoo-"),
                       "ReWOO 应产生稳定的工具调用 ID");
      return ToolDecision::Approve;
    };
    agent.set_conversation_callback([&](const Json& event) {
      const std::string kind(event.at("event").as_string());
      if(kind=="tool_call"||kind=="tool_result") {
        ++tool_events;
        loop_demo::check(!event.contains("arguments")&&!event.contains("output"),
                         "工具参数和结果默认不得泄漏到会话监听器");
      }
    });
    const auto result=agent.rewoo("读取源码并检查 unit 测试目标",options);
    loop_demo::check(result.status==LoopStatus::Completed,
                     "ReWOO 未完成: "+result.error);
    loop_demo::check(result.answer=="已读取源文件，并检查了 unit 测试目标。",
                     "Solver 回答错误");
    loop_demo::check(result.model_calls==2 && result.tool_calls==2 &&
                     result.plan.size()==2 && result.iterations==2,
                     "ReWOO 模型/工具轮次错误");
    loop_demo::check(read_count==1 && check_count==1 &&
                     approvals==2 && tool_events==4,
                     "依赖工具、审批或事件计数错误");

    // 故障演示：计划引用未注册工具，必须先拒绝整个计划，不能部分执行。
    auto bad=std::make_shared<loop_demo::MockModel>();
    bad->handle=[](const Json&,unsigned) {
      return loop_demo::answer(R"({"steps":[
        {"id":"read","tool":"local__read_source",
         "arguments":{"path":"ok"},"depends_on":[]},
        {"id":"bad","tool":"local__unregistered",
         "arguments":{},"depends_on":["read"]}
      ]})");
    };
    Agent invalid(cfg,bad);
    int side_effects=0;
    invalid.tool("read_source","安全读取",
      [&](const std::string& path){++side_effects;return path;},
      mymcp::arg<std::string>("path"));
    options.max_tool_calls=3;
    const auto failed=invalid.rewoo("测试非法计划",options);
    loop_demo::check(failed.status==LoopStatus::Failed && side_effects==0,
                     "非法计划不得运行部分工具");

    std::cout<<"run_rewoo_demo: planner -> MCP tools -> solver / invalid plan OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"run_rewoo_demo 失败: "<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_rewoo_demo();}
