// 使用本机 HTTP 服务器验证真实 SSE（而非将完整回答拆成伪增量）。
#include "myagent/agent.hpp"
#include "httplib.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace myagent;
namespace j=mymcp::json;
static std::string answer_stream() {
  return
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hel\"}\n\n"
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hello\"}]}]}}\n\n";
}
static std::string chat_stream() {
  return
    "data: {\"choices\":[{\"delta\":{\"content\":\"Chat \"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"SSE\"},\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":3,\"total_tokens\":14}}\n\n"
    "data: [DONE]\n\n";
}
int main() {
  httplib::Server server;
  server.Post("/v1/responses",[](const httplib::Request& req,httplib::Response& res){
    const auto payload=j::parse(req.body);
    assert(payload.at("stream").as_bool());
    res.set_content(answer_stream(),"text/event-stream");
  });
  server.Post("/v1/chat/completions",[](const httplib::Request& req,httplib::Response& res){
    const auto payload=j::parse(req.body);
    assert(payload.at("stream").as_bool());
    res.set_content(chat_stream(),"text/event-stream");
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  if(port<=0)throw std::runtime_error("Cannot bind SSE test server");
  std::thread listener([&]{server.listen_after_bind();});
  for(int i=0;i<200&&!server.is_running();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  try {
    for(const auto mode:{ModelProtocol::Responses,ModelProtocol::ChatCompletions}) {
      Config config;
      config.api_key="local-test-placeholder";
      config.base_url="http://127.0.0.1:"+std::to_string(port);
      config.model="test-model";
      config.model_protocol=mode;
      config.auto_discover_skills=false;
      config.auto_skill_selection=false;
      Agent agent(config);
      std::string streamed;
      int deltas=0;
      LoopOptions options;
      options.on_text_delta=[&](const std::string& phase,const std::string& delta){
        assert(phase=="react");
        streamed+=delta;
        ++deltas;
      };
      const auto result=agent.react("say hello",options);
      if(result.status!=LoopStatus::Completed)
        throw std::runtime_error("SSE loop failed: "+result.error);
      assert(deltas==2);
      assert(streamed==(mode==ModelProtocol::Responses?"Hello":"Chat SSE"));
      assert(result.answer==streamed);
      if(mode==ModelProtocol::ChatCompletions)assert(result.usage.total_tokens==14);
    }
  }catch(...){server.stop();listener.join();throw;}
  server.stop();listener.join();
  std::cout<<"SSE streaming tests passed\n";
}
