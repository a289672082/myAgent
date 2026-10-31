// Reflection：先生成草稿，再由模型自检、修订，直到不再需要修订。
#include "loop_demo_support.hpp"
#include <iostream>
#include <memory>

int run_reflection_demo() {
  try {
    auto model = std::make_shared<loop_demo::MockModel>();
    model->handle = [](const myagent::Json& request, unsigned round) {
      if (round == 1) return loop_demo::answer("std::vector 访问元素不需要检查索引。");
      if (round == 2) {
        loop_demo::check(!myagent::json::if_contains(request, "tools"),
                         "反思阶段不得调用工具");
        return loop_demo::answer(R"JSON({"needs_revision":true,"feedback":"应当检查索引，或使用 at()"} )JSON");
      }
      if (round == 3) return loop_demo::answer("C++ vector 可使用 at() 做边界检查。");
      return loop_demo::answer(R"({"needs_revision":false,"feedback":"边界检查建议已补充"} )");
    };
    myagent::Agent agent(loop_demo::config(), model);
    myagent::LoopOptions options;
    options.max_revisions = 2;
    const auto result = agent.reflect("如何安全读取 std::vector 元素？", options);
    loop_demo::show(result);
    loop_demo::check(result.status == myagent::LoopStatus::Completed &&
                     result.evaluations.size() == 2 &&
                     loop_demo::has(result.answer, "at()"),
                     "Reflection 未正确反思修订");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_reflection_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_reflection_demo(); }
