#include "session.hpp"
#ifdef MYAGENT_CONSOLE_DEVELOPER_TOOLS
#include "developer_tools/developer_tools.hpp"
#include <filesystem>
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace myagent_console {
namespace {
std::string lower_ascii(std::string text) {
  for(char& c:text)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
  return text;
}

std::filesystem::path default_settings_path() {
#ifdef _WIN32
  if(const wchar_t* base=_wgetenv(L"APPDATA"))return std::filesystem::path(base)/"myAgent"/"console.json";
#else
  if(const char* base=std::getenv("XDG_CONFIG_HOME"))return std::filesystem::path(base)/"myAgent"/"console.json";
  if(const char* home=std::getenv("HOME"))return std::filesystem::path(home)/".config"/"myAgent"/"console.json";
#endif
  throw std::runtime_error("无法确定用户配置目录");
}

std::string protect_key(const std::string& key) {
#ifdef _WIN32
  if(key.empty())return {};
  DATA_BLOB in{static_cast<DWORD>(key.size()),reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))},out{};
  if(!CryptProtectData(&in,L"myAgent API Key",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))
    throw std::runtime_error("无法加密 API Key");
  static constexpr char hex[]="0123456789abcdef";
  std::string result; result.reserve(out.cbData*2);
  for(DWORD i=0;i<out.cbData;++i) {result+=hex[out.pbData[i]>>4];result+=hex[out.pbData[i]&15];}
  LocalFree(out.pbData); return result;
#else
  return key;
#endif
}
std::string unprotect_key(const std::string& value) {
#ifdef _WIN32
  if(value.empty())return {};
  if(value.size()%2)throw std::runtime_error("保存的 API Key 格式无效");
  auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;};
  std::vector<BYTE> bytes;
  for(size_t i=0;i<value.size();i+=2) {int a=digit(value[i]),b=digit(value[i+1]);if(a<0||b<0)throw std::runtime_error("保存的 API Key 格式无效");bytes.push_back(static_cast<BYTE>(a*16+b));}
  DATA_BLOB in{static_cast<DWORD>(bytes.size()),bytes.data()},out{};
  if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))throw std::runtime_error("无法解密保存的 API Key");
  std::string result(reinterpret_cast<char*>(out.pbData),out.cbData);LocalFree(out.pbData);return result;
#else
  return value;
#endif
}
std::string trim(std::string_view input) {
  const auto start=input.find_first_not_of(" \t\r\n");
  if(start==std::string_view::npos)return {};
  const auto stop=input.find_last_not_of(" \t\r\n");
  return std::string(input.substr(start,stop-start+1));
}

bool valid_identifier(std::string_view name) {
  if(name.empty() || name.size()>40)return false;
  for(const unsigned char ch:name)
    if(!std::isalnum(ch) && ch!='_' && ch!='-')return false;
  return true;
}

bool valid_env_name(std::string_view name) {
  if(name.empty() || name.size()>128)return false;
  if(!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front()=='_'))
    return false;
  for(const unsigned char ch:name)
    if(!std::isalnum(ch) && ch!='_')return false;
  return true;
}

// 词法处理只用来识别 slash 参数。不会经 shell 执行命令。
// Windows 文件路径的普通反斜杠会保留，支持单双引号括住有空格的路径。
std::vector<std::string> words(std::string_view line) {
  std::vector<std::string> result;
  std::string current;
  char quote=0;
  bool started=false;
  for(std::size_t i=0;i<line.size();++i) {
    const char c=line[i];
    if(quote) {
      if(c==quote) {quote=0;continue;}
      if(quote=='"' && c=='\\' && i+1<line.size() &&
         (line[i+1]=='"' || line[i+1]=='\\')) {
        current+=line[++i];continue;
      }
      current+=c;
    } else if(c=='"' || c=='\'') {
      quote=c;started=true;
    } else if(std::isspace(static_cast<unsigned char>(c))) {
      if(started) {result.push_back(std::move(current));current.clear();started=false;}
    } else {current+=c;started=true;}
  }
  if(quote)throw std::invalid_argument("参数引号没有闭合");
  if(started)result.push_back(std::move(current));
  return result;
}

