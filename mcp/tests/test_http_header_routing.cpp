// x-mcp-header 回归：真实 loopback HTTP MCP 客户端与 Mock Server 互通。
//
// 流程：
// 1. tools/list 的工具 Schema 分两页返回，第二页也含 x-mcp-header；
// 2. tools/call 自动带上 MCP-Param-* 请求头（字符串、布尔、整数、嵌套字段）；
// 3. 重新读取工具目录后替换旧 Schema，不遗留旧参数头；
// 4. SSE list_changed 事件使目录失效，下一次调用必须重新获取全部分页；
// 5. 重复的大小写不敏感头名必须在工具执行之前被拒绝。
#include "mymcp/mcp.hpp"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mymcp;
using namespace std::chrono_literals;
namespace {
void check(bool ok,const std::string& message) {
  if(!ok)throw std::runtime_error(message);
}
struct RunningServer {
  httplib::Server server;
  std::thread worker;
  ~RunningServer() {
    server.stop();
    if(worker.joinable())worker.join();
  }
  int listen() {
    const int port=server.bind_to_any_port("127.0.0.1");
    check(port>0,"HTTP loopback bind failed");
    worker=std::thread([this]{server.listen_after_bind();});
    for(int i=0;i<300 && !server.is_running();++i)
      std::this_thread::sleep_for(5ms);
    check(server.is_running(),"Mock HTTP server failed to start");
    return port;
  }
};
Json make_reply(const Json& request,Json result) {
  return json::object{{"jsonrpc","2.0"},{"id",request.at("id")},
                      {"result",std::move(result)}};
}
Json primary_tool(int generation) {
  auto schema=json::parse(R"({"name":"primary","inputSchema":{"type":"object",
    "properties":{"options":{"type":"object","properties":{
      "enabled":{"type":"boolean","x-mcp-header":"Enabled"}}},
      "label":{"type":"string","x-mcp-header":"Label"}}}})");
  if(generation>=2)
    schema["inputSchema"]["properties"]["options"]["properties"]["enabled"]["x-mcp-header"]="NewEnabled";
  if(generation>=3) // 与 NewEnabled 不同大小写但相同 HTTP header 名称。
    schema["inputSchema"]["properties"]["label"]["x-mcp-header"]="newenabled";
  return schema;
}
Json secondary_tool() {
  return json::parse(R"({"name":"secondary","inputSchema":{"type":"object",
    "properties":{"id":{"type":"integer","x-mcp-header":"Item"}}}})");
}
} // namespace

int main() {
  try {
    RunningServer fixture;
    std::atomic<int> generation{1},list_requests{0},tool_calls{0};
    std::atomic<int> bad_headers{0},notifications_sent{0};
    std::atomic_bool notify_once{false};

    fixture.server.Post("/mcp",[&](const httplib::Request& req,httplib::Response& res) {
      const Json request=Json::parse(req.body);
      const std::string method(request.at("method").as_string());
      Json result=json::object{};
      if(method=="server/discover") {
        result=json::object{
          {"supportedVersions",json::array{ProtocolVersion}},
          {"capabilities",json::object{}}};
      } else if(method=="tools/list") {
        ++list_requests;
        const Json& params=request.at("params");
        const bool second=params.contains("cursor");
        if(second) {
          result=json::object{{"tools",json::array{secondary_tool()}}};
        } else {
          result=json::object{{"tools",json::array{primary_tool(generation.load())}},
                              {"nextCursor","second"}};
        }
      } else if(method=="tools/call") {
        ++tool_calls;
        const std::string name(request.at("params").at("name").as_string());
        bool good=false;
        if(name=="secondary") {
          good=req.get_header_value("Mcp-Param-Item")=="42";
        } else if(name=="primary") {
          const bool updated=generation.load()>=2;
          const std::string flag=updated?"Mcp-Param-NewEnabled":"Mcp-Param-Enabled";
          const std::string stale=updated?"Mcp-Param-Enabled":"Mcp-Param-NewEnabled";
          good=req.has_header(flag) && !req.has_header(stale) &&
               req.has_header("Mcp-Param-Label");
          if(updated)good=good && req.get_header_value(flag)=="false";
          else good=good &&
            req.get_header_value(flag)=="true" &&
            req.get_header_value("Mcp-Param-Label").starts_with("=?base64?");
        }
        if(!good)++bad_headers;
        result=json::object{{"isError",false},{"content",json::array{
          json::object{{"type","text"},{"text",good?"headers OK":"headers missing"}}}}};
      } else {
        result=json::object{};
      }
      const Json reply=make_reply(request,std::move(result));
      if(method=="tools/call" && notify_once.exchange(false)) {
        ++notifications_sent;
        const Json event=json::object{
          {"jsonrpc","2.0"},{"method","notifications/tools/list_changed"},
          {"params",json::object{}}};
        res.set_content("data: "+json::serialize(event)+"\n\n"+
                        "data: "+json::serialize(reply)+"\n\n","text/event-stream");
      } else {
        res.set_content(json::serialize(reply),"application/json");
      }
    });

    const int port=fixture.listen();
    auto client=Client::http("http://127.0.0.1:"+std::to_string(port)+"/mcp");

    // 第一次调用 secondary：完整查询两页工具 Schema，才执行第 2 页工具。
    const auto first=client.call("secondary",json::object{{"id",42}});
    check(first.at("isError")==false,"Second-page tool call failed");
    check(list_requests==2 && tool_calls==1 && bad_headers==0,
          "Header schema from second page was not applied");

    // 嵌套字段及 Unicode 字符串由 Schema 注解转换为正确的 HTTP Header。
    (void)client.call("primary",json::object{
      {"options",json::object{{"enabled",true}}},{"label","中文标签"}});
    check(list_requests==2 && tool_calls==2 && bad_headers==0,
          "Tool cache was not reused or nested/unicode headers were incorrect");

    // 再次 tools/list 获取全新 Schema，不能保留上一代的旧 MCP-Param-Enabled。
    generation=2;
    const auto catalog=client.list_all("tools/list","tools");
    check(catalog.is_array() && catalog.size()==2 && list_requests==4,
          "Paginated catalog refresh failed");
    (void)client.call("primary",json::object{
      {"options",json::object{{"enabled",false}}},{"label","new label"}});
    check(list_requests==4 && tool_calls==3 && bad_headers==0,
          "Updated schema failed to replace cached header annotation");

    // 服务端以 SSE 发送 list_changed，下一次调用必须自动重取完整工具目录。
    notify_once=true;
    (void)client.call("secondary",json::object{{"id",42}});
    check(notifications_sent==1 && bad_headers==0,
          "SSE notification or mirrored header handling failed");
    (void)client.call("primary",json::object{
      {"options",json::object{{"enabled",false}}},{"label","fresh"}});
    check(list_requests==6 && tool_calls==5 && bad_headers==0,
          "SSE list_changed did not invalidate the cached HTTP header schema");

    // 错误的重复 Header 注解不可产生不确定的请求头，更不能执行工具。
    generation=3;
    const int before_calls=tool_calls;
    bool rejected=false;
    try {(void)client.list_all("tools/list","tools");}
    catch(const std::exception&) {rejected=true;}
    check(rejected && tool_calls==before_calls,
          "Duplicate case-insensitive x-mcp-header annotation was accepted");

    std::cout<<"HTTP x-mcp-header pagination, refresh, SSE invalidation and validation OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"HTTP header routing regression failed: "<<e.what()<<'\n';
    return 1;
  }
}
