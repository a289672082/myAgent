#include "plan_manager.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace myagent::detail {
namespace {
std::string required_string(const Json& obj, const char* field) {
  const Json* value = json::if_contains(obj, field);
  if (!value || !value->is_string() || value->as_string().empty())
    throw std::runtime_error(std::string("Plan requires a nonempty string: ") + field);
  return std::string(value->as_string());
}
Json parse_json_object(const std::string& raw, const char* phase) {
  try {
    Json object = json::parse(raw);
    if (!object.is_object())
      throw std::runtime_error("expected a JSON object");
    return object;
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string(phase) + " invalid JSON: " + e.what());
  }
}
std::string get_status(const Json& step) {
  return required_string(step, "status");
}
} // namespace

Json PlanManager::parse_steps(const std::string& model_json,
                              const std::set<std::string>& previous,
                              const std::set<std::string>& completed,
                              std::size_t allowance) const {
  const auto plan = parse_json_object(model_json, "Planner");
  const Json* raw = json::if_contains(plan, "steps");
  if (!raw || !raw->is_array() || raw->empty() || raw->size() > allowance)
    throw std::runtime_error("Planner requires 1..max_plan_steps steps");
  Json parsed = json::array{};
  auto seen = previous;
  auto allowed_dependencies = completed;
  for (const auto& item : raw->array_range()) {
    if (!item.is_object()) throw std::runtime_error("Plan step must be an object");
    const auto id = required_string(item, "id");
    const auto description = required_string(item, "description");
    if (id.size() > 64 || description.size() > 2048 || !seen.insert(id).second)
      throw std::runtime_error("Plan step id/description too long or duplicate id: " + id);
    Json dependencies = json::array{};
    const Json* provided = json::if_contains(item, "depends_on");
    if (provided) {
      if (!provided->is_array()) throw std::runtime_error("depends_on must be an array");
      std::set<std::string> unique;
      for (const auto& dep : provided->array_range()) {
        if (!dep.is_string() || dep.as_string().empty())
          throw std::runtime_error("Dependency must be a nonempty step ID");
        const std::string name(dep.as_string());
        if (!allowed_dependencies.count(name) || !unique.insert(name).second)
          throw std::runtime_error("Dependency must refer to an earlier new step or a completed step: " + name);
        dependencies.push_back(name);
      }
    }
    std::string criteria;
    if (const Json* value = json::if_contains(item, "success_criteria")) {
      if (!value->is_string() || value->as_string().size() > 2048)
        throw std::runtime_error("success_criteria must be a string <= 2048 bytes");
      criteria = std::string(value->as_string());
    }
    parsed.push_back(json::object{
      {"id", id}, {"description", description}, {"depends_on", dependencies},
      {"success_criteria", criteria}, {"status", "pending"}, {"attempts", 0},
      {"result", ""}, {"verification", "not_run"}, {"evidence", json::object{}}
    });
    allowed_dependencies.insert(id); // 自动阻止向未来步骤、自己或环形依赖。
  }
  return parsed;
}

void PlanManager::initialize(const std::string& model_json) {
  if (!steps_.empty()) throw std::logic_error("Plan already initialized");
  steps_ = parse_steps(model_json, {}, {}, max_steps_);
}

void PlanManager::replace_pending(const std::string& model_json) {
  std::set<std::string> existing, completed;
  std::size_t preserved = 0;
  for (const auto& item : steps_.array_range()) {
    existing.insert(required_string(item, "id"));
    if (get_status(item) == "completed") completed.insert(required_string(item, "id"));
    if (get_status(item) != "pending" && get_status(item) != "ready") ++preserved;
  }
  if (preserved >= max_steps_)
    throw std::runtime_error("Plan step budget exhausted");
  // 在修改旧计划之前，先确保新计划的 ID、依赖和容量全部合法。
  const auto replacement = parse_steps(model_json, existing, completed,
                                       max_steps_ - preserved);
  Json next = json::array{};
  for (auto step : steps_.array_range()) {
    const auto status = get_status(step);
    if (status == "pending" || status == "ready") {
      // 旧步骤不会再执行；保留状态和审计记录，但不占用当前执行槽。
      step["status"] = "superseded";
    }
    next.push_back(std::move(step));
  }
  for (const auto& item : replacement.array_range()) next.push_back(item);
  steps_ = std::move(next);
}