bool safe_http_url(std::string_view url) {
  if(url.size()>2048 || url.find('?')!=std::string_view::npos ||
     url.find('#')!=std::string_view::npos || url.find('@')!=std::string_view::npos)
    return false;
  bool secure=url.starts_with("https://");
  bool insecure=url.starts_with("http://");
  if(!secure && !insecure)return false;
  const auto start=secure?8u:7u;
  const auto end=url.find('/',start);
  const auto authority=url.substr(start,end==std::string_view::npos
                                            ?std::string_view::npos:end-start);
  if(authority.empty())return false;
  if(secure)return true;
  // 默认拒绝把 API Key / Bearer Token 传到远程明文 HTTP 服务。
  return authority=="localhost" || authority.starts_with("localhost:") ||
         authority=="127.0.0.1" || authority.starts_with("127.0.0.1:") ||
         authority=="[::1]" || authority.starts_with("[::1]:");
}

std::string loop_name(myagent::LoopMode mode) {
  switch(mode) {
    case myagent::LoopMode::ReAct:return "react";
    case myagent::LoopMode::PlanAndExecute:return "plan";
    case myagent::LoopMode::Reflection:return "reflect";
    case myagent::LoopMode::EvaluatorOptimizer:return "evaluate";
    case myagent::LoopMode::ReWOO:return "rewoo";
    case myagent::LoopMode::ExecuteVerifyRepair:return "execute-verify-repair";
  }
  return "unknown";
}
std::string permission_name(Permissions p) {
  switch(p) {
    case Permissions::Ask:return "ask (每次工具调用确认)";
    case Permissions::Deny:return "deny (禁止全部外部工具)";
    case Permissions::Allow:return "allow (自动批准全部工具，有风险)";
  }
  return "unknown";
}

std::string help() {
  return
    "MYAGENT CONSOLE | Codex 风格独立测试客户端\n"
    "直接输入问题并回车对话；Ctrl+C 复制选区、清空输入或取消任务；Ctrl+D 空闲时退出。\n\n"
    "  /help                        显示命令\n"
    "  /status                      当前配置（永不显示密钥）\n"
    "  /listSkills                  查看已发现的 Skill\n"
    "  /baseurl <https://...>        设置模型服务地址，允许本地 HTTP\n"
    "  /model <name>                设置模型名\n"
    "  /protocol responses|chat     切换 Responses / Chat Completions\n"
    "  /apikey                      打开遮挡输入框\n"
    "  /apikey <key>                直接设置 API Key\n"
    "  /apikey env <VAR>            从环境变量获取 API Key\n"
    "  /apikey clear                清除 API Key\n"
    "  /mcp                        列出 MCP 服务\n"
    "  /tools                      查看/开关内置原生开发工具\n"
    "  /tools on|off               开启/禁用 23 个原生工具\n"
    "  /mcp add <name> stdio <program> [args...]\n"
    "  /mcp add <name> http <https://...> [TOKEN_ENV]\n"
    "  /mcp remove <name>           删除 MCP 服务\n"
    "  /mcp refresh                 刷新工具目录缓存\n"
    "  /permissions ask|deny|allow  工具执行审批；默认 ask\n"
    "  /mode react|plan|reflect|evaluate|rewoo\n"
    "  /new                        新建对话，保留当前模型和 MCP 配置\n"
    "  /clear                      清空当前对话和屏幕\n"
    "  /usage                      显示本次客户端 Token 用量\n"
    "  /exit                       退出\n\n"
    "配置会自动保存；直接输入的密钥会显示在输入框中。"
    "HTTP MCP Token 从环境变量读入，不写入聊天记录。\n"
    "提示：切换模型、URL、协议、API Key、MCP 会重建 Agent，旧对话将被清空。";
}
} // namespace

