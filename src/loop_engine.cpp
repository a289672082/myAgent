#include "myagent/agent.hpp"
#include "loop_internal.hpp"
#include "plan_manager.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace myagent {
namespace {
std::string field_string(const Json& value, const std::string& name) {
  const Json* field = json::if_contains(value, name);
  if (!field || !field->is_string())
    throw std::runtime_error("Loop output is missing string field: " + name);
  return std::string(field->as_string());
}
Json parse_object(const std::string& text, const std::string& phase) {
  try {
    Json value = json::parse(text);
    if (!value.is_object()) throw std::runtime_error("expected a JSON object");
    return value;
  } catch (const std::exception& e) {
    throw std::runtime_error(phase + " must return a JSON object: " + e.what());
  }
}
void validate_options(const LoopOptions& options) {
  if (!options.max_iterations || !options.max_model_calls ||
      !options.max_plan_steps || !std::isfinite(options.acceptance_score) ||
      options.acceptance_score < 0.0 || options.acceptance_score > 1.0)
    throw std::invalid_argument("Invalid loop options: positive iteration/model/plan budgets and score 0..1 required");
}
void trace_event(LoopResult& result, const LoopOptions& options,
                 const std::string& phase, const std::string& event,
                 unsigned iteration) {
  Json item = json::object{{"phase", phase}, {"event", event},
                            {"iteration", iteration}};
  result.trace.push_back(item);
  if (options.on_event) options.on_event(item);
}
} // namespace

