// Plan-and-Execute：生成 JSON 计划，逐步执行，并归纳实际执行结果。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>

int run_plan_execute_demo() {
  try {
    auto model = std::make_shared<loop_demo::MockModel>();
    model->handle = [](const myagent::Json& request, unsigned round) {
      if (round == 1) {
        // Planner 没有工具权限，必须先返回结构化步骤。
        loop_demo::check(!myagent::json::if_contains(request, "tools"),
                         "规划阶段不得执行外部工具");
        return loop_demo::answer(R"({"steps":[{"id":"inspect","description":"查询项目文件列表"},{"id":"summarize","description":"汇总检查结果"}]})");
      }
      if (round == 2) return loop_demo::tool_call("local__list_files", R"({"directory":"src"})");
      if (round == 3) {
        loop_demo::check(loop_demo::has(std::string(
          request.at("input")[4].at("output").as_string()), "agent.cpp"),
          "执行阶段没有观察到 MCP 工具结果");
        return loop_demo::answer("已通过 MCP 查询 src 文件列表");
      }
      if (round == 4) return loop_demo::answer("已汇总模拟文件清单");
      loop_demo::check(!myagent::json::if_contains(request, "tools"),
                       "汇总阶段不得执行外部工具");
      return loop_demo::answer("已按计划完成两步：查询文件与汇总内容。");
    };
    myagent::Agent agent(loop_demo::config(), model);
    agent.tool("list_files", "列出某目录中的示例文件",
      [](const std::string& directory) {
        return directory == "src" ? "agent.cpp, loop_engine.cpp" : "";
      }, mymcp::arg<std::string>("directory"));
    myagent::LoopOptions options;
    options.max_plan_steps = 3;
    const auto result = agent.plan_and_execute("列出 src 文件并汇总", options);
    loop_demo::show(result);
    loop_demo::check(result.status == myagent::LoopStatus::Completed &&
                     result.plan.size() == 2 && result.tool_calls == 1,
                     "Plan-and-Execute 未完成所有步骤");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_plan_execute_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_plan_execute_demo(); }
