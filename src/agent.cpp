#include "myagent/agent.hpp"
#include "httplib.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace myagent {
namespace {
constexpr std::size_t MaxMediaBytes = 20 * 1024 * 1024;
constexpr std::size_t MaxSkillBytes = 128 * 1024;
constexpr std::size_t MaxToolResultBytes = 256 * 1024;

std::string as_string(const Json& v, const std::string& key, std::string fallback = {}) {
  const Json* p = json::if_contains(v, key);
  return p && p->is_string() ? std::string(p->as_string()) : fallback;
}
std::string trim(std::string s) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
  if (s.size() > 1 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\'')))
    s = s.substr(1, s.size() - 2);
  return s;
}
std::string read_file(const std::filesystem::path& file, std::size_t limit) {
  if (!std::filesystem::is_regular_file(file))
    throw std::runtime_error("Not a regular file: " + file.string());
  if (std::filesystem::file_size(file) > limit)
    throw std::runtime_error("File exceeds configured safety limit: " + file.string());
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read: " + file.string());
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (s.size() > limit) throw std::runtime_error("File exceeds limit");
  return s;
}
void write_file(const std::filesystem::path& file, const std::string& data) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(data.data(), static_cast<std::streamsize>(data.size())))
    throw std::runtime_error("Cannot write: " + file.string());
}
std::string base64(const std::string& input) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(4 * ((input.size() + 2) / 3));
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(input[i]);
    const unsigned b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
    const unsigned c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
    out += alphabet[a >> 2];
    out += alphabet[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < input.size() ? alphabet[c & 63] : '=';
  }
  return out;
}
std::string api_key(const Config& config) {
  if (!config.api_key.empty()) return config.api_key;
  const char* env = std::getenv("OPENAI_API_KEY");
  if (!env || !*env) throw std::runtime_error("OPENAI_API_KEY is not configured");
  return env;
}
httplib::Headers headers(const Config& config) {
  return {{"Authorization", "Bearer " + api_key(config)}};
}
void check_http(const httplib::Result& result, const std::string& endpoint) {
  if (!result) throw std::runtime_error("HTTP transport failed at " + endpoint + ": " +
                                        httplib::to_string(result.error()));
  if (result->status < 200 || result->status >= 300) {
    // HTTP error bodies may include external text, but not the Authorization header.
    std::string body = result->body.substr(0, 1024);
    throw std::runtime_error(endpoint + " HTTP " + std::to_string(result->status) + ": " + body);
  }
}
void configure(httplib::Client& client) {
  client.set_connection_timeout(10);
  client.set_read_timeout(120);
  client.set_write_timeout(120);
  client.set_follow_location(false); // Never forward a bearer token on redirects.
}
std::string media_type(const std::filesystem::path& file) {
  auto ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  if (ext == ".gif") return "image/gif";
  throw std::invalid_argument("Unsupported image extension: " + ext);
}
std::string safe_tool_name(const std::string& name) {
  std::string s;
  for (unsigned char c : name) {
    if (std::isalnum(c) && c < 128 || c == '_' || c == '-') s += static_cast<char>(c);
    else s += '_';
  }
  if (s.empty()) throw std::invalid_argument("MCP tool name cannot be empty");
  return s;
}
std::string output_text(const Json& response) {
  std::string text;
  const Json* output = json::if_contains(response, "output");
  if (output && output->is_array()) {
    for (const auto& item : output->array_range()) {
      if (as_string(item, "type") != "message") continue;
      const Json* content = json::if_contains(item, "content");
      if (!content || !content->is_array()) continue;
      for (const auto& part : content->array_range()) {
        const auto type = as_string(part, "type");
        if (type == "output_text") text += as_string(part, "text");
        else if (type == "refusal") text += as_string(part, "refusal");
      }
    }
  }
  if (text.empty()) text = as_string(response, "output_text");
  return text;
}
} // namespace

