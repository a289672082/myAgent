#pragma once
// C++20 OpenAI Responses Agent. MCP is provided by the existing mymcp SDK.
#include "mymcp/mcp.hpp"
#include "myagent/loop.hpp"
#include "myagent/context.hpp"
#include "myagent/tool_policy.hpp"
#include "myagent/memory.hpp"
#include <chrono>
#include <mutex>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <limits>
#include <string>
#include <vector>

namespace myagent {
using mymcp::Json;
using mymcp::Client;
using mymcp::Server;
using mymcp::Arg;
namespace json = mymcp::json;

// Supply API credentials and endpoint explicitly; no environment variable fallback.
// Credentials are not written to conversation history or saved by save_history().
enum class ModelProtocol { Responses, ChatCompletions };
struct Config {
  std::string api_key;  // Required for the OpenAI backend; supplied by the application.
  std::string agent_id; // 宿主用于日志/工具审批的标识，不能由模型修改。
  std::string model = "gpt-4.1-mini";
  std::string instructions = "You are a helpful assistant.";
  std::string base_url; // Required: base origin/path or an exact Responses URL.
  // Optional audio gateway prefix. Empty uses base_url origin's /v1/audio.
  std::string audio_base_url;
  // true: append /v1/responses (unless URL already ends with it).
  // false: POST to base_url exactly, including its path/query.
  bool append_responses_path = true;
  ModelProtocol model_protocol = ModelProtocol::Responses; // 其他协议通过 Model 注入扩展。
  unsigned max_tool_rounds = 8;
  std::size_t max_history_items = 200; // Reject overflow instead of silently dropping tool calls.
  bool auto_discover_skills = true;   // Scan project/user Skill folders on construction.
  bool auto_skill_selection = true;  // Offer on-demand Skill tools to the model.
  ContextOptions context; // 上下文压缩配置；默认禁用，需按模型填上下文容量。
  ToolPolicy tool_policy; // MCP/Skill 工具权限。危险工具在调用前由宿主审批。
  bool memory_retrieval = false; // 只有宿主明确开启才向模型提供检索出的记忆条目。
  unsigned max_memory_hits = 3;
  unsigned http_timeout_seconds = 60; // 模型 HTTP 请求读写/连接超时；不影响外部 MCP 自身超时。
};

struct Skill {
  std::string name;
  std::string description;
  std::string content; // Empty during discovery: full SKILL.md is read on demand.
  std::filesystem::path source;
  // Agent Skills standard optional frontmatter fields (metadata only).
  std::string license;
  std::string compatibility;
  std::map<std::string, std::string> metadata;
  std::string allowed_tools; // Informational; never grants tool permissions.
  bool auto_invocable = true; // Honors optional Codex/Claude opt-out.
};

// A skill is trusted prompt material, not an executable plugin.
// Validates Agent Skills YAML frontmatter with yaml-cpp; instructions load on demand.
class SkillLibrary {
public:
  void load(const std::filesystem::path& directory);
  // Project: .agents/skills, .claude/skills, skills/ (CWD); user: ~/.agents/skills, ~/.claude/skills.
  // Existing directory entries win when names collide; no automatic script execution.
  void discover();
  const std::vector<Skill>& all() const { return skills_; }
  const Skill& get(const std::string& name) const;
  std::string read(const std::string& name) const;
  std::vector<std::string> supporting_files(const std::string& name) const;
  std::string read_file(const std::string& name, const std::string& relative_path) const;
private:
  void load_directory(const std::filesystem::path& directory, bool ignore_duplicates);
  std::vector<Skill> skills_;
};

// Injectable boundary: unit tests can exercise the complete tool loop offline.
class Model {
public:
  virtual ~Model() = default;
  virtual Json create(const Json& request) = 0;
  // The default keeps injected providers source-compatible. Built-in HTTP
  // backends override this to enforce a per-request transport deadline.
  virtual Json create_with_timeout(const Json& request,
                                  std::chrono::milliseconds timeout) {
    (void)timeout;
    return create(request);
  }
};

class OpenAIResponses final : public Model {
public:
  explicit OpenAIResponses(Config config);
  Json create(const Json& request) override;
  Json create_with_timeout(const Json& request,
                           std::chrono::milliseconds timeout) override;
  // Audio uses the dedicated OpenAI audio endpoints, not an unsupported
  // Responses input_audio content-part shape.
  std::string transcribe(const std::filesystem::path& file,
                         const std::string& model = "gpt-4o-mini-transcribe",
                         std::chrono::milliseconds timeout = {}) const;
  void speech(const std::string& text, const std::filesystem::path& output,
              const std::string& voice = "alloy",
              const std::string& model = "gpt-4o-mini-tts",
              std::chrono::milliseconds timeout = {}) const;
private:
  Config config_;
};

// 将 OpenAI Chat Completions 兼容接口适配到统一 Responses 结构。
class ChatCompletionsModel final : public Model {
public:
  explicit ChatCompletionsModel(Config config);
  Json create(const Json& request) override;
  Json create_with_timeout(const Json& request,
                           std::chrono::milliseconds timeout) override;
private:
  Config config_;
};

// Single-user, synchronous agent session. Not thread safe.
// Externally supplied MCP clients must outlive the registration, via shared_ptr.
class Agent {
public:
  explicit Agent(Config config, std::shared_ptr<Model> model = {});
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;

