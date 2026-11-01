#include "myagent/agent.hpp"
#include "httplib.h"
#include <algorithm>
#include <stdexcept>
namespace myagent {
namespace j=mymcp::json;
namespace {
std::string field(const Json& v,const char* name) {
  const auto* p=j::if_contains(v,name);
  return p&&p->is_string()?std::string(p->as_string()):std::string{};
}
std::string content_text(const Json& content) {
  if(content.is_string())return std::string(content.as_string());
  std::string result;
  if(content.is_array())for(const auto& p:content.array_range()) {
    auto v=field(p,"text");
    if(!v.empty())result+=v;
  }
  return result;
}
Json chat_content(const Json& content) {
  if(!content.is_array())return content;
  Json parts=j::array{};
  for(const auto& p:content.array_range()) {
    const auto type=field(p,"type");
    if(type=="input_text"||type=="output_text") {
      parts.push_back(j::object{{"type","text"},{"text",field(p,"text")}});
    } else if(type=="input_image") {
      parts.push_back(j::object{{"type","image_url"},
        {"image_url",j::object{{"url",field(p,"image_url")}}}});
    } else throw std::runtime_error("Unsupported Chat Completions content part: "+type);
  }
  return parts;
}
Json chat_messages(const Json& response_input,const std::string& instructions) {
  Json result=j::array{};
  if(!instructions.empty())result.push_back(j::object{{"role","system"},{"content",instructions}});
  for(const auto& item:response_input.array_range()) {
    const auto type=field(item,"type");
    if(type=="function_call") {
      const Json call=j::object{{"id",field(item,"call_id")},{"type","function"},
        {"function",j::object{{"name",field(item,"name")},
                              {"arguments",field(item,"arguments")}}}};
      // Responses 的多个 function_call 属于同一模型回复。Chat Completions
      // 需要一条 assistant 消息携带全部 tool_calls，而不是多条相邻 assistant。
      if(!result.empty() && field(result[result.size()-1],"role")=="assistant" &&
         j::if_contains(result[result.size()-1],"tool_calls")) {
        result[result.size()-1]["tool_calls"].push_back(call);
      } else {
        result.push_back(j::object{{"role","assistant"},{"content",nullptr},
          {"tool_calls",j::array{call}}});
      }
    } else if(type=="function_call_output") {
      result.push_back(j::object{{"role","tool"},
        {"tool_call_id",field(item,"call_id")},{"content",field(item,"output")}});
    } else if(type=="message"||type.empty()) {
      const auto role=field(item,"role");
      const auto* content=j::if_contains(item,"content");
      if(role.empty()||!content)throw std::runtime_error("Invalid Chat input role/content");
      result.push_back(j::object{{"role",role},{"content",chat_content(*content)}});
    } else throw std::runtime_error("Unsupported input type for Chat Completions: "+type);
  }
  return result;
}
Json convert_usage(const Json& raw) {
  if(!raw.is_object())return nullptr;
  const auto* input=j::if_contains(raw,"prompt_tokens");
  const auto* output=j::if_contains(raw,"completion_tokens");
  if(!input||!output)return nullptr;
  Json usage=j::object{{"input_tokens",*input},{"output_tokens",*output}};
  if(const auto* total=j::if_contains(raw,"total_tokens"))usage["total_tokens"]=*total;
  if(const auto* detail=j::if_contains(raw,"prompt_tokens_details")) {
    if(const auto* cache=j::if_contains(*detail,"cached_tokens"))
      usage["input_tokens_details"]=j::object{{"cached_tokens",*cache}};
  }
  if(const auto* detail=j::if_contains(raw,"completion_tokens_details")) {
    if(const auto* reason=j::if_contains(*detail,"reasoning_tokens"))
      usage["output_tokens_details"]=j::object{{"reasoning_tokens",*reason}};
  }
  return usage;
}
} // namespace

ChatCompletionsModel::ChatCompletionsModel(Config config):config_(std::move(config)) {
  if(config_.api_key.empty()||config_.base_url.empty())
    throw std::invalid_argument("ChatCompletionsModel requires base_url and api_key");
  if(!config_.http_timeout_seconds)
    throw std::invalid_argument("ChatCompletionsModel HTTP timeout must be positive");
}
Json ChatCompletionsModel::create(const Json& request) {
  // Chat Completions 适配器适用于提供 OpenAI 兼容 /chat/completions 的服务；
  // Anthropic/Gemini 原生协议及音频流式调用需用户注入其他 Model 实现。
  const auto& url=config_.base_url;
  const auto scheme=url.rfind("https://",0)==0?8u:
     (url.rfind("http://",0)==0?7u:0u);
  if(!scheme||url.find('@',scheme)!=std::string::npos||url.find('#')!=std::string::npos)
    throw std::invalid_argument("Invalid Chat base_url");
  const auto boundary=url.find_first_of("/?",scheme);
  const auto origin=url.substr(0,boundary);
  if(origin.size()==scheme)
    throw std::invalid_argument("Chat base_url requires a host");
  const auto query_at=url.find('?',scheme);
  const auto path_end=query_at==std::string::npos?url.size():query_at;
  std::string path=(boundary!=std::string::npos && url[boundary]=='/')?
      url.substr(boundary,path_end-boundary):"/";
  const std::string query=query_at==std::string::npos?"":url.substr(query_at);
  if(config_.append_responses_path) {
    while(path.size()>1 && path.back()=='/')path.pop_back();
    if(!path.ends_with("/chat/completions")) {
      // An existing /v1 API prefix must not become /v1/v1.
      path+=(path.ends_with("/v1")?"":"/v1");
      path+="/chat/completions";
    }
  }
  const std::string target=path+query;
  const auto* input=j::if_contains(request,"input");
  if(!input||!input->is_array())throw std::invalid_argument("Responses input required");
  Json payload=j::object{{"model",request.at("model")},
    {"messages",chat_messages(*input,field(request,"instructions"))}};
  if(const auto* tools=j::if_contains(request,"tools"); tools && tools->is_array() && !tools->empty()) {
    Json definitions=j::array{};
    for(const auto& item:tools->array_range())
      definitions.push_back(j::object{{"type","function"},
        {"function",j::object{{"name",item.at("name")},
          {"description",item.at("description")},{"parameters",item.at("parameters")}}}});
    payload["tools"]=std::move(definitions);
  }
  // Forward the backend's desired tool concurrency instead of allowing the
  // Chat provider to silently return multiple tool calls when disabled.
  if(const auto* parallel=j::if_contains(request,"parallel_tool_calls");
     parallel && parallel->is_bool())
    payload["parallel_tool_calls"]=*parallel;
  httplib::Client client(origin);
  const auto limit=config_.http_timeout_seconds;
  client.set_connection_timeout(std::min(limit,10u));
  client.set_read_timeout(limit);client.set_write_timeout(limit);
  client.set_follow_location(false);
  httplib::Headers headers={{"Authorization","Bearer "+config_.api_key}};
  auto res=client.Post(target,headers,j::serialize(payload),"application/json");
  if(!res)throw std::runtime_error("Chat HTTP transport failed: "+httplib::to_string(res.error()));
  if(res->status<200||res->status>=300)
    throw std::runtime_error("Chat HTTP "+std::to_string(res->status)+": "+res->body.substr(0,500));
  const auto json=j::parse(res->body);
  const auto* choices=j::if_contains(json,"choices");
  if(!choices||!choices->is_array()||choices->empty())
    throw std::runtime_error("Chat Completions missing choices");
  const auto& choice=(*choices)[0];
  const auto& message=choice.at("message");
  Json outputs=j::array{};
  if(const auto* body=j::if_contains(message,"content");body && body->is_string() &&
       !body->as_string().empty())
    outputs.push_back(j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",*body}}}}});
  if(const auto* calls=j::if_contains(message,"tool_calls");calls&&calls->is_array())
    for(const auto& call:calls->array_range()) {
      const auto& function=call.at("function");
      outputs.push_back(j::object{{"type","function_call"},
        {"call_id",call.at("id")},{"name",function.at("name")},
        {"arguments",function.at("arguments")}});
    }
  const auto finish=field(choice,"finish_reason");
  // Only a normal stop or a tool-call handoff represents a completed turn.
  // content_filter, length and nonstandard/unknown reasons are not success.
  const bool complete=(finish=="stop" || finish=="tool_calls");
  Json converted=j::object{{"status",complete?"completed":"incomplete"},
      {"output",std::move(outputs)}};
  if(const auto* usage=j::if_contains(json,"usage")) {
    const auto normalized=convert_usage(*usage);
    if(!normalized.is_null())converted["usage"]=normalized;
  }
  return converted;
}
} // namespace myagent