void SkillLibrary::load(const std::filesystem::path& directory) {
  if (!std::filesystem::is_directory(directory))
    throw std::invalid_argument("Skill directory does not exist: " + directory.string());
  std::vector<Skill> added;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file() || entry.path().filename() != "SKILL.md") continue;
    std::string contents = read_file(entry.path(), MaxSkillBytes);
    // Normalize CRLF: Windows text-mode SKILL.md files use \r\n.
    contents.erase(std::remove(contents.begin(), contents.end(), '\r'), contents.end());
    std::string name = entry.path().parent_path().filename().string();
    std::string description;
    if (contents.rfind("---\n", 0) == 0) {
      const auto end = contents.find("\n---", 4);
      if (end != std::string::npos) {
        std::istringstream lines(contents.substr(4, end - 4));
        std::string line;
        while (std::getline(lines, line)) {
          const auto colon = line.find(':');
          if (colon == std::string::npos) continue;
          const auto key = trim(line.substr(0, colon));
          if (key == "name") name = trim(line.substr(colon + 1));
          if (key == "description") description = trim(line.substr(colon + 1));
        }
        contents = contents.substr(end + 4);
      }
    }
    if (name.empty() || name.size() > 80)
      throw std::invalid_argument("Invalid skill name in " + entry.path().string());
    auto clash = [&](const Skill& s) { return s.name == name; };
    if (std::any_of(skills_.begin(), skills_.end(), clash) ||
        std::any_of(added.begin(), added.end(), clash))
      throw std::invalid_argument("Duplicate skill: " + name);
    added.push_back(Skill{std::move(name), std::move(description),
                          std::move(contents), entry.path()});
    if (skills_.size() + added.size() > 64) throw std::runtime_error("Too many skills");
  }
  std::sort(added.begin(), added.end(),
            [](const Skill& a, const Skill& b) { return a.name < b.name; });
  skills_.insert(skills_.end(), std::make_move_iterator(added.begin()),
                 std::make_move_iterator(added.end()));
}
const Skill& SkillLibrary::get(const std::string& name) const {
  for (const auto& skill : skills_) if (skill.name == name) return skill;
  throw std::out_of_range("Unknown skill: " + name);
}

OpenAIResponses::OpenAIResponses(Config config) : config_(std::move(config)) {}
Json OpenAIResponses::create(const Json& request) {
  httplib::Client client(config_.base_url);
  configure(client);
  auto result = client.Post("/v1/responses", headers(config_),
                            json::serialize(request), "application/json");
  check_http(result, "/v1/responses");
  return json::parse(result->body);
}
std::string OpenAIResponses::transcribe(const std::filesystem::path& file,
                                        const std::string& model) const {
  const std::string bytes = read_file(file, MaxMediaBytes);
  httplib::Client client(config_.base_url);
  configure(client);
  httplib::UploadFormDataItems items = {
    {"file", bytes, file.filename().string(), "application/octet-stream"},
    {"model", model, "", ""}
  };
  auto result = client.Post("/v1/audio/transcriptions", headers(config_), items);
  check_http(result, "/v1/audio/transcriptions");
  const auto body = json::parse(result->body);
  const auto text = as_string(body, "text");
  if (text.empty()) throw std::runtime_error("Audio transcription returned no text");
  return text;
}
void OpenAIResponses::speech(const std::string& text,
                             const std::filesystem::path& output,
                             const std::string& voice, const std::string& model) const {
  httplib::Client client(config_.base_url);
  configure(client);
  const Json request = json::object{{"model", model}, {"voice", voice},
                                    {"input", text}, {"response_format", "mp3"}};
  auto result = client.Post("/v1/audio/speech", headers(config_),
                            json::serialize(request), "application/json");
  check_http(result, "/v1/audio/speech");
  write_file(output, result->body); // MP3 bytes; caller chooses destination filename.
}

