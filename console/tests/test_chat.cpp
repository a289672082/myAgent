// 独立控制台的“直接聊天”回归：注入 SDK 公开 Model（完全离线）。
// 验证两轮历史复用、新建对话时历史清空，以及不调用任何网络后端。
// 不需要 API Key、FTXUI 输入/TTY，也不修改 myAgent SDK。
#include "session.hpp"
#include "httplib.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool yes,const char* problem) {
  if(!yes)throw std::runtime_error(problem);
}
struct TestDirectory {
  std::filesystem::path old=std::filesystem::current_path();
  std::filesystem::path path=std::filesystem::temp_directory_path()/
    ("myagent-chat-log-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestDirectory() {std::filesystem::create_directories(path);std::filesystem::current_path(path);}
  ~TestDirectory() {std::filesystem::current_path(old);std::error_code ec;std::filesystem::remove_all(path,ec);}
};
struct MockModel final : myagent::Model {
  int requests=0;
  myagent::Json create(const myagent::Json& request) override {
    ++requests;
    const auto data=mymcp::json::serialize(request.at("input"));
    if(requests==2) {
      check(data.find("reply-1")!=std::string::npos,
            "Second turn did not receive previous chat answer");
    }
    if(requests==3) {
      check(data.find("reply-1")==std::string::npos,
            "New chat must not contain old conversation history");
      check(data.find("reply-2")==std::string::npos,
            "New chat must not contain old conversation history");
    }
    const auto part=myagent::json::object{
      {"type","output_text"},{"text","reply-"+std::to_string(requests)}};
    const auto message=myagent::json::object{
      {"type","message"},{"role","assistant"},
      {"content",myagent::json::array{part}}};
    return myagent::json::object{
      {"status","completed"},{"output",myagent::json::array{message}}};
  }
};
}
int main() {
  try {
    TestDirectory dir;
    std::filesystem::create_directories(dir.path/"skills"/"console-fixture");
    {
      std::ofstream skill(dir.path/"skills"/"console-fixture"/"SKILL.md");
      skill<<"---\nname: console-fixture\ndescription: Example skill for console\n---\nBody\n";
    }
    auto model=std::make_shared<MockModel>();
    myagent_console::ConsoleSession session(model,dir.path/"config.json");
    session.set_api_key("TEST-LOG-SECRET");
    const auto before=session.handle("/listSkills");
    check(before.kind==myagent_console::CommandKind::Info &&
          before.message.find("console-fixture")!=std::string::npos &&
          before.message.find("Agent 尚未初始化")!=std::string::npos,
          "skill listing before agent initialization failed");
    check(!session.suggestions("/LISTS").empty(),"skill listing completion missing");
    int tool_approvals=0;
    const auto chat=[&](const std::string& prompt) {
      return session.chat(prompt,
        [](const std::string&,const std::string&){},
        [&](const myagent::ToolApprovalRequest&) {
          ++tool_approvals;
          return false; // 默认测试不允许任何 MCP 副作用
        },
        [](const std::string&){},
        []{return false;});
    };
    const auto first=chat("First text TEST-LOG-SECRET");
    check(first.status==myagent::LoopStatus::Completed && first.answer=="reply-1",
          "First mock conversation failed");
    const auto after=session.handle("/LISTSKILLS");
    check(after.kind==myagent_console::CommandKind::Info &&
          after.message.find("console-fixture")!=std::string::npos &&
          after.message.find("当前 Agent")!=std::string::npos,
          "active agent skill listing failed");
    const auto second=chat("Second text");
    check(second.status==myagent::LoopStatus::Completed && second.answer=="reply-2",
          "Second conversation did not reuse history");
    check(session.handle("/new").kind==myagent_console::CommandKind::Info,
          "New conversation command failed");
    const auto third=chat("Third text");
    check(third.status==myagent::LoopStatus::Completed && third.answer=="reply-3",
          "Chat after /new failed");
    check(model->requests==3 && tool_approvals==0,
          "Extra model or unauthorized MCP execution observed");
    httplib::Server server;
    std::string received;
    server.Post("/v1/responses",[&](const httplib::Request& req,httplib::Response& res) {
      received=req.body;
      res.set_content("data: {\"type\":\"response.output_text.delta\",\"delta\":\"OK\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"OK\"}]}]}}\n\n", "text/event-stream");
    });
    const int port=server.bind_to_any_port("127.0.0.1");
    check(port>0,"could not bind test server");
    std::jthread listener([&]{server.listen_after_bind();});
    for(int i=0;i<200 && !server.is_running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    myagent_console::ConsoleSession http_session({},dir.path/"http-config.json");
    http_session.set_api_key("TEST-LOG-SECRET");
    check(http_session.handle("/baseurl http://127.0.0.1:"+std::to_string(port)).kind==myagent_console::CommandKind::Info,"test base URL rejected");
    const auto http_result=http_session.chat("HTTP test",[](const auto&,const auto&){},
      [](const auto&){return false;},[](const auto&){},[]{return false;});
    server.stop();listener.join();
    check(http_result.status==myagent::LoopStatus::Completed,"HTTP test failed");
    std::ifstream log(dir.path/"myagent_console.log",std::ios::binary);
    std::string line; bool request_seen=false,response_seen=false;
    while(std::getline(log,line)) {
      const auto record=mymcp::json::parse(line);
      const auto event=std::string(record.at("event").as_string());
      if(event=="request") {
        check(mymcp::json::serialize(record.at("data"))==mymcp::json::serialize(mymcp::json::parse(received)),"logged request differs from HTTP body");
        request_seen=true;
      } else if(event=="response" && record.at("data").contains("type") &&
                std::string(record.at("data").at("type").as_string())=="response.completed")response_seen=true;
      check(line.find("TEST-LOG-SECRET")==std::string::npos,"API Key exposed in log");
    }
    check(request_seen && response_seen,"HTTP JSON request or response missing from log");

    std::cout<<"myagent_console_chat: offline multi-turn, history reset and SDK API OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"myagent_console_chat failed: "<<e.what()<<'\n';
    return 1;
  }
}
