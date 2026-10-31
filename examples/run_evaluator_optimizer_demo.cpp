// Evaluator-Optimizer — 评估优化循环
//
// 生成候选 → 按明确标准评分（score/passed/issues）→ 优化候选 → 再次评估。
// 与 Reflection 的一般自我检查不同：这里必须有结构化评价条件，
// 只有 passed=true、分数达到阈值、问题清单为空时才结束。
// 可以让一个模型负责生成，另一个模型负责评价；但模型评分不是客观测试结果。
// 这个离线示例用 Mock 模型实现逐轮打分和优化，无真实 API 请求。
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
