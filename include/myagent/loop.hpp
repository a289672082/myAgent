#pragma once
// Unified, synchronous Agent loops. Uses one Model/MCP/Skills runtime.
#include "mymcp/mcp.hpp"
#include "myagent/token_usage.hpp"
#include <map>
#include <set>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace myagent {
using mymcp::Json;

enum class LoopMode { ReAct, PlanAndExecute, Reflection, EvaluatorOptimizer };
enum class LoopStatus { Completed, LimitReached, Cancelled, Failed };

// 宿主可用真实工具证据验收一步，例如验证编译 exit_code == 0。
// passed 由应用代码产生；不能把模型自述的“成功”当成客观验证。
struct StepValidation {
  bool passed = false;
  std::string reason;
  Json evidence = mymcp::json::object{};
};

// Audio is transcribed once before entering the loop. Images are kept as
// Responses input_image parts, subject to the model's vision capabilities.
struct LoopInput {
  std::string text;
  std::vector<std::filesystem::path> images;
  std::filesystem::path audio;
};

struct LoopOptions {
  unsigned max_iterations = 8;
  unsigned max_model_calls = 32; // Total across all phases and tool calls.
  unsigned max_tool_calls = 32;  // Total across all phases.
  unsigned max_plan_steps = 8;
  unsigned max_replans = 1;
  unsigned max_step_retries = 0; // 默认不重试，避免重复执行写文件/提交等有副作用的工具。
  bool retry_steps_with_tools = false; // 仅明确授权后才允许自动重复调用工具的步骤。
  // Dedupe identical non-idempotent tool calls across replans/revisions.
  // Register external MCP write tools here; the built-in workspace_write is protected.
  std::set<std::string> non_idempotent_tools{"local__workspace_write"};
  unsigned max_revisions = 3;
  double acceptance_score = 0.85;
  std::string planner_model;   // Empty uses Config::model on the same backend.
  std::string evaluator_model; // Empty uses Config::model on the same backend.
  // Executor 必须返回明确的 status/summary。可选宿主验证器能查看
  // 本步骤工具原始返回（不会默认泄露到 trace）；返回 false 即进入失败恢复。
  // 不设置时，完成的步骤标记为 unverified（模型报告完成，但未经客观验证）。
  std::function<StepValidation(const Json& step, const Json& execution,
                               const Json& tool_results)> validate_step;
  bool commit_to_history = true;
  // Optional. Requires an OpenAIResponses backend and a speech-capable gateway.
  std::filesystem::path speech_output;
  std::string speech_voice = "alloy";
  // Both callbacks run synchronously on the calling thread. Not reentrant.
  std::function<bool()> cancelled;
  std::chrono::milliseconds timeout{0}; // 0=无限；调用间检查；HTTP 在 Config 内设置传输超时。
  std::function<void(const Json&)> on_event;
};

struct LoopResult {
  LoopStatus status = LoopStatus::Failed;
  std::string answer;
  std::string error;
  std::string audio_transcript;
  std::filesystem::path speech_output;
  Json plan = mymcp::json::array{};
  Json evaluations = mymcp::json::array{};
  Json trace = mymcp::json::array{};
  unsigned model_calls = 0;
  unsigned tool_calls = 0;
  unsigned iterations = 0;
  // 本次循环所有模型响应的真实 Token 消耗，含 Planner/Executor/Evaluator。
  // 缺失 usage 的调用计入 unreported_calls，可能低估实际费用。
  TokenUsage usage;
  std::map<std::string, TokenUsage> usage_by_phase; // plan/execute/reflect/evaluate...
  std::map<std::string, TokenUsage> usage_by_model; // 按实际请求的模型名汇总
};
} // namespace myagent
