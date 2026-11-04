#include "myagent/agent.hpp"
#include "loop_internal.hpp"
#include "execution_journal.hpp"
#include "plan_manager.hpp"
#include "trace_spans.hpp"
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
  const std::lock_guard<std::recursive_mutex> session_guard(session_mutex_);
  LoopResult result;
  const auto turn_started=std::chrono::steady_clock::now();
  // 指针仅在持有 session_mutex_ 的本次 Loop 内有效；异常也必须清理。
  struct ResetActive {
    std::function<void()> reset;
    ~ResetActive(){reset();}
  } reset_active{[&]{active_journal_=nullptr;active_loop_options_=nullptr;
                       active_trace_id_.clear();active_turn_id_.clear();
                       active_task_id_.clear();}};
  std::unique_ptr<detail::ExecutionJournal> journal;
  Json compacted_history_prefix = json::array{}; // 记录本次 Loop 被压缩的原始会话前缀。
  std::set<std::string> executed_effects; // 跨阶段防止非幂等调用被重新规划重复执行。
  last_usage_ = {}; // 即使因取消/限制而失败，返回的 usage 仍是本次 Loop。
  retries_used_=0;last_cost_usd_=0;
  active_trace_id_=options.trace_id.empty()?detail::trace_hex(16):options.trace_id;
  if(active_trace_id_.size()!=32 ||
     !std::all_of(active_trace_id_.begin(),active_trace_id_.end(),[](char c) {
       return (c>='0'&&c<='9')||(c>='a'&&c<='f');
     }))
    throw std::invalid_argument("Trace ID must be 32 lowercase hex characters");
  result.trace_id=active_trace_id_;
  active_turn_id_=detail::trace_hex(16);
  active_task_id_=options.task_id;
  Json working = history_; // All phases share an isolated working history.
  Json original_input = nullptr;
  Json latest_response = nullptr;
  try {
    validate_options(options);
    if(options.resume_checkpoint && options.checkpoint_path.empty())
      throw std::invalid_argument("resume_checkpoint requires checkpoint_path");
    if(!options.checkpoint_path.empty()) {
      // V1 快照只接受文本任务：多媒体文件内容会变化，不允许无校验重放。
      if(!input.images.empty() || !input.audio.empty())
        throw std::invalid_argument("Checkpoint currently supports text-only LoopInput");
      journal=std::make_unique<detail::ExecutionJournal>(
          options.checkpoint_path,options.resume_checkpoint,input.text,mode,
          history_,config_.model);
      active_journal_=journal.get();
    }
    active_loop_options_=&options;
    const auto deadline = options.timeout.count() > 0
        ? std::chrono::steady_clock::now() + options.timeout
        : std::chrono::steady_clock::time_point::max();
    const auto cancelled = [&]() {
      return (options.cancelled && options.cancelled()) ||
             std::chrono::steady_clock::now() >= deadline;
    };
    const std::function<std::chrono::milliseconds()> remaining_time=
        deadline==std::chrono::steady_clock::time_point::max()
        ? std::function<std::chrono::milliseconds()>{}
        : std::function<std::chrono::milliseconds()>([&](){
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline-std::chrono::steady_clock::now());
          });
    if (cancelled())
      throw detail::LoopAbort(LoopStatus::Cancelled, "Agent loop cancelled");
    if(pause_requested_.load(std::memory_order_acquire))
      throw detail::LoopAbort(LoopStatus::Paused,"Agent paused before task preparation");
    original_input = prepare_loop_input(input, result.audio_transcript,remaining_time);
    // 仅公开宿主原始文本与媒体文件数量，不泄露二进制 data URL。
    emit_conversation(json::object{{"event","user_message"},{"agent_id",config_.agent_id},
      {"phase","loop"},{"content",input.text},{"image_count",input.images.size()},
      {"has_audio",!input.audio.empty()},{"trace_id",result.trace_id}});
    if(cancelled())
      throw detail::LoopAbort(LoopStatus::Cancelled,"Agent deadline exceeded during input preparation");
    const std::string task = input.text.empty()
        ? (input.audio.empty() ? "Analyze the provided image(s)." :
                                "Analyze the provided audio.")
        : input.text;

    auto invoke = [&](const std::string& phase, Json content,
                      const std::string& prompt, bool tools_enabled,
                      unsigned max_rounds, const std::string& override_model = {},
                      Json* step_tool_results = nullptr) {
      if(pause_requested_.load(std::memory_order_acquire))
        throw detail::LoopAbort(LoopStatus::Paused,"Agent paused before phase: "+phase);
      trace_event(result, options, phase, "started", result.iterations);
      const auto turn = run_stage(std::move(content), working, prompt, override_model,
          tools_enabled, result.model_calls, result.tool_calls,
          options.max_model_calls, options.max_tool_calls, max_rounds,
          cancelled, &result.trace, options.on_event, phase,
          step_tool_results, &result.usage, &result.usage_by_phase,
          &result.usage_by_model, &compacted_history_prefix,
          &executed_effects, &options.non_idempotent_tools,
          &options.idempotent_tools, options.protect_unclassified_tools,
          remaining_time);
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
      if(pause_requested_.load(std::memory_order_acquire))
        throw detail::LoopAbort(LoopStatus::Paused,"Agent paused before ReAct");
      trace_event(result, options, "react", "started", 0);
      const auto turn = run_stage(original_input, working,
        "Solve the task by choosing necessary actions, observing tool results "
        "and providing a grounded final answer. Do not call tools without need.",
        {}, true, result.model_calls, result.tool_calls,
        options.max_model_calls, options.max_tool_calls,
        std::min(config_.max_tool_rounds, options.max_iterations - 1),
        cancelled, &result.trace, options.on_event, "react",
        nullptr, &result.usage, &result.usage_by_phase, &result.usage_by_model,
        &compacted_history_prefix, &executed_effects,
        &options.non_idempotent_tools, &options.idempotent_tools,
        options.protect_unclassified_tools, remaining_time);
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
      detail::PlanManager manager(options.max_plan_steps,options.max_replans);
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
    case LoopMode::ReWOO: {
      // ReWOO — 一次规划、确定性执行工具、最后统一汇总
      //
      // ① Planner：模型只产生 JSON 工具计划（id/tool/arguments/depends_on），
      //    没有工具调用权限，也不能声称已执行过任何实际操作。
      // ② Executor：在开始执行前完整验证计划的工具名、参数、依赖和预算；
      //    此后 C++ 按顺序调用 MCP，不再询问模型下一步干什么。
      // ③ Solver：收集真实 MCP 结果，最后只调用一次模型生成回答。
      //
      // ReAct 是“调用→观察→再决策”；ReWOO 则是“规划→执行固定计划→汇总”。
      // 只支持预先确定的 JSON 参数；运行时不允许工具结果偷偷改写后续计划。
      // 有写操作的工具继续受 ToolPolicy、人工审批、Checkpoint 和重复调用保护。
      if (options.max_model_calls < 2)
        throw detail::LoopAbort(LoopStatus::LimitReached,
          "ReWOO requires a planner and a solver model call");
      if (options.max_tool_calls == 0)
        throw detail::LoopAbort(LoopStatus::LimitReached,
          "ReWOO requires a positive tool call budget");
      const auto mcp_opts=[&]() {
        if(!config_.mcp_timeout_seconds || !config_.max_mcp_response_bytes ||
           config_.max_mcp_response_bytes > 4u*1024u*1024u)
          throw std::invalid_argument("Invalid MCP transport limits");
        mymcp::RequestOptions opts;
        opts.timeout=std::chrono::seconds(config_.mcp_timeout_seconds);
        if(remaining_time) {
          const auto left=remaining_time();
          if(left.count()<=0)
            throw detail::LoopAbort(LoopStatus::Cancelled,"ReWOO MCP deadline exceeded");
          opts.timeout=std::min(opts.timeout,left);
        }
        opts.cancelled=cancelled;
        opts.max_response_bytes=config_.max_mcp_response_bytes;
        return opts;
      };
      std::map<std::string,Route> routes;
      const Json tool_catalog=tool_definitions(routes,mcp_opts());
      if(routes.empty())
        throw std::runtime_error("ReWOO requires at least one authorized MCP tool");
      // 不把 Skill 加载/读取工具直接暴露为 ReWOO 工具计划：它们依赖当前
      // 回合的 Skill 激活状态，必须留给原来的 run_stage 执行。
      Json available=json::array{};
      for(const auto& tool:tool_catalog.array_range()) {
        const std::string name=field_string(tool,"name");
        if(routes.count(name)) available.push_back(tool);
      }
      const std::string catalog=json::serialize(available);
      if(catalog.size()>256*1024)
        throw std::runtime_error("ReWOO catalog too large for planning prompt");
      const std::string plan_text=invoke("rewoo_plan",original_input,
        "You are a deterministic tool planner. You cannot call tools. "
        "Return ONLY JSON: {\"steps\":[{\"id\":\"s1\",\"tool\":\"name\","
        "\"arguments\":{},\"depends_on\":[]}]}. "
        "Use ONLY names in this tool catalog: "+catalog+
        ". Each step must use a tool once with fully specified JSON arguments. "
        "Dependencies must refer only to earlier step IDs. "
        "Do not invent observations. Avoid write tools unless essential. "
        "No variables or dynamic placeholders.",false,0,options.planner_model);
      const Json plan=parse_object(plan_text,"ReWOO planner");
      const Json* steps=json::if_contains(plan,"steps");
      if(!steps || !steps->is_array() || steps->empty() ||
         steps->size()>options.max_plan_steps)
        throw std::runtime_error("ReWOO plan must contain 1..max_plan_steps entries");
      if(steps->size()>options.max_iterations || steps->size()>options.max_tool_calls)
        throw detail::LoopAbort(LoopStatus::LimitReached,
                                 "ReWOO plan exceeds step/tool budget");
      // 先验证完整计划，后执行第一条工具，避免运行到一半才发现非法工具或依赖。
      std::set<std::string> seen;
      result.plan=json::array{};
      for(const auto& step:steps->array_range()) {
        if(!step.is_object())throw std::runtime_error("ReWOO step must be object");
        const auto id=field_string(step,"id");
        const auto name=field_string(step,"tool");
        const auto* args=json::if_contains(step,"arguments");
        const auto* deps=json::if_contains(step,"depends_on");
        if(id.empty() || id.size()>64 || !seen.insert(id).second ||
           !routes.count(name) || !args || !args->is_object() ||
           !deps || !deps->is_array() || deps->size()>options.max_plan_steps ||
           json::serialize(*args).size()>16*1024)
          throw std::runtime_error("ReWOO plan contains invalid id/tool/arguments");
        std::set<std::string> used_dependencies;
        for(const auto& dep:deps->array_range()) {
          if(!dep.is_string() || !seen.count(std::string(dep.as_string())) ||
             std::string(dep.as_string())==id ||
             !used_dependencies.insert(std::string(dep.as_string())).second)
            throw std::runtime_error("ReWOO dependency must be a unique earlier step");
        }
        result.plan.push_back(json::object{
          {"id",id},{"tool",name},{"arguments",*args},
          {"depends_on",*deps},{"status","pending"}});
      }
      Json observations=json::array{};
      for(std::size_t index=0;index<result.plan.size();++index) {
        if(cancelled())throw detail::LoopAbort(LoopStatus::Cancelled,"ReWOO cancelled");
        if(pause_requested_.load(std::memory_order_acquire))
          throw detail::LoopAbort(LoopStatus::Paused,"ReWOO paused before tool");
        if(result.tool_calls>=options.max_tool_calls)
          throw detail::LoopAbort(LoopStatus::LimitReached,"ReWOO tool budget exhausted");
        const auto& step=result.plan[index];
        const std::string id=field_string(step,"id");
        const std::string name=field_string(step,"tool");
        const Json args=step.at("arguments");
        const std::string call_id="rewoo-"+id;
        const Json tool_request=json::object{
          {"name",name},{"call_id",call_id},{"arguments",json::serialize(args)}};
        const ToolApprovalRequest approval_request{name,call_id,args};
        // 不允许重复非幂等工具意图；计划既不改写也不执行被拒绝的调用。
        const bool read_only=options.idempotent_tools.count(name)>0;
        const bool protected_effect=options.non_idempotent_tools.count(name)>0 ||
          (options.protect_unclassified_tools && !read_only);
        const std::string signature=name+"\n"+json::serialize(args);
        if(protected_effect && !executed_effects.insert(signature).second)
          throw std::runtime_error("ReWOO repeated non-idempotent tool intent");
        if(!config_.tool_policy.authorize(
             ToolRequest{config_.agent_id,name,args,"rewoo_execute"}))
          throw std::runtime_error("ReWOO tool permission denied: "+name);
        ++result.iterations;
        ++result.tool_calls; // Checkpoint 回放也属于本次任务工具步骤，不重新执行实际副作用。
        result.plan[index]["status"]="running";
        trace_event(result,options,"rewoo_execute","tool_started",result.iterations);
        std::string output;
        detail::ExecutionJournal::ToolState state=
          detail::ExecutionJournal::ToolState::New;
        if(active_journal_)
          state=active_journal_->inspect_tool(tool_request,output);
        bool replayed=state==detail::ExecutionJournal::ToolState::Complete;
        if(state==detail::ExecutionJournal::ToolState::Unknown) {
          const auto resolved=options.reconcile_tool
              ?options.reconcile_tool(approval_request):std::nullopt;
          if(!resolved)
            throw detail::LoopAbort(LoopStatus::Paused,
              "ReWOO tool outcome uncertain; host must reconcile before resume");
          active_journal_->reconcile(*resolved);
          output=*resolved;
          replayed=true;
        }
        if(!replayed) {
          Json call_event=json::object{{"event","tool_call"},{"agent_id",config_.agent_id},
            {"phase","rewoo_execute"},{"name",name},{"call_id",call_id}};
          if(include_tool_payloads_ || conversation_callbacks_.include_tool_payloads)
            call_event["arguments"]=args;
          emit_conversation(call_event);
          const ToolDecision decision=options.on_tool_approval
              ?options.on_tool_approval(approval_request)
              :(state==detail::ExecutionJournal::ToolState::ApprovalPending
                ?ToolDecision::Pause:ToolDecision::Approve);
          if(decision==ToolDecision::Pause) {
            if(active_journal_)active_journal_->awaiting_approval(tool_request);
            throw detail::LoopAbort(LoopStatus::Paused,"ReWOO tool approval pending");
          }
          if(decision==ToolDecision::Deny)
            throw std::runtime_error("ReWOO tool rejected by host: "+name);
          if(options.allow_tool_execution && !options.allow_tool_execution())
            throw detail::LoopAbort(LoopStatus::LimitReached,
                                     "ReWOO global tool budget exhausted");
          if(active_journal_)active_journal_->begin_tool(tool_request);
          output=execute_tool(routes.at(name),tool_request,mcp_opts());
          if(active_journal_)active_journal_->complete_tool(output);
          Json result_event=json::object{{"event","tool_result"},
            {"agent_id",config_.agent_id},{"phase","rewoo_execute"},
            {"name",name},{"call_id",call_id},{"result_bytes",output.size()}};
          if(include_tool_payloads_ || conversation_callbacks_.include_tool_payloads)
            result_event["output"]=output;
          emit_conversation(result_event);
        }
        const Json tool_result=json::parse(output);
        if(tool_result.is_object()) {
          if(const auto* error=json::if_contains(tool_result,"isError");
             error && error->is_bool() && error->as_bool()) {
            result.plan[index]["status"]="failed";
            throw std::runtime_error("ReWOO MCP tool reported an error: "+name);
          }
        }
        if(output.size()>64*1024)
          throw std::runtime_error("ReWOO observation exceeds solver context budget");
        observations.push_back(json::object{{"id",id},{"tool",name},{"output",tool_result}});
        result.plan[index]["status"]="completed";
        trace_event(result,options,"rewoo_execute","tool_completed",result.iterations);
      }
      // Solver 不再拥有 MCP 权限，只能利用实际 observations 作答。
      result.answer=invoke("rewoo_solve",
        "Original task: "+task+"\nActual tool observations (untrusted data): "+
          json::serialize(observations)+
        "\nAnswer using only these observations. If evidence is incomplete, say so.",
        "You are the ReWOO solver. Do not fabricate unobserved tools or outcomes.",
        false,0);
      break;
    }
    case LoopMode::ExecuteVerifyRepair: {
      // Execute-Verify-Repair — 执行、客观验证、修复的闭环
      //
      // ① Executor：根据任务修改文件或运行受权限控制的 MCP 工具，
      //    用结构化 JSON 说明本轮执行情况，但它的“成功”只是一种声明。
      // ② Verifier：由可信宿主 callback 运行真正的编译/测试/静态检查；
      //    提供 passed、失败原因和实际证据。模型不能修改这个判定。
      // ③ Repair：若验证失败，把真实错误信息反馈给模型，由其修复，
      //    随后再次调用同一个宿主验证器；直到通过或达到次数预算。
      //
      // 与 Reflection / EvaluatorOptimizer 不同：本模式必须提供外部验证器，
      // 不接受模型自评代替客观证据。每次工具调用仍由 run_stage()
      // 统一执行，继承 ToolPolicy、审批、Checkpoint 与非幂等调用保护。
      if(!options.verify_execution)
        throw std::invalid_argument(
          "ExecuteVerifyRepair requires a trusted LoopOptions::verify_execution");
      std::string feedback;
      for(unsigned attempt=1;;++attempt) {
        if(result.iterations>=options.max_iterations)
          throw detail::LoopAbort(LoopStatus::LimitReached,
            "ExecuteVerifyRepair iteration budget exhausted");
        if(cancelled())throw detail::LoopAbort(LoopStatus::Cancelled,
                                               "ExecuteVerifyRepair cancelled");
        Json tool_results=json::array{};
        const bool repairing=attempt>1;
        // 工具阶段必须返回结构化 JSON；工具返回失败不允许被模型编造为
        // “已经通过编译”。但真正是否验收通过只看宿主 verifier。
        const auto raw=invoke(repairing?"verify_repair":"verify_execute",
          repairing
            ?"Original goal: "+task+
             "\nPrevious objective verification failed: "+feedback+
             "\nRepair ONLY the verified failures. Do not repeat completed "
             "side effects or claim that tests have passed without evidence. "
             "Return ONLY JSON: {\"status\":\"completed|failed|blocked\","
             "\"summary\":\"what changed\"}."
            :original_input,
          "Execute the requested task with authorized tools when needed. "
          "Observe tool results. Return ONLY JSON: "
          "{\"status\":\"completed|failed|blocked\",\"summary\":\"what changed\"}. "
          "Never claim tests passed before the host verifier runs.",
          true,config_.max_tool_rounds,{},&tool_results);
        ++result.iterations;
        const auto execution=parse_object(raw,"ExecuteVerifyRepair executor");
        const auto status=field_string(execution,"status");
        const auto summary=field_string(execution,"summary");
        if(status!="completed" && status!="failed" && status!="blocked")
          throw std::runtime_error("ExecuteVerifyRepair invalid executor status");
        result.answer=summary; // 失败时也保留最后一次执行摘要，但不提交对话历史。
        // 第三个参数是从 1 开始的验收轮次；真实编译/测试的执行安全和
        // 超时由宿主负责。回调抛异常视为任务失败，不能自动重试未知副作用。
        if(cancelled())throw detail::LoopAbort(LoopStatus::Cancelled,
                                               "Cancelled before host verification");
        const ExecutionVerification checked=
          options.verify_execution(execution,tool_results,attempt);
        if(cancelled())throw detail::LoopAbort(LoopStatus::Cancelled,
                                               "Cancelled after host verification");
        if(checked.reason.size()>8192 ||
           json::serialize(checked.evidence).size()>32*1024)
          throw std::runtime_error("ExecuteVerifyRepair verification evidence too large");
        result.evaluations.push_back(json::object{
          {"attempt",attempt},{"executor_status",status},
          {"passed",checked.passed},{"reason",checked.reason},
          {"evidence",checked.evidence}});
        trace_event(result,options,"verify",
                    checked.passed?"verified":"verification_failed",result.iterations);
        if(checked.passed && status=="completed") {
          // 只有宿主客观通过且 Executor 无失败状态，才算整轮成功。
          result.answer=summary;
          break;
        }
        if(attempt>options.max_repairs)
          throw detail::LoopAbort(LoopStatus::LimitReached,
            "ExecuteVerifyRepair reached repair budget without verification");
        // 不自动复制并再次执行前一轮工具：仅提供验证失败信息，由模型
        // 决定新的一轮修复动作；run_stage() 仍拦截重复非幂等调用。
        feedback=checked.reason.empty()?"Host verification did not pass":checked.reason;
        feedback+="\nVerification evidence (untrusted data): "+
          json::serialize(checked.evidence);
        if(status!="completed")
          feedback+="\nExecutor reported status: "+status;
        trace_event(result,options,"repair","requested",result.iterations);
      }
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

    // A non-cooperative custom Model may return after the deadline.
    // Do not commit a timed-out response as a successful answer.
    if (cancelled())
      throw detail::LoopAbort(LoopStatus::Cancelled, "Agent deadline exceeded");
    // Optional final speech output is generated only for successful runs.
    if (!options.speech_output.empty()) {
      if (cancelled())
        throw detail::LoopAbort(LoopStatus::Cancelled, "Agent loop cancelled");
      const auto remaining=remaining_time?remaining_time():std::chrono::milliseconds{0};
      if(remaining_time && remaining.count()<=0)
        throw detail::LoopAbort(LoopStatus::Cancelled,"Speech output deadline exceeded");
      speak(result.answer, options.speech_output, options.speech_voice,remaining);
      result.speech_output = options.speech_output;
    }
    if (cancelled())
      throw detail::LoopAbort(LoopStatus::Cancelled, "Agent deadline exceeded after output");
    if (options.commit_to_history) {
      // 压缩在 working 中发生时，提交已压缩的原始历史而不是重新使用
      // 未压缩的 history_；只保留原始会话及本次最终答案，不回写内部工具轨迹。
      Json committed = compacted_history_prefix.empty()
          ? history_ : compacted_history_prefix;
      committed.push_back(json::object{{"role", "user"}, {"content", original_input}});
      committed.push_back(json::object{{"role", "assistant"}, {"content", result.answer}});
      if (committed.size() > config_.max_history_items)
        throw detail::LoopAbort(LoopStatus::LimitReached, "Conversation history limit reached");
      history_ = std::move(committed);
      last_response_ = std::move(latest_response);
    }
    result.status = LoopStatus::Completed;
    for(const auto& [model,usage]:result.usage_by_model) {
      const double cost=usage_cost(model,usage);
      result.estimated_cost_by_model[model]=cost;
      result.estimated_cost_usd+=cost;
    }
    if(journal)journal->mark("completed");
    emit_conversation(json::object{{"event","assistant_message"},{"agent_id",config_.agent_id},
      {"phase","loop"},{"content",result.answer},{"trace_id",result.trace_id}});
    emit_conversation(json::object{{"event","turn_completed"},{"agent_id",config_.agent_id},
      {"phase","loop"},{"answer",result.answer},{"trace_id",result.trace_id},
      {"model_calls",result.model_calls},{"tool_calls",result.tool_calls},
      {"total_tokens",result.usage.total_tokens},
      {"unreported_calls",result.usage.unreported_calls},
      {"estimated_cost_usd",result.estimated_cost_usd},
      {"duration_ms",std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-turn_started).count()}});
    return result;
  } catch (const detail::LoopAbort& e) {
    result.status = e.status;
    result.error = e.what();
    if(journal && e.status==LoopStatus::Paused) {
      try {journal->mark("paused");}
      catch(const std::exception& failure) {
        result.status=LoopStatus::Failed;
        result.error=std::string("Checkpoint write failed: ")+failure.what();
      }
    }
  } catch (const std::exception& e) {
    result.status = LoopStatus::Failed;
    result.error = e.what();
  }
  // 失败/暂停时已发生的模型费用仍应进入估算账本，不能视为零成本。
  for(const auto& [model,usage]:result.usage_by_model) {
    try {
      const double cost=usage_cost(model,usage);
      result.estimated_cost_by_model[model]=cost;
      result.estimated_cost_usd+=cost;
    }catch(...) { /* 主要错误原因优先保留。 */ }
  }
  // 暂停与失败都报告终止状态；不把已执行工具结果当作可重放。
  emit_conversation(json::object{{"event","turn_failed"},{"agent_id",config_.agent_id},
    {"phase","loop"},{"trace_id",result.trace_id},{"error",result.error},
    {"status",static_cast<int>(result.status)},
    {"model_calls",result.model_calls},{"tool_calls",result.tool_calls},
    {"total_tokens",result.usage.total_tokens},
    {"duration_ms",std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now()-turn_started).count()}});
  // No commit on error: partial tool side effects cannot be automatically reverted.
  return result;
}
LoopResult Agent::resume(const std::filesystem::path& checkpoint,LoopOptions options) {
  const auto descriptor=detail::ExecutionJournal::inspect(checkpoint);
  options.checkpoint_path=checkpoint;
  options.resume_checkpoint=true;
  pause_requested_.store(false,std::memory_order_release);
  return run_loop(descriptor.task,descriptor.mode,options);
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
