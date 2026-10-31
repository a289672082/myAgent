// ReAct：模型根据图片和任务选择工具、观察结果并回答（离线 Mock）。
#include "loop_demo_support.hpp"
#include <filesystem>
#include <iostream>
#include <memory>

int run_react_demo() {
  try {
    auto model = std::make_shared<loop_demo::MockModel>();
    model->handle = [](const myagent::Json& request, unsigned round) {
      if (round == 1) {
        // 多模态任务：图片进入 Responses input_image，不重复编写图片编码逻辑。
        const auto& parts = request.at("input")[0].at("content");
        loop_demo::check(parts.size() == 2, "应同时传递文字和图片");
        loop_demo::check(parts[1].at("type").as_string() == "input_image",
                         "图片必须编码成 input_image");
        return loop_demo::tool_call("local__add", R"({"a":2,"b":3})");
      }
      const auto& history = request.at("input");
      loop_demo::check(loop_demo::has(std::string(history[2].at("output").as_string()), "5"),
                       "工具结果没有返回模型");
      return loop_demo::answer("示例图片已经传入，计算工具返回 5。");
    };
    myagent::Agent agent(loop_demo::config(), model);
    // 本地 MCP 工具：和现有 ask() 复用同一个注册接口。
    agent.tool("add", "计算两个整数的和", [](int a, int b) { return a + b; },
               mymcp::arg<int>("a"), mymcp::arg<int>("b"));
    myagent::LoopInput input;
    input.text = "阅读这张示例图片，并计算 2+3";
    input.images = {std::filesystem::path(MYAGENT_LOOP_DEMO_DIR) / "sample.png"};
    myagent::LoopOptions options;
    options.max_iterations = 5;
    const auto result = agent.react(input, options);
    loop_demo::show(result);
    loop_demo::check(result.status == myagent::LoopStatus::Completed &&
                     result.tool_calls == 1 && result.model_calls == 2,
                     "ReAct 推理行动循环未完成");
    loop_demo::check(agent.history().size() == 2, "主对话仅保存用户请求和最终答案");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_react_demo failed: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_react_demo(); }
