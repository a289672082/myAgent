// 真实本地 HTTP MCP 服务端：验证接收前字节限额及整个请求的截止时间。
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
#include "httplib.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main(){
  httplib::Server server;
  server.Post("/oversized",[](const httplib::Request&,httplib::Response& response){
    response.set_content(std::string(4096,'x'),"application/json");
  });
  server.Post("/stall",[](const httplib::Request&,httplib::Response& response){
    response.set_chunked_content_provider("text/event-stream",
      [](std::size_t,httplib::DataSink& sink) {
        std::this_thread::sleep_for(1200ms);
        static constexpr char frame[]=": idle\\n\\n";
        return sink.write(frame,sizeof(frame)-1);
      });
  });
  server.Post("/slow-stream",[](const httplib::Request&,httplib::Response& response){
    response.set_chunked_content_provider("text/event-stream",
      [](std::size_t,httplib::DataSink& sink) {
        std::this_thread::sleep_for(25ms);
        static constexpr char frame[]=": keepalive\n\n";
        return sink.write(frame,sizeof(frame)-1);
      });
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  assert(port>0);
  std::thread runner([&]{server.listen_after_bind();});
  for(int i=0;i<200 && !server.is_running();++i)std::this_thread::sleep_for(5ms);
  assert(server.is_running());
  const auto origin="http://127.0.0.1:"+std::to_string(port);
  auto custom=mymcp::Client([](const mymcp::Json&){return mymcp::json::object{};});
  assert(!custom.has_bounded_transport());
  auto oauth=std::make_shared<mymcp::oauth::Session>(origin+"/oversized");
  auto oauth_client=mymcp::Client::http_oauth(origin+"/oversized",oauth);
  assert(!oauth_client.has_bounded_transport());
  auto supported=mymcp::Client::http(origin+"/oversized");
  assert(supported.has_bounded_transport());
  mymcp::RequestOptions limited;
  limited.timeout=2s;
  limited.max_response_bytes=256;
  auto oversized=mymcp::Client::http(origin+"/oversized");
  bool failed=false;
  try{(void)oversized.request("server/discover",mymcp::json::object{},limited);}
  catch(const std::runtime_error& e) {
    failed=true;
    assert(std::string(e.what()).find("too large")!=std::string::npos);
  }
  assert(failed);

  mymcp::RequestOptions timed;
  timed.timeout=200ms;
  timed.max_response_bytes=1024*1024;
  auto slow=mymcp::Client::http(origin+"/slow-stream");
  const auto start=std::chrono::steady_clock::now();
  failed=false;
  try{(void)slow.request("server/discover",mymcp::json::object{},timed);}
  catch(const std::runtime_error&){failed=true;}
  assert(failed);
  assert(std::chrono::steady_clock::now()-start < 4s);
  // Cancellation must stop a socket blocked waiting for data, without
  // waiting for another SSE callback or the full transport timeout.
  std::atomic_bool stop{false};
  mymcp::RequestOptions cancelled;
  cancelled.timeout=3s;
  cancelled.cancelled=[&]{return stop.load();};
  auto stall=mymcp::Client::http(origin+"/stall");
  std::thread signal([&] {std::this_thread::sleep_for(90ms);stop=true;});
  const auto canceled_start=std::chrono::steady_clock::now();
  failed=false;
  try{(void)stall.request("server/discover",mymcp::json::object{},cancelled);}
  catch(const std::runtime_error&){failed=true;}
  signal.join();
  assert(failed);
  assert(std::chrono::steady_clock::now()-canceled_start<1s);

  server.stop();runner.join();
  std::cout<<"HTTP MCP frame limits and total deadline OK\n";
}
