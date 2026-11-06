// 本机 HTTP 服务演示实际请求和响应的 JSON 回调，不需要外网或真实密钥。
#include "myagent/agent.hpp"
#include "httplib.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
  httplib::Server server;
  server.Post("/v1/responses",[](const httplib::Request&,httplib::Response& res) {
    res.set_content(R"({"status":"completed","output":[]})","application/json");
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  if(port<=0) {std::cerr<<"Cannot bind local server\n";return 1;}
  std::thread listener([&]{server.listen_after_bind();});
  try {
    for(int i=0;i<200 && !server.is_running();++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    myagent::Config config;
    config.api_key="local-placeholder";
    config.base_url="http://127.0.0.1:"+std::to_string(port);
    unsigned requests=0,responses=0;
    config.on_model_http_json=[&](std::string_view event,const myagent::Json& body) {
      if(event=="request")++requests;
      if(event=="response")++responses;
      std::cout<<myagent::json::serialize(myagent::json::object{
        {"event",std::string(event)},{"data",body}})<<'\n';
    };
    myagent::OpenAIResponses model(config);
    model.create(myagent::json::object{{"model","example"},
                                      {"input",myagent::json::array{}}});
    if(requests!=1 || responses!=1)throw std::runtime_error("Missing HTTP JSON callback");
    server.stop();listener.join();
    return 0;
  } catch(const std::exception& e) {
    server.stop();listener.join();
    std::cerr<<e.what()<<'\n';return 1;
  }
}
