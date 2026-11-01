// 通过真实 stdio 子进程验证 Agent 将剩余截止时间和接收预算下发 SDK。
#include "myagent/agent.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif
using namespace myagent;
using namespace std::chrono_literals;
struct Mock final:Model {
  unsigned calls=0;
  Json create(const Json&) override {
    ++calls;
    return mymcp::json::object{{"status","completed"},{"output",mymcp::json::array{
      mymcp::json::object{{"type","message"},{"role","assistant"},
        {"content",mymcp::json::array{mymcp::json::object{
          {"type","output_text"},{"text","model"}}}}}}}};
  }
};
int main(int argc,char** argv){
  if(argc>1) {
#ifndef _WIN32
    if(std::string(argv[1])=="stubborn")std::signal(SIGTERM,SIG_IGN);
#endif
    std::string message;
    std::getline(std::cin,message);
    if(std::string(argv[1])=="huge") {
      std::cout<<R"({"jsonrpc":"2.0","id":1,"result":{"data":")"
               << std::string(25000,'a')<<"\"}}\n"<<std::flush;
    } else std::this_thread::sleep_for(10s);
    return 0;
  }
  const auto path=std::filesystem::absolute(argv[0]).string();
  auto make_config=[]{
    Config c;c.auto_discover_skills=false;c.auto_skill_selection=false;
    c.mcp_timeout_seconds=1;c.max_mcp_response_bytes=4096;
    return c;
  };
  auto model=std::make_shared<Mock>();
  Agent blocked(make_config(),model);
  blocked.add_mcp_stdio("hung",path,{"stubborn"});
  LoopOptions opts;opts.timeout=350ms;
  const auto begin=std::chrono::steady_clock::now();
  auto result=blocked.react("MCP must not hang",opts);
  assert(result.status!=LoopStatus::Completed);
  assert(std::chrono::steady_clock::now()-begin<5s);
  assert(model->calls==0);

  auto tiny=make_config();
  tiny.max_mcp_response_bytes=1000;
  Agent oversized(tiny,model);
  oversized.add_mcp_stdio("huge",path,{"huge"});
  const auto start=std::chrono::steady_clock::now();
  auto second=oversized.react("Reject oversized tool list",opts);
  assert(second.status!=LoopStatus::Completed);
  assert(std::chrono::steady_clock::now()-start<5s);
  assert(model->calls==0);
  // 严格模式不允许将任意 C++ lambda 当作可强制终止的工具。
  auto strict=make_config();
  strict.strict_process_isolation=true;
  strict.api_key="offline-test-key";
  strict.base_url="http://127.0.0.1:1";
  bool rejected=false;
  try {Agent invalid(strict,model);assert(false);}
  catch(const std::invalid_argument&){rejected=true;}
  assert(rejected); // 普通 Mock 不声明 deadline guarantee。

  Agent guarded(strict); // 内置 HTTP Model 有有界传输。
  rejected=false;
  try{guarded.tool("unsafe","C++ callback",[] {return true;});}
  catch(const std::invalid_argument&){rejected=true;}
  assert(rejected);
  rejected=false;
  try {
    guarded.add_mcp("unbounded",std::make_shared<mymcp::Client>(
       [](const Json&){return mymcp::json::object{};}));
  } catch(const std::invalid_argument&){rejected=true;}
  assert(rejected);
  guarded.add_mcp_stdio("safe_child",path,{"stubborn"});
  std::cout<<"Agent MCP transport deadline and predecode limit tests OK\n";
}
