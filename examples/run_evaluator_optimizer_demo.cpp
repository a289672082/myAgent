// Evaluator-Optimizer：生成候选、独立评分、反馈优化并重新评估。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>

int run_evaluator_optimizer_demo() {
  try {
    auto model = std::make_shared<loop_demo::MockModel>();
    model->handle = [](const myagent::Json& request, unsigned round) {
      if (round == 1) return loop_demo::answer("auto* ptr = new int(3);");
      if (round == 2) {
        loop_demo::check(request.at("model").as_string() == "quality-reviewer",
                         "独立评估模型名称未生效");
        loop_demo::check(!myagent::json::if_contains(request, "tools"),
                         "评估阶段不得拥有写入工具权限");
        return loop_demo::answer(R"({"score":0.4,"passed":false,"feedback":"需要 RAII 管理对象生命周期","issues":["memory leak"]})");
      }
      if (round == 3) return loop_demo::answer("auto ptr = std::make_unique<int>(3);");
      return loop_demo::answer(R"({"score":0.95,"passed":true,"feedback":"符合资源管理要求","issues":[]})");
    };
    myagent::Agent agent(loop_demo::config(), model);
    myagent::LoopOptions options;
    options.acceptance_score = 0.9;
    options.evaluator_model = "quality-reviewer";
    const auto result = agent.evaluate_and_optimize("编写安全的 C++ 对象创建代码", options);
    loop_demo::show(result);
    loop_demo::check(result.status == myagent::LoopStatus::Completed &&
                     result.evaluations.size() == 2 &&
                     loop_demo::has(result.answer, "make_unique"),
                     "Evaluator-Optimizer 没有达到质量要求");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_evaluator_optimizer_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_evaluator_optimizer_demo(); }
