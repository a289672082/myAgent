#include "myagent/agent.hpp"
#include "loop_internal.hpp"
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
Json plan_steps(const std::string& text, unsigned remaining) {
  const auto parsed = parse_object(text, "Planner");
  const Json* steps = json::if_contains(parsed, "steps");
  if (!steps || !steps->is_array() || steps->empty() || steps->size() > remaining)
    throw std::runtime_error("Planner must return 1..max_plan_steps JSON steps");
  Json validated = json::array{};
  std::set<std::string> names;
  for (const auto& step : steps->array_range()) {
    if (!step.is_object()) throw std::runtime_error("Plan steps must be objects");
    const auto id = field_string(step, "id");
    const auto task = field_string(step, "description");
    if (id.empty() || id.size() > 64 || task.empty() || task.size() > 2048 ||
        !names.insert(id).second)
      throw std::runtime_error("Invalid plan step id/description or duplicate id");
    validated.push_back(json::object{{"id", id}, {"description", task},
                                      {"status", "pending"}, {"result", ""}});
  }
  return validated;
}
bool blocked_step(const std::string& output) {
  // Optional structured execution response: blocked steps prompt replanning.
  // Free-form output is considered completed and remains the summary.
  try {
    const auto json_output = json::parse(output);
    if (!json_output.is_object()) return false;
    const Json* status = json::if_contains(json_output, "status");
    return status && status->is_string() && status->as_string() == "blocked";
  } catch (const std::exception&) {
    return false;
  }
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
                      unsigned max_rounds, const std::string& override_model = {}) {
      trace_event(result, options, phase, "started", result.iterations);
      const auto turn = run_stage(std::move(content), working, prompt, override_model,
          tools_enabled, result.model_calls, result.tool_calls,
          options.max_model_calls, options.max_tool_calls, max_rounds,
          options.cancelled, &result.trace, options.on_event, phase);
      latest_response = turn.response;
      trace_event(result, options, phase, "completed", result.iterations);
      return turn.answer;
    };
    // Planner, critic and evaluator never receive callable tools by default.
    // Only the execution/draft phases can interact with MCP and Skill tools.
    constexpr auto plan_prompt =
      "You are a planner, not an executor. Do not claim to have run tools. "
      "Return ONLY a JSON object of this shape: "
      "{\"steps\":[{\"id\":\"step-1\",\"description\":\"...\"}]}. "
      "Create a short ordered plan; no markdown.";
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
      // Each tool round is a decision/action/observation cycle. A final answer
      // is a terminal observation; hidden model reasoning is never exposed.
      trace_event(result, options, "react", "started", 0);
      const auto turn = run_stage(original_input, working,
        "Solve the task by choosing necessary actions, observing tool results "
        "and providing a grounded final answer. Do not call tools without need.",
        {}, true, result.model_calls, result.tool_calls,
        options.max_model_calls, options.max_tool_calls,
        std::min(config_.max_tool_rounds, options.max_iterations - 1),
        options.cancelled, &result.trace, options.on_event, "react");
      latest_response = turn.response;
      result.answer = turn.answer;
      result.iterations = std::min(options.max_iterations, result.tool_calls + 1);
      trace_event(result, options, "react", "completed", result.iterations);
      break;
    }
    case LoopMode::PlanAndExecute: {
      const auto first_plan = invoke("plan", original_input, plan_prompt, false,
                                     0, options.planner_model);
      auto pending = plan_steps(first_plan, options.max_plan_steps);
      std::set<std::string> ids;
      for (const auto& item : pending.array_range())
        ids.insert(field_string(item, "id"));
      for (const auto& item : pending.array_range()) result.plan.push_back(item);
      unsigned next = 0, replans = 0;
      while (next < result.plan.size()) {
        if (result.iterations >= options.max_iterations)
          throw detail::LoopAbort(LoopStatus::LimitReached, "Plan step limit reached");
        const auto id = field_string(result.plan[next], "id");
        const auto description = field_string(result.plan[next], "description");
        ++result.iterations;
        const std::string outcome = invoke("execute", "Execute the next plan step " +
             id + ": " + description + "\n\nOriginal goal: " + task +
             "\nReport the actual outcome. If genuinely blocked, return JSON "
             "{\"status\":\"blocked\",\"reason\":\"...\"}.",
             "You are executing exactly one plan step. Use permitted tools if "
             "needed; do not re-run completed side-effecting steps.", true,
             config_.max_tool_rounds);
        result.plan[next]["result"] = outcome;
        if (blocked_step(outcome)) {
          result.plan[next]["status"] = "blocked";
          if (replans >= options.max_replans)
            throw detail::LoopAbort(LoopStatus::LimitReached, "Replanning limit reached");
          if (result.plan.size() >= options.max_plan_steps)
            throw detail::LoopAbort(LoopStatus::LimitReached, "Plan step budget exhausted");
          ++replans;
          // Keep the completed / blocked steps immutable; append only new steps.
          const std::string replanned = invoke("replan",
              "Original task: " + task + "\nCurrent plan and outcomes: " +
                json::serialize(result.plan) +
                "\nProvide new remaining steps only; never repeat prior side effects.",
              plan_prompt, false, 0, options.planner_model);
          auto added = plan_steps(replanned,
                                 options.max_plan_steps - static_cast<unsigned>(result.plan.size()));
          for (const auto& step : added.array_range()) {
            const auto new_id = field_string(step, "id");
            if (!ids.insert(new_id).second)
              throw std::runtime_error("Replanned step IDs must be new and unique");
            result.plan.push_back(step);
          }
        } else {
          result.plan[next]["status"] = "completed";
        }
        ++next;
      }
      result.answer = invoke("synthesize",
        "Original task: " + task + "\nExecuted steps and actual results: " +
          json::serialize(result.plan) + "\nSummarize only supported outcomes.",
        "Provide a final answer grounded in completed steps. Mention any "
        "blocked steps or unverifiable outcomes. No more tool calls.",
        false, 0);
      break;
    }
    case LoopMode::Reflection: {
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
