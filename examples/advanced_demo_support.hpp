#pragma once
// 高级 Agent 六组 Demo 的共享离线模拟器（不开启网络）。
#include "myagent/agent.hpp"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
namespace advanced_demo {
using myagent::Json;
namespace j=myagent::json;
inline void ensure(bool condition,const std::string& message) {
  if(!condition)throw std::runtime_error("Advanced demo: "+message);
}
inline Json answer(const std::string& text,Json usage=nullptr) {
  Json response=j::object{{"status","completed"},{"output",j::array{
    j::object{{"type","message"},{"role","assistant"},
              {"content",j::array{j::object{{"type","output_text"},{"text",text}}}}}
  }}};
  if(!usage.is_null())response["usage"]=usage;
  return response;
}
inline Json tool_call(const std::string& name,const std::string& id,
                      const std::string& arguments="{}") {
  return j::object{{"status","completed"},{"output",j::array{
    j::object{{"type","function_call"},{"name",name},
              {"call_id",id},{"arguments",arguments}}
  }}};
}
struct Mock final:myagent::Model {
  unsigned calls=0;
  std::function<Json(const Json&)> handler;
  Json create(const Json& request)override {
    ++calls;
    return handler(request);
  }
};
inline myagent::Config config() {
  myagent::Config c;
  c.auto_discover_skills=false;
  c.auto_skill_selection=false;
  c.model="offline-model";
  return c;
}
inline bool has_tool_result(const Json& request) {
  for(const auto& item:request.at("input").array_range()) {
    if(!item.is_object())continue;
    const auto* type=j::if_contains(item,"type");
    if(type && type->is_string() &&
       type->as_string()=="function_call_output")return true;
  }
  return false;
}
} // namespace advanced_demo
