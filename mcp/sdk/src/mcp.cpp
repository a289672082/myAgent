#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "mymcp/mcp.hpp"
#include "mymcp/schema.hpp"
#include "mymcp/oauth_client.hpp"
#include "mymcp/oauth_jwt.hpp"
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <ctime>
#include <cmath>
#include <httplib.h>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <pthread.h>
#endif
#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <limits>
#include <thread>
#include <chrono>
#include <atomic>
#include <regex>
#include <cstdint>

namespace mymcp {

namespace {
const Json null_json = nullptr;
const Json& find(const Json& object, std::string_view key) {
  if (!object.is_object()) return null_json;
  if (const auto* p = json::if_contains(object, key)) return *p;
  return null_json;
}
std::string string_of(const Json& obj, std::string_view key, std::string fallback = {}) {
  const auto& v = find(obj, key);
  return v.is_string() ? std::string(v.as_string()) : fallback;
}
Json rpc_error(Json id, int code, std::string message, Json data = nullptr) {
  json::object e{{"code", code}, {"message", std::move(message)}};
  if (!data.is_null()) e["data"] = std::move(data);
  return json::object{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(e)}};
}
Json rpc_success(Json id, Json result) {
  return json::object{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}
Json complete(Json value, bool modern) {
  if (modern && value.is_object() && !value.contains("resultType"))
    value["resultType"] = "complete";
  return value;
}
std::string ascii_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string encode_b64(std::string_view s) {
  constexpr auto tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (std::size_t i = 0; i < s.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(s[i]);
    const unsigned b = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    const unsigned c = i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0;
    result += tbl[a >> 2]; result += tbl[((a & 3) << 4) | (b >> 4)];
    result += i + 1 < s.size() ? tbl[((b & 15) << 2) | (c >> 6)] : '=';
    result += i + 2 < s.size() ? tbl[c & 63] : '=';
  }
  return result;
}
std::optional<std::string> decode_b64(std::string_view input) {
  if (input.size() % 4) return std::nullopt;
  std::string r;
  for (std::size_t i = 0; i < input.size(); i += 4) {
    unsigned vals[4]{};
    for (int k = 0; k < 4; ++k) {
      char c = input[i + k];
      if (c >= 'A' && c <= 'Z') vals[k] = c - 'A';
      else if (c >= 'a' && c <= 'z') vals[k] = c - 'a' + 26;
      else if (c >= '0' && c <= '9') vals[k] = c - '0' + 52;
      else if (c == '+') vals[k] = 62;
      else if (c == '/') vals[k] = 63;
      else if (c == '=' && i + 4 == input.size() && k >= 2) vals[k] = 0;
      else return std::nullopt;
    }
    r += char((vals[0] << 2) | (vals[1] >> 4));
    if (input[i+2] != '=') r += char((vals[1] << 4) | (vals[2] >> 2));
    if (input[i+3] != '=') r += char((vals[2] << 6) | vals[3]);
  }
  return r;
}
std::string encode_header(std::string s) {
  bool safe = !s.empty() && s.front() != ' ' && s.back() != ' ' && s.front() != '\t' && s.back() != '\t';
  for (unsigned char c : s) if (c < 0x20 || c > 0x7e) safe = false;
  if (s.starts_with("=?base64?") && s.ends_with("?=")) safe = false;
  return safe ? s : "=?base64?" + encode_b64(s) + "?=";
}
std::optional<std::string> decode_header(std::string s) {
  if (s.starts_with("=?base64?")) {
    if (!s.ends_with("?=")) return std::nullopt;
    return decode_b64(std::string_view(s).substr(9, s.size()-11));
  }
  for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return std::nullopt;
  return s;
}
struct HeaderParam { std::string header; std::vector<std::string> path; std::string type; };
void extract_header_fields(const Json& schema, std::vector<std::string> path,
                           std::vector<HeaderParam>& out) {
  if (!schema.is_object()) return;
  const auto& ann = find(schema, "x-mcp-header");
  if (ann.is_string()) {
    std::string name(ann.as_string());
    std::string type = string_of(schema, "type");
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c){
      return std::isalnum(c) || std::string("!#$%&'*+-.^_`|~").find(char(c)) != std::string::npos;
    }) || (type != "integer" && type != "string" && type != "boolean"))
      throw Error(-32602, "Invalid x-mcp-header annotation");
    out.push_back({"Mcp-Param-" + name, path, type});
  }
  const auto& properties = find(schema, "properties");
  if (properties.is_object()) for (const auto& entry : properties.object_range()) { const auto name = entry.key(); const auto& child = entry.value();
    auto p = path;
    p.emplace_back(name);
    extract_header_fields(child, std::move(p), out);
  }
}
std::vector<HeaderParam> header_fields(const Json& schema) {
  std::vector<HeaderParam> out;
  extract_header_fields(schema, {}, out);
  std::map<std::string, bool> seen;
  for (const auto& p : out) if (!seen.emplace(ascii_lower(p.header), true).second)
    throw Error(-32602, "Duplicate x-mcp-header annotation");
  return out;
}
const Json& path_value(const Json& root, const std::vector<std::string>& path) {
  const Json* p = &root;
  for (const auto& part : path) p = &find(*p, part);
  return *p;
}
std::string header_value(const Json& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_bool()) return v.as_bool() ? "true" : "false";
  if (v.is_int64()) {
    if (v.as_integer<std::int64_t>() < -9007199254740991LL || v.as_integer<std::int64_t>() > 9007199254740991LL)
      throw Error(-32602, "HTTP mirrored integers must be IEEE754 safe integers");
    return std::to_string(v.as_integer<std::int64_t>());
  }
  if (v.is_uint64()) {
    if (v.as_integer<std::uint64_t>() > 9007199254740991ULL)
      throw Error(-32602, "HTTP mirrored integers must be IEEE754 safe integers");
    return std::to_string(v.as_integer<std::uint64_t>());
  }
  throw Error(-32602, "Invalid HTTP mirrored parameter value");
}
void paginate_list(Json& values, const Json& params, Json& result,
                   const std::string& method, std::size_t page_size) {
  const std::size_t total = values.size();
  const Json& cursor = find(params, "cursor");
  std::size_t offset = 0;
  if (!cursor.is_null()) {
    if (!cursor.is_string()) throw Error(-32602, "Cursor must be a string");
    const std::string token(cursor.as_string());
    const std::string prefix = method + ":";
    if (!token.starts_with(prefix)) throw Error(-32602, "Invalid cursor");
    const auto digits = token.substr(prefix.length());
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
      throw Error(-32602, "Invalid cursor");
    try {
      const auto number = std::stoull(digits);
      if (number > (std::numeric_limits<std::size_t>::max)()) throw std::out_of_range("cursor");
      offset = static_cast<std::size_t>(number);
    } catch (...) { throw Error(-32602, "Invalid cursor"); }
    if (offset >= total) throw Error(-32602, "Expired or invalid cursor");
  }
  json::array page;
  const std::size_t end = offset + (std::min)(page_size, total - offset);
  for (auto i = offset; i < end; ++i) page.push_back(values[i]);
  values = std::move(page);
  if (end < total) result["nextCursor"] = method + ":" + std::to_string(end);
}
// RFC6570 operator-aware URI template reverse matching for common scalar/list values.
// Literal, reserved, fragment, label, path, matrix and query operators are supported.
// Ambiguous exploded associative structures are rejected rather than guessed.
std::optional<json::object> template_match(std::string_view pattern,std::string_view uri) {
  if(pattern.size()>4096||uri.size()>8192)return std::nullopt;
  struct Variable{std::string name;bool explode=false;std::size_t prefix=0;};
  struct Expression{char op=0;std::vector<Variable> variables;};
  std::vector<Expression> expressions;
  std::string regex_text="^";
  auto decode=[](std::string_view value)->std::optional<std::string>{
    auto hex=[](char c)->int{
      if(c>='0'&&c<='9')return c-'0';
      if(c>='a'&&c<='f')return c-'a'+10;
      if(c>='A'&&c<='F')return c-'A'+10;
      return -1;
    };
    std::string result;
    for(std::size_t i=0;i<value.size();++i){
      if(value[i]!='%'){result+=value[i];continue;}
      if(i+2>=value.size())return std::nullopt;
      int hi=hex(value[i+1]),lo=hex(value[i+2]);
      if(hi<0||lo<0||(hi==0&&lo==0))return std::nullopt;
      result+=static_cast<char>((hi<<4)|lo);i+=2;
    }
    return result;
  };
  for(std::size_t i=0;i<pattern.size();++i){
    if(pattern[i]=='}')return std::nullopt;
    if(pattern[i]!='{'){
      if(std::string_view(R"(\.[]{}()+*?^$|)").find(pattern[i])!=std::string_view::npos)
        regex_text.push_back('\\');
      regex_text+=pattern[i];continue;
    }
    auto end=pattern.find('}',i+1);
    if(end==std::string_view::npos||end==i+1)return std::nullopt;
    auto spec=pattern.substr(i+1,end-i-1);
    Expression expr;
    if(std::string_view("+#./;?&").find(spec.front())!=std::string_view::npos){
      expr.op=spec.front();spec.remove_prefix(1);
    }
    if(spec.empty()||expressions.size()>=32)return std::nullopt;
    for(std::size_t first=0;first<spec.size();){
      auto next=spec.find(',',first);
      auto item=spec.substr(first,next==std::string_view::npos?next:next-first);
      Variable var;
      if(!item.empty()&&item.back()=='*'){var.explode=true;item.remove_suffix(1);}
      else if(auto colon=item.find(':');colon!=std::string_view::npos){
        auto n=item.substr(colon+1);
        if(n.empty()||n.size()>4||!std::all_of(n.begin(),n.end(),[](unsigned char c){return std::isdigit(c);}))
          return std::nullopt;
        var.prefix=std::stoul(std::string(n));if(!var.prefix)return std::nullopt;
        item=item.substr(0,colon);
      }
      if(item.empty()||!std::all_of(item.begin(),item.end(),[](unsigned char c){
        return std::isalnum(c)||c=='_'||c=='.';
      }))return std::nullopt;
      var.name=std::string(item);
      if(std::any_of(expr.variables.begin(),expr.variables.end(),[&](const Variable& old){return old.name==var.name;}))
        return std::nullopt;
      expr.variables.push_back(std::move(var));
      if(next==std::string_view::npos)break;
      first=next+1;
    }
    switch(expr.op){
      case '?':regex_text+=R"((?:\?([^#]*?))?)";break;
      case '&':regex_text+=R"((?:&([^#]*?))?)";break;
      case ';':regex_text+=R"((?:;([^?#]*?))?)";break;
      case '/':regex_text+=R"((?:/([^?#]*?))?)";break;
      case '.':regex_text+=R"((?:\.([^/?#]*?))?)";break;
      case '#':regex_text+=R"((?:#(.*?))?)";break;
      case '+':regex_text+=R"(([^#]*?))";break;
      default:regex_text+=R"(([^/?#]*?))";break;
    }
    expressions.push_back(std::move(expr));i=end;
  }
  regex_text.push_back('$');
  try{
    std::smatch match;
    const std::string input(uri);
    if(!std::regex_match(input,match,std::regex(regex_text)))return std::nullopt;
    json::object result;
    for(std::size_t k=0;k<expressions.size();++k){
      const auto& expr=expressions[k];const auto raw=match[k+1].str();
      if(raw.empty())continue;
      const auto assign=[&](const Variable& var,std::string_view value)->bool{
        auto decoded=decode(value);if(!decoded)return false;
        if(var.prefix&&decoded->size()>var.prefix)return false;
        if(result.contains(var.name)&&result.at(var.name)!=*decoded)return false;
        result[var.name]=*decoded;return true;
      };
      const auto split=[&](char delim){
        std::vector<std::string_view> items;std::string_view str(raw);
        for(std::size_t start=0;start<=str.size();){
          auto end=str.find(delim,start);
          items.push_back(str.substr(start,end==std::string_view::npos?end:end-start));
          if(end==std::string_view::npos)break;
          start=end+1;
        }
        return items;
      };
      if(expr.op=='?'||expr.op=='&'||expr.op==';'){
        for(auto pair:split(expr.op==';'?';':'&')){
          const auto eq=pair.find('=');
          const auto name=decode(pair.substr(0,eq));if(!name)return std::nullopt;
          auto variable=std::find_if(expr.variables.begin(),expr.variables.end(),
            [&](const Variable& v){return v.name==*name;});
          if(variable==expr.variables.end())return std::nullopt;
          auto value=eq==std::string_view::npos?std::string_view{}:pair.substr(eq+1);
          if(variable->explode&&result.contains(variable->name)){
            auto decoded=decode(value);if(!decoded)return std::nullopt;
            auto& existing=result.at(variable->name);
            if(!existing.is_array())existing=json::array{existing};
            existing.push_back(*decoded);
          }else if(!assign(*variable,value))return std::nullopt;
        }
      }else{
        auto values=split(expr.op=='/'?'/':expr.op=='.'?'.':',');
        if(expr.variables.size()==1){
          const auto& var=expr.variables.front();
          if(var.explode&&values.size()>1){
            json::array list;
            for(auto value:values){auto decoded=decode(value);if(!decoded)return std::nullopt;list.push_back(*decoded);}
            result[var.name]=std::move(list);
          }else if(!assign(var,raw))return std::nullopt;
        }else{
          if(values.size()!=expr.variables.size())return std::nullopt;
          for(std::size_t v=0;v<values.size();++v)if(!assign(expr.variables[v],values[v]))return std::nullopt;
        }
      }
    }
    return result;
  }catch(const std::regex_error&){return std::nullopt;}
}

