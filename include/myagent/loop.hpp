#pragma once
// Unified, synchronous Agent loops. Uses one Model/MCP/Skills runtime.
#include "mymcp/mcp.hpp"
#include "myagent/token_usage.hpp"
#include <map>
#include <optional>
#include <set>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace myagent {
using mymcp::Json;

// LoopMode 表示 Agent 任务的执行策略，而非模型类型或多 Agent 的协作架构。
// ReWOO：一次规划多个确定的 MCP 工具调用 → 按依赖执行（中途不再问模型）→ 汇总。
// ExecuteVerifyRepair：模型执行/修改 → 宿主真实验证（如编译、测试）→ 失败反馈 → 修复并重验。
// 为兼容已有 Checkpoint，保留前四项的枚举值不变，只在末尾添加新模式。
enum class LoopMode {
  ReAct, PlanAndExecute, Reflection, EvaluatorOptimizer,
  ReWOO, ExecuteVerifyRepair
};
enum class LoopStatus { Completed, LimitReached, Cancelled, Failed, Paused };

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

// 工具审批可返回 Pause：立即保存执行现场并把控制权交回 UI，而不是阻塞线程。
enum class ToolDecision { Approve, Deny, Pause };
struct ToolApprovalRequest {
  std::string name;
  std::string call_id;
  Json arguments = mymcp::json::object{};
};

// 由宿主产生的客观验证证据，不接受模型声称“测试已通过”作为证据。
// passed：编译/测试/静态检查是否真正满足目标；reason：失败原因/日志摘要；
// evidence：可选退出码、测试统计等结构化证据；可能含敏感信息，宿主需自行脱敏。
struct ExecutionVerification {
  bool passed = false;
  std::string reason;
  Json evidence = mymcp::json::object{};
};

struct LoopOptions {
  unsigned max_iterations = 8;
  unsigned max_model_calls = 32; // Total across all phases and tool calls.
  unsigned max_tool_calls = 32;  // Total across all phases.
  // Plan-and-Execute / ReWOO 共用的最大计划步骤数；ReWOO 运行时不可扩充计划。
  // ReWOO 每一步都按 ToolPolicy / on_tool_approval 审批，失败立即终止而不擅自重试。
  unsigned max_plan_steps = 8;
  unsigned max_replans = 1;
  unsigned max_step_retries = 0; // 默认不重试，避免重复执行写文件/提交等有副作用的工具。
  bool retry_steps_with_tools = false; // 仅明确授权后才允许自动重复调用工具的步骤。
  // Dedupe identical non-idempotent tool calls across replans/revisions.
  // Register external MCP write tools here; the built-in workspace_write is protected.
  std::set<std::string> non_idempotent_tools{"local__workspace_write"};
  // Fail closed: an unknown MCP tool might write, delete, purchase or commit.
  // Explicitly opt read-only tools out, after verifying their semantics.
  bool protect_unclassified_tools = true;
  std::set<std::string> idempotent_tools{
      "local__workspace_read","local__workspace_list"};
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
  // 设定后每次模型响应/工具意图/工具结果都会原子写入文件；仅支持文本任务。
  std::filesystem::path checkpoint_path;
  bool resume_checkpoint = false;
  // 对每个尚未执行的工具决定批准、拒绝或暂停。未设置时沿用 ToolPolicy。
  std::function<ToolDecision(const ToolApprovalRequest&)> on_tool_approval;
  // 崩溃后若工具已经开始而未确认结果，只允许宿主核实并提供结果，绝不重放。
  std::function<std::optional<std::string>(const ToolApprovalRequest&)> reconcile_tool;
  // 面向 UI 的公开文本增量。内置 HTTP 模型使用真实 SSE；不输出隐藏推理。
  std::function<void(const std::string& phase,const std::string& delta)> on_text_delta;
  // Optional. Requires an OpenAIResponses backend and a speech-capable gateway.
  std::filesystem::path speech_output;
  std::string speech_voice = "alloy";
  // Both callbacks run synchronously on the calling thread. Not reentrant.
  std::function<bool()> cancelled;
  std::chrono::milliseconds timeout{0}; // 0=无限；调用间检查；HTTP 在 Config 内设置传输超时。
  std::function<void(const Json&)> on_event;
  // 安全追踪仅包含模型/工具名、耗时、错误类别，不复制提示词及响应正文。
  std::function<void(const Json&)> on_span;
  std::string trace_id; // 可选的外部 Trace ID（32 个十六进制字符），用于多 Agent 关联。
  std::string task_id; // 可选的宿主任务 ID；团队并行任务使用，回调可关联具体委派。
  // 嵌套子 Agent 可复用原子全局配额；false 时拒绝新的实际模型/工具操作。
  std::function<bool()> allow_model_request;
  std::function<bool()> allow_tool_execution;
  // 每条真实模型响应计入全局预算，Checkpoint 回放不会再次计费。
  std::function<void(const std::string&,const TokenUsage&,double)> on_model_usage;
  std::filesystem::path otel_jsonl_path; // 可选 OTEL 兼容 span JSONL，调用方负责保护文件。
  // 以下是新增模式的配置，附加到结构体末尾，不改变旧 LoopOptions 聚合初始化顺序。
  // Execute-Verify-Repair：最多允许失败后做几轮修复；0=只执行一次并验证一次。
  // 每轮修复都复用 MCP 工具权限与模型/工具调用预算；同一非幂等工具意图不能自动重放。
  unsigned max_repairs = 2;
  // Execute-Verify-Repair：必须由宿主实现真正的构建/测试/检查过程。
  // 流程：Executor 返回 JSON 结果与工具观察 → 此回调检查真实状态 →
  // 返回 passed=true 才能结束；失败时 reason/evidence 用于指导下一轮修复。
  // 回调执行在当前调用线程，需自行保证安全边界和超时；不允许任意重放有副作用的命令。
  // 第三个参数 attempt 从 1 开始，后续每轮修复 +1。
  std::function<ExecutionVerification(const Json& execution,
      const Json& tool_results, unsigned attempt)> verify_execution;
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
  double estimated_cost_usd=0;
  std::map<std::string,double> estimated_cost_by_model;
  std::string trace_id;
};
} // namespace myagent
