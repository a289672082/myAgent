#include "mymcp/mcp.hpp"
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using namespace mymcp;
#define CHECK(x) do{if(!(x))throw std::runtime_error("HTTP resilience: " #x);}while(false)
int main() {
  httplib::Server server;
  std::atomic<int> read_calls{0},progress_events{0};
  std::mutex ids_mutex;
  std::vector<Json> ids;
  server.Post("/mcp",[&](const httplib::Request& request,httplib::Response& response){
    const Json msg=Json::parse(request.body);
    const auto method=std::string(msg.at("method").as_string());
    if (method=="server/discover") {
      const auto reply=json::object{{"jsonrpc","2.0"},{"id",msg.at("id")},
        {"result",json::object{{"resultType","complete"},
           {"supportedVersions",json::array{ProtocolVersion}},{"capabilities",json::object{}}}}};
      response.set_content(json::serialize(reply),"application/json");
      return;
    }
    if (method=="resources/read") {
      {std::lock_guard guard(ids_mutex);ids.push_back(msg.at("id"));}
      if (++read_calls==1) {
        response.set_content("","application/json"); // 模拟丢失回复。
      } else {
        const auto reply=json::object{{"jsonrpc","2.0"},{"id",msg.at("id")},
          {"result",json::object{{"resultType","complete"},
            {"contents",json::array{json::object{{"uri","test://ok"},{"text","ok"}}}}}}};
        response.set_content(json::serialize(reply),"application/json");
      }
      return;
    }
    if (method=="tools/call") {
      const auto event=json::object{{"jsonrpc","2.0"},{"method","notifications/progress"},
        {"params",json::object{{"progressToken","progress-1"},{"progress",1}}}};
      const auto reply=json::object{{"jsonrpc","2.0"},{"id",msg.at("id")},
        {"result",detail::make_tool_result("done")}};
      response.set_content("data: "+json::serialize(event)+"\r\n\r\n"+
                           "data: "+json::serialize(reply)+"\r\n\r\n","text/event-stream");
      return;
    }
    if (method=="subscriptions/listen") {
      const auto ack=json::object{{"jsonrpc","2.0"},
        {"method","notifications/subscriptions/acknowledged"},
        {"params",json::object{{"subscriptionId","test"}}}};
      response.set_content("data: "+json::serialize(ack)+"\r\n\r\n","text/event-stream");
      return;
    }
    response.status=404;
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  CHECK(port>0);
  std::thread worker([&]{server.listen_after_bind();});
  struct Cleanup {
    httplib::Server& server; std::thread& thread;
    ~Cleanup(){server.stop();if(thread.joinable())thread.join();}
  } cleanup{server,worker};
  for(int i=0;i<300 && !server.is_running();++i)std::this_thread::sleep_for(5ms);
  CHECK(server.is_running());
  const std::string url="http://127.0.0.1:"+std::to_string(port)+"/mcp";
  auto client=Client::http(url,{},[&](const Json& n){
    if(n.is_object()&&n.contains("method")&&n.at("method")=="notifications/progress")
      ++progress_events;
  });
  RequestOptions opts;opts.timeout=3000ms;
  auto result=client.request_readonly_retry(
      "resources/read",json::object{{"uri","test://ok"}},2,opts);
  CHECK(result.at("contents")[0].at("text")=="ok");
  CHECK(read_calls==2);
  {std::lock_guard guard(ids_mutex);CHECK(ids.size()==2&&ids[0]!=ids[1]);}
  bool denied=false;
  try {(void)client.request_readonly_retry("tools/call");}
  catch(const std::invalid_argument&){denied=true;}
  CHECK(denied);
  CHECK(client.request_with_progress(
    "tools/call",json::object{{"name","dummy"}}, "progress-1")
    .at("content")[0].at("text")=="done");
  CHECK(progress_events==1);
  ListenOptions listen;listen.max_frame_bytes=4096;listen.timeout=1000ms;
  int ack_count=0;
  Client::listen_http(url,json::object{{"toolsListChanged",true}},
    [&](const Json& event){++ack_count;return false;},listen);
  CHECK(ack_count==1);
  std::cout<<"HTTP read retry with new ID, CRLF SSE, explicit listen close passed\n";
}
