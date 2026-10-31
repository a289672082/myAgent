#include "myagent/agent.hpp"
#include "httplib.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <set>
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
std::string read_bounded_file(const std::filesystem::path& file, std::size_t limit) {
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
// Split the origin from the HTTP request target. Passing a URL with a path
// directly to httplib::Client can drop that path, so POST using the target.
struct ParsedUrl {
  std::string origin;
  std::string path;
  std::string query;
};
ParsedUrl parse_base_url(const std::string& url) {
  std::size_t scheme_size = 0;
  if (url.rfind("https://", 0) == 0) scheme_size = 8;
  else if (url.rfind("http://", 0) == 0) scheme_size = 7;
  else throw std::invalid_argument("OpenAI base URL must start with http:// or https://");
  if (url.find('#') != std::string::npos)
    throw std::invalid_argument("OpenAI base URL must not contain a fragment");
  const auto authority_end = url.find_first_of("/?", scheme_size);
  const std::string origin = url.substr(0, authority_end);
  const auto authority = origin.substr(scheme_size);
  if (authority.empty() || authority.find('@') != std::string::npos)
    throw std::invalid_argument("OpenAI base URL requires a host without userinfo");
  const auto query_pos = url.find('?', scheme_size);
  const auto path_end = query_pos == std::string::npos ? url.size() : query_pos;
  std::string path = "/";
  if (authority_end != std::string::npos && url[authority_end] == '/')
    path = url.substr(authority_end, path_end - authority_end);
  return {origin, std::move(path),
          query_pos == std::string::npos ? std::string{} : url.substr(query_pos)};
}
std::string responses_target(const Config& config, const ParsedUrl& url) {
  if (!config.append_responses_path) return url.path + url.query;
  std::string path = url.path;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path.ends_with("/v1/responses")) return path + url.query;
  return (path == "/" ? std::string{} : path) + "/v1/responses" + url.query;
}