// 验证 prompt 声明字段，不允许重复名称或把非字符串值作为参数。
void validate_prompt_arguments(const Json& arguments) {
  if (!arguments.is_array()) throw std::invalid_argument("Prompt arguments must be an array");
  std::set<std::string> names;
  for (const auto& item : arguments.array_range()) {
    if (!item.is_object() || !find(item, "name").is_string() ||
        string_of(item, "name").empty() ||
        (!find(item, "description").is_null() && !find(item, "description").is_string()) ||
        (!find(item, "title").is_null() && !find(item, "title").is_string()) ||
        (!find(item, "required").is_null() && !find(item, "required").is_bool()) ||
        !names.insert(string_of(item, "name")).second)
      throw std::invalid_argument("Invalid or duplicate MCP prompt argument");
  }
}
void validate_prompt_values(const Json& arguments, const Json& supplied) {
  if (!supplied.is_object()) throw Error(-32602, "Prompt arguments must be an object");
  for (const auto& argument : arguments.array_range()) {
    const auto name = string_of(argument, "name");
    const Json& value = find(supplied, name);
    if (value.is_null()) {
      if (find(argument, "required") == true)
        throw Error(-32602, "Missing required prompt argument: " + name);
    } else if (!value.is_string()) throw Error(-32602, "Prompt arguments must be strings: " + name);
  }
  // 忽略未知参数以保持旧客户端互操作性，但不将其误认为已声明字段。
}
void validate_prompt_content(const Json& content) {
  if (content.is_array()) {
    if (content.empty()) throw Error(-32603, "Empty prompt content list");
    for (const auto& item : content.array_range()) validate_prompt_content(item);
    return;
  }
  if (!content.is_object()) throw Error(-32603, "Prompt content must be an object");
  const auto type = string_of(content, "type");
  if (type == "text") {
    if (!find(content, "text").is_string()) throw Error(-32603, "Invalid prompt text");
  } else if (type == "image" || type == "audio") {
    if (!find(content, "data").is_string() || !find(content, "mimeType").is_string() ||
        !decode_b64(string_of(content, "data")))
      throw Error(-32603, "Invalid prompt binary content");
  } else if (type == "resource") {
    const auto& resource = find(content, "resource");
    if (!resource.is_object() || !find(resource, "uri").is_string() ||
        (find(resource, "text").is_string() == find(resource, "blob").is_string()) ||
        (find(resource, "blob").is_string() && !decode_b64(string_of(resource, "blob"))))
      throw Error(-32603, "Invalid embedded prompt resource");
  } else if (type == "resource_link") {
    if (!find(content, "uri").is_string() || !find(content, "name").is_string())
      throw Error(-32603, "Invalid resource link");
  } else throw Error(-32603, "Unknown prompt content type");
}
void validate_prompt_output(const Json& result) {
  if (!result.is_object() || !find(result, "messages").is_array())
    throw Error(-32603, "Prompt result requires messages array");
  if (!find(result, "description").is_null() && !find(result, "description").is_string())
    throw Error(-32603, "Invalid prompt result description");
  for (const auto& message : find(result, "messages").array_range()) {
    if (!message.is_object() ||
        (string_of(message, "role") != "user" && string_of(message, "role") != "assistant"))
      throw Error(-32603, "Invalid prompt message role");
    validate_prompt_content(find(message, "content"));
  }
}
void validate_resource_output(const Json& result) {
  if (!result.is_object() || !find(result, "contents").is_array())
    throw Error(-32603, "Resource result requires contents");
  for (const auto& item : find(result, "contents").array_range()) {
    if (!item.is_object() || !find(item, "uri").is_string() ||
        (find(item, "text").is_string() == find(item, "blob").is_string()) ||
        (!find(item, "mimeType").is_null() && !find(item, "mimeType").is_string()) ||
        (find(item, "blob").is_string() && !decode_b64(string_of(item, "blob"))))
      throw Error(-32603, "Invalid resource contents");
  }
}
// MRTR 中各轮输入必须是已声明的方法和符合规范的结构。
void validate_mrtr_requests(const Json& requests) {
  if (!requests.is_object() || requests.empty() || requests.size() > 32)
    throw Error(-32603, "Invalid or excessive MRTR inputRequests");
  for (const auto& item : requests.object_range()) {
    const Json& req = item.value();
    const auto method = string_of(req, "method");
    const auto& params = find(req, "params");
    if (!params.is_object()) throw Error(-32603, "MRTR request params must be object");
    if (method == "elicitation/create") {
      const auto mode = string_of(params, "mode", "form");
      if (!find(params, "message").is_string() ||
          (mode != "form" && mode != "url") ||
          (mode == "form" && !find(params, "requestedSchema").is_object()) ||
          (mode == "url" && !find(params, "url").is_string()))
        throw Error(-32603, "Invalid MRTR elicitation request");
    } else if (method == "sampling/createMessage") {
      if (!find(params, "messages").is_array() ||
          !find(params, "maxTokens").is_number() ||
          find(params, "maxTokens").as_double() <= 0)
        throw Error(-32603, "Invalid MRTR sampling request");
      for (const auto& message : find(params, "messages").array_range()) {
        if (!message.is_object() ||
            (string_of(message, "role") != "user" &&
             string_of(message, "role") != "assistant"))
          throw Error(-32603, "Invalid MRTR sampling message");
        validate_prompt_content(find(message, "content"));
      }
    } else if (method != "roots/list") {
      throw Error(-32603, "Unsupported MRTR request method");
    }
  }
}
void validate_mrtr_answers(const Json& requests, const Json& answers) {
  if (!answers.is_object() || answers.size() != requests.size())
    throw Error(-32602, "MRTR inputResponses must answer each requested ID");
  for (const auto& request : requests.object_range()) {
    const auto& answer = find(answers, request.key());
    if (!answer.is_object()) throw Error(-32602, "Invalid MRTR answer");
    const auto method = string_of(request.value(), "method");
    if (method == "elicitation/create") {
      const auto action = string_of(answer, "action");
      if (action != "accept" && action != "decline" && action != "cancel")
        throw Error(-32602, "Invalid elicitation action");
      if (action == "accept") {
        const auto& content = find(answer, "content");
        if (!content.is_object()) throw Error(-32602, "Missing elicitation content");
        const auto& schema = find(find(request.value(), "params"), "requestedSchema");
        if (schema.is_object()) {
          std::string why;
          if (!validate_schema(schema, content, &why))
            throw Error(-32602, "Elicitation schema violation: " + why);
        }
      }
    } else if (method == "sampling/createMessage") {
      if (string_of(answer, "role") != "assistant")
        throw Error(-32602, "Sampling answer must be assistant message");
      validate_prompt_content(find(answer, "content"));
    } else if (method == "roots/list") {
      const auto& roots = find(answer, "roots");
      if (!roots.is_array()) throw Error(-32602, "Invalid roots answer");
      for (const auto& root : roots.array_range()) {
        if (!find(root, "uri").is_string() || !find(root, "name").is_string())
          throw Error(-32602, "Invalid roots entry");
      }
    }
  }
}
} // anonymous namespace

