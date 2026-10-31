#include "myagent/multi_agent.hpp"
#include <yaml-cpp/yaml.h>
#include <stdexcept>

namespace myagent {
namespace {
std::string scalar(const YAML::Node& map,const char* key,bool required=false) {
  const auto n=map[key];
  if (!n) {
    if(required)throw std::invalid_argument(std::string("Missing agent config: ")+key);
    return {};
  }
  if(!n.IsScalar())throw std::invalid_argument(std::string("Not a scalar: ")+key);
  return n.as<std::string>();
}
bool flag(const YAML::Node& map,const char* key,bool fallback) {
  return map[key]?map[key].as<bool>():fallback;
}
void reject_inline_secret(const YAML::Node& config,const char* key) {
  if(config[key])throw std::invalid_argument(
    std::string("Inline secrets are not allowed in team files: ")+key);
}
} // namespace

void AgentTeam::add_agents_from_file(const std::filesystem::path& file,
    const std::function<std::string(const std::string&)>& credential_resolver) {
  // key_ref 由可信宿主从秘密仓库读取；配置文件不含明文 API Key。
  if(!credential_resolver)throw std::invalid_argument("Credential resolver required");
  const auto root=YAML::LoadFile(file.string()); // YAML 同时支持 JSON 格式
  if(!root.IsMap()||!root["agents"]||!root["agents"].IsSequence()||
     root["agents"].size()>64)
    throw std::invalid_argument("Agent config requires agents array (max 64)");
  // 先检查全部结构与名字，以减少部分注册的概率；
  // 构造与外部 stdio 启动仍可能失败，调用方应处理异常。
  std::vector<AgentSpec> specs;
  for(const auto& item:root["agents"]) {
    if(!item.IsMap())throw std::invalid_argument("Each agent must be a map");
    AgentSpec spec;
    spec.name=scalar(item,"name",true);
    spec.description=scalar(item,"description",true);
    const auto cfg=item["config"];
    if(!cfg || !cfg.IsMap())throw std::invalid_argument("Agent config missing");
    reject_inline_secret(cfg,"api_key");
    spec.config.base_url=scalar(cfg,"base_url",true);
    spec.config.model=scalar(cfg,"model",true);
    const auto protocol=scalar(cfg,"model_protocol");
    if(protocol.empty()||protocol=="responses")
      spec.config.model_protocol=ModelProtocol::Responses;
    else if(protocol=="chat_completions")
      spec.config.model_protocol=ModelProtocol::ChatCompletions;
    else throw std::invalid_argument("Unknown model_protocol: "+protocol);
    spec.config.append_responses_path=flag(cfg,"append_responses_path",true);
    spec.config.auto_discover_skills=flag(cfg,"auto_discover_skills",false);
    spec.config.auto_skill_selection=flag(cfg,"auto_skill_selection",true);
    const auto ref=scalar(cfg,"api_key_ref",true);
    spec.config.api_key=credential_resolver(ref);
    if(spec.config.api_key.empty())throw std::runtime_error("Missing API credential for "+ref);
    spec.config.instructions=scalar(cfg,"instructions");
    if(spec.config.instructions.empty())spec.config.instructions="You are a helpful assistant.";
    spec.workspace=scalar(item,"workspace");
    spec.allow_workspace_write=flag(item,"allow_workspace_write",false);
    const auto skills=item["skills"];
    if(skills) {
      if(!skills.IsSequence())throw std::invalid_argument("skills must be list");
      for(const auto& entry:skills) {
        if(!entry.IsScalar())throw std::invalid_argument("skills paths must be strings");
        auto directory=std::filesystem::path(entry.as<std::string>());
        if(directory.is_relative())directory=file.parent_path()/directory;
        spec.skill_directories.push_back(std::move(directory));
      }
    }
    const auto active=item["use_skills"];
    if(active) {
      if(!active.IsSequence())throw std::invalid_argument("use_skills must be list");
      for(const auto& entry:active)spec.explicit_skills.push_back(entry.as<std::string>());
    }
    const auto mcp=item["mcp"];
    if(mcp) {
      if(!mcp.IsSequence())throw std::invalid_argument("mcp must be list");
      for(const auto& endpoint:mcp) {
        if(!endpoint.IsMap())throw std::invalid_argument("MCP entry must be map");
        reject_inline_secret(endpoint,"bearer_token");
        McpEndpointSpec connection;
        connection.prefix=scalar(endpoint,"prefix",true);
        const auto protocol=scalar(endpoint,"transport",true);
        if(protocol=="http") {
          connection.transport=McpEndpointSpec::Transport::Http;
          connection.address=scalar(endpoint,"url",true);
          if(auto token_ref=scalar(endpoint,"bearer_token_ref");!token_ref.empty())
            connection.bearer_token=credential_resolver(token_ref);
        } else if(protocol=="stdio") {
          connection.transport=McpEndpointSpec::Transport::Stdio;
          connection.address=scalar(endpoint,"executable",true);
          if(endpoint["args"]) {
            if(!endpoint["args"].IsSequence())throw std::invalid_argument("stdio args must be list");
            for(const auto& arg:endpoint["args"])
              connection.args.push_back(arg.as<std::string>());
          }
        } else throw std::invalid_argument("OAuth requires an authorized live session; register it in C++");
        spec.mcp.push_back(std::move(connection));
      }
    }
    specs.push_back(std::move(spec));
  }
  for(auto& spec:specs)add_agent(std::move(spec));
}
} // namespace myagent