  // Use existing Client::process / Client::http / Client::http_oauth.
  // Prefix makes remote tool names unique in the OpenAI tool namespace.
  void add_mcp(std::string prefix, std::shared_ptr<Client> client);

  // Recommended API: Agent owns the MCP client and its transport lifetime.
  // No need to construct mymcp::Client or shared_ptr in application code.
  // All three use the existing myMcpTools SDK; discovery happens on ask().
  void add_mcp_http(std::string prefix, std::string url,
                    std::string bearer_token = {},
                    std::function<void(const Json&)> on_notification = {});
  // Starts an MCP stdio child process. Args are passed without shell expansion.
  void add_mcp_stdio(std::string prefix, std::string executable,
                     std::vector<std::string> args = {},
                     std::function<void(const Json&)> on_notification = {});
  // Session must already have a valid OAuth token, or be able to refresh one.
  // Interactive OAuth authorization remains the host application's responsibility.
  void add_mcp_oauth(std::string prefix, std::string url,
                     std::shared_ptr<mymcp::oauth::Session> session,
                     std::function<void(const Json&)> on_notification = {});

  // MCP tool definitions are registered on a private in-process Server.
  // This preserves mymcp JSON Schema validation and typed conversion.
  void tool(std::string name, std::string description,
            Json schema, Server::ToolHandler handler);
  template<class F, class... Ts>
  void tool(std::string name, std::string description, F fn, Arg<Ts>... args) {
    const std::lock_guard<std::recursive_mutex> guard(session_mutex_);
    local_.tool(std::move(name), std::move(description),
                std::move(fn), std::move(args)...);
  }
  Server& local_mcp_server() { return local_; }

  void load_skills(const std::filesystem::path& directory);
  void use_skill(const std::string& name);
  const SkillLibrary& skills() const { return skills_; }

  // Four synchronous loop strategies; input may mix text, images and audio.
  // The loop owns transient phase history; on success only the task/final answer
  // is committed to the normal conversation, if commit_to_history is true.
  LoopResult run_loop(const LoopInput& input, LoopMode mode,
                      const LoopOptions& options = {});
  LoopResult run_loop(const std::string& task, LoopMode mode,
                      const LoopOptions& options = {});
  LoopResult react(const LoopInput& input, const LoopOptions& options = {});
  LoopResult react(const std::string& task, const LoopOptions& options = {});
  LoopResult plan_and_execute(const LoopInput& input, const LoopOptions& options = {});
  LoopResult plan_and_execute(const std::string& task, const LoopOptions& options = {});
  LoopResult reflect(const LoopInput& input, const LoopOptions& options = {});
  LoopResult reflect(const std::string& task, const LoopOptions& options = {});
  LoopResult evaluate_and_optimize(const LoopInput& input, const LoopOptions& options = {});
  LoopResult evaluate_and_optimize(const std::string& task, const LoopOptions& options = {});