ConsoleSession::ConsoleSession(std::shared_ptr<myagent::Model> test_model,
                               std::filesystem::path settings_path)
    :settings_path_(settings_path.empty()?default_settings_path():std::move(settings_path)),
     test_model_(std::move(test_model)) {
  config_.agent_id="console";
  config_.instructions="You are a useful coding assistant. Use configured tools "
                        "only when necessary, and report failures accurately.";
  config_.base_url="https://api.openai.com";
  if(const char* v=std::getenv("MYAGENT_BASE_URL");
     v && safe_http_url(v)) config_.base_url=v;
  if(const char* v=std::getenv("MYAGENT_MODEL");v && *v)config_.model=v;
  if(const char* v=std::getenv("MYAGENT_API_KEY");v && *v)config_.api_key=v;
  else if(const char* v=std::getenv("OPENAI_API_KEY");v && *v)config_.api_key=v;
  load_settings();
}

void ConsoleSession::load_settings() {
  if(!std::filesystem::exists(settings_path_))return;
  std::ifstream input(settings_path_,std::ios::binary);
  if(!input)throw std::runtime_error("无法读取控制台配置文件");
  const std::string content((std::istreambuf_iterator<char>(input)),{});
  const auto json=myagent::json::parse(content);
  if(!json.is_object())throw std::runtime_error("控制台配置文件格式无效");
  if(json.contains("api_key")) {
    saved_key_=unprotect_key(std::string(json.at("api_key").as_string()));
    config_.api_key=*saved_key_;
  }
  if(json.contains("base_url")) {
    auto value=std::string(json.at("base_url").as_string());
    if(!safe_http_url(value))throw std::runtime_error("保存的模型地址无效");
    config_.base_url=value;saved_base_url_=std::move(value);
  }
  if(json.contains("model")) {
    auto value=std::string(json.at("model").as_string());
    if(value.empty() || value.size()>128)throw std::runtime_error("保存的模型名称无效");
    config_.model=value;saved_model_=std::move(value);
  }
}

