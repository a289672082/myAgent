#pragma once
// 多 Agent 协作：协调者负责规划/委派；每个 Worker 拥有独立的 Agent、
// Config(base_url/api_key/model)、MCP/Skill、历史与 Token 账本。
#include "myagent/agent.hpp"
#include <cstddef>
#include <filesystem>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace myagent {
struct McpEndpointSpec {
  enum class Transport { Http, Stdio, OAuth };
  Transport transport = Transport::Http;
  std::string prefix;
  std::string address; // HTTP URL 或 stdio executable
  std::string bearer_token;
  std::vector<std::string> args;
  std::shared_ptr<mymcp::oauth::Session> oauth_session;
};
struct AgentSpec {
  std::string name;
  std::string description;
  Config config;
  // 可选工作区：只为当前子 Agent 注册受路径约束的本地文件工具。
  // 不等同于 OS 沙箱；调用方添加的任意 MCP 工具仍由调用方控制权限。
  std::filesystem::path workspace;
  bool allow_workspace_write = false;
  std::vector<std::filesystem::path> skill_directories; // 构造后自动加载的专用目录
  std::vector<std::string> explicit_skills;             // 加载后直接 use_skill
  std::vector<McpEndpointSpec> mcp;                     // 子 Agent 独立 MCP 注册
};
// 交给子 Agent 的是任务相关的摘要和文件引用，而非主 Agent 的完整对话历史。
struct TaskContext {
  std::string goal;
  Json relevant_context = mymcp::json::object{};
  Json artifacts = mymcp::json::object{};
  Json constraints = mymcp::json::object{};
};
struct TaskValidation {
  bool passed = false;
  std::string reason;
  Json evidence = mymcp::json::object{};
};
struct TeamTask {
  std::string id;
  std::string agent;
  std::string description;
  std::vector<std::string> depends_on; // 只能依赖计划中较早的任务。
  LoopMode mode = LoopMode::ReAct;
  TaskContext context; // 调用方传入的有限任务上下文；默认空。
  int priority = 0; // 就绪任务优先级，数值大者先调度。
  std::vector<std::string> fallback_agents; // 失败后可尝试的备选 Agent（仅原任务无工具副作用）。
};
struct DelegationResult;
struct TeamOptions {
  unsigned max_task_retries = 0; // 默认不重复调用；仅无工具副作用的失败可安全重试。
  unsigned max_delegations = 8;       // 主协调器最多可产生多少个子任务
  unsigned max_parallel_agents = 2;   // 同时运行的不同 Agent 数量，至少 1
  unsigned max_depth = 1;             // 当前架构只有 coordinator -> worker 这一层
  unsigned max_model_calls = 64;      // 全团队模型调用预算
  unsigned max_tool_calls = 64;       // 全团队工具调用预算，0=禁用工具但允许纯模型任务
  std::uint64_t max_reported_tokens = 0; // 0 = 不限制；计费按请求后报告，软阈值
  unsigned max_handoff_chars = 6000;  // 向子 Agent 仅传相关上游摘要
  LoopOptions worker_loop;
  std::function<bool()> cancelled;
  // 团队运行事件（started/completed/failed/reassigned）；仅携带 ID/耗时，不记录密钥或工具正文。
  std::function<void(const Json&)> on_event;
  std::filesystem::path event_log_path; // 可选 JSONL 审计日志
  std::chrono::milliseconds timeout{0}; // 任务队列取消及轮次间截止时间
  std::filesystem::path checkpoint_path; // 任务图持久化：仅同一任务图可恢复
  bool resume_checkpoint = false;
  // 任务完成后由宿主程序审查真实证据（例如测试退出码）。
  // 不设置时仅是模型报告完成，不能声称客观测试已通过。
  std::function<TaskValidation(const TeamTask&, const DelegationResult&)> validate_task;
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
  unsigned attempts = 1;
  Json details = mymcp::json::object{}; // Worker Loop 的 plan/evaluations，包含实际工具证据。
  std::string verification = "unverified";
  Json verification_evidence = mymcp::json::object{};
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
  Json events = mymcp::json::array{}; // 团队调度事件，便于排查任务耗时和并行顺序。
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
  // 从 YAML/JSON 加载声明式 Agent 配置，key_ref 由宿主解析，不在文件中储存明文。
  void add_agents_from_file(const std::filesystem::path& file,
      const std::function<std::string(const std::string& key_ref)>& credential_resolver);
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
