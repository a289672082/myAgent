// Plan-and-Execute — 规划执行循环（离线 Demo，中文说明）
//
// 用户任务 → Planner 创建带步骤 ID/依赖/验收条件的计划
//         → Executor 按顺序调用 MCP/本地工具
//         → Step Validator 用真实工具结果验证当前步骤
//         → 验证失败则重试（安全时）或 Replanner 替换未执行的计划
//         → 按实际完成、失败、跳过情况给出最终总结。
//
// 适用：C++ 项目检查、修改、编译、测试等需要明确进度的任务。
// 与 ReAct 不同：ReAct 每次根据观察临时决定下一步；这里先制定计划。
// 两者复用同一个 run_stage() Tool Calling 基础设施。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>

int run_plan_execute_demo() {
  try {
    auto model = std::make_shared<loop_demo::MockModel>();
    // 模拟文件查询工具确实被调用过。模型一句“已查询”不能代替验证。
    bool directory_was_listed = false;
    model->handle = [](const myagent::Json& request, unsigned round) {
      if (round == 1) {
        // Planner 只负责计划，不允许直接调用文件写入/命令工具。
        loop_demo::check(!myagent::json::if_contains(request, "tools"),
                         "规划阶段不得执行外部工具");
        return loop_demo::answer(R"JSON({"steps":[
          {"id":"inspect","description":"查询 src 文件列表","depends_on":[],
           "success_criteria":"真实目录工具已返回文件列表"},
          {"id":"summarize","description":"汇总检查结果","depends_on":["inspect"],
           "success_criteria":"包含已获取文件列表"}
        ]})JSON");
      }
      if (round == 2) return loop_demo::tool_call("local__list_files", R"({"directory":"src"})");
      if (round == 3) {
        loop_demo::check(loop_demo::has(std::string(
          request.at("input")[4].at("output").as_string()), "agent.cpp"),
          "执行阶段没有观察到 MCP 工具结果");
        return loop_demo::answer(R"({"status":"completed","summary":"已查询到 agent.cpp 和 loop_engine.cpp"})");
      }
      if (round == 4)
        return loop_demo::answer(R"({"status":"completed","summary":"已汇总模拟文件清单"})");
      loop_demo::check(!myagent::json::if_contains(request, "tools"),
                       "最终归纳阶段不得执行外部工具");
      return loop_demo::answer("真实文件查询已完成，并已汇总文件清单。");
    };
    myagent::Agent agent(loop_demo::config(), model);
    agent.tool("list_files", "列出某目录中的示例文件",
      [&](const std::string& directory) {
        directory_was_listed = directory == "src";
        return directory_was_listed ? "agent.cpp, loop_engine.cpp" : "";
      }, mymcp::arg<std::string>("directory"));
    myagent::LoopOptions options;
    options.max_plan_steps = 3;
    options.validate_step = [&](const myagent::Json& step,
                                const myagent::Json& execution,
                                const myagent::Json& tool_results) {
      // 这里演示由宿主代码决定成功，不让模型自行给编译/工具调用盖章。
      const std::string id(step.at("id").as_string());
      const bool passed = directory_was_listed &&
        (id == "summarize" ||
         (tool_results.size() == 1 &&
          std::string(tool_results[0].at("name").as_string()) == "local__list_files"));
      myagent::StepValidation verified;
      verified.passed = passed;
      verified.reason = passed ? "文件查询成功" : "没有真实文件查询结果";
      verified.evidence = myagent::json::object{{"listed", directory_was_listed}};
      return verified;
    };
    const auto result = agent.plan_and_execute("列出 src 文件并汇总", options);
    loop_demo::show(result);
    loop_demo::check(result.status == myagent::LoopStatus::Completed &&
                     result.plan.size() == 2 && result.tool_calls == 1,
                     "Plan-and-Execute 未完成所有步骤");
    loop_demo::check(result.plan[0].at("verification").as_string() == "verified",
                     "需要由宿主验证器确认通过，而不是只相信模型");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_plan_execute_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_plan_execute_demo(); }