namespace detail {
Json make_tool_result(const Json& v) {
  return json::object{{"content", json::array{json::object{{"type", "text"}, {"text", json::serialize(v)}}}},
          {"structuredContent", v}, {"isError", false}};
}
Json make_tool_result(const std::string& s) {
  return json::object{{"content", json::array{json::object{{"type", "text"}, {"text", s}}}}, {"isError", false}};
}
Json make_tool_result(const char* s) { return make_tool_result(std::string(s)); }
Json make_tool_result(bool v) { return make_tool_result(v ? "true" : "false"); }
Json make_tool_result(int v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(long v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(long long v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(double v) { return make_tool_result(std::to_string(v)); }
} // namespace detail

Server::Server(std::string name, std::string version) : name_(std::move(name)), version_(std::move(version)) {}
void Server::add_tool(std::string name, std::string description, Json schema, ToolHandler handler) {
  if (!schema.is_object()) throw std::invalid_argument("Tool input schema must be an object");
  if (!handler) throw std::invalid_argument("Tool handler cannot be empty");
  if (!tools_.emplace(name, Tool{std::move(description), std::move(schema), std::move(handler), false}).second)
    throw std::invalid_argument("Duplicate tool: " + name);
}
void Server::add_tool_context(std::string name, std::string description, Json schema, ToolHandler handler) {
  if (!schema.is_object() || !handler) throw std::invalid_argument("Invalid tool schema or handler");
  if (!tools_.emplace(name, Tool{std::move(description), std::move(schema), std::move(handler), true}).second)
    throw std::invalid_argument("Duplicate tool: " + name);
}
void Server::add_task_tool(std::string name, std::string description, Json schema, TaskHandler handler) {
  if (!handler) throw std::invalid_argument("Invalid task handler");
  const std::string key = name;
  add_tool(std::move(name), std::move(description), std::move(schema),
    [this, job = std::move(handler)](const Json&) -> Json {
      return start_task(job);
    });
  tools_.at(key).requires_tasks = true;
}
void Server::resource(std::string uri, std::string name, std::function<std::string()> reader, std::string mime) {
  if (!resources_.emplace(uri, Resource{std::move(name), std::move(mime), std::move(reader), false}).second)
    throw std::invalid_argument("Duplicate resource: " + uri);
}
void Server::resource_binary(std::string uri, std::string name,
                             std::function<std::string()> reader, std::string mime) {
  if (!reader || mime.empty() || !resources_.emplace(uri, Resource{
      std::move(name), std::move(mime), std::move(reader), true}).second)
    throw std::invalid_argument("Duplicate or invalid binary resource");
}
void Server::prompt(std::string name, std::string description, std::function<std::string(const Json&)> reader) {
  if (!prompts_.emplace(name, Prompt{std::move(description), std::move(reader), {}, json::array{}}).second)
    throw std::invalid_argument("Duplicate prompt: " + name);
}
void Server::resource_template(std::string uri_template, std::string name,
                               std::function<Json(const std::string&, const Json&)> reader,
                               std::string mime, std::string description) {
  if (!reader || uri_template.empty()) throw std::invalid_argument("Invalid resource template");
  if (!templates_.emplace(uri_template, ResourceTemplate{std::move(name), std::move(mime),
           std::move(description), std::move(reader)}).second)
    throw std::invalid_argument("Duplicate resource template");
}
void Server::prompt_full(std::string name, std::string description,
                         std::function<Json(const Json&)> handler, Json arguments) {
  if (!handler) throw std::invalid_argument("Invalid prompt handler");
  validate_prompt_arguments(arguments);
  if (!prompts_.emplace(name, Prompt{std::move(description), {}, std::move(handler),
                                     std::move(arguments)}).second)
    throw std::invalid_argument("Duplicate prompt");
}
void Server::prompt_metadata(const std::string& name, Json metadata) {
  auto it = prompts_.find(name);
  if (it == prompts_.end() || !metadata.is_object())
    throw std::invalid_argument("Unknown prompt or invalid metadata");
  if ((!find(metadata, "title").is_null() && !find(metadata, "title").is_string()) ||
      (!find(metadata, "icons").is_null() && !find(metadata, "icons").is_array()))
    throw std::invalid_argument("Invalid prompt title/icons");
  for (const auto& field : metadata.object_range()) {
    if (field.key() != "title" && field.key() != "icons")
      throw std::invalid_argument("Unsupported prompt metadata");
  }
  it->second.metadata = std::move(metadata);
}
void Server::completion(std::function<Json(const Json&)> handler) {
  if (!handler) throw std::invalid_argument("Invalid completion handler");
  completion_ = std::move(handler);
}
void Server::method(std::string name, std::function<Json(const Json&)> handler) {
  if (!handler || name.empty() || !methods_.emplace(name, std::move(handler)).second)
    throw std::invalid_argument("Duplicate or invalid method");
}
void Server::extension(std::string id, Json settings) {
  if (id.find('/') == std::string::npos || !settings.is_object())
    throw std::invalid_argument("Extension identifier must be namespaced and settings must be an object");
  extensions_[id] = std::move(settings);
}
void Server::protect_http(Json metadata, TokenVerifier verifier) {
  if (!verifier || !metadata.is_object() || !find(metadata, "resource").is_string() ||
      !find(metadata, "authorization_servers").is_array())
    throw std::invalid_argument("Valid protected resource metadata and token verifier required");
  token_verifier_ = std::move(verifier);
  protected_resource_metadata_ = std::move(metadata);
  cache_scope_ = "private"; // authenticated responses must not be shared between callers
}
void Server::protect_http_jwt(Json metadata, oauth::JwtPolicy policy) {
  // 令牌校验器必须和 Server 生命周期一致，使用 shared_ptr 捕获。
  auto verifier = std::make_shared<oauth::JwtVerifier>(std::move(policy));
  protect_http(std::move(metadata),
    [verifier](const std::string& token, const std::string& method,
               const std::string& name) {
      return verifier->verify(token, method, name);
    });
}
bool Server::verify_http(const std::string& token, const std::string& method,
                         const std::string& name) const {
  try { return token_verifier_ && !token.empty() && token_verifier_(token, method, name); }
  catch (...) { return false; }
}
void Server::cache(std::uint64_t ttl_ms, std::string scope) {
  if (scope != "public" && scope != "private") throw std::invalid_argument("Invalid cache scope");
  ttl_ms_ = ttl_ms;
  cache_scope_ = std::move(scope);
}
void Server::page_size(std::size_t value) {
  if (!value || value > 100000) throw std::invalid_argument("Invalid page size");
  page_size_ = value;
}
std::shared_ptr<Server::Subscription> Server::subscribe(Json id, Json filter) {
  if (!(id.is_string() || id.is_int64() || id.is_uint64()) || !filter.is_object())
    throw Error(-32602, "Invalid subscription ID/filter");
  json::object accepted;
  for (const auto& entry : filter.object_range()) { const auto name = entry.key(); const auto& value = entry.value();
    const std::string key(name);
    if (key == "resourceSubscriptions") {
      if (!value.is_array()) throw Error(-32602, "resourceSubscriptions must be an array");
      json::array resources;
      for (const auto& uri : value.array_range()) {
        if (!uri.is_string()) throw Error(-32602, "Resource subscription URI must be string");
        if (resources.size() > 10000) throw Error(-32602, "Too many subscriptions");
        resources.push_back(uri);
      }
      accepted[key] = std::move(resources);
    } else if (key == "toolsListChanged" || key == "promptsListChanged" || key == "resourcesListChanged") {
      if (!value.is_bool()) throw Error(-32602, "Subscription flags must be boolean");
      if (value.as_bool()) accepted[key] = true;
    } else throw Error(-32602, "Unknown subscription filter: " + key);
  }
  auto sub = std::make_shared<Subscription>();
  sub->id = std::move(id);
  sub->notifications = std::move(accepted);
  // Acknowledgement is always queued before any other notification.
  sub->messages.push_back(json::object{{"jsonrpc", "2.0"}, {"method", "notifications/subscriptions/acknowledged"},
    {"params", json::object{{"_meta", json::object{{"io.modelcontextprotocol/subscriptionId", sub->id}}},
                {"notifications", sub->notifications}}}});
  {
    std::lock_guard lock(subscriber_mutex_);
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
       [](const auto& entry){ return entry.expired(); }), subscribers_.end());
    subscribers_.push_back(sub);
  }
  return sub;
}
void Server::publish(const std::string& method, Json params) {
  std::string filter;
  if (method == "notifications/tools/list_changed") filter = "toolsListChanged";
  else if (method == "notifications/prompts/list_changed") filter = "promptsListChanged";
  else if (method == "notifications/resources/list_changed") filter = "resourcesListChanged";
  else if (method == "notifications/resources/updated") filter = "resourceSubscriptions";
  else throw std::invalid_argument("Unsupported subscription notification");
  if (!params.is_object()) throw std::invalid_argument("Notification parameters must be an object");
  const std::string uri = string_of(params, "uri");
  if (filter == "resourceSubscriptions" && uri.empty())
    throw std::invalid_argument("Resource update notification requires URI");
  std::lock_guard lock(subscriber_mutex_);
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    auto sub = it->lock();
    if (!sub) { it = subscribers_.erase(it); continue; }
    ++it;
    const Json& allowed = find(sub->notifications, filter);
    bool selected = allowed.is_bool() && allowed.as_bool();
    if (allowed.is_array()) for (const auto& entry : allowed.array_range())
      if (entry.is_string() && entry.as_string() == uri) selected = true;
    if (!selected) continue;
    Json payload = params;
    payload["_meta"] = json::object{{"io.modelcontextprotocol/subscriptionId", sub->id}};
    Json event = json::object{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(payload)}};
    {
      std::lock_guard stream_lock(sub->mutex);
      if (!sub->closed && sub->messages.size() < 4096) sub->messages.push_back(std::move(event));
    }
    sub->ready.notify_one();
  }
}
void Server::enable_tasks() {
  extension("io.modelcontextprotocol/tasks");
}

namespace {
std::string secure_task_id() {
  unsigned char b[24];
  if (RAND_bytes(b, sizeof(b)) != 1) throw Error(-32603, "Cryptographic RNG failure");
  constexpr char hex[] = "0123456789abcdef";
  std::string id; id.reserve(sizeof(b) * 2);
  for (unsigned char byte : b) {id += hex[byte >> 4]; id += hex[byte & 15];}
  return id;
}
std::string utc_now() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm output{};
#ifdef _WIN32
  gmtime_s(&output, &t);
#else
  gmtime_r(&t, &output);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &output);
  return buffer;
}
}

void Server::run_task(const std::shared_ptr<TaskState>& task, Json input_responses) {
  std::thread([task, input_responses = std::move(input_responses)]() mutable {
    Json answer;
    try {
      answer = task->handler(input_responses, task->cancel_requested);
      std::lock_guard lock(task->mutex);
      if (task->details.at("status") == "cancelled") return;
      task->details["lastUpdatedAt"] = utc_now();
      if (!task->cancel_requested.load() && answer.is_object() &&
          json::if_contains(answer, "resultType") && answer.at("resultType") == "input_required") {
        const Json& items = answer.at("inputRequests");
        if (!items.is_object() || items.empty()) throw Error(-32603, "Invalid task input requests");
        for (const auto& entry : items.object_range()) { const auto key = entry.key(); const auto& item = entry.value();
          if (task->used_input_ids.count(std::string(key))) throw Error(-32603, "Task input ID reused");
          if (!item.is_object()) throw Error(-32603, "Invalid input request");
          task->used_input_ids.insert(std::string(key));
        }
        task->details["status"] = "input_required";
        task->details["inputRequests"] = items;
      } else if (task->cancel_requested.load()) {
        task->details["status"] = "cancelled";
      } else if (answer.is_object()) {
        task->details["status"] = "completed";
        task->details["result"] = std::move(answer);
      } else throw Error(-32603, "Task handler must return a JSON object");
    } catch (const Error& error) {
      std::lock_guard lock(task->mutex);
      task->details["status"] = "failed";
      task->details["error"] = json::object{{"code", error.code}, {"message", error.what()}};
      task->details["lastUpdatedAt"] = utc_now();
    } catch (const std::exception& error) {
      std::lock_guard lock(task->mutex);
      task->details["status"] = "failed";
      task->details["error"] = json::object{{"code", -32603}, {"message", error.what()}};
      task->details["lastUpdatedAt"] = utc_now();
    }
  }).detach();
}

Json Server::start_task(TaskHandler handler, std::uint64_t ttl_ms, std::uint64_t poll_ms) {
  if (!extensions_.contains("io.modelcontextprotocol/tasks"))
    throw Error(-32603, "Call enable_tasks() before returning task handles");
  if (!handler || !poll_ms || !ttl_ms || poll_ms > 3600000 || ttl_ms > 604800000)
    throw Error(-32602, "Invalid task handler or task timing");
  auto task = std::make_shared<TaskState>();
  const auto id = secure_task_id();
  const auto timestamp = utc_now();
  task->handler = std::move(handler);
  task->expires_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
  task->details = json::object{{"taskId", id}, {"status", "working"},
     {"createdAt", timestamp}, {"lastUpdatedAt", timestamp},
     {"ttlMs", ttl_ms}, {"pollIntervalMs", poll_ms}};
  {
    std::lock_guard lock(task_mutex_);
    // Expired tasks may be purged, including from a long-lived server.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      if (now >= it->second->expires_at) it = tasks_.erase(it);
      else ++it;
    }
    if (tasks_.size() >= 10000) throw Error(-32603, "Task capacity exceeded");
    tasks_.emplace(id, task);
  }
  Json result = task->details;
  run_task(task, nullptr);
  result["resultType"] = "task";
  return result;
}

Json Server::task_state(const std::string& id) {
  std::shared_ptr<TaskState> task;
  {
    std::lock_guard lock(task_mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) throw Error(-32602, "Unknown taskId");
    if (std::chrono::steady_clock::now() >= it->second->expires_at) {
      tasks_.erase(it);
      throw Error(-32602, "Task expired");
    }
    task = it->second;
  }
  std::lock_guard lock(task->mutex);
  Json status = task->details;
  status["resultType"] = "complete";
  return status;
}

void Server::close_subscription(const std::shared_ptr<Subscription>& subscription) {
  if (!subscription) return;
  {
    std::lock_guard lock(subscription->mutex);
    subscription->closed = true;
  }
  subscription->ready.notify_all();
}

const Json* Server::tool_schema(const std::string& name) const {
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second.schema;
}

