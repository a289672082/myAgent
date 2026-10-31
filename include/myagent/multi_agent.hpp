#pragma once
// 多 Agent 协作：协调者负责规划/委派；每个 Worker 拥有独立的 Agent、
// Config(base_url/api_key/model)、MCP/Skill、历史与 Token 账本。
#include "myagent/agent.hpp"
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace myagent {
struct AgentSpec {
  std::string name;
  std::string description;
  Config config;
  // 可选工作区：只为当前子 Agent 注册受路径约束的本地文件工具。
  // 不等同于 OS 沙箱；调用方添加的任意 MCP 工具仍由调用方控制权限。
  std::filesystem::path workspace;
  bool allow_workspace_write = false;
};
struct TeamTask {
  std::string id;
  std::string agent;
  std::string description;
  std::vector<std::string> depends_on; // 只能依赖计划中较早的任务。
  LoopMode mode = LoopMode::ReAct;
};
struct TeamOptions {
  unsigned max_delegations = 8;       // 主协调器最多可产生多少个子任务
  unsigned max_parallel_agents = 2;   // 同时运行的不同 Agent 数量，至少 1
  unsigned max_depth = 1;             // 当前架构只有 coordinator -> worker 这一层
  unsigned max_model_calls = 64;      // 全团队模型调用预算
  unsigned max_tool_calls = 64;       // 全团队工具调用预算
  std::uint64_t max_reported_tokens = 0; // 0 = 不限制；计费按请求后报告，软阈值
  unsigned max_handoff_chars = 6000;  // 向子 Agent 仅传相关上游摘要
  LoopOptions worker_loop;
  std::function<bool()> cancelled;
};
enum class TeamStatus { Completed, Partial, LimitReached, Cancelled, Failed };
struct DelegationResult {
  std::string id;
  std::string agent;
  LoopStatus status = LoopStatus::Failed;
  std::string answer;
  std::string error;
  TokenUsage usage;
  unsigned model_calls = 0;
  unsigned tool_calls = 0;
};
struct TeamResult {
  TeamStatus status = TeamStatus::Failed;
  std::string answer;
  std::string error;
  std::vector<DelegationResult> tasks;
  TokenUsage usage;
  std::map<std::string, TokenUsage> usage_by_agent;
  unsigned model_calls = 0;
  unsigned tool_calls = 0;
};

class AgentTeam {
public:
  explicit AgentTeam(Config coordinator, std::shared_ptr<Model> coordinator_model = {});
  AgentTeam(const AgentTeam&) = delete;
  AgentTeam& operator=(const AgentTeam&) = delete;

  // 名称必须唯一；模型配置按值复制进各自的 Agent，不继承主 Agent API Key。
  void add_agent(AgentSpec spec, std::shared_ptr<Model> model = {});
  Agent& agent(const std::string& name);
  const Agent& coordinator() const { return *coordinator_; }
  // 直接委派：单独执行指定 Agent 的一种已有 Loop。
  DelegationResult delegate(const std::string& name, const LoopInput& input,
                            LoopMode mode = LoopMode::ReAct,
                            const LoopOptions& options = {});
  DelegationResult delegate(const std::string& name, const std::string& task,
                            LoopMode mode = LoopMode::ReAct,
                            const LoopOptions& options = {});

  // 显式协作：调用方给出任务图；按依赖调度，允许不同 Agent 并行。
  TeamResult collaborate(const std::vector<TeamTask>& tasks,
                         const TeamOptions& options = {});
  // 自动协作：协调器只生成结构化分工和最后总结，没有工具权限；
  // 不向协调器公开子 Agent 的 API Key、baseUrl 或内部完整历史。
  TeamResult run(const std::string& goal, const TeamOptions& options = {});
private:
  struct Worker {
    AgentSpec spec;
    std::unique_ptr<Agent> instance;
  };
  void register_workspace_tools(Worker& worker);
  TeamResult execute_tasks(const std::vector<TeamTask>& tasks,
                           const TeamOptions& options);
  static void add_usage(TeamResult& result, const std::string& agent,
                        const TokenUsage& usage);
  std::unique_ptr<Agent> coordinator_;
  std::map<std::string, std::unique_ptr<Worker>> workers_;
};
} // namespace myagent
