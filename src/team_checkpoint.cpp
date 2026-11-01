#include "team_checkpoint.hpp"
#include <algorithm>
#include <iterator>
#include <fstream>
#include <stdexcept>
#include <string>
namespace myagent::detail {
namespace json = mymcp::json;
namespace {
Json task_graph(const std::vector<TeamTask>& tasks) {
  Json graph = json::array{};
  for(const auto& task:tasks) {
    Json deps=json::array{};
    for(const auto& name:task.depends_on)deps.push_back(name);
    Json fallbacks=json::array{};
    for(const auto& name:task.fallback_agents)fallbacks.push_back(name);
    graph.push_back(json::object{
      {"id",task.id},{"agent",task.agent},{"description",task.description},
      {"depends_on",deps},{"mode",static_cast<int>(task.mode)},
      {"priority",task.priority},{"fallback_agents",fallbacks},{"goal",task.context.goal},
      {"context",task.context.relevant_context},
      {"artifacts",task.context.artifacts},{"constraints",task.context.constraints}
    });
  }
  return graph;
}
Json usage_json(const TokenUsage& usage) {
  return json::object{{"input_tokens",usage.input_tokens},
    {"output_tokens",usage.output_tokens},{"total_tokens",usage.total_tokens},
    {"cached_input_tokens",usage.cached_input_tokens},
    {"reasoning_output_tokens",usage.reasoning_output_tokens},
    {"reported_calls",usage.reported_calls},{"unreported_calls",usage.unreported_calls}};
}
TokenUsage read_usage(const Json& j) {
  TokenUsage u;
  u.input_tokens=j.at("input_tokens").as<std::uint64_t>();
  u.output_tokens=j.at("output_tokens").as<std::uint64_t>();
  u.total_tokens=j.at("total_tokens").as<std::uint64_t>();
  u.cached_input_tokens=j.at("cached_input_tokens").as<std::uint64_t>();
  u.reasoning_output_tokens=j.at("reasoning_output_tokens").as<std::uint64_t>();
  u.reported_calls=j.at("reported_calls").as<std::uint64_t>();
  u.unreported_calls=j.at("unreported_calls").as<std::uint64_t>();
  return u;
}
Json delegation_json(const std::string& id,const DelegationResult& r) {
  return json::object{{"id",id},{"agent",r.agent},
    {"status",static_cast<int>(r.status)},{"answer",r.answer},{"error",r.error},
    {"verification",r.verification},{"verification_evidence",r.verification_evidence},
    {"usage",usage_json(r.usage)},{"model_calls",r.model_calls},
    {"tool_calls",r.tool_calls},{"attempts",r.attempts},{"details",r.details}};
}
DelegationResult read_delegation(const Json& item) {
  DelegationResult r;
  r.id=std::string(item.at("id").as_string());
  r.agent=std::string(item.at("agent").as_string());
  r.status=static_cast<LoopStatus>(item.at("status").as<int>());
  r.answer=std::string(item.at("answer").as_string());
  r.error=std::string(item.at("error").as_string());
  r.verification=std::string(item.at("verification").as_string());
  r.verification_evidence=item.at("verification_evidence");
  r.model_calls=item.at("model_calls").as<unsigned>();
  r.tool_calls=item.at("tool_calls").as<unsigned>();
  r.attempts=item.at("attempts").as<unsigned>();
  r.usage=read_usage(item.at("usage"));
  r.details=item.at("details");
  return r;
}
} // namespace
void save_team_checkpoint(const std::filesystem::path& path,
                          const std::vector<TeamTask>& tasks,
                          const std::map<std::string, DelegationResult>& finished,
                          const std::set<std::string>& inflight,
                          const std::map<std::string, std::vector<DelegationResult>>& prior_attempts) {
  if(path.empty())return;
  Json records=json::array{};
  for(const auto& [id,r]:finished)
    records.push_back(delegation_json(id,r));
  Json failures=json::array{};
  for(const auto& [id,attempts]:prior_attempts)
    for(const auto& r:attempts)
      failures.push_back(delegation_json(id,r));
  Json running=json::array{};
  for(const auto& name:inflight)running.push_back(name);
  const auto serialized=json::serialize(json::object{
    {"format","myagent-team-checkpoint-v2"},{"tasks",task_graph(tasks)},
    {"completed_records",records},{"prior_attempts",failures},{"inflight",running}});
  if(serialized.size()>8*1024*1024)throw std::runtime_error("Checkpoint > 8 MiB");
  auto temp=path;temp+=".tmp";
  {
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);
    if(!out)throw std::runtime_error("Cannot open checkpoint temporary file");
    out.write(serialized.data(),static_cast<std::streamsize>(serialized.size()));
    out.flush();
    if(!out)throw std::runtime_error("Failed to write checkpoint");
  }
  // 同目录 rename，避免写出半截 JSON。宿主仍需保证目录可信和权限正确。
  std::filesystem::rename(temp,path);
}
RestoredTeamCheckpoint load_team_checkpoint(
    const std::filesystem::path& path,const std::vector<TeamTask>& tasks,
    const std::function<TaskValidation(const TeamTask&,const DelegationResult&)>& validator) {
  if(!validator)throw std::runtime_error(
    "Checkpoint resume requires a host validator for external side effects");
  if(!std::filesystem::exists(path))throw std::runtime_error("Checkpoint missing");
  if(std::filesystem::file_size(path)>8*1024*1024)
    throw std::runtime_error("Checkpoint too large");
  std::ifstream in(path,std::ios::binary);
  if(!in)throw std::runtime_error("Cannot read checkpoint");
  const auto text=std::string(std::istreambuf_iterator<char>(in),{});
  const auto data=json::parse(text);
  if(!data.is_object()||data.at("format").as_string()!="myagent-team-checkpoint-v2"||
     json::serialize(data.at("tasks"))!=json::serialize(task_graph(tasks)))
    throw std::runtime_error("Checkpoint task graph mismatch");
  if(!data.at("inflight").empty())
    throw std::runtime_error("Checkpoint contains in-flight side effects; manual reconciliation required");
  RestoredTeamCheckpoint restored;
  const auto find_task=[&](const DelegationResult& r) {
    return std::find_if(tasks.begin(),tasks.end(),[&](const TeamTask& x) {
      return x.id==r.id && (x.agent==r.agent ||
        std::find(x.fallback_agents.begin(),x.fallback_agents.end(),r.agent)!=x.fallback_agents.end());
    });
  };
  for(const auto& item:data.at("prior_attempts").array_range()) {
    auto r=read_delegation(item);
    if(find_task(r)==tasks.end() || r.status==LoopStatus::Completed || r.tool_calls!=0)
      throw std::runtime_error("Invalid prior attempt in checkpoint");
    restored.prior_attempts[r.id].push_back(std::move(r));
  }
  for(const auto& item:data.at("completed_records").array_range()) {
    auto r=read_delegation(item);
    auto t=find_task(r);
    if(t==tasks.end()||!restored.finished.emplace(r.id,r).second)
      throw std::runtime_error("Checkpoint task invalid or duplicate");
    if(r.status!=LoopStatus::Completed || r.verification!="verified")
      throw std::runtime_error("Checkpoint has unverified/failed work; manual reconciliation required");
    const auto checked=validator(*t,r);
    if(!checked.passed)
      throw std::runtime_error("Checkpoint revalidation failed: "+checked.reason);
  }
  return restored;
}
} // namespace myagent::detail
