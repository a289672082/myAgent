#pragma once
// 独立示例共用离线 Mock，不使用真实 API 或环境变量。
#include "myagent/agent.hpp"
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace loop_demo {
using myagent::Json;
namespace json = myagent::json;
inline void check(bool ok, const std::string& detail) {
  if (!ok) throw std::runtime_error("Loop demo check failed: " + detail);
}
inline bool has(const std::string& s, const std::string& fragment) {
  return s.find(fragment) != std::string::npos;
}
inline Json answer(const std::string& text) {
  return json::object{{"status", "completed"}, {"output", json::array{
    json::object{{"type", "message"}, {"role", "assistant"},
                 {"content", json::array{
                   json::object{{"type", "output_text"}, {"text", text}}
                 }}}}}};
}
inline Json tool_call(const std::string& name, const std::string& arguments) {
  return json::object{{"status", "completed"}, {"output", json::array{
    json::object{{"type", "function_call"}, {"name", name}, {"call_id", "demo-call"},
                 {"arguments", arguments}}}}};
}
struct MockModel final : myagent::Model {
  unsigned round = 0;
  std::function<Json(const Json&, unsigned)> handle;
  Json create(const Json& request) override {
    return handle(request, ++round);
  }
};
inline myagent::Config config() {
  myagent::Config c;
  // 示例配置是明确的局部值；MockModel 不会请求以下网络地址。
  c.base_url = "https://api.openai.com";
  c.api_key = "sk-example-not-a-real-key";
  c.model = "gpt-4.1-mini";
  c.auto_discover_skills = false;
  c.auto_skill_selection = false;
  return c;
}
inline void show(const myagent::LoopResult& result) {
  std::cout << "status=" << static_cast<int>(result.status)
            << " model_calls=" << result.model_calls
            << " tool_calls=" << result.tool_calls
            << " iterations=" << result.iterations << '\n'
            << "answer: " << result.answer << '\n';
  if (!result.error.empty()) std::cerr << result.error << '\n';
}
} // namespace loop_demo