Json Server::handle(const Json& req, bool allow_legacy) {
  if (!req.is_object() || string_of(req, "jsonrpc") != "2.0" || !find(req, "method").is_string())
    return rpc_error(nullptr, -32600, "Invalid JSON-RPC request");
  const Json id = find(req, "id");
  const bool notification = !req.contains("id");
  if (!notification && !(id.is_string() || id.is_int64() || id.is_uint64()))
    return rpc_error(nullptr, -32600, "Invalid JSON-RPC ID");
  const std::string method = string_of(req, "method");
  const Json& params = find(req, "params");
  const Json& meta = find(params, "_meta");
  const bool modern = meta.is_object() && !find(meta, "io.modelcontextprotocol/protocolVersion").is_null();
  if (notification) {
    // Modern core notifications are restricted to stdio cancellation.
    if (method == "notifications/cancelled" || method == "notifications/initialized") return nullptr;
    return nullptr;
  }
  Json result;
  try {
    if (method == "initialize" && !modern && allow_legacy) {
      legacy_initialized_ = true;
      auto offered = string_of(params, "protocolVersion", LegacyProtocolVersion);
      if (offered != "2025-11-25" && offered != "2025-06-18" && offered != "2025-03-26")
        offered = LegacyProtocolVersion;
      Json caps = json::object{{"tools", json::object{}}, {"resources", json::object{}}, {"prompts", json::object{}}};
      return rpc_success(id, json::object{{"protocolVersion", offered}, {"capabilities", caps},
                              {"serverInfo", json::object{{"name", name_}, {"version", version_}}}});
    }
    if (!modern && (!legacy_initialized_ || !allow_legacy)) throw Error(-32600, "Modern requests require per-request _meta");
    if (modern) {
      if (!params.is_object()) throw Error(-32602, "Request parameters must be an object");
      if (!find(meta, "io.modelcontextprotocol/clientCapabilities").is_object())
        throw Error(-32602, "Missing clientCapabilities in _meta");
      const Json& info = find(meta, "io.modelcontextprotocol/clientInfo");
      if (!info.is_object() || !find(info, "name").is_string() ||
          !find(info, "version").is_string())
        throw Error(-32602, "Missing or invalid clientInfo in _meta");
      auto version = string_of(meta, "io.modelcontextprotocol/protocolVersion");
      if (version != ProtocolVersion) {
        return rpc_error(id, -32022, "Unsupported protocol version",
                         json::object{{"requested", version}, {"supported", json::array{ProtocolVersion, LegacyProtocolVersion}}});
      }
    }
    if (method == "server/discover") {
      if (!modern) throw Error(-32601, "Method not found");
      json::object capabilities{{"tools", json::object{}}, {"resources", json::object{}},
                                {"prompts", json::object{}}};
      if (completion_) capabilities["completions"] = json::object{};
      if (!extensions_.empty()) capabilities["extensions"] = extensions_;
      result = json::object{{"supportedVersions", allow_legacy ? json::array{ProtocolVersion, LegacyProtocolVersion} : json::array{ProtocolVersion}},
                {"capabilities", std::move(capabilities)}};
      if (!instructions_.empty()) result["instructions"] = instructions_;
    } else if (method == "tools/list") {
      json::array list;
      for (const auto& [name, tool] : tools_) list.push_back(json::object{{"name", name}, {"description", tool.description}, {"inputSchema", tool.schema}});
      result = json::object{{"tools", std::move(list)}};
      paginate_list(result.at("tools"), params, result, method, page_size_);
    } else if (method == "tools/call") {
      const auto name = string_of(params, "name");
      const auto it = tools_.find(name);
      if (it == tools_.end()) throw Error(-32602, "Unknown tool: " + name);
      if (it->second.requires_tasks &&
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Task tool requires the io.modelcontextprotocol/tasks client extension");
      const Json& args = find(params, "arguments");
      if (!args.is_null() && !args.is_object()) throw Error(-32602, "Tool arguments must be an object");
      // Validate before entering the tool, including raw/context handlers.
      const Json actual_args = args.is_object() ? args : Json(json::object{});
      std::string schema_error;
      if (!validate_schema(it->second.schema, actual_args, &schema_error))
        throw Error(-32602, "Tool inputSchema violation: " + schema_error);
      try {
        result = it->second.handler(it->second.with_context ? params :
                                     (args.is_object() ? args : json::object{}));
      } catch (const std::exception& e) {
        result = json::object{{"content", json::array{json::object{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}};
      }
      if (!result.is_object()) throw Error(-32603, "Invalid tool result");
    } else if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
      if (!modern || !extensions_.contains("io.modelcontextprotocol/tasks") ||
          !find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions").is_object() ||
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Missing Required Client Capability: io.modelcontextprotocol/tasks");
      const std::string task_id = string_of(params, "taskId");
      if (task_id.empty()) throw Error(-32602, "Missing taskId");
      if (method == "tasks/get") result = task_state(task_id);
      else {
        std::shared_ptr<TaskState> task;
        {
          std::lock_guard lock(task_mutex_);
          auto it = tasks_.find(task_id);
          if (it == tasks_.end()) throw Error(-32602, "Unknown taskId");
          if (std::chrono::steady_clock::now() >= it->second->expires_at) {
            tasks_.erase(it);
            throw Error(-32602, "Task expired");
          }
          task = it->second;
        }
        if (method == "tasks/cancel") {
          task->cancel_requested = true;
          {
            std::lock_guard lock(task->mutex);
            if (task->details.at("status") == "input_required") {
              task->details["status"] = "cancelled";
              task->details.erase("inputRequests");
              task->details["lastUpdatedAt"] = utc_now();
            }
          }
        } else {
          const auto& input = find(params, "inputResponses");
          if (!input.is_object()) throw Error(-32602, "inputResponses must be an object");
          bool resume = false;
          Json answers;
          {
            std::lock_guard lock(task->mutex);
            if (task->details.at("status") == "input_required") {
              auto& outstanding = task->details.at("inputRequests");
              for (const auto& entry : input.object_range()) { const auto key = entry.key(); const auto& answer = entry.value();
                if (outstanding.contains(key)) {
                  task->responses[std::string(key)] = answer;
                  outstanding.erase(key);
                }
              }
              if (outstanding.empty()) {
                resume = true;
                answers = task->responses;
                task->responses.clear();
                task->details.erase("inputRequests");
                task->details["status"] = "working";
                task->details["lastUpdatedAt"] = utc_now();
              }
            }
          }
          if (resume) run_task(task, std::move(answers));
        }
        result = json::object{};
      }
    } else if (method == "resources/list") {
      json::array list;
      for (const auto& [uri, res] : resources_) list.push_back(json::object{{"uri", uri}, {"name", res.name}, {"mimeType", res.mime}});
      result = json::object{{"resources", std::move(list)}};
      paginate_list(result.at("resources"), params, result, method, page_size_);
    } else if (method == "resources/read") {
      const auto uri = string_of(params, "uri");
      const auto it = resources_.find(uri);
      if (it != resources_.end()) {
        Json item = json::object{{"uri", uri}, {"mimeType", it->second.mime}};
        const auto contents = it->second.reader();
        if (it->second.binary) item["blob"] = encode_b64(contents);
        else item["text"] = contents;
        result = json::object{{"contents", json::array{std::move(item)}}};
      } else {
        bool matched = false;
        for (const auto& [pattern, entry] : templates_) {
          auto bindings = template_match(pattern, uri);
          if (!bindings) continue;
          result = entry.reader(uri, *bindings);
          matched = true;
          break;
        }
        if (!matched) throw Error(-32602, "Resource not found: " + uri);
      }
      if (string_of(result, "resultType") != "input_required") validate_resource_output(result);
    } else if (method == "resources/templates/list") {
      json::array list;
      for (const auto& [uri, t] : templates_) {
        Json entry = json::object{{"uriTemplate", uri}, {"name", t.name}, {"mimeType", t.mime}};
        if (!t.description.empty()) entry["description"] = t.description;
        list.push_back(std::move(entry));
      }
      result = json::object{{"resourceTemplates", std::move(list)}};
      paginate_list(result.at("resourceTemplates"), params, result, method, page_size_);
    } else if (method == "prompts/list") {
      json::array list;
      for (const auto& [name, prompt] : prompts_) {
        Json desc = json::object{{"name", name}, {"description", prompt.description}};
        if (!prompt.arguments.empty()) desc["arguments"] = prompt.arguments;
        if (prompt.metadata.is_object()) for (const auto& field : prompt.metadata.object_range())
          desc[std::string(field.key())] = field.value();
        list.push_back(std::move(desc));
      }
      result = json::object{{"prompts", std::move(list)}};
      paginate_list(result.at("prompts"), params, result, method, page_size_);
    } else if (method == "prompts/get") {
      const auto name = string_of(params, "name");
      const auto it = prompts_.find(name);
      if (it == prompts_.end()) throw Error(-32602, "Unknown prompt: " + name);
      const Json& args = find(params, "arguments");
      const Json values = args.is_null() ? Json(json::object{}) : args;
      validate_prompt_values(it->second.arguments, values);
      if (it->second.full_reader) result = it->second.full_reader(values);
      else result = json::object{{"messages", json::array{json::object{{"role", "user"}, {"content", json::object{{"type", "text"}, {"text", it->second.reader(values)}}}}}}};
      if (string_of(result, "resultType") != "input_required") validate_prompt_output(result);
    } else if (method == "completion/complete" && completion_) {
      if (!params.is_object() || !find(params, "ref").is_object() || !find(params, "argument").is_object())
        throw Error(-32602, "Invalid completion request");
      result = completion_(params);
      if (!result.is_object() || !find(result, "completion").is_object() ||
          !find(find(result, "completion"), "values").is_array() ||
          find(find(result, "completion"), "values").size() > 100)
        throw Error(-32603, "Invalid completion result");
    } else if (auto registered = methods_.find(method); registered != methods_.end()) {
      if (!params.is_object()) throw Error(-32602, "Invalid method parameters");
      result = registered->second(params);
    } else if (method == "ping" && !modern) {
      result = json::object{};
    } else {
      throw Error(-32601, "Method not found: " + method);
    }
    if (!result.is_object()) throw Error(-32603, "Handler must return an object");
    if (modern && find(result, "resultType") == "task") {
      if (method != "tools/call" || !extensions_.contains("io.modelcontextprotocol/tasks") ||
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Task handles require the io.modelcontextprotocol/tasks client extension");
    }
    if (modern && find(result, "resultType") == "input_required") {
      if (method != "tools/call" && method != "prompts/get" && method != "resources/read")
        throw Error(-32603, "input_required is not allowed for this method");
      const auto& requests = find(result, "inputRequests");
      validate_mrtr_requests(requests);
      const auto& state = find(result, "requestState");
      if (!state.is_null() && (!state.is_string() || state.as_string().size() > 16384))
        throw Error(-32603, "Invalid MRTR requestState");
      for (const auto& item : requests.object_range()) {
        const auto method_name = string_of(item.value(), "method");
        const auto& caps = find(meta, "io.modelcontextprotocol/clientCapabilities");
        if ((method_name == "sampling/createMessage" && !find(caps, "sampling").is_object()) ||
            (method_name == "elicitation/create" && !find(caps, "elicitation").is_object()) ||
            (method_name == "roots/list" && !find(caps, "roots").is_object()))
          throw Error(-32021, "MRTR client capability missing: " + method_name);
      }
    }
    result = complete(std::move(result), modern);
    if (modern) {
      json::object meta_out = find(result, "_meta").is_object() ? find(result, "_meta") : json::object{};
      meta_out["io.modelcontextprotocol/serverInfo"] = json::object{{"name", name_}, {"version", version_}};
      result["_meta"] = std::move(meta_out);
      if (method == "server/discover" || method == "tools/list" || method == "prompts/list" ||
          method == "resources/list" || method == "resources/templates/list" || method == "resources/read") {
        if (find(result, "resultType") == "complete") {
          result["ttlMs"] = ttl_ms_;
          result["cacheScope"] = cache_scope_;
        }
      }
    }
    return rpc_success(id, std::move(result));
  } catch (const Error& e) { return rpc_error(id, e.code, e.what()); }
  catch (const std::exception& e) { return rpc_error(id, -32603, e.what()); }
}

namespace {
struct ActiveRequest {
  Json token = nullptr;
  std::atomic_bool cancelled{false};
  std::mutex mutex;
  double last_progress = -std::numeric_limits<double>::infinity();
  std::function<void(const Json&)> emit;
};
thread_local ActiveRequest* current_request = nullptr;
struct RequestGuard {
  ActiveRequest* previous;
  explicit RequestGuard(ActiveRequest* request) : previous(current_request) {current_request = request;}
  ~RequestGuard() {current_request = previous;}
};
}
void Server::progress(double value, std::optional<double> total, std::string message) {
  auto* ctx = current_request;
  if (!ctx || ctx->token.is_null() || ctx->cancelled.load()) return;
  if (!std::isfinite(value) || (total && (!std::isfinite(*total) || *total < 0)))
    throw std::invalid_argument("Progress and total must be finite, non-negative values");
  Json event;
  {
    std::lock_guard lock(ctx->mutex);
    if (value <= ctx->last_progress) throw std::invalid_argument("Progress must increase");
    ctx->last_progress = value;
    Json params = json::object{{"progressToken", ctx->token}, {"progress", value}};
    if (total) params["total"] = *total;
    if (!message.empty()) params["message"] = std::move(message);
    event = json::object{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", std::move(params)}};
  }
  if (!ctx->cancelled.load()) ctx->emit(event);
}
bool Server::is_cancelled() const {return current_request && current_request->cancelled.load();}

void Server::run_stdio() {
  std::mutex stdout_mutex;
  std::mutex active_mutex;
  std::vector<std::shared_ptr<Subscription>> subscriptions;
  std::vector<std::thread> workers;
  std::map<std::string, std::shared_ptr<ActiveRequest>> active;
  auto output = [&](const Json& value) {
    std::lock_guard lock(stdout_mutex);
    std::cout << json::serialize(value) << '\n' << std::flush;
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    try {
      Json request = json::parse(line);
      const auto method = string_of(request, "method");
      if (method == "notifications/cancelled") {
        const Json& cancelled = find(find(request, "params"), "requestId");
        std::lock_guard lock(active_mutex);
        for (auto& sub : subscriptions) if (sub->id == cancelled) close_subscription(sub);
        auto it = active.find(json::serialize(cancelled));
        if (it != active.end()) it->second->cancelled = true;
        continue;
      }
      if (method == "subscriptions/listen") {
        Json checked = handle(request);
        const Json& error_code = find(find(checked, "error"), "code");
        if (!error_code.is_int64() || error_code.as_integer<std::int64_t>() != -32601) {
          if (!checked.is_null()) output(checked);
          continue;
        }
        std::shared_ptr<Subscription> subscription;
        try {subscription = subscribe(find(request, "id"), find(find(request, "params"), "notifications"));}
        catch (const Error& e) {output(rpc_error(find(request, "id"), e.code, e.what())); continue;}
        subscriptions.push_back(subscription);
        workers.emplace_back([subscription, &output] {
          for (;;) {
            Json item;
            {
              std::unique_lock lock(subscription->mutex);
              subscription->ready.wait(lock, [&] {
                return subscription->closed || !subscription->messages.empty();
              });
              if (subscription->closed) return;
              item = std::move(subscription->messages.front());
              subscription->messages.pop_front();
            }
            output(item);
          }
        });
        continue;
      }
      if (find(request, "id").is_null()) {
        const auto result = handle(request);
        if (!result.is_null()) output(result);
        continue;
      }
      auto context = std::make_shared<ActiveRequest>();
      context->token = find(find(find(request, "params"), "_meta"), "progressToken");
      // A malformed progress token is ignored, not echoed.
      if (!context->token.is_string() && !context->token.is_int64() &&
          !context->token.is_uint64()) context->token = nullptr;
      context->emit = output;
      std::string key = json::serialize(find(request, "id"));
      {
        std::lock_guard lock(active_mutex);
        if (active.count(key)) {output(rpc_error(find(request, "id"), -32600, "Duplicate active request ID")); continue;}
        active[key] = context;
      }
      workers.emplace_back([this, request = std::move(request), key, context, &output, &active, &active_mutex] {
        RequestGuard guard(context.get());
        Json response = handle(request);
        if (!context->cancelled.load() && !response.is_null()) output(response);
        std::lock_guard lock(active_mutex);
        active.erase(key);
      });
    } catch (const std::exception&) {
      output(rpc_error(nullptr, -32700, "Parse error"));
    }
  }
  {
    std::lock_guard lock(active_mutex);
    for (auto& [id, request] : active) request->cancelled = true;
  }
  for (auto& sub : subscriptions) close_subscription(sub);
  for (auto& worker : workers) worker.join();
}

namespace {
struct HttpReply {
  int status = 200;
  Json json_result = nullptr;
  std::string challenge;
};
std::string header(const httplib::Request& request, const std::string& name) {
  return request.has_header(name) ? request.get_header_value(name) : std::string{};
}
HttpReply on_http(Server& server, const httplib::Request& req,
                  const std::string& bind_host, unsigned short port,
                  bool validate_only = false) {
  Json id = nullptr;
  if (req.path != "/mcp")
    return {404, rpc_error(id, -32601, "MCP endpoint is POST /mcp"), {}};
  const auto origin = header(req, "Origin");
  if (!origin.empty() && origin != "http://" + bind_host + ":" + std::to_string(port) &&
      !(bind_host == "127.0.0.1" && origin == "http://localhost:" + std::to_string(port)))
    return {403, rpc_error(id, -32000, "Invalid Origin"), {}};
  const std::string accept = header(req, "Accept");
  if (accept.find("application/json") == std::string::npos ||
      accept.find("text/event-stream") == std::string::npos ||
      header(req, "Content-Type").find("application/json") == std::string::npos)
    return {400, rpc_error(id, -32600, "Invalid HTTP Content-Type or Accept"), {}};
  if (req.body.size() > 4 * 1024 * 1024)
    return {413, rpc_error(id, -32600, "Request body too large"), {}};
  Json body;
  try { body = json::parse(req.body); }
  catch (const std::exception&) { return {400, rpc_error(nullptr, -32700, "Parse error"), {}}; }
  id = find(body, "id");
  const Json& params = find(body, "params");
  const Json& meta = find(params, "_meta");
  std::string method = string_of(body, "method");
  if (server.http_requires_auth()) {
    const std::string authorization = header(req, "Authorization");
    const std::string token = authorization.starts_with("Bearer ") ? authorization.substr(7) : std::string{};
    const std::string target = string_of(params, method == "resources/read" ? "uri" :
                                       method.starts_with("tasks/") ? "taskId" : "name");
    if (!server.verify_http(token, method, target)) {
      const std::string resource = string_of(server.protected_resource_metadata(), "resource");
      const auto scheme_end = resource.find("://");
      const auto path_begin = scheme_end == std::string::npos ? std::string::npos :
                              resource.find('/', scheme_end + 3);
      const std::string base = resource.substr(0, path_begin);
      return {401, rpc_error(id, -32001, "Unauthorized"),
              "Bearer resource_metadata=\"" + base + "/.well-known/oauth-protected-resource\""};
    }
  }
  // Pre-2026 Codex/Claude clients use initialize and per-connection style
  // requests without the 2026 _meta contract. Older Streamable HTTP servers
  // may omit MCP-Session-Id entirely; we do so here to remain stateless.
  const std::string ver = string_of(meta, "io.modelcontextprotocol/protocolVersion");
  const std::string version_header = header(req, "MCP-Protocol-Version");
  const bool legacy = ver.empty();
  if (legacy) {
    if (!version_header.empty() && version_header != LegacyProtocolVersion &&
        version_header != "2025-06-18" && version_header != "2025-03-26")
      return {400, rpc_error(id, -32022, "Unsupported protocol version",
               json::object{{"requested", version_header},
                            {"supported", json::array{ProtocolVersion, LegacyProtocolVersion,
                                                       "2025-06-18", "2025-03-26"}}}), {}};
  } else {
    if (version_header != ver || header(req, "Mcp-Method") != method)
      return {400, rpc_error(id, -32020, "HeaderMismatch: version or method"), {}};
    if (ver != ProtocolVersion)
      return {400, rpc_error(id, -32022, "Unsupported protocol version",
               json::object{{"requested", ver}, {"supported", json::array{ProtocolVersion}}}), {}};
  }
  if (!legacy && (method == "tools/call" || method == "resources/read" || method == "prompts/get" ||
      method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel")) {
    const std::string expected = string_of(params, method == "resources/read" ? "uri" :
                            method.starts_with("tasks/") ? "taskId" : "name");
    const auto decoded = decode_header(header(req, "Mcp-Name"));
    if (!decoded || *decoded != expected)
      return {400, rpc_error(id, -32020, "HeaderMismatch: Mcp-Name"), {}};
  }
  if (!legacy && method == "tools/call") {
    const auto schema = server.tool_schema(string_of(params, "name"));
    if (schema) try {
      for (const auto& field : header_fields(*schema)) {
        const Json& val = path_value(find(params, "arguments"), field.path);
        const auto actual = header(req, field.header);
        if (val.is_null()) {
          if (!actual.empty()) throw Error(-32020, "HeaderMismatch: unexpected " + field.header);
        } else {
          const auto decoded = decode_header(actual);
          if (!decoded || *decoded != header_value(val)) throw Error(-32020, "HeaderMismatch: " + field.header);
        }
      }
    } catch (const Error& e) {return {400, rpc_error(id, -32020, e.what()), {}};}
  }
  // Streamable HTTP may return JSON for quick requests, or SSE for a
  // request carrying a progressToken. Validation must finish before headers.
  if (validate_only) return {200, nullptr, {}};
  Json response = server.handle(body, legacy);
  if (response.is_null()) return {202, nullptr, {}};
  if (find(response, "error").is_object() && find(find(response, "error"), "code").is_int64() &&
      find(find(response, "error"), "code").as_integer<std::int64_t>() == -32601)
    return {404, response, {}};
  return {200, response, {}};
}
void reply(httplib::Response& out, const HttpReply& result) {
  out.status = result.status;
  if (!result.challenge.empty()) out.set_header("WWW-Authenticate", result.challenge);
  if (!result.json_result.is_null()) out.set_content(json::serialize(result.json_result), "application/json");
}
} // namespace

void Server::run_http(unsigned short port, std::string host) {
  if (host != "127.0.0.1" && host != "localhost" && host != "::1")
    throw std::invalid_argument("Plain HTTP may only bind to loopback; use HTTPS reverse proxy for remote access");
  httplib::Server server;
  server.set_payload_max_length(4 * 1024 * 1024);
  server.Get("/.well-known/oauth-protected-resource", [this](const auto&, auto& response) {
    if (!http_requires_auth()) { response.status=404; return; }
    response.set_content(json::serialize(protected_resource_metadata()), "application/json");
  });
  // The old Streamable HTTP GET notification channel is deliberately not
  // opened (clients can use request-scoped SSE; 2026 uses subscriptions/listen).
  server.Get("/mcp", [](const auto&, auto& out) {
    out.status = 405;
    out.set_header("Allow", "POST");
  });
  server.Post("/mcp", [this, host, port](const httplib::Request& req, httplib::Response& out) {
    Json body = nullptr;
    try {body = json::parse(req.body);} catch (...) {}
    const Json& token = find(find(find(body, "params"), "_meta"), "progressToken");
    const bool stream_progress = string_of(body, "method") == "tools/call" &&
        (token.is_string() || token.is_int64() || token.is_uint64());
    auto checked = on_http(*this, req, host, port, stream_progress);
    if (stream_progress) {
      if (checked.status != 200) { reply(out, checked); return; }
      out.set_header("Cache-Control", "no-cache");
      out.set_header("X-Accel-Buffering", "no");
      const bool legacy = string_of(find(find(body, "params"), "_meta"),
                                    "io.modelcontextprotocol/protocolVersion").empty();
      out.set_chunked_content_provider("text/event-stream",
        [this, body, token, legacy](std::size_t, httplib::DataSink& sink) {
          ActiveRequest context;
          context.token = token;
          context.emit = [&sink, &context](const Json& message) {
            const auto data = "data: " + json::serialize(message) + "\n\n";
            if (!sink.write(data.data(), data.size())) context.cancelled = true;
          };
          {
            RequestGuard guard(&context);
            Json response = handle(body, legacy);
            if (!context.cancelled.load() && !response.is_null()) context.emit(response);
          }
          sink.done();
          return false;
        });
      return;
    }
    if (string_of(body, "method") != "subscriptions/listen" || checked.status != 404 ||
        string_of(find(checked.json_result, "error"), "message") != "Method not found: subscriptions/listen") {
      reply(out, checked); return;
    }
    std::shared_ptr<Subscription> subscription;
    try {subscription = subscribe(find(body, "id"), find(find(body, "params"), "notifications"));}
    catch (const Error& e) { reply(out, {400, rpc_error(find(body,"id"),e.code,e.what()),{}}); return; }
    out.set_header("Cache-Control", "no-cache");
    out.set_header("X-Accel-Buffering", "no");
    out.set_chunked_content_provider("text/event-stream",
      [this, subscription](std::size_t, httplib::DataSink& sink) {
        Json next = nullptr;
        {
          std::unique_lock lock(subscription->mutex);
          if (!subscription->ready.wait_for(lock,std::chrono::seconds(15),[&]{
            return subscription->closed || !subscription->messages.empty();
          })) {return sink.write(": keepalive\n\n", 13);}
          if (subscription->closed) {
            Json finished = rpc_success(subscription->id, json::object{{"resultType","complete"},
              {"_meta",json::object{{"io.modelcontextprotocol/subscriptionId",subscription->id}}}});
            const auto data = "data: " + json::serialize(finished) + "\n\n";
            sink.write(data.data(), data.size()); sink.done(); return false;
          }
          next = std::move(subscription->messages.front()); subscription->messages.pop_front();
        }
        auto data = "data: " + json::serialize(next) + "\n\n";
        if (!sink.write(data.data(), data.size())) {close_subscription(subscription); return false;}
        return true;
      }, [this, subscription](bool) { close_subscription(subscription); });
  });
  if (!server.listen(host, port)) throw std::runtime_error("Cannot listen on HTTP interface");
}

namespace {
// The stdio process transport has no external runtime dependencies.
struct ProcessState {
  std::function<void(const Json&)> on_notification;
  std::mutex mutex;
  FILE* child_stdout = nullptr;
  FILE* child_stdin = nullptr;
  std::string stdout_pending;
  bool dead = false;
#ifdef _WIN32
  PROCESS_INFORMATION process{};
  HANDLE process_job=nullptr; // Kill descendants as a group on deadline.
#else
  pid_t pid = -1;
#endif
  ProcessState(const std::string& executable, const std::vector<std::string>& args,
               std::function<void(const Json&)> callback) : on_notification(std::move(callback)) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd_out=nullptr, wr_out=nullptr, rd_in=nullptr, wr_in=nullptr;
    if (!CreatePipe(&rd_out,&wr_out,&sa,0) || !CreatePipe(&rd_in,&wr_in,&sa,0))
      throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(rd_out,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(wr_in,HANDLE_FLAG_INHERIT,0);
    auto quote=[](const std::string& arg) {
      std::string out="\"";
      std::size_t slashes=0;
      for (char ch:arg) {
        if(ch=='\\'){++slashes;continue;}
        if(ch=='\"'){out.append(slashes*2+1,'\\');out+='\"';slashes=0;continue;}
        out.append(slashes,'\\');slashes=0;out+=ch;
      }
      out.append(slashes*2,'\\');return out+'\"';
    };
    std::string cmd=quote(executable);
    for(const auto& a:args)cmd+=' '+quote(a);
    STARTUPINFOA si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=rd_in;si.hStdOutput=wr_out;si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    std::vector<char> command(cmd.begin(),cmd.end());command.push_back(0);
    BOOL ok=CreateProcessA(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&process);
    CloseHandle(rd_in);CloseHandle(wr_out);
    if(!ok){CloseHandle(rd_out);CloseHandle(wr_in);throw std::runtime_error("CreateProcess failed");}
    process_job=CreateJobObjectA(nullptr,nullptr);
    if(process_job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
      info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if(!SetInformationJobObject(process_job,JobObjectExtendedLimitInformation,&info,sizeof(info)) ||
         !AssignProcessToJobObject(process_job,process.hProcess)) {
        CloseHandle(process_job);process_job=nullptr;
      }
    }
    child_stdout=_fdopen(_open_osfhandle((intptr_t)rd_out,_O_RDONLY),"rb");
    child_stdin=_fdopen(_open_osfhandle((intptr_t)wr_in,_O_WRONLY),"wb");
#else
    int in_pipe[2],out_pipe[2];
    if(pipe(in_pipe)!=0) throw std::runtime_error("pipe failed");
    if(pipe(out_pipe)!=0){close(in_pipe[0]);close(in_pipe[1]);throw std::runtime_error("pipe failed");}
    pid=fork();
    if(pid<0){close(in_pipe[0]);close(in_pipe[1]);close(out_pipe[0]);close(out_pipe[1]);throw std::runtime_error("fork failed");}
    if(pid==0){
      (void)setpgid(0,0); // Own process group: terminate grandchildren too.
      dup2(in_pipe[0],STDIN_FILENO);dup2(out_pipe[1],STDOUT_FILENO);
      close(in_pipe[0]);close(in_pipe[1]);close(out_pipe[0]);close(out_pipe[1]);
      std::vector<char*> argv;argv.push_back(const_cast<char*>(executable.c_str()));
      for(const auto& a:args)argv.push_back(const_cast<char*>(a.c_str()));
      argv.push_back(nullptr);execvp(executable.c_str(),argv.data());_exit(127);
    }
    (void)setpgid(pid,pid); // Race-safe with child-side setpgid.
    close(in_pipe[0]);close(out_pipe[1]);
    child_stdin=fdopen(in_pipe[1],"w");child_stdout=fdopen(out_pipe[0],"r");
#endif
    if(!child_stdin||!child_stdout)throw std::runtime_error("stdio stream creation failed");
  }
  // Never wait forever for a child which ignores SIGTERM.
  void terminate_child() noexcept {
    dead=true;
#ifdef _WIN32
    if(process.hProcess) {
      if(process_job)TerminateJobObject(process_job,1);
      else TerminateProcess(process.hProcess,1);
      WaitForSingleObject(process.hProcess,2000);
    }
#else
    if(pid>0) {
      // Only signal a process group if this child leads its own group.
      // Otherwise avoid accidentally signalling the Agent's process group.
      if(getpgid(pid)==pid)kill(-pid,SIGKILL);
      else kill(pid,SIGKILL);
      for(int attempt=0;attempt<200;++attempt) {
        int status=0;
        const auto rc=waitpid(pid,&status,WNOHANG);
        if(rc==pid || (rc<0 && errno==ECHILD)) {pid=-1;break;}
        if(rc<0 && errno!=EINTR)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
#endif
  }
  ~ProcessState() {
    if(child_stdin)fclose(child_stdin);
#ifdef _WIN32
    if(process.hProcess) {
      if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT)terminate_child();
      CloseHandle(process.hProcess);
      CloseHandle(process.hThread);
    }
    if(process_job)CloseHandle(process_job);
#else
    if(pid>0) {
      if(getpgid(pid)==pid)kill(-pid,SIGTERM);
      else kill(pid,SIGTERM);
      bool exited=false;
      for(int attempt=0;attempt<25;++attempt) {
        int status=0;const auto rc=waitpid(pid,&status,WNOHANG);
        if(rc==pid || (rc<0 && errno==ECHILD)) {exited=true;pid=-1;break;}
        if(rc<0 && errno!=EINTR)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if(!exited)terminate_child();
    }
#endif
    if(child_stdout)fclose(child_stdout);
  }
  Json send(const Json& message, const RequestOptions& options = {}) {
    std::lock_guard lock(mutex);
    if(dead)throw std::runtime_error("MCP child is no longer available");
    const auto deadline=options.timeout.count()>0
        ? std::chrono::steady_clock::now()+options.timeout
        : std::chrono::steady_clock::time_point::max();
    const auto limit=options.max_response_bytes ? options.max_response_bytes
                                                 : std::size_t{8}*1024*1024;
    const auto check=[&]() {
      if(options.cancelled && options.cancelled()) {
        terminate_child();throw std::runtime_error("MCP stdio request cancelled");
      }
      if(std::chrono::steady_clock::now()>=deadline) {
        terminate_child();throw std::runtime_error("MCP stdio request deadline exceeded");
      }
    };
    auto line=json::serialize(message)+"\n";
    check();
#ifdef _WIN32
    // A synchronous Windows pipe write can block when the child stops reading.
    // Perform it on a dedicated I/O thread and cancel that exact thread's
    // pending synchronous I/O when the child is terminated on deadline.
    std::atomic_bool write_done{false};
    std::exception_ptr write_error;
    std::thread writer([&] {
      try {
        const auto fd=_fileno(child_stdin);
        HANDLE handle=reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        std::size_t offset=0;
        while(offset<line.size()) {
          DWORD written=0;
          const DWORD count=static_cast<DWORD>(std::min<std::size_t>(
              line.size()-offset,65536));
          if(!WriteFile(handle,line.data()+offset,count,&written,nullptr)
              || written==0)
            throw std::runtime_error("MCP child stdin closed");
          offset+=written;
        }
      } catch(...) {write_error=std::current_exception();}
      write_done.store(true,std::memory_order_release);
    });
    bool write_timed_out=false;
    while(!write_done.load(std::memory_order_acquire)) {
      try {check();}
      catch(...) {
        write_timed_out=true;
        CancelSynchronousIo(static_cast<HANDLE>(writer.native_handle()));
        // Killing the child closes its read end and releases pipe writers.
        terminate_child();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    writer.join();
    if(write_timed_out)
      throw std::runtime_error("MCP stdio write cancelled or timed out");
    if(write_error)std::rethrow_exception(write_error);
#else
    // O_NONBLOCK + poll handles children which never read from stdin.
    const int input_fd=fileno(child_stdin);
    const int old_flags=fcntl(input_fd,F_GETFL,0);
    if(old_flags<0 || fcntl(input_fd,F_SETFL,old_flags|O_NONBLOCK)<0)
      throw std::runtime_error("Cannot configure MCP stdin nonblocking I/O");
    std::size_t sent=0;
    while(sent<line.size()) {
      check();
      pollfd fd{input_fd,POLLOUT,0};
      const int ready=poll(&fd,1,10);
      if(ready<0 && errno==EINTR)continue;
      if(ready<0)throw std::runtime_error("MCP stdin poll failed");
      if(ready==0)continue;
      if(fd.revents&(POLLERR|POLLHUP|POLLNVAL))
        throw std::runtime_error("MCP child stdin closed");
      // A disconnected child must raise EPIPE, never terminate the Agent via
      // process-wide SIGPIPE. Block SIGPIPE on this I/O thread only, then
      // consume a newly generated pending signal before restoring its mask.
      sigset_t blocked,old_mask,pending_before;
      sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);
      if(pthread_sigmask(SIG_BLOCK,&blocked,&old_mask)!=0)
        throw std::runtime_error("Cannot mask MCP pipe signals");
      sigpending(&pending_before);
      const bool prior_sigpipe=sigismember(&pending_before,SIGPIPE)==1;
      const auto bytes=::write(input_fd,line.data()+sent,line.size()-sent);
      const int write_error=errno;
      if(bytes<0 && write_error==EPIPE && !prior_sigpipe) {
        timespec immediate{0,0};
        (void)sigtimedwait(&blocked,nullptr,&immediate);
      }
      pthread_sigmask(SIG_SETMASK,&old_mask,nullptr);
      if(bytes<0 && (write_error==EINTR||write_error==EAGAIN))continue;
      if(bytes<=0)throw std::runtime_error("MCP child stdin write failed");
      sent+=static_cast<std::size_t>(bytes);
    }
#endif
    if(find(message,"id").is_null())return nullptr;
    for(;;) {
      check();
      const auto newline=stdout_pending.find('\n');
      if(newline!=std::string::npos) {
        const std::string output=stdout_pending.substr(0,newline);
        stdout_pending.erase(0,newline+1);
        if(output.size()>limit) {
          terminate_child();throw std::runtime_error("MCP stdout message too large");
        }
        Json msg=json::parse(output);
        if(find(msg,"id")==find(message,"id"))return msg;
        if(find(msg,"method").is_string() && on_notification)on_notification(msg);
        continue;
      }
      char buffer[4096];std::size_t received=0;
#ifdef _WIN32
      HANDLE handle=reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(child_stdout)));
      DWORD available=0;
      if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr))
        throw std::runtime_error("MCP child stdout closed");
      if(!available) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      DWORD bytes=0;
      if(!ReadFile(handle,buffer,std::min<DWORD>(available,sizeof(buffer)),&bytes,nullptr)
          || !bytes)
        throw std::runtime_error("MCP child stdout read failed");
      received=bytes;
#else
      pollfd fd{fileno(child_stdout),POLLIN,0};
      const int ready=poll(&fd,1,10);
      if(ready<0 && errno==EINTR)continue;
      if(ready<0)throw std::runtime_error("MCP stdout poll failed");
      if(!ready)continue;
      const auto bytes=::read(fd.fd,buffer,sizeof(buffer));
      if(bytes<0 && (errno==EINTR||errno==EAGAIN))continue;
      if(bytes<=0)throw std::runtime_error("MCP child stdout closed");
      received=static_cast<std::size_t>(bytes);
#endif
      if(received>limit || stdout_pending.size()>limit-received) {
        terminate_child();throw std::runtime_error("MCP stdout message too large");
      }
      stdout_pending.append(buffer,received);
    }
  }
};
struct ParsedUrl {std::string host,port,target,scheme;};
ParsedUrl parse_url(const std::string& url) {
  auto at=url.find("://");
  if(at==std::string::npos || (url.substr(0,at)!="http" && url.substr(0,at)!="https"))
    throw std::invalid_argument("Invalid HTTP(S) URL");
  auto scheme=url.substr(0,at); auto from=at+3;
  auto end=url.find_first_of("/?#",from);
  auto authority=url.substr(from,end==std::string::npos?end:end-from);
  if(authority.empty()||authority.find('@')!=std::string::npos)throw std::invalid_argument("Invalid URL authority");
  std::string host,port=scheme=="https"?"443":"80";
  if(authority[0]=='['){auto close=authority.find(']');if(close==std::string::npos)throw std::invalid_argument("Invalid IPv6 URL");host=authority.substr(1,close-1);if(close+1<authority.size()){if(authority[close+1]!=':')throw std::invalid_argument("Invalid port");port=authority.substr(close+2);}}
  else {auto colon=authority.find(':');host=authority.substr(0,colon);if(colon!=std::string::npos)port=authority.substr(colon+1);}
  return {host,port,end==std::string::npos?"/mcp":url.substr(end),scheme};
}
std::unique_ptr<httplib::Client> new_http_client(const ParsedUrl& url) {
  auto c=std::make_unique<httplib::Client>(url.scheme+"://"+url.host+":"+url.port);
  c->set_connection_timeout(5,0);c->set_read_timeout(30,0);
  c->set_follow_location(false);
  // HTTPS verification is ON by default in cpp-httplib OpenSSL builds.
  c->enable_server_certificate_verification(true);
  return c;
}
struct HttpState {
  ParsedUrl url;
  // 工具目录分页过程中先写入暂存区；只有最后一页校验完毕才能发布。
  // x-mcp-header 来源于 tools/list 的 inputSchema，绝不由模型自行指定请求头。
  std::map<std::string, Json> tool_schemas;
  std::map<std::string, Json> staged_tool_schemas;
  std::size_t staged_schema_bytes = 0;
  bool staging_active = false;
  std::atomic_bool catalog_ready{false};
  std::mutex mutex;
  std::string token;
  std::shared_ptr<oauth::Session> oauth_session;
  std::function<void(const Json&)> on_notification;
  explicit HttpState(const std::string& path,std::string bearer):url(parse_url(path)),token(std::move(bearer)){}

  // 目录变更、刷新失败时不沿用旧的 header Schema，避免发送过期参数头。
  // 调用者必须已经持有 mutex；catalog_ready 是跨调用读取的状态标志。
  void invalidate_tool_catalog() {
    catalog_ready.store(false,std::memory_order_release);
    tool_schemas.clear();
    staged_tool_schemas.clear();
    staged_schema_bytes=0;
    staging_active=false;
  }

  // 将 tools/list 的所有分页结果合并为完整快照，防止只缓存第一页导致
  // 后续 tools/call 丢失 MCP-Param-* 请求头。限制额外缓存的条目数/字节数。
  void capture_tool_page(const Json& reply,const Json& params) {
    const Json& result=find(reply,"result");
    const Json& tools=find(result,"tools");
    if(!result.is_object() || !tools.is_array())return; // 上层 JSON-RPC 检查错误。
    const bool first=find(params,"cursor").is_null();
    if(first) {
      staged_tool_schemas.clear();
      staged_schema_bytes=0;
      staging_active=true;
    } else if(!staging_active) {
      // 未经过起始页的孤立分页不能成为完整可用的目录。
      return;
    }
    for(const auto& tool:tools.array_range()) {
      if(!tool.is_object() || !find(tool,"name").is_string() ||
         string_of(tool,"name").empty())
        throw Error(-32602,"Invalid MCP tool schema entry");
      const Json& schema=find(tool,"inputSchema");
      if(!schema.is_object())continue; // 不存在头路由注解时保持旧服务器兼容。
      const auto headers=header_fields(schema); // 提前校验非法/重复 Header 注解。
      if(headers.empty())continue;
      const std::string name=string_of(tool,"name");
      const std::string serialized=json::serialize(schema);
      constexpr std::size_t MaxHeaderCatalogBytes=4*1024*1024;
      if(serialized.size()>MaxHeaderCatalogBytes-staged_schema_bytes ||
         staged_tool_schemas.size()>=4096 ||
         !staged_tool_schemas.emplace(name,schema).second)
        throw Error(-32602,"MCP header schema catalog too large or duplicate tool");
      staged_schema_bytes+=serialized.size();
    }
    const Json& cursor=find(result,"nextCursor");
    if(!cursor.is_null() && (!cursor.is_string() || cursor.as_string().empty()))
      throw Error(-32602,"Invalid MCP tool catalog nextCursor");
    if(cursor.is_null()) {
      tool_schemas=std::move(staged_tool_schemas);
      staged_tool_schemas.clear();
      staged_schema_bytes=0;
      staging_active=false;
      catalog_ready.store(true,std::memory_order_release);
    }
  }

  Json send(const Json& msg, const RequestOptions& options = {}) {
    std::lock_guard lock(mutex);
    const auto deadline=options.timeout.count()>0
      ? std::chrono::steady_clock::now()+options.timeout
      : std::chrono::steady_clock::time_point::max();
    const auto expired=[&] {
      return (options.cancelled && options.cancelled()) ||
             std::chrono::steady_clock::now()>=deadline;
    };
    if(expired())throw std::runtime_error("MCP HTTP request cancelled or timed out");
    const auto method=string_of(msg,"method");
    const Json& params=find(msg,"params");
    if(method=="tools/list" && find(params,"cursor").is_null())
      invalidate_tool_catalog(); // 先置失效，失败后不能继续用旧 Schema。
    httplib::Headers headers{{"Accept","application/json, text/event-stream"}};
    const auto version = string_of(find(params,"_meta"), "io.modelcontextprotocol/protocolVersion");
    const bool legacy = version.empty();
    if (!legacy) {
      headers.emplace("MCP-Protocol-Version", version);
      headers.emplace("Mcp-Method", method);
    } else if (method != "initialize") {
      headers.emplace("MCP-Protocol-Version", LegacyProtocolVersion);
    }
    const std::string bearer = oauth_session ? oauth_session->bearer_token() : token;
    if(!bearer.empty())headers.emplace("Authorization","Bearer "+bearer);
    if(!legacy && (method=="tools/call"||method=="prompts/get"||method=="resources/read"||method.starts_with("tasks/")))
      headers.emplace("Mcp-Name",encode_header(string_of(params,method=="resources/read"?"uri":method.starts_with("tasks/")?"taskId":"name")));
    if(!legacy && method=="tools/call"){
      auto it=tool_schemas.find(string_of(params,"name"));
      if(it!=tool_schemas.end())for(const auto& field:header_fields(it->second)){
        const Json& value=path_value(find(params,"arguments"),field.path);
        if(!value.is_null())headers.emplace(field.header,encode_header(header_value(value)));
      }
    }
    auto c=new_http_client(url);
    if(options.timeout.count()>0) {
      const auto bound=std::max(std::chrono::milliseconds(1),options.timeout);
      const auto connection=std::min(bound,std::chrono::milliseconds(5000));
      const auto to_times=[](std::chrono::milliseconds time) {
        return std::pair<time_t,time_t>{static_cast<time_t>(time.count()/1000),
                                        static_cast<time_t>((time.count()%1000)*1000)};
      };
      const auto [cs,cu]=to_times(connection);
      const auto [rs,ru]=to_times(std::min(bound,std::chrono::milliseconds(30000)));
      c->set_connection_timeout(cs,cu);
      c->set_read_timeout(rs,ru);
      c->set_write_timeout(rs,ru);
      // cpp-httplib's max_timeout is a *whole-request* deadline, unlike
      // read_timeout which is refreshed by every received chunk.
      c->set_max_timeout(bound);
    }
    // Stop a transport blocked inside the socket when the caller cancels
    // before another read callback occurs. Client::stop() is thread-safe.
    std::jthread cancel_watcher;
    if(options.cancelled) {
      const auto cancelled=options.cancelled;
      // Capture the callback BY VALUE: the watcher outlives this if-block.
      cancel_watcher=std::jthread([&,cancelled](std::stop_token stop) {
        while(!stop.stop_requested()) {
          if(cancelled()) {c->stop();return;}
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
    }
    // Read SSE progressively rather than buffering the response body until
    // completion. A progress notification can now reach the app immediately.
    std::string pending;
    std::string decode_error;
    std::size_t received_total=0;
    // Limit individual frames and the overall response when a caller
    // supplies a tighter transport budget.
    const auto frame_limit=options.max_response_bytes
        ? std::min(options.max_response_bytes,std::size_t{4}*1024*1024)
        : std::size_t{4}*1024*1024;
    Json streamed_result = nullptr;
    auto response=c->Post(url.target, headers, json::serialize(msg), "application/json",
      [&](const char* data, std::size_t length) -> bool {
        if(expired()) {
          decode_error="MCP HTTP request cancelled or timed out";
          return false;
        }
        if(length>frame_limit || pending.size()>frame_limit-length ||
           (options.max_response_bytes &&
            (received_total>options.max_response_bytes ||
             length>options.max_response_bytes-received_total))) {
          decode_error="HTTP response/SSE frame too large";
          return false;
        }
        received_total+=length;
        pending.append(data,length);
        for (;;) {
          auto end = pending.find("\n\n");
          if (end == std::string::npos) break;
          std::string frame = pending.substr(0, end);
          pending.erase(0, end + 2);
          std::istringstream lines(frame);
          std::string line, event_data;
          while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("data:")) {
              if (!event_data.empty()) event_data += '\n';
              auto data = line.substr(5);
              if (!data.empty() && data.front() == ' ') data.erase(0,1);
              event_data += data;
            }
          }
          if (event_data.empty()) continue;
          try {
            Json event = json::parse(event_data);
            if (find(event,"id") == find(msg,"id") && event.contains("id"))
              streamed_result = std::move(event);
            else if (find(event,"method").is_string()) {
              if(string_of(event,"method")=="notifications/tools/list_changed")
                invalidate_tool_catalog();
              if(on_notification)on_notification(event);
            }
          } catch (const std::exception& e) {
            decode_error = std::string("Malformed SSE response: ") + e.what();
            return false;
          }
        }
        return true;
      });
    if (!decode_error.empty()) throw std::runtime_error(decode_error);
    if(expired())throw std::runtime_error("MCP HTTP request cancelled or timed out");
    if(!response)throw std::runtime_error("HTTP transport error: "+httplib::to_string(response.error()));
    if(response->status==202)return nullptr;
    Json decoded;
    if(response->get_header_value("Content-Type").starts_with("text/event-stream")){
      if(streamed_result.is_null())throw std::runtime_error("SSE stream closed without matching result");
      decoded=std::move(streamed_result);
    } else {
      if(pending.empty())throw std::runtime_error("Empty HTTP response");
      decoded=json::parse(pending);
    }
    if(method=="tools/list")capture_tool_page(decoded,params);
    return decoded;
  }
};
} // namespace

void Client::listen_http(const std::string& url, Json notifications,
                         std::function<bool(const Json&)> on_event) {
  const auto target=parse_url(url);
  const Json request=json::object{{"jsonrpc","2.0"},{"id",1},{"method","subscriptions/listen"},
    {"params",json::object{{"notifications",std::move(notifications)},
      {"_meta",json::object{{"io.modelcontextprotocol/protocolVersion",ProtocolVersion},
         {"io.modelcontextprotocol/clientCapabilities",json::object{}},
         {"io.modelcontextprotocol/clientInfo",json::object{{"name","mymcp-listener"},{"version","1.0"}}}}}}}};
  auto c=new_http_client(target);
  c->set_read_timeout(3600,0);
  httplib::Headers headers{{"Accept","application/json, text/event-stream"},
    {"MCP-Protocol-Version",ProtocolVersion},{"Mcp-Method","subscriptions/listen"}};
  std::string pending;
  bool continue_reading=true;
  auto res=c->Post(target.target,headers,json::serialize(request),"application/json",
    [&](const char* data,std::size_t len){
      pending.append(data,len);
      for(;;){auto sep=pending.find("\n\n");if(sep==std::string::npos)break;
        auto frame=pending.substr(0,sep);pending.erase(0,sep+2);
        std::string event;std::istringstream lines(frame);std::string line;
        while(std::getline(lines,line)){
          if(!line.empty()&&line.back()=='\r')line.pop_back();
          if(line.starts_with("data:")){if(!event.empty())event+='\n';auto part=line.substr(5);if(!part.empty()&&part[0]==' ')part.erase(0,1);event+=part;}
        }
        if(!event.empty()&&!on_event(json::parse(event))){continue_reading=false;return false;}
      }
      if(pending.size()>4*1024*1024)throw std::runtime_error("SSE frame too large");
      return true;
    });
  if(!res&&continue_reading)throw std::runtime_error("HTTP SSE connection failed");
  if(res&&res->status!=200)throw std::runtime_error("HTTP SSE subscription rejected");
}

Client::Client(std::function<Json(const Json&)> send, std::string name)
    : send_(std::move(send)), name_(std::move(name)) {}
Client Client::process(const std::string& executable, std::vector<std::string> args,
                       std::function<void(const Json&)> on_notification) {
  const bool callback_present=static_cast<bool>(on_notification);
  auto state = std::make_shared<ProcessState>(executable, args, std::move(on_notification));
  Client client([state](const Json& message){ return state->send(message); });
  client.send_bounded_=[state](const Json& message,const RequestOptions& options) {
    return state->send(message,options);
  };
  client.has_untrusted_callback_=callback_present;
  return client;
}
Client Client::http(const std::string& url, std::string bearer_token,
                    std::function<void(const Json&)> on_notification) {
  const bool callback_present=static_cast<bool>(on_notification);
  auto state = std::make_shared<HttpState>(url, std::move(bearer_token));
  state->on_notification = std::move(on_notification);
  Client client([state](const Json& message){ return state->send(message); });
  client.send_bounded_=[state](const Json& message,const RequestOptions& options) {
    return state->send(message,options);
  };
  client.http_ = true;
  client.http_catalog_ready_=[state] {
    return state->catalog_ready.load(std::memory_order_acquire);
  };
  client.has_untrusted_callback_=callback_present;
  return client;
}
Client Client::http_oauth(const std::string& url, std::shared_ptr<oauth::Session> session,
                          std::function<void(const Json&)> on_notification) {
  const bool callback_present=static_cast<bool>(on_notification);
  if (!session) throw std::invalid_argument("OAuth session is null");
  // Token 属于一个特定的 MCP Resource，不能因调用者传错 URL 就泄漏到
  // 同域其他路径、其他端口或其他主机。严格比较构造时验证的原始资源 URI。
  if(url != session->resource_url())
    throw std::invalid_argument("OAuth MCP URL does not match session resource");
  const auto endpoint=parse_url(url);
  const bool local=endpoint.host=="127.0.0.1" ||
                   endpoint.host=="localhost" || endpoint.host=="::1";
  if(endpoint.scheme!="https" && !(endpoint.scheme=="http" && local))
    throw std::invalid_argument("OAuth MCP target requires HTTPS outside loopback");
  if(endpoint.target.find('#')!=std::string::npos)
    throw std::invalid_argument("OAuth MCP target must not contain URL fragment");
  auto state = std::make_shared<HttpState>(url, std::string{});
  state->oauth_session = std::move(session);
  state->on_notification = std::move(on_notification);
  Client client([state](const Json& message){ return state->send(message); });
  client.send_bounded_=[state](const Json& message,const RequestOptions& options) {
    return state->send(message,options);
  };
  client.http_ = true;
  client.http_catalog_ready_=[state] {
    return state->catalog_ready.load(std::memory_order_acquire);
  };
  client.requires_oauth_refresh_ = true; // refresh() may block outside HTTP request budget
  client.has_untrusted_callback_=callback_present;
  return client;
}
Json Client::request(const std::string& method, Json params,
                     const RequestOptions& options) {
  if (!params.is_object()) throw std::invalid_argument("Request params must be an object");
  const auto deadline=options.timeout.count()>0
      ? std::chrono::steady_clock::now()+options.timeout
      : std::chrono::steady_clock::time_point::max();
  const auto budget=[&]() {
    if(options.cancelled && options.cancelled())
      throw std::runtime_error("MCP request cancelled");
    RequestOptions next=options;
    if(deadline!=std::chrono::steady_clock::time_point::max()) {
      if(std::chrono::steady_clock::now()>=deadline)
        throw std::runtime_error("MCP request deadline exceeded");
      next.timeout=std::max(std::chrono::milliseconds{1},
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline-std::chrono::steady_clock::now()));
    }
    return next;
  };
  if (!connected_ && method != "server/discover" && method != "initialize")
    discover(budget());
  for (int round = 0; round < 9; ++round) {
    Json actual_params = params;
    if (!legacy_) {
      json::object metadata = find(actual_params, "_meta").is_object() ?
                              find(actual_params, "_meta") : json::object{};
      metadata["io.modelcontextprotocol/protocolVersion"] = ProtocolVersion;
      metadata["io.modelcontextprotocol/clientCapabilities"] = capabilities_;
      metadata["io.modelcontextprotocol/clientInfo"] = json::object{{"name", name_}, {"version", "1.0.0"}};
      actual_params["_meta"] = std::move(metadata);
    }
    Json message = json::object{{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}, {"params", std::move(actual_params)}};
    const auto request_options=budget();
    Json response=send_bounded_ ? send_bounded_(message,request_options)
                                : send_(message);
    // A custom in-process sender cannot be forcibly interrupted. Still
    // report expiration if it returned after the requested deadline.
    (void)budget();
    if (!response.is_object()) throw std::runtime_error("Invalid JSON-RPC response");
    if (find(response, "id") != find(message, "id")) throw std::runtime_error("JSON-RPC response ID mismatch");
    const Json& e = find(response, "error");
    if (e.is_object()) throw Error(static_cast<int>(find(e, "code").as_integer<std::int64_t>()), string_of(e, "message", "MCP error"));
    const Json& result = find(response, "result");
    if (!result.is_object()) throw std::runtime_error("Invalid MCP result");
    if (method == "tools/list") tools_known_ = true;
    if (string_of(result, "resultType") != "input_required") return result;
    if (!input_handler_ && !sampling_handler_ && !elicitation_handler_ && !roots_handler_)
      return result;  // 允许没有交互处理器的宿主自行接管。
    if (round == 8) throw Error(-32603, "Input-required rounds exceeded");
    const Json& input_requests = find(result, "inputRequests");
    validate_mrtr_requests(input_requests);
    Json answers = json::object{};
    if (input_handler_) {
      answers = input_handler_(input_requests); // 保留兼容性：一次性处理所有请求。
    } else {
      for (const auto& item : input_requests.object_range()) {
        const auto method_name = string_of(item.value(), "method");
        const auto& handler = method_name == "sampling/createMessage" ? sampling_handler_ :
                              method_name == "elicitation/create" ? elicitation_handler_ : roots_handler_;
        if (!handler) throw Error(-32021, "MRTR handler not configured: " + method_name);
        answers[std::string(item.key())] = handler(find(item.value(), "params"));
      }
    }
    validate_mrtr_answers(input_requests, answers);
    params["inputResponses"] = std::move(answers);
    const Json& state = find(result, "requestState");
    if (state.is_string() && state.as_string().size() <= 16384) params["requestState"] = state;
    else params.erase("requestState");
  }
  throw Error(-32603, "Input-required rounds exceeded");
}
Json Client::request_with_progress(const std::string& method, Json params, Json token) {
  if (!params.is_object() || !(token.is_string() || token.is_int64() || token.is_uint64()))
    throw std::invalid_argument("Invalid progress parameters/token");
  Json metadata = find(params, "_meta").is_object() ? find(params,"_meta") : Json(json::object{});
  metadata["progressToken"] = std::move(token);
  params["_meta"] = std::move(metadata);
  return request(method, std::move(params));
}
Json Client::discover(const RequestOptions& options) {
  if (connected_) return discovery_;
  const auto deadline=options.timeout.count()>0
      ? std::chrono::steady_clock::now()+options.timeout
      : std::chrono::steady_clock::time_point::max();
  const auto remaining=[&] {
    if(options.cancelled && options.cancelled())
      throw std::runtime_error("MCP discovery cancelled");
    auto request_options=options;
    if(deadline!=std::chrono::steady_clock::time_point::max()) {
      if(std::chrono::steady_clock::now()>=deadline)
        throw std::runtime_error("MCP discovery deadline exceeded");
      request_options.timeout=std::max(std::chrono::milliseconds{1},
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline-std::chrono::steady_clock::now()));
    }
    return request_options;
  };
  try {
    Json info = request("server/discover",json::object{},remaining());
    const auto& versions = find(info, "supportedVersions");
    if (!versions.is_array()) throw Error(-32600, "Malformed discovery response");
    for (const auto& version : versions.array_range()) {
      if (version.is_string() && version.as_string() == ProtocolVersion) {
        connected_ = true;
        discovery_ = info;
        return info;
      }
    }
    if (!allow_legacy_) throw Error(-32022, "Server does not support 2026-07-28");
  } catch (const Error& e) {
    if (!allow_legacy_ || (e.code != -32601 && e.code != -32600 && e.code != -32022)) throw;
  }
  // stdio fallback for pre-2026 servers. Never negotiate modern versions in initialize.
  legacy_ = true;
  Json init = request("initialize", json::object{{"protocolVersion", LegacyProtocolVersion},
                                     {"clientInfo", json::object{{"name", name_}, {"version", "1.0.0"}}},
                                     {"capabilities", json::object{}}},remaining());
  connected_ = true;
  discovery_ = init;
  // The legacy notifications/initialized message has no response. Send it directly.
  Json notification = json::object{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
  if(send_bounded_) (void)send_bounded_(notification,remaining());
  else (void)send_(notification);
  return init;
}
Json Client::tools() { return request("tools/list"); }
Json Client::call(const std::string& name, Json arguments,
                  const RequestOptions& options) {
  // 只有完整缓存（含所有分页）才可构造 x-mcp-header 参数请求头。
  // 收到 list_changed、或上次刷新失败时必须重新获取工具目录。
  if (http_ && (!tools_known_ || (http_catalog_ready_ && !http_catalog_ready_())))
    list_all("tools/list", "tools",options);
  return request("tools/call",
    json::object{{"name", name}, {"arguments", std::move(arguments)}},options);
}
Json Client::resources() { return request("resources/list"); }
Json Client::read(const std::string& uri) { return request("resources/read", json::object{{"uri", uri}}); }
Json Client::prompts() { return request("prompts/list"); }
Json Client::resource_templates() { return request("resources/templates/list"); }
void Client::enable_tasks() { extension("io.modelcontextprotocol/tasks"); }
Json Client::get_task(const std::string& id) { return request("tasks/get", json::object{{"taskId", id}}); }
Json Client::update_task(const std::string& id, Json responses) {
  return request("tasks/update", json::object{{"taskId", id}, {"inputResponses", std::move(responses)}});
}
Json Client::cancel_task(const std::string& id) { return request("tasks/cancel", json::object{{"taskId", id}}); }
Json Client::complete(Json ref, std::string name, std::string value, Json context) {
  return request("completion/complete", json::object{{"ref", std::move(ref)},
         {"argument", json::object{{"name", name}, {"value", value}}}, {"context", std::move(context)}});
}
void Client::extension(std::string name, Json settings) {
  if (connected_) throw std::logic_error("Extensions must be configured before discovery");
  if (name.find('/') == std::string::npos || !settings.is_object())
    throw std::invalid_argument("Invalid extension identifier/settings");
  if (!capabilities_.contains("extensions")) capabilities_["extensions"] = json::object{};
  capabilities_.at("extensions")[name] = std::move(settings);
}
Json Client::list_all(const std::string& method, const std::string& array_field,
                      const RequestOptions& options) {
  const auto deadline=options.timeout.count()>0
      ? std::chrono::steady_clock::now()+options.timeout
      : std::chrono::steady_clock::time_point::max();
  if (method != "tools/list" && method != "resources/list" && method != "prompts/list" &&
      method != "resources/templates/list") throw std::invalid_argument("Not a list operation");
  Json combined = json::array{};
  std::set<std::string> visited;
  Json params = json::object{};
  for (size_t i = 0; i < 10000; ++i) {
    auto per_page=options;
    if(options.cancelled && options.cancelled())
      throw std::runtime_error("MCP catalog discovery cancelled");
    if(deadline!=std::chrono::steady_clock::time_point::max()) {
      if(std::chrono::steady_clock::now()>=deadline)
        throw std::runtime_error("MCP catalog deadline exceeded");
      per_page.timeout=std::max(std::chrono::milliseconds{1},
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline-std::chrono::steady_clock::now()));
    }
    auto response=request(method,params,per_page);
    if (!find(response, array_field).is_array()) throw Error(-32603, "Invalid list response");
    for (const auto& item : find(response, array_field).array_range()) combined.push_back(item);
    const auto& next = find(response, "nextCursor");
    if (next.is_null()) return combined;
    if (!next.is_string()) throw Error(-32603, "Invalid nextCursor");
    const std::string cursor(next.as_string());
    if (!visited.insert(cursor).second) throw Error(-32603, "Pagination loop detected");
    params = json::object{{"cursor", cursor}};
  }
  throw Error(-32603, "Too many list pages");
}
Json Client::get_prompt(const std::string& name, Json arguments) {
  return request("prompts/get", json::object{{"name", name}, {"arguments", std::move(arguments)}});
}
} // namespace mymcp