std::size_t PlanManager::next_ready() const {
  std::set<std::string> completed;
  for (const auto& step : steps_.array_range())
    if (get_status(step) == "completed") completed.insert(required_string(step, "id"));
  for (std::size_t i = 0; i < steps_.size(); ++i) {
    const auto& step = steps_[i];
    if (get_status(step) != "pending" && get_status(step) != "ready") continue;
    bool ready = true;
    for (const auto& dep : step.at("depends_on").array_range())
      if (!completed.count(std::string(dep.as_string()))) ready = false;
    if (ready) return i;
  }
  return no_step;
}

void PlanManager::start(std::size_t index) {
  auto& step = steps_[index];
  if (get_status(step) != "pending" && get_status(step) != "ready")
    throw std::logic_error("Only pending steps can start");
  step["status"] = "running";
  step["attempts"] = step.at("attempts").as<unsigned>() + 1;
}

void PlanManager::finish(std::size_t index, const std::string& status,
                         const std::string& summary, const std::string& verification,
                         const Json& evidence) {
  if (get_status(steps_[index]) != "running")
    throw std::logic_error("Cannot finish a step that is not running");
  if (status != "completed" && status != "failed" && status != "blocked")
    throw std::invalid_argument("Unknown step outcome");
  auto& step = steps_[index];
  step["status"] = status;
  step["result"] = summary;
  step["verification"] = verification;
  step["evidence"] = evidence;
}

void PlanManager::retry(std::size_t index) {
  if (get_status(steps_[index]) != "failed" && get_status(steps_[index]) != "blocked")
    throw std::logic_error("Only failed/blocked steps can be retried");
  steps_[index]["status"] = "pending";
}

StepOutcome validate_step(const Json& step, const std::string& raw,
                          const Json& tool_results, const LoopOptions& options) {
  const auto execution = parse_json_object(raw, "Executor");
  StepOutcome outcome;
  outcome.status = required_string(execution, "status");
  outcome.summary = required_string(execution, "summary");
  if (outcome.status != "completed" && outcome.status != "failed" &&
      outcome.status != "blocked")
    throw std::runtime_error("Executor status must be completed, failed or blocked");
  if (const Json* value = json::if_contains(execution, "evidence"))
    outcome.evidence = *value;
  outcome.verification = "unverified";
  // 无论模型是否声称成功，只要工具明确返回 isError=true，就不能标记完成。
  for (const auto& record : tool_results.array_range()) {
    const auto& value = record.at("output");
    if (!value.is_string()) continue;
    try {
      const auto tool_json = json::parse(std::string(value.as_string()));
      if (!tool_json.is_object()) continue;
      const Json* error = json::if_contains(tool_json, "isError");
      if (error && error->is_bool() && error->as_bool()) {
        outcome.status = "failed";
        outcome.verification = "failed";
        outcome.summary += " [Tool returned isError=true]";
        return outcome;
      }
    } catch (const std::exception&) {
      // 一般 MCP 工具可能返回非 JSON 文本，不能把解析失败视作测试通过。
    }
  }
  if (outcome.status != "completed") return outcome;
  if (options.validate_step) {
    const StepValidation validation = options.validate_step(step, execution, tool_results);
    if (!validation.passed) {
      outcome.status = "failed";
      outcome.verification = "failed";
      outcome.summary += " [Validator: " + validation.reason + "]";
    } else {
      outcome.verification = "verified";
    }
    outcome.evidence = validation.evidence;
  }
  // 如果没有宿主验证器，只记录模型报告的完成，不伪称已编译或测试通过。
  return outcome;
}
} // namespace myagent::detail