void ConsoleSession::save_settings() const {
  myagent::json::object data;
  if(saved_key_)data["api_key"]=protect_key(*saved_key_);
  if(saved_base_url_)data["base_url"]=*saved_base_url_;
  if(saved_model_)data["model"]=*saved_model_;
  const auto parent=settings_path_.parent_path();
  if(!parent.empty())std::filesystem::create_directories(parent);
#ifndef _WIN32
  if(!parent.empty())std::filesystem::permissions(parent,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
#endif
  auto temporary=settings_path_;temporary+=".tmp";
  {
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    if(!out)throw std::runtime_error("无法保存控制台配置");
    out<<myagent::json::serialize(data);out.close();
    if(!out)throw std::runtime_error("写入控制台配置失败");
  }
#ifdef _WIN32
  if(!MoveFileExW(temporary.c_str(),settings_path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("无法替换控制台配置文件");
#else
  std::filesystem::permissions(temporary,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write,std::filesystem::perm_options::replace);
  std::filesystem::rename(temporary,settings_path_);
#endif
}

void ConsoleSession::reset_agent() {agent_.reset();}

void ConsoleSession::set_api_key(std::string value) {
  if(value.size()>16384)throw std::invalid_argument("API Key 太长");
  auto previous=saved_key_;
  saved_key_=value;
  try {save_settings();} catch(...) {saved_key_=std::move(previous);throw;}
  config_.api_key=std::move(value);
  reset_agent(); // 不继承旧模型/密钥的连接与会话历史
}

CommandResult ConsoleSession::status() const {
  std::ostringstream ss;
  ss<<"model:        "<<config_.model
    <<"\nbase_url:     "<<config_.base_url
    <<"\nprotocol:     "<<(config_.model_protocol==myagent::ModelProtocol::Responses?"responses":"chat")
    <<"\napi_key:      "<<(has_credentials()?"已设置 (隐藏)":"未设置")
    <<"\nloop:         "<<loop_name(mode_)
    <<"\npermissions:  "<<permission_name(permissions_)
    <<"\nmcp servers:  "<<endpoints_.size()
    <<"\nnative tools: "<<(native_tools_enabled_?"enabled (23)":"disabled");
  if(agent_)ss<<"\nused tokens:  "<<agent_->total_usage().total_tokens;
  return {CommandKind::Info,ss.str()};
}

std::vector<std::string> ConsoleSession::suggestions(std::string_view prefix) const {
  static const std::vector<std::string> commands{
    "/help","/status","/model","/baseurl","/apikey","/protocol","/mcp","/tools",
    "/permissions","/mode","/usage","/new","/clear","/exit","/listSkills"};
  std::vector<std::string> output;
  if(prefix.empty() || prefix.front()!='/')return output;
  const auto normalized=lower_ascii(std::string(prefix));
  for(const auto& command:commands)
    if(lower_ascii(command).starts_with(normalized) && output.size()<6)output.push_back(command);
  return output;
}

std::string ConsoleSession::mcp_command(const std::vector<std::string>& w,
                                        CommandKind& kind) {
  if(w.size()==1 || (w.size()==2 && w[1]=="list")) {
    std::string output="MCP 服务 ("+std::to_string(endpoints_.size())+")";
    for(const auto& e:endpoints_) {
      output+="\n  "+e.name+"  "+e.transport+"  "+e.target;
      if(!e.bearer_env.empty())output+="  [TOKEN_ENV: "+e.bearer_env+"]";
    }
    output+="\n使用 /mcp add <name> stdio|http ... 添加服务";
    return output;
  }
  if(w.size()==2 && w[1]=="refresh") {
    if(agent_)agent_->invalidate_mcp_catalog();
    return "MCP 目录缓存已请求失效；下次对话时按需重新发现。";
  }
  if(w.size()==3 && w[1]=="remove") {
    const auto it=std::find_if(endpoints_.begin(),endpoints_.end(),
                              [&](const auto& e){return e.name==w[2];});
    if(it==endpoints_.end()){kind=CommandKind::Error;return "MCP 服务不存在: "+w[2];}
    endpoints_.erase(it);
    reset_agent();
    return "MCP 服务已删除；Agent 会话已重置。";
  }
  if(w.size()>=5 && w[1]=="add") {
    const auto& name=w[2];
    if(!valid_identifier(name)) {
      kind=CommandKind::Error;return "MCP 名称仅允许字母数字、_、-，最长 40 字符。";
    }
    if(std::any_of(endpoints_.begin(),endpoints_.end(),
                   [&](const auto& e){return e.name==name;})) {
      kind=CommandKind::Error;return "MCP 名称重复。请先 /mcp remove。";
    }
    if(endpoints_.size()>=32){kind=CommandKind::Error;return "最多允许配置 32 个 MCP 服务。";}
    McpEntry entry;
    entry.name=name;
    entry.transport=w[3];
    entry.target=w[4];
    if(entry.transport=="stdio") {
      if(w.size()>36 || entry.target.empty() || entry.target.size()>2048) {
        kind=CommandKind::Error;return "stdio 路径或参数数量超出限制。";
      }
      entry.args.assign(w.begin()+5,w.end());
    } else if(entry.transport=="http") {
      if(w.size()>6 || !safe_http_url(entry.target)) {
        kind=CommandKind::Error;
        return "HTTP MCP 必须使用 HTTPS 或本机 loopback HTTP，不支持 URL 凭据/查询/片段。";
      }
      if(w.size()==6) {
        if(!valid_env_name(w[5])) {
          kind=CommandKind::Error;return "TOKEN_ENV 必须是环境变量名称，不允许输入 Token 明文。";
        }
        entry.bearer_env=w[5];
      }
    } else {
      kind=CommandKind::Error;return "支持的 MCP 类型是 stdio、http；OAuth 登录需由独立宿主完成。";
    }
    endpoints_.push_back(std::move(entry));
    reset_agent();
    return "MCP 已添加；Agent 会话已重置。首次对话时会连接服务。";
  }
  kind=CommandKind::Error;
  return "用法：/mcp [list|refresh|add <name> stdio|http ...|remove <name>]";
}

CommandResult ConsoleSession::handle(std::string_view input) {
  const std::string line=trim(input);
  if(line.empty())return {CommandKind::Info,{}};
  if(line.size()>64*1024)return {CommandKind::Error,"输入过长（上限 64 KiB）。"};
  if(line.front()!='/')return {CommandKind::Send,line};
  try {
    const auto w=words(line);
    if(w.empty())return {CommandKind::Info,{}};
    const std::string cmd=lower_ascii(w[0]);
    if(cmd=="/help" && w.size()==1)return {CommandKind::Info,help()};
    if((cmd=="/status" || cmd=="/config") && w.size()==1)return status();
    if(cmd=="/listskills" && w.size()==1) {
      myagent::SkillLibrary discovered;
      if(!agent_)discovered.discover();
      const auto& skills=agent_?agent_->skills().all():discovered.all();
      std::ostringstream output;
      output<<(agent_?"当前 Agent 已发现的 Skill":"当前目录可发现的 Skill（Agent 尚未初始化）")
            <<" ("<<skills.size()<<")";
      if(skills.empty())output<<"\n未发现可用的 Skill。";
      for(const auto& skill:skills) {
        const auto path=skill.source.generic_u8string();
        output<<"\n\n  "<<skill.name<<"\n    "<<skill.description
              <<"\n    路径: "<<std::string(path.begin(),path.end())
              <<"\n    自动调用: "<<(skill.auto_invocable?"允许":"禁用");
      }
      return {CommandKind::Info,output.str()};
    }
    if((cmd=="/exit" || cmd=="/quit") && w.size()==1)
      return {CommandKind::Exit,"退出 myAgent Console"};
    if((cmd=="/new" || cmd=="/clear") && w.size()==1) {
      if(agent_)agent_->clear_history();
      return {cmd=="/clear"?CommandKind::ClearView:CommandKind::Info,
              "已开始新对话；模型、Key 与 MCP 配置保持不变。"};
    }
    if(cmd=="/usage" && w.size()==1) {
      if(!agent_)return {CommandKind::Info,"本次客户端尚无模型调用。"};
      return {CommandKind::Info,
              "Tokens (total): "+std::to_string(agent_->total_usage().total_tokens)};
    }
    if(cmd=="/apikey") {
      if(w.size()==1)return {CommandKind::SecretPrompt,"请输入 API Key（字符遮挡，设置后自动保存）"};
      if(w.size()==2 && w[1]=="clear"){
        set_api_key({});
        return {CommandKind::Info,"API Key 已清除；Agent 会话已重置。"};
      }
      if(w.size()==3 && w[1]=="env" && valid_env_name(w[2])) {
        const char* value=std::getenv(w[2].c_str());
        if(!value || !*value)return {CommandKind::Error,"环境变量不存在或为空。"};
        set_api_key(value);
        return {CommandKind::Info,"API Key 已从环境变量加载（不会显示）；会话已重置。"};
      }
      if(w.size()==2 && w[1]!="env" && !w[1].empty()) {
        set_api_key(w[1]);
        return {CommandKind::Info,"API Key 已设置；Agent 会话已重置。"};
      }
      return {CommandKind::Error,
              "用法：/apikey [<key>|clear|env <变量名>]。"};
    }
    if(cmd=="/baseurl" && w.size()==2) {
      if(!safe_http_url(w[1]))
        return {CommandKind::Error,"模型地址必须是 HTTPS 或本机 HTTP；禁止 URL 用户信息、查询和片段。"};
      auto previous=saved_base_url_;saved_base_url_=w[1];
      try {save_settings();} catch(...) {saved_base_url_=std::move(previous);throw;}
      config_.base_url=w[1];reset_agent();
      return {CommandKind::Info,"模型 Base URL 已更新；Agent 会话已重置。"};
    }
    if(cmd=="/model" && w.size()==2 && w[1].size()<=128) {
      if(w[1].empty())return {CommandKind::Error,"模型名称不能为空。"};
      auto previous=saved_model_;saved_model_=w[1];
      try {save_settings();} catch(...) {saved_model_=std::move(previous);throw;}
      config_.model=w[1];reset_agent();
      return {CommandKind::Info,"模型已设置为 "+w[1]+"；Agent 会话已重置。"};
    }
    if(cmd=="/protocol" && w.size()==2) {
      if(w[1]=="responses")config_.model_protocol=myagent::ModelProtocol::Responses;
      else if(w[1]=="chat")config_.model_protocol=myagent::ModelProtocol::ChatCompletions;
      else return {CommandKind::Error,"使用 /protocol responses 或 /protocol chat。"};
      reset_agent();
      return {CommandKind::Info,"协议已更新；Agent 会话已重置。"};
    }
    if(cmd=="/permissions" && w.size()==2) {
      if(w[1]=="ask")permissions_=Permissions::Ask;
      else if(w[1]=="deny")permissions_=Permissions::Deny;
      else if(w[1]=="allow")permissions_=Permissions::Allow;
      else return {CommandKind::Error,"使用 /permissions ask|deny|allow"};
      return {CommandKind::Info,"工具权限模式："+permission_name(permissions_)};
    }
    if(cmd=="/mode" && w.size()==2) {
      if(w[1]=="react")mode_=myagent::LoopMode::ReAct;
      else if(w[1]=="plan")mode_=myagent::LoopMode::PlanAndExecute;
      else if(w[1]=="reflect")mode_=myagent::LoopMode::Reflection;
      else if(w[1]=="evaluate")mode_=myagent::LoopMode::EvaluatorOptimizer;
      else if(w[1]=="rewoo")mode_=myagent::LoopMode::ReWOO;
      else return {CommandKind::Error,
                   "支持 react|plan|reflect|evaluate|rewoo；验证修复模式需要宿主真实验证器，不在控制台中伪造。"};
      return {CommandKind::Info,"Loop 模式已切换为 "+loop_name(mode_)};
    }
    if(cmd=="/tools") {
#ifdef MYAGENT_CONSOLE_DEVELOPER_TOOLS
      if(w.size()==1) {
        return {CommandKind::Info,
          std::string("本地原生工具：")+(native_tools_enabled_?"已启用":"已禁用")+
          "\n23 个工具来自独立 developer_tools 库，工作区为控制台启动目录。" +
          "\n工具调用默认逐次询问；危险写入、编译、下载必须经批准。" +
          "\n支持 /tools on 与 /tools off（修改后清除当前对话）。"};
      }
      if(w.size()==2 && (w[1]=="on" || w[1]=="off")) {
        native_tools_enabled_=(w[1]=="on");
        reset_agent();
        return {CommandKind::Info,
          native_tools_enabled_?"原生开发工具已开启（23 个）；会话已重置。":
                                "原生开发工具已关闭；会话已重置。"};
      }
      return {CommandKind::Error,"用法：/tools [on|off]"};
#else
      return {CommandKind::Error,"当前构建未开启 DeveloperTools 扩展库。"};
#endif
    }
    if(cmd=="/mcp") {
      CommandKind kind=CommandKind::Info;
      auto answer=mcp_command(w,kind);
      return {kind,std::move(answer)};
    }
    return {CommandKind::Error,"未知命令，请输入 /help。"};
  } catch(const std::exception& e) {
    return {CommandKind::Error,std::string("参数错误：")+e.what()};
  }
}

void ConsoleSession::ensure_agent() {
  if(agent_)return;
  if(config_.base_url=="https://api.openai.com" && !has_credentials() && !test_model_)
    throw std::runtime_error("缺少 API Key。输入 /apikey 或设置 MYAGENT_API_KEY。");
  config_.on_model_http_json=[this](std::string_view kind,const myagent::Json& body) {
    std::ofstream log("myagent_console.log",std::ios::binary|std::ios::app);
    if(!log)throw std::runtime_error("无法打开当前目录的 myagent_console.log");
    std::string serialized=myagent::json::serialize(body);
    if(!config_.api_key.empty()) {
      const auto quoted=myagent::json::serialize(myagent::Json(config_.api_key));
      const auto key=quoted.substr(1,quoted.size()-2);
      for(std::size_t pos=0;(pos=serialized.find(key,pos))!=std::string::npos;pos+=10)
        serialized.replace(pos,key.size(),"[REDACTED]");
    }
    log<<"{\"event\":"<<myagent::json::serialize(myagent::Json(std::string(kind)))
       <<",\"data\":"<<serialized<<"}\n";
    log.flush();
    if(!log)throw std::runtime_error("写入 myagent_console.log 失败");
  };
  auto created=std::make_unique<myagent::Agent>(config_,test_model_);
#ifdef MYAGENT_CONSOLE_DEVELOPER_TOOLS
  // 外部 MCP 与本地原生工具共用同一 MCP Schema/权限回路，但无需启动子进程。
  // 所有工具约束在控制台的启动工作目录；修改权限后会重建 Agent。
  if(native_tools_enabled_) {
    developer_tools::RegisterToAgent(*created,
      {std::filesystem::current_path()},developer_tools::ToolOptions::All());
  }
#endif
  for(const auto& endpoint:endpoints_) {
    if(endpoint.transport=="stdio") {
      created->add_mcp_stdio(endpoint.name,endpoint.target,endpoint.args);
    } else {
      std::string token;
      if(!endpoint.bearer_env.empty()) {
        const char* value=std::getenv(endpoint.bearer_env.c_str());
        if(!value || !*value)
          throw std::runtime_error("MCP HTTP Token 环境变量未设置："+endpoint.bearer_env);
        token=value;
      }
      created->add_mcp_http(endpoint.name,endpoint.target,std::move(token));
    }
  }
  agent_=std::move(created);
}

myagent::LoopResult ConsoleSession::chat(const std::string& message,
    const std::function<void(const std::string&,const std::string&)>& on_delta,
    const std::function<bool(const myagent::ToolApprovalRequest&)>& on_approval,
    const std::function<void(const std::string&)>& on_activity,
    const std::function<bool()>& cancelled) {
  ensure_agent();
  myagent::LoopOptions options;
  options.max_iterations=12;
  options.max_model_calls=32;
  options.max_tool_calls=32;
  options.timeout=std::chrono::minutes(10);
  options.cancelled=cancelled;
  options.on_text_delta=on_delta;
  options.on_tool_approval=[&](const myagent::ToolApprovalRequest& request) {
    switch(permissions_) {
      case Permissions::Allow:return myagent::ToolDecision::Approve;
      case Permissions::Deny:return myagent::ToolDecision::Deny;
      case Permissions::Ask:
        return on_approval && on_approval(request)
          ?myagent::ToolDecision::Approve:myagent::ToolDecision::Deny;
    }
    return myagent::ToolDecision::Deny;
  };
  options.on_event=[&](const myagent::Json& event) {
    if(!on_activity || !event.is_object())return;
    if(event.contains("event") && event.at("event").is_string()) {
      const auto kind=std::string(event.at("event").as_string());
      if(kind=="tool_started" || kind=="tool_completed" ||
         kind=="model_started" || kind=="model_completed")
        on_activity(kind);
    }
  };
  return agent_->run_loop(message,mode_,options);
}

} // namespace myagent_console
