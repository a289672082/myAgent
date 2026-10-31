#pragma once
// C++20 OpenAI Responses Agent. MCP is provided by the existing mymcp SDK.
#include "mymcp/mcp.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
struct Config {
  std::string api_key;  // Required for the OpenAI backend; supplied by the application.
  std::string model = "gpt-4.1-mini";
  std::string instructions = "You are a helpful assistant.";
  std::string base_url; // Required for the OpenAI backend, e.g. https://api.openai.com
  unsigned max_tool_rounds = 8;
  std::size_t max_history_items = 200; // Reject overflow instead of silently dropping tool calls.
};

struct Skill {
  std::string name;
  std::string description;
  std::string content;
  std::filesystem::path source;
};

// A skill is trusted prompt material, not an executable plugin.
// Supports SKILL.md with simple YAML frontmatter (name/description).
class SkillLibrary {
public:
  void load(const std::filesystem::path& directory);
  const std::vector<Skill>& all() const { return skills_; }
  const Skill& get(const std::string& name) const;
private:
  std::vector<Skill> skills_;
};

// Injectable boundary: unit tests can exercise the complete tool loop offline.
class Model {
public:
  virtual ~Model() = default;
  virtual Json create(const Json& request) = 0;
};

class OpenAIResponses final : public Model {
public:
  explicit OpenAIResponses(Config config);
  Json create(const Json& request) override;
  // Audio uses the dedicated OpenAI audio endpoints, not an unsupported
  // Responses input_audio content-part shape.
  std::string transcribe(const std::filesystem::path& file,
                         const std::string& model = "gpt-4o-mini-transcribe") const;
  void speech(const std::string& text, const std::filesystem::path& output,
              const std::string& voice = "alloy",
              const std::string& model = "gpt-4o-mini-tts") const;
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
    local_.tool(std::move(name), std::move(description),
                std::move(fn), std::move(args)...);
  }
  Server& local_mcp_server() { return local_; }

  void load_skills(const std::filesystem::path& directory);
  void use_skill(const std::string& name);
  const SkillLibrary& skills() const { return skills_; }

  std::string ask(const std::string& text);
  // An array of OpenAI input content parts (e.g., input_text, input_image).
  std::string ask_parts(Json content);
  std::string ask_image(const std::string& question, const std::filesystem::path& image);
  // Transcribe audio first; then pass the transcription through the Agent.
  std::string ask_audio(const std::filesystem::path& audio,
                        const std::string& question = {});
  void speak(const std::string& text, const std::filesystem::path& output,
             const std::string& voice = "alloy");

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
  std::string run(Json user_content);

  Config config_;
  std::shared_ptr<Model> model_;
  Server local_;
  std::vector<Endpoint> endpoints_;
  SkillLibrary skills_;
  std::vector<std::string> active_skills_;
  Json history_ = json::array{};
  Json last_response_ = nullptr;
};

} // namespace myagent
