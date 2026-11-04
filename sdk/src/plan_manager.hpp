#pragma once
// Plan-and-Execute 的内部计划管理器：模型提出计划，程序负责确认状态和依赖。
#include "myagent/loop.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace myagent::detail {

class PlanManager {
public:
  explicit PlanManager(unsigned max_steps,unsigned max_replans=1)
      : max_steps_(max_steps), max_history_steps_(static_cast<std::size_t>(
        std::min<std::uint64_t>(4096,static_cast<std::uint64_t>(max_steps)*
        (static_cast<std::uint64_t>(max_replans)+1)))) {}

  // 初始计划与重新规划都必须通过结构校验，失败不改变已有计划。
  void initialize(const std::string& model_json);
  void replace_pending(const std::string& model_json);

  // 返回当前可执行步骤下标；依赖未满足时不允许越过前置步骤执行。
  std::size_t next_ready() const;
  void start(std::size_t index);
  void finish(std::size_t index, const std::string& status,
              const std::string& summary, const std::string& verification,
              const Json& evidence);
  void retry(std::size_t index);
  const Json& steps() const { return steps_; }
  Json& steps() { return steps_; }

  static constexpr std::size_t no_step = static_cast<std::size_t>(-1);
private:
  Json parse_steps(const std::string& model_json, const std::set<std::string>& previous,
                   const std::set<std::string>& completed,
                   std::size_t allowance) const;
  unsigned max_steps_;
  std::size_t max_history_steps_;
  Json steps_ = mymcp::json::array{};
};

// 对模型报告做模式校验、拦截工具错误，再交由宿主回调做客观验收。
struct StepOutcome {
  std::string status;       // completed / failed / blocked
  std::string summary;
  std::string verification; // unverified / verified / failed
  Json evidence = mymcp::json::object{};
};
StepOutcome validate_step(const Json& step, const std::string& model_response,
                          const Json& tool_results, const LoopOptions& options);
} // namespace myagent::detail