  std::string ask(const std::string& text);
  // 仅供协调器规划/汇总：不发现、不展示、不调用 MCP 或 Skill 工具。
  std::string ask_without_tools(const std::string& text);
  // Coordinator 专用：限制这次无工具请求（包括可能触发的摘要调用）。
  // 零预算会在发送任何模型请求前被拒绝。
  std::string ask_without_tools(const std::string& text, unsigned max_model_calls);
  std::string ask_without_tools(const std::string& text, unsigned max_model_calls,
                                std::chrono::milliseconds max_duration);
  unsigned last_model_attempts() const { return last_model_attempts_; }
  // An array of OpenAI input content parts (e.g., input_text, input_image).
  std::string ask_parts(Json content);
  std::string ask_image(const std::string& question, const std::filesystem::path& image);
  // Transcribe audio first; then pass the transcription through the Agent.
  std::string ask_audio(const std::filesystem::path& audio,
                        const std::string& question = {});
  void speak(const std::string& text, const std::filesystem::path& output,
             const std::string& voice = "alloy",
             std::chrono::milliseconds timeout = {});

  // 统计全部从 Responses API 的 usage 字段读取；不进行文本字符估算。
  // last_usage()：上一次 ask()/Loop 的调用（即便最终失败，也保留已收到的用量）。
  // total_usage()：本 Agent 构造以来累计用量，包含失败任务中的已返回响应。
  // clear_history() 不清除 Token 用量，避免统计口径与实际账单不一致。
  const TokenUsage& last_usage() const { return last_usage_; }
  const TokenUsage& total_usage() const { return total_usage_; }
  void reset_usage(); // 仅手动清除统计，不修改对话历史。

  // 对话摘要压缩：不删除孤立工具结果，也不推测实际 Token 消耗。
  CompressionResult compress_context();
  ContextStats context_stats() const { return context_.stats(history_); }
  void remember(MemoryRecord record);
  std::vector<MemoryRecord> recall(const std::string& query,std::size_t top_k=5) const;
  void save_memory(const std::filesystem::path& file) const;
  void load_memory(const std::filesystem::path& file);
  const Json& history() const { return history_; }
  const Json& last_response() const { return last_response_; }
  void clear_history();
  void save_history(const std::filesystem::path& file) const;
  void load_history(const std::filesystem::path& file);

private:
  struct Endpoint {
    std::string prefix;
    std::shared_ptr<Client> client;
  };
  struct Route {
    std::shared_ptr<Client> client;
    std::string native_name;
  };
  void validate_mcp_prefix(const std::string& prefix) const;
  Json tool_definitions(std::map<std::string, Route>& routes);
  std::string execute_tool(const Route& route, const Json& call);
  std::string instructions() const;
  std::string run(Json user_content, bool allow_tools = true,
                  unsigned max_model_calls = std::numeric_limits<unsigned>::max(),
                  std::chrono::milliseconds max_duration = {});
  struct TurnResult {
    std::string answer;
    Json response;
  };
  TurnResult run_stage(Json user_content, Json& working,
      const std::string& phase_instructions, const std::string& model_override,
      bool allow_tools, unsigned& model_calls, unsigned& tool_calls,
      unsigned max_model_calls, unsigned max_tool_calls, unsigned max_rounds,
      const std::function<bool()>& cancelled, Json* trace,
      const std::function<void(const Json&)>& on_event, const std::string& phase,
      Json* step_tool_results = nullptr, TokenUsage* run_usage = nullptr,
      std::map<std::string, TokenUsage>* usage_by_phase = nullptr,
      std::map<std::string, TokenUsage>* usage_by_model = nullptr,
      Json* compacted_history_prefix = nullptr,
      std::set<std::string>* executed_effects = nullptr,
      const std::set<std::string>* protected_tools = nullptr,
      const std::set<std::string>* idempotent_tools = nullptr,
      bool protect_unclassified_tools = false,
      const std::function<std::chrono::milliseconds()>& remaining_time = {});
  Json prepare_loop_input(const LoopInput& input, std::string& transcript,
      const std::function<std::chrono::milliseconds()>& remaining_time = {});

  Config config_;
  ContextManager context_;
  MemoryStore memory_;
  std::shared_ptr<Model> model_;
  Server local_;
  std::vector<Endpoint> endpoints_;
  SkillLibrary skills_;
  std::map<std::string, std::string> manual_skill_contents_; // Explicit use_skill() only.
  Json history_ = json::array{};
  Json last_response_ = nullptr;
  TokenUsage last_usage_;
  TokenUsage total_usage_;
  unsigned last_model_attempts_ = 0; // 包含失败/无 usage 的实际 HTTP 请求次数
  mutable std::recursive_mutex session_mutex_; // 保护同一 Agent 的对话与模型执行，避免并发冲突。
};

} // namespace myagent