LoopResult Agent::run_loop(const LoopInput& input, LoopMode mode,
                           const LoopOptions& options) {
  LoopResult result;
  last_usage_ = {}; // 即使因取消/限制而失败，返回的 usage 仍是本次 Loop。
  Json working = history_; // All phases share an isolated working history.
  Json original_input = nullptr;
  Json latest_response = nullptr;
  try {
    validate_options(options);
    if (options.cancelled && options.cancelled())
      throw detail::LoopAbort(LoopStatus::Cancelled, "Agent loop cancelled");
    original_input = prepare_loop_input(input, result.audio_transcript);
    const std::string task = input.text.empty()
        ? (input.audio.empty() ? "Analyze the provided image(s)." :
                                "Analyze the provided audio.")
        : input.text;

    auto invoke = [&](const std::string& phase, Json content,
                      const std::string& prompt, bool tools_enabled,
                      unsigned max_rounds, const std::string& override_model = {},
                      Json* step_tool_results = nullptr) {
      trace_event(result, options, phase, "started", result.iterations);
      const auto turn = run_stage(std::move(content), working, prompt, override_model,
          tools_enabled, result.model_calls, result.tool_calls,
          options.max_model_calls, options.max_tool_calls, max_rounds,
          options.cancelled, &result.trace, options.on_event, phase,
          step_tool_results, &result.usage, &result.usage_by_phase,
          &result.usage_by_model);
      latest_response = turn.response;
      trace_event(result, options, phase, "completed", result.iterations);
      return turn.answer;
    };
    // Planner, critic and evaluator never receive callable tools by default.
    // Only the execution/draft phases can interact with MCP and Skill tools.
    // Plan-and-Execute 的计划必须用稳定 ID、依赖及验收条件描述。
    // Planner 不拥有工具，不能凭空声称已经读文件、编译或完成测试。
    constexpr auto plan_prompt =
      "You are the planner, not an executor. NEVER claim to have called tools. "
      "Return ONLY JSON: {\"steps\":[{\"id\":\"step-1\","
      "\"description\":\"...\",\"depends_on\":[],"
      "\"success_criteria\":\"...\"}]}. "
      "Dependencies must reference an earlier new step or an already completed step; "
      "use short unique IDs. Give observable acceptance criteria. No markdown.";
    constexpr auto critic_prompt =
      "Review the current draft against the original task and available evidence. "
      "Do not invent verification or new facts. Return ONLY JSON: "
      "{\"needs_revision\":false,\"feedback\":\"...\"}. "
      "Set needs_revision=true when you identify a concrete correctable issue.";
    constexpr auto evaluator_prompt =
      "Independently assess the draft using task requirements, accuracy, "
      "completeness and verifiable constraints. No tools are available. "
      "Return ONLY JSON: "
      "{\"score\":0.0,\"passed\":false,\"feedback\":\"...\",\"issues\":[]}. "
      "score must be a number between 0 and 1. Never claim external tests ran "
      "unless tool evidence is present.";

    switch (mode) {
    case LoopMode::ReAct: {
      // ReAct Loop — 推理行动循环
      //
      // 判断下一步 → 执行动作（调用 MCP/本地工具）→ 观察结果 → 再次判断。
      // 例如修改 C++ 项目：先列目录、读取代码、修改文件、运行测试，
      // 再依据实际错误决定修复、继续读取还是完成回答。
      //
      // ReAct 强调“观察结果改变下一步行动”，Tool Calling Loop 则是
      // 发起工具调用和回传结果的基础设施，两者共用 run_stage() 即可。
      // 只记录可观察的工具事件，不索要或公开模型内部隐藏推理。
      trace_event(result, options, "react", "started", 0);
      const auto turn = run_stage(original_input, working,
        "Solve the task by choosing necessary actions, observing tool results "
        "and providing a grounded final answer. Do not call tools without need.",
        {}, true, result.model_calls, result.tool_calls,
        options.max_model_calls, options.max_tool_calls,
        std::min(config_.max_tool_rounds, options.max_iterations - 1),
        options.cancelled, &result.trace, options.on_event, "react",
        nullptr, &result.usage, &result.usage_by_phase, &result.usage_by_model);
      latest_response = turn.response;
      result.answer = turn.answer;
      result.iterations = std::min(options.max_iterations, result.tool_calls + 1);
      trace_event(result, options, "react", "completed", result.iterations);
      break;
    }
    case LoopMode::PlanAndExecute: {
      // Plan-and-Execute — 规划执行循环
      //
      // 用户任务 → Planner 给出 JSON 计划 → Executor 执行一个步骤
      //          → Step Validator 检查工具结果/验收证据
      //          → 成功：按依赖继续下一步
      //          → 失败/阻塞：安全重试，或 Replanner 替换“未执行”的剩余计划
      //          → 综合真实执行结果，输出最终回答。
      //
      // 典型场景：先读 C++ 项目目录，再修改源文件，接着编译并运行测试。
      // 编译失败时不能简单把“修复完成”视为成功，必须根据工具退出码、
      // 项目自定义验收器等真实证据判定；并且不能重新执行已完成的写入步骤。
      //
      // 与 ReAct 的区别：这里先有明确的计划/依赖/验收标准，每一步有独立状态。
      // 它仍复用 run_stage() 的同一套 MCP Tool Calling 底层执行器。
      detail::PlanManager manager(options.max_plan_steps);
      manager.initialize(invoke("plan", original_input, plan_prompt, false,
                                0, options.planner_model));
      result.plan = manager.steps();
      unsigned replans = 0;
      while (true) {
        const auto next = manager.next_ready();
        if (next == detail::PlanManager::no_step) {
          // 没有可执行步骤，不代表成功：也可能是某项依赖无法满足。
          bool pending = false;
          for (const auto& step : manager.steps().array_range()) {
            const std::string status(step.at("status").as_string());
            if (status == "pending" || status == "ready") pending = true;
          }
          if (pending)
            throw std::runtime_error("Remaining plan steps have unsatisfied dependencies");
          break;
        }
        if (result.iterations >= options.max_iterations)
          throw detail::LoopAbort(LoopStatus::LimitReached, "Plan execution step limit reached");
        manager.start(next);
        result.plan = manager.steps();
        const auto id = field_string(manager.steps()[next], "id");
        const auto description = field_string(manager.steps()[next], "description");
        const std::string criteria(manager.steps()[next].at("success_criteria").as_string());
        ++result.iterations;
        Json tool_records = json::array{};
        const std::string execution = invoke("execute",
          "Execute ONLY step " + id + ": " + description +
          "\nAcceptance criteria: " + criteria + "\nOriginal goal: " + task +
          "\nReturn ONLY JSON: {\"status\":\"completed|failed|blocked\","
          "\"summary\":\"what happened\",\"evidence\":{}}. "
          "A tool error, missing verification or failed build must not be reported as completed.",
          "You are the executor of one plan step. You MAY use authorized tools. "
          "Observe the ACTUAL tool result before declaring success. "
          "Never repeat completed side effects.",
          true, config_.max_tool_rounds, {}, &tool_records);
        const auto outcome = detail::validate_step(manager.steps()[next],
                                                   execution, tool_records, options);
        manager.finish(next, outcome.status, outcome.summary, outcome.verification,
                       outcome.evidence);
        result.plan = manager.steps();
        trace_event(result, options, "plan", "step_" + outcome.status, result.iterations);
        if (outcome.status == "completed") continue;

        // 自动重试只适用于没有工具操作的步骤。已写文件/执行命令的步骤
        // 默认禁止自动重试，以免多次提交、重复支付或覆盖文件。
        const unsigned attempts = manager.steps()[next].at("attempts").as<unsigned>();
        const bool retry_allowed = attempts <= options.max_step_retries &&
          (tool_records.empty() || options.retry_steps_with_tools);
        if (retry_allowed) {
          manager.retry(next);
          result.plan = manager.steps();
          trace_event(result, options, "plan", "step_retry", result.iterations);
          continue;
        }
        if (replans >= options.max_replans)
          throw detail::LoopAbort(LoopStatus::LimitReached,
                                  "Step failed/blocked and replanning limit reached");
        ++replans;
        // 不更改已完成/失败/阻塞的历史；未执行的步骤会被 superseded，
        // 接下来由新的计划代替，而不是把旧步骤和新步骤一起盲目执行。
        const std::string replacement = invoke("replan",
          "Original task: " + task +
          "\nExecution history with statuses and evidence: " +
          json::serialize(manager.steps()) +
          "\nReturn ONLY REPLACEMENT remaining steps (new unique IDs), "
          "not the completed or obsolete pending steps. Respect dependencies, "
          "preserve completed work, and do not repeat non-idempotent side effects.",
          plan_prompt, false, 0, options.planner_model);
        manager.replace_pending(replacement);
        result.plan = manager.steps();
        trace_event(result, options, "plan", "replanned", result.iterations);
      }
      result.answer = invoke("synthesize",
        "Original task: " + task + "\nActual plan history: " +
          json::serialize(result.plan) +
          "\nReport completed, failed/blocked and superseded steps truthfully. "
          "A step marked unverified is only model-reported success, not a proven test pass.",
        "Summarize actual outcomes and verification evidence. Do not claim tests "
        "passed without tool/validator proof. No further tool calls.",
        false, 0);
      break;
    }
    case LoopMode::Reflection: {
      // Reflection Loop — 自我反思循环
      //
      // 生成草稿 → 自检（needs_revision + feedback）→ 修订草稿
      //          → 再次自检，直至通过或达到修订上限。
      // 例如生成 C++ 代码后，检查命名、异常处理、RAII 和潜在 Bug。
      // 同一个模型可分阶段生成/审查，但模型的自我评价不等于编译通过。
      // 当前仅初稿阶段可调用工具，反思/修订只修改文本；
      // 若需“写文件→真实编译→根据报错修复”的闭环，应使用执行工具的
      // Plan-and-Execute 或 ReAct，而不是假装反思阶段运行了测试。
      std::string draft = invoke("draft", original_input,
        "Draft a complete response. Call tools when needed to gather evidence. "
        "Do not fabricate a tool result.", true, config_.max_tool_rounds);
      for (unsigned revision = 0;;) {
        ++result.iterations;
        const auto critic = parse_object(invoke("reflect",
          "Original task: " + task + "\nDraft:\n" + draft, critic_prompt,
          false, 0, options.evaluator_model), "Reflection");
        const Json* needs = json::if_contains(critic, "needs_revision");
        if (!needs || !needs->is_bool())
          throw std::runtime_error("Reflection needs boolean needs_revision");
        const auto feedback = field_string(critic, "feedback");
        result.evaluations.push_back(critic);
        if (!needs->as_bool()) { result.answer = draft; break; }
        if (revision >= options.max_revisions ||
            result.iterations >= options.max_iterations) {
          result.answer = draft;
          throw detail::LoopAbort(LoopStatus::LimitReached, "Reflection revision limit reached");
        }
        ++revision;
        const auto revised = invoke("revise",
            "Original task: " + task + "\nDraft:\n" + draft +
            "\nCritique:\n" + feedback + "\nRevise the draft. No tools are available.",
            "Return a revised answer addressing justified feedback. "
            "Do not invent evidence or claim new tool executions.", false, 0);
        if (revised == draft) {
          result.answer = draft;
          throw detail::LoopAbort(LoopStatus::LimitReached, "Reflection made no progress");
        }
        draft = revised;
      }
      break;
    }
    case LoopMode::EvaluatorOptimizer: {
      // Evaluator-Optimizer — 评估优化循环
      //
      // 生成候选 → 独立评估 score/passed/issues → 按问题清单优化
      //          → 再评估，直到通过阈值且无问题，或达到修订上限。
      // 相比 Reflection 的“自我检查”，这里将评价标准显式结构化，
      // 也可指定不同 evaluator_model，但仍可使用同一个后端。
      // 评分来自模型，不等价于单元测试或外部质量保障。
      // 评估与优化阶段禁用工具，避免评估器产生未授权的文件修改。
      std::string draft = invoke("generate", original_input,
        "Generate a complete candidate solution. Gather evidence with tools "
        "if needed and do not claim unperformed checks.", true, config_.max_tool_rounds);
      for (unsigned revision = 0;;) {
        ++result.iterations;
        auto evaluation = parse_object(invoke("evaluate",
          "Original task: " + task + "\nCandidate:\n" + draft,
          evaluator_prompt, false, 0, options.evaluator_model), "Evaluator");
        const Json* score = json::if_contains(evaluation, "score");
        const Json* passed = json::if_contains(evaluation, "passed");
        const Json* issues = json::if_contains(evaluation, "issues");
        if (!score || !score->is_number() || !passed || !passed->is_bool() ||
            !issues || !issues->is_array())
          throw std::runtime_error("Evaluator requires score, passed and issues fields");
        const auto value = score->as<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
          throw std::runtime_error("Evaluator score must be between 0 and 1");
        const auto feedback = field_string(evaluation, "feedback");
        result.evaluations.push_back(evaluation);
        if (passed->as_bool() && value >= options.acceptance_score && issues->empty()) {
          result.answer = draft;
          break;
        }
        if (revision >= options.max_revisions ||
            result.iterations >= options.max_iterations) {
          result.answer = draft;
          throw detail::LoopAbort(LoopStatus::LimitReached, "Evaluator optimization limit reached");
        }
        ++revision;
        const auto revised = invoke("optimize",
          "Original task: " + task + "\nCurrent candidate:\n" + draft +
          "\nEvaluator feedback:\n" + feedback + "\nIssues: " +
          json::serialize(*issues) + "\nImprove the solution without tools.",
          "Produce an improved candidate. Address all legitimate issues, "
          "and do not claim any unperformed validation.", false, 0);
        if (revised == draft) {
          result.answer = draft;
          throw detail::LoopAbort(LoopStatus::LimitReached, "Optimizer made no progress");
        }
        draft = revised;
      }
      break;
    }
    }

    // Optional final speech output is generated only for successful runs.
    if (!options.speech_output.empty()) {
      if (options.cancelled && options.cancelled())
        throw detail::LoopAbort(LoopStatus::Cancelled, "Agent loop cancelled");
      speak(result.answer, options.speech_output, options.speech_voice);
      result.speech_output = options.speech_output;
    }
    if (options.commit_to_history) {
      Json committed = history_;
      committed.push_back(json::object{{"role", "user"}, {"content", original_input}});
      committed.push_back(json::object{{"role", "assistant"}, {"content", result.answer}});
      if (committed.size() > config_.max_history_items)
        throw detail::LoopAbort(LoopStatus::LimitReached, "Conversation history limit reached");
      history_ = std::move(committed);
      last_response_ = std::move(latest_response);
    }
    result.status = LoopStatus::Completed;
    return result;
  } catch (const detail::LoopAbort& e) {
    result.status = e.status;
    result.error = e.what();
  } catch (const std::exception& e) {
    result.status = LoopStatus::Failed;
    result.error = e.what();
  }
  // No commit on error: partial tool side effects cannot be automatically reverted.
  return result;
}
LoopResult Agent::run_loop(const std::string& task, LoopMode mode,
                           const LoopOptions& options) {
  return run_loop(LoopInput{task, {}, {}}, mode, options);
}
LoopResult Agent::react(const LoopInput& input, const LoopOptions& options) {
  return run_loop(input, LoopMode::ReAct, options);
}
LoopResult Agent::react(const std::string& task, const LoopOptions& options) {
  return run_loop(task, LoopMode::ReAct, options);
}
LoopResult Agent::plan_and_execute(const LoopInput& input, const LoopOptions& options) {
  return run_loop(input, LoopMode::PlanAndExecute, options);
}
LoopResult Agent::plan_and_execute(const std::string& task, const LoopOptions& options) {
  return run_loop(task, LoopMode::PlanAndExecute, options);
}
LoopResult Agent::reflect(const LoopInput& input, const LoopOptions& options) {
  return run_loop(input, LoopMode::Reflection, options);
}
LoopResult Agent::reflect(const std::string& task, const LoopOptions& options) {
  return run_loop(task, LoopMode::Reflection, options);
}
LoopResult Agent::evaluate_and_optimize(const LoopInput& input,
                                        const LoopOptions& options) {
  return run_loop(input, LoopMode::EvaluatorOptimizer, options);
}
LoopResult Agent::evaluate_and_optimize(const std::string& task,
                                        const LoopOptions& options) {
  return run_loop(task, LoopMode::EvaluatorOptimizer, options);
}
} // namespace myagent
