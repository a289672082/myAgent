// 独立控制台的“直接聊天”回归：注入 SDK 公开 Model（完全离线）。
// 验证两轮历史复用、新建对话时历史清空，以及不调用任何网络后端。
// 不需要 API Key、FTXUI 输入/TTY，也不修改 myAgent SDK。
#include "session.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void check(bool yes,const char* problem) {
  if(!yes)throw std::runtime_error(problem);
}
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
    return myagent::json::object{
      {"status","completed"},
      {"output",myagent::json::array{
        myagent::json::object{{"type","message"},{"role","assistant"},
          {"content",myagent::json::array{
            myagent::json::object{{"type","output_text"},
              {"text","reply-"+std::to_string(requests)}}}}}}}};
  }
};
}
int main() {
  try {
    auto model=std::make_shared<MockModel>();
    myagent_console::ConsoleSession session(model);
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
    const auto first=chat("First text");
    check(first.status==myagent::LoopStatus::Completed && first.answer=="reply-1",
          "First mock conversation failed");
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

    std::cout<<"myagent_console_chat: offline multi-turn, history reset and SDK API OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"myagent_console_chat failed: "<<e.what()<<'\n';
    return 1;
  }
}