Agent::Agent(Config config, std::shared_ptr<Model> model)
  : config_(std::move(config)),
    model_(model ? std::move(model) : std::make_shared<OpenAIResponses>(config_)),
    local_("agent-local", "1.0.0") {
  auto loopback = std::make_shared<Client>(
    [this](const Json& request) { return local_.handle(request); }, "agent-local-client");
  add_mcp("local", std::move(loopback));
}
void Agent::validate_mcp_prefix(const std::string& prefix) const {
  if (prefix.empty() || safe_tool_name(prefix) != prefix)
    throw std::invalid_argument("MCP prefix must contain only A-Z, a-z, 0-9, _ or -");
  for (const auto& endpoint : endpoints_) {
    if (endpoint.prefix == prefix)
      throw std::invalid_argument("Duplicate MCP prefix: " + prefix);
  }
}
void Agent::add_mcp(std::string prefix, std::shared_ptr<Client> client) {
  if (!client) throw std::invalid_argument("MCP client is null");
  validate_mcp_prefix(prefix);
  endpoints_.push_back({std::move(prefix), std::move(client)});
}
void Agent::add_mcp_http(std::string prefix, std::string url,
                         std::string bearer_token,
                         std::function<void(const Json&)> on_notification) {
  validate_mcp_prefix(prefix); // Check first; do not construct needless clients.
  if (url.empty()) throw std::invalid_argument("MCP HTTP URL cannot be empty");
  add_mcp(std::move(prefix), std::make_shared<Client>(
    Client::http(url, std::move(bearer_token), std::move(on_notification))));
}
void Agent::add_mcp_stdio(std::string prefix, std::string executable,
                          std::vector<std::string> args,
                          std::function<void(const Json&)> on_notification) {
  validate_mcp_prefix(prefix); // Reject collisions before starting a process.
  if (executable.empty()) throw std::invalid_argument("MCP executable cannot be empty");
  add_mcp(std::move(prefix), std::make_shared<Client>(
    Client::process(executable, std::move(args), std::move(on_notification))));
}
void Agent::add_mcp_oauth(std::string prefix, std::string url,
                          std::shared_ptr<mymcp::oauth::Session> session,
                          std::function<void(const Json&)> on_notification) {
  validate_mcp_prefix(prefix);
  if (url.empty()) throw std::invalid_argument("MCP OAuth URL cannot be empty");
  if (!session) throw std::invalid_argument("MCP OAuth session cannot be null");
  add_mcp(std::move(prefix), std::make_shared<Client>(
    Client::http_oauth(url, std::move(session), std::move(on_notification))));
}
void Agent::tool(std::string name, std::string description,
                 Json schema, Server::ToolHandler handler) {
  local_.add_tool(std::move(name), std::move(description),
                  std::move(schema), std::move(handler));
}
void Agent::load_skills(const std::filesystem::path& directory) { skills_.load(directory); }
void Agent::use_skill(const std::string& name) {
  skills_.get(name);
  if (std::find(active_skills_.begin(), active_skills_.end(), name) == active_skills_.end())
    active_skills_.push_back(name);
}
std::string Agent::instructions() const {
  std::string result = config_.instructions;
  for (const auto& name : active_skills_) {
    const auto& skill = skills_.get(name);
    result += "\n\n# Skill: " + skill.name + "\n" + skill.description +
              "\n\n" + skill.content;
  }
  return result;
}
Json Agent::tool_definitions(std::map<std::string, Route>& routes) {
  Json tools = json::array{};
  for (const auto& endpoint : endpoints_) {
    endpoint.client->discover();
    const Json catalog = endpoint.client->list_all("tools/list", "tools");
    for (const auto& tool : catalog.array_range()) {
      const auto name = as_string(tool, "name");
      const auto qualified = endpoint.prefix + "__" + safe_tool_name(name);
      if (qualified.size() > 64 || routes.count(qualified))
        throw std::runtime_error("MCP tool name too long or colliding: " + qualified);
      routes.emplace(qualified, Route{endpoint.client, name});
      const Json* schema = json::if_contains(tool, "inputSchema");
      tools.push_back(json::object{{"type", "function"}, {"name", qualified},
          {"description", as_string(tool, "description")},
          {"parameters", schema && schema->is_object() ? *schema :
                           Json(json::object{{"type", "object"}, {"properties", json::object{}}})},
          {"strict", false}});
    }
  }
  return tools;
}
std::string Agent::execute_tool(const Route& route, const Json& call) {
  try {
    const auto raw = as_string(call, "arguments", "{}");
    const auto args = json::parse(raw);
    if (!args.is_object()) throw std::invalid_argument("Tool arguments must be a JSON object");
    auto result = json::serialize(route.client->call(route.native_name, args));
    if (result.size() > MaxToolResultBytes)
      return R"({"isError":true,"message":"Tool output exceeds 256 KiB limit"})";
    return result;
  } catch (const std::exception& e) {
    return json::serialize(json::object{{"isError", true}, {"message", e.what()}});
  }
}
std::string Agent::run(Json user_content) {
  Json proposed = history_; // Commit only after a completed model turn.
  proposed.push_back(json::object{{"role", "user"}, {"content", std::move(user_content)}});
  std::map<std::string, Route> routes;
  const Json tools = tool_definitions(routes);
  for (unsigned round = 0; round <= config_.max_tool_rounds; ++round) {
    if (proposed.size() > config_.max_history_items)
      throw std::runtime_error("Conversation exceeded max_history_items; clear_history()");
    Json request = json::object{{"model", config_.model}, {"instructions", instructions()},
                                {"input", proposed}, {"store", false},
                                {"parallel_tool_calls", false}};
    if (!tools.empty()) request["tools"] = tools;
    const Json response = model_->create(request);
    const auto status = as_string(response, "status");
    if (!status.empty() && status != "completed")
      throw std::runtime_error("Model response status: " + status);
    const Json* output = json::if_contains(response, "output");
    if (!output || !output->is_array())
      throw std::runtime_error("Model response missing output array");
    bool has_calls = false;
    for (const auto& item : output->array_range()) {
      proposed.push_back(item);
      if (as_string(item, "type") == "function_call") has_calls = true;
    }
    if (!has_calls) {
      const auto answer = output_text(response);
      history_ = std::move(proposed);
      last_response_ = response;
      return answer;
    }
    if (round == config_.max_tool_rounds)
      throw std::runtime_error("Agent reached max_tool_rounds");
    for (const auto& item : output->array_range()) {
      if (as_string(item, "type") != "function_call") continue;
      const auto id = as_string(item, "call_id");
      const auto name = as_string(item, "name");
      if (id.empty() || !routes.count(name))
        throw std::runtime_error("Unknown tool call ID or name: " + name);
      proposed.push_back(json::object{
        {"type", "function_call_output"}, {"call_id", id},
        {"output", execute_tool(routes.at(name), item)}
      });
    }
  }
  throw std::runtime_error("Agent exceeded tool call limit");
}
std::string Agent::ask(const std::string& text) { return run(text); }
std::string Agent::ask_parts(Json content) {
  if (!content.is_array() || content.empty())
    throw std::invalid_argument("ask_parts requires a nonempty array of content parts");
  return run(std::move(content));
}
std::string Agent::ask_image(const std::string& question, const std::filesystem::path& image) {
  const auto mime = media_type(image);
  const auto data_url = "data:" + mime + ";base64," + base64(read_file(image, MaxMediaBytes));
  return ask_parts(json::array{
    json::object{{"type", "input_text"}, {"text", question}},
    json::object{{"type", "input_image"}, {"image_url", data_url}}
  });
}
std::string Agent::ask_audio(const std::filesystem::path& file, const std::string& question) {
  auto openai = std::dynamic_pointer_cast<OpenAIResponses>(model_);
  if (!openai) throw std::logic_error("ask_audio requires OpenAIResponses model backend");
  const auto transcript = openai->transcribe(file);
  return ask((question.empty() ? "Respond to this audio transcript:\n" : question + "\n") +
             transcript);
}
void Agent::speak(const std::string& text, const std::filesystem::path& output,
                  const std::string& voice) {
  auto openai = std::dynamic_pointer_cast<OpenAIResponses>(model_);
  if (!openai) throw std::logic_error("speak requires OpenAIResponses model backend");
  openai->speech(text, output, voice);
}
void Agent::clear_history() {
  history_ = json::array{};
  last_response_ = nullptr;
}
void Agent::save_history(const std::filesystem::path& file) const {
  write_file(file, json::serialize(history_));
}
void Agent::load_history(const std::filesystem::path& file) {
  Json data = json::parse(read_file(file, 8 * 1024 * 1024));
  if (!data.is_array() || data.size() > config_.max_history_items)
    throw std::runtime_error("Invalid history file");
  for (const auto& entry : data.array_range())
    if (!entry.is_object()) throw std::runtime_error("Invalid history entry");
  history_ = std::move(data);
  last_response_ = nullptr;
}
} // namespace myagent