httplib::Headers headers(const Config& config) {
  // Model backend validates explicit credentials at construction.
  return {{"Authorization", "Bearer " + config.api_key}};
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

// Read only the YAML metadata at discovery time; body is read when selected.
void SkillLibrary::load_directory(const std::filesystem::path& directory,
                                  bool ignore_duplicates) {
  if (!std::filesystem::is_directory(directory))
    throw std::invalid_argument("Skill directory does not exist: " + directory.string());
  std::vector<Skill> added;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied)) {
    if (entry.is_symlink() || !entry.is_regular_file() ||
        entry.path().filename() != "SKILL.md") continue;
    if (std::filesystem::file_size(entry.path()) > MaxSkillBytes)
      throw std::runtime_error("Skill exceeds size limit: " + entry.path().string());
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read skill metadata");
    std::string line;
    if (!std::getline(in, line) || trim(line) != "---")
      throw std::invalid_argument("Skill requires YAML frontmatter: " + entry.path().string());
    std::string name;
    std::string description;
    std::size_t metadata_bytes = 0;
    bool closed = false;
    while (std::getline(in, line)) {
      metadata_bytes += line.size() + 1;
      if (metadata_bytes > 16 * 1024)
        throw std::runtime_error("Skill frontmatter exceeds 16 KiB");
      if (trim(line) == "---") { closed = true; break; }
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const auto key = trim(line.substr(0, colon));
      if (key == "name") name = trim(line.substr(colon + 1));
      if (key == "description") description = trim(line.substr(colon + 1));
    }
    if (!closed || name.empty() || description.empty() || name.size() > 64 ||
        description.size() > 1024 || name.find_first_of("\r\n") != std::string::npos)
      throw std::invalid_argument("Invalid Skill frontmatter: " + entry.path().string());
    auto clash = [&](const Skill& skill) { return skill.name == name; };
    if (std::any_of(skills_.begin(), skills_.end(), clash) ||
        std::any_of(added.begin(), added.end(), clash)) {
      if (ignore_duplicates) continue; // Project Skill wins over a global Skill.
      throw std::invalid_argument("Duplicate skill: " + name);
    }
    added.push_back(Skill{std::move(name), std::move(description), {}, entry.path()});
    if (skills_.size() + added.size() > 64) throw std::runtime_error("Too many skills");
  }
  std::sort(added.begin(), added.end(),
            [](const Skill& a, const Skill& b) { return a.name < b.name; });
  skills_.insert(skills_.end(), std::make_move_iterator(added.begin()),
                 std::make_move_iterator(added.end()));
}
void SkillLibrary::load(const std::filesystem::path& directory) {
  load_directory(directory, false);
}
void SkillLibrary::discover() {
  namespace fs = std::filesystem;
  const auto cwd = fs::current_path();
  // Most-local first; stop ascending at a Git worktree boundary.
  auto dir = cwd;
  for (int depth = 0; depth < 16; ++depth) {
    for (const auto& relative : {".agents/skills", ".claude/skills"}) {
      const auto root = dir / relative;
      if (fs::is_directory(root)) load_directory(root, true);
    }
    if (dir == cwd && fs::is_directory(dir / "skills"))
      load_directory(dir / "skills", true); // Backward-compatible examples.
    if (fs::exists(dir / ".git") || dir == dir.root_path()) break;
    const auto parent = dir.parent_path();
    if (parent == dir || parent.empty()) break;
    dir = parent;
  }
  // HOME / USERPROFILE is for Skill discovery only, never model credentials.
  const char* home = std::getenv("HOME");
#ifdef _WIN32
  if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
  if (home && *home) {
    for (const auto& relative : {".agents/skills", ".claude/skills"}) {
      const auto root = fs::path(home) / relative;
      if (fs::is_directory(root)) load_directory(root, true);
    }
  }
}
const Skill& SkillLibrary::get(const std::string& name) const {
  for (const auto& skill : skills_) if (skill.name == name) return skill;
  throw std::out_of_range("Unknown skill: " + name);
}
std::string SkillLibrary::read(const std::string& name) const {
  const auto& skill = get(name);
  std::string contents = read_bounded_file(skill.source, MaxSkillBytes);
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'), contents.end());
  return contents;
}
std::vector<std::string> SkillLibrary::supporting_files(const std::string& name) const {
  const auto root = get(name).source.parent_path();
  std::vector<std::string> result;
  for (const auto& folder : {"references", "scripts", "assets"}) {
    const auto start = root / folder;
    if (!std::filesystem::is_directory(start) || std::filesystem::is_symlink(start))
      continue;
    for (auto iter = std::filesystem::recursive_directory_iterator(
          start, std::filesystem::directory_options::skip_permission_denied);
         iter != std::filesystem::recursive_directory_iterator(); ++iter) {
      if (iter->is_symlink()) { iter.disable_recursion_pending(); continue; }
      if (!iter->is_regular_file()) continue;
      result.push_back(iter->path().lexically_relative(root).generic_string());
      if (result.size() >= 64) break;
    }
    if (result.size() >= 64) break;
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::string SkillLibrary::read_file(const std::string& name,
                                    const std::string& relative_path) const {
  namespace fs = std::filesystem;
  const auto root = get(name).source.parent_path();
  const fs::path rel(relative_path);
  if (rel.empty() || rel.is_absolute() || rel.has_root_name() ||
      relative_path.find('\\') != std::string::npos)
    throw std::invalid_argument("Skill file path must be relative");
  auto part = rel.begin();
  if (part == rel.end() || (*part != "references" && *part != "scripts" && *part != "assets"))
    throw std::invalid_argument("Skill file must be inside references/, scripts/ or assets/");
  for (const auto& segment : rel) {
    if (segment == ".." || segment == "." || segment.empty())
      throw std::invalid_argument("Skill file path traversal is not permitted");
  }
  fs::path path = root;
  for (const auto& segment : rel) {
    path /= segment;
    if (fs::is_symlink(path))
      throw std::invalid_argument("Skill symlinks are not permitted");
  }
  auto content = read_bounded_file(path, MaxSkillBytes);
  if (content.find('\0') != std::string::npos)
    throw std::invalid_argument("Binary Skill asset cannot be read as text");
  return content;
}

OpenAIResponses::OpenAIResponses(Config config) : config_(std::move(config)) {
  if (config_.api_key.empty())
    throw std::invalid_argument("OpenAI API key is required: set Config::api_key explicitly");
  if (config_.base_url.empty())
    throw std::invalid_argument("OpenAI base URL is required: set Config::base_url explicitly");
  (void)parse_base_url(config_.base_url);
}
Json OpenAIResponses::create(const Json& request) {
  const auto url = parse_base_url(config_.base_url);
  const auto target = responses_target(config_, url);
  httplib::Client client(url.origin);
  configure(client);
  auto result = client.Post(target, headers(config_),
                            json::serialize(request), "application/json");
  check_http(result, target);
  return json::parse(result->body);
}
std::string OpenAIResponses::transcribe(const std::filesystem::path& file,
                                        const std::string& model) const {
  const std::string bytes = read_bounded_file(file, MaxMediaBytes);
  // Audio APIs are separate endpoints on the same origin, not Responses URLs.
  const auto url = parse_base_url(config_.base_url);
  httplib::Client client(url.origin);
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
  // Audio APIs are separate endpoints on the same origin, not Responses URLs.
  const auto url = parse_base_url(config_.base_url);
  httplib::Client client(url.origin);
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
  if (config_.auto_discover_skills) skills_.discover();
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
  // Explicit activation stays pinned until the caller changes the Agent.
  if (!manual_skill_contents_.count(name))
    manual_skill_contents_.emplace(name, skills_.read(name));
}
std::string Agent::instructions() const {
  std::string result = config_.instructions;
  for (const auto& [name, content] : manual_skill_contents_)
    result += "\n\n# Explicit Skill: " + name + "\n" + content;
  if (config_.auto_skill_selection && !skills_.all().empty()) {
    result += "\n\n# Available Skills (metadata only)\n"
              "Use agent_skill_load with the matching skill name when a task "
              "would benefit from its instructions. Do not load irrelevant "
              "skills. Use agent_skill_read_file only if the loaded skill "
              "requires supporting reference material. Never execute scripts "
              "without an explicitly authorized execution tool.\n";
    std::size_t budget = 8000;
    for (const auto& skill : skills_.all()) {
      const std::string line = "- " + skill.name + ": " +
                               skill.description.substr(0, 300) + "\n";
      if (line.size() > budget) { result += "(Skill catalog truncated)\n"; break; }
      result += line;
      budget -= line.size();
    }
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
  if (config_.auto_skill_selection && !skills_.all().empty()) {
    // Keep built-in Skill tools separate from externally registered MCP routes.
    if (routes.count("agent_skill_load") || routes.count("agent_skill_read_file"))
      throw std::runtime_error("An MCP tool collides with an internal Skill tool");
    tools.push_back(json::object{
      {"type", "function"}, {"name", "agent_skill_load"},
      {"description", "Load one relevant Skill's complete SKILL.md on demand; no script execution."},
      {"parameters", json::object{
        {"type", "object"}, {"properties", json::object{
          {"name", json::object{{"type", "string"}, {"description", "Skill name from catalog"}}}
        }}, {"required", json::array{"name"}}, {"additionalProperties", false}
      }}, {"strict", false}
    });
    tools.push_back(json::object{
      {"type", "function"}, {"name", "agent_skill_read_file"},
      {"description", "Read a UTF-8 text file under a previously loaded Skill's references/, scripts/ or assets/ folder; never executes a script."},
      {"parameters", json::object{
        {"type", "object"}, {"properties", json::object{
          {"name", json::object{{"type", "string"}}},
          {"path", json::object{{"type", "string"}, {"description", "Relative path inside selected Skill"}}}
        }}, {"required", json::array{"name", "path"}}, {"additionalProperties", false}
      }}, {"strict", false}
    });
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
  std::set<std::string> loaded_skills; // Per-request automatic selection.
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
      if (id.empty()) throw std::runtime_error("Missing tool call ID");
      std::string output;
      if (name == "agent_skill_load" || name == "agent_skill_read_file") {
        if (!config_.auto_skill_selection || skills_.all().empty())
          throw std::runtime_error("Skill tools are disabled");
        try {
          const Json args = json::parse(as_string(item, "arguments", "{}"));
          if (!args.is_object()) throw std::invalid_argument("Skill args must be an object");
          const auto skill_name = as_string(args, "name");
          if (skill_name.empty()) throw std::invalid_argument("Skill name is required");
          if (name == "agent_skill_load") {
            const auto content = skills_.read(skill_name);
            loaded_skills.insert(skill_name);
            const auto files = skills_.supporting_files(skill_name);
            Json paths = json::array{};
            for (const auto& path : files) paths.push_back(path);
            output = json::serialize(json::object{
              {"name", skill_name}, {"instructions", content}, {"supporting_files", paths},
              {"note", "Scripts are listed/read as text only; not executed"}
            });
          } else {
            if (!loaded_skills.count(skill_name) && !manual_skill_contents_.count(skill_name))
              throw std::invalid_argument("Load the Skill before reading supporting files");
            const auto path = as_string(args, "path");
            output = json::serialize(json::object{
              {"name", skill_name}, {"path", path},
              {"content", skills_.read_file(skill_name, path)}
            });
          }
        } catch (const std::exception& e) {
          output = json::serialize(json::object{{"isError", true}, {"message", e.what()}});
        }
      } else {
        const auto route = routes.find(name);
        if (route == routes.end())
          throw std::runtime_error("Unknown tool: " + name);
        output = execute_tool(route->second, item);
      }
      proposed.push_back(json::object{
        {"type", "function_call_output"}, {"call_id", id},
        {"output", output}
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
  const auto data_url = "data:" + mime + ";base64," + base64(read_bounded_file(image, MaxMediaBytes));
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
  Json data = json::parse(read_bounded_file(file, 8 * 1024 * 1024));
  if (!data.is_array() || data.size() > config_.max_history_items)
    throw std::runtime_error("Invalid history file");
  for (const auto& entry : data.array_range())
    if (!entry.is_object()) throw std::runtime_error("Invalid history entry");
  history_ = std::move(data);
  last_response_ = nullptr;
}
} // namespace myagent
