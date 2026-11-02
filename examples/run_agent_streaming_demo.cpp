// HTTP SSE 流式输出专项 Demo：本机模拟服务端，不用真实 API Key 或外网。
// 实际走 OpenAI Responses / Chat Completions HTTP 客户端，而不是手动拆分完整回答。
#include "myagent/agent.hpp"
#include "httplib.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace j=myagent::json;
namespace {
void ensure(bool passed,const std::string& message) {
  if(!passed)throw std::runtime_error("Streaming Demo: "+message);
}
// 服务端按照 Responses 事件协议先发两段文字，再发送完整最终响应。
// Agent 会将 delta 发给 GUI，同时保留最终响应供原有 Loop 使用。
std::string responses_sse() {
  return
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"你好\"}\n\n"
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"，世界\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"你好，世界\"}]}]}}\n\n";
}
std::string chat_sse() {
  return
    "data: {\"choices\":[{\"delta\":{\"content\":\"Chat \"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"流式演示\"},\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}\n\n"
    "data: [DONE]\n\n";
}
void demonstrate(myagent::ModelProtocol protocol,int port) {
  myagent::Config config;
  config.base_url="http://127.0.0.1:"+std::to_string(port);
  config.api_key="local-demo-not-a-real-key";
  config.model="offline-sse-demo";
  config.model_protocol=protocol;
  config.auto_discover_skills=false;
  config.auto_skill_selection=false;
  myagent::Agent agent(config);
  myagent::LoopOptions options;
  std::string assembled;
  unsigned parts=0;
  // 模型通过 SSE 发来新文本时同步回调：桌面程序可把 delta 投递到 UI 线程。
  // phase 用于区分 react/execute/reflect 等阶段；不包含隐藏推理内容。
  options.on_text_delta=[&](const std::string& phase,const std::string& delta) {
    ensure(phase=="react","流式事件阶段不正确");
    ++parts;
    assembled+=delta;
    std::cout<<delta<<std::flush;
  };
  std::cout<<(protocol==myagent::ModelProtocol::Responses?
             "[Responses SSE] ":"[Chat Completions SSE] ");
  const auto result=agent.react("请说你好",options);
  std::cout<<"\n";
  ensure(result.status==myagent::LoopStatus::Completed,
         "Loop 未完成："+result.error);
  ensure(parts==2,"没有收到两个真实 SSE 文本增量");
  const std::string expected=protocol==myagent::ModelProtocol::Responses?
      "你好，世界":"Chat 流式演示";
  ensure(assembled==expected && result.answer==expected,
         "流式内容与完整最终回答不一致");
  if(protocol==myagent::ModelProtocol::ChatCompletions)
    ensure(result.usage.total_tokens==15,"Chat SSE usage 没有正确统计");
  std::cout<<"[验收] 文本增量 "<<parts<<" 段；最终回答与流式拼接结果一致。\n";
}
} // namespace

int run_agent_streaming_demo() {
  httplib::Server server;
  server.Post("/v1/responses",[](const httplib::Request& req,httplib::Response& res) {
    const auto body=j::parse(req.body);
    if(!body.at("stream").as_bool()) {
      res.status=400;
      return;
    }
    res.set_content(responses_sse(),"text/event-stream");
  });
  server.Post("/v1/chat/completions",[](const httplib::Request& req,httplib::Response& res) {
    const auto body=j::parse(req.body);
    if(!body.at("stream").as_bool()) {
      res.status=400;
      return;
    }
    res.set_content(chat_sse(),"text/event-stream");
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  if(port<=0) {
    std::cerr<<"run_agent_streaming_demo 失败：无法创建本机 SSE 服务。\n";
    return 1;
  }
  std::thread listener([&]{server.listen_after_bind();});
  try {
    for(int n=0;n<200 && !server.is_running();++n)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ensure(server.is_running(),"本机 HTTP 服务器未启动");
    demonstrate(myagent::ModelProtocol::Responses,port);
    demonstrate(myagent::ModelProtocol::ChatCompletions,port);
    server.stop();
    listener.join();
    std::cout<<"run_agent_streaming_demo: 全部检查通过。\n";
    return 0;
  }catch(const std::exception& e) {
    server.stop();
    listener.join();
    std::cerr<<"run_agent_streaming_demo 失败："<<e.what()<<"\n";
    return 1;
  }
}
int main(){return run_agent_streaming_demo();}
