// 跨平台 MCP stdio 资源边界回归；同一可执行文件的子进程模式不依赖 Python。
#include "mymcp/mcp.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif
using namespace std::chrono_literals;
int main(int argc,char** argv) {
  if(argc>1) {
#ifndef _WIN32
    if(std::string(argv[1])=="stubborn") std::signal(SIGTERM,SIG_IGN);
#endif
    if(std::string(argv[1])=="no-read") {
      std::this_thread::sleep_for(10s);return 0;
    }
    std::string request;
    if(!std::getline(std::cin,request))return 0;
    if(std::string(argv[1])=="large") {
      std::cout << R"({"jsonrpc":"2.0","id":1,"result":{"data":")"
                << std::string(4096,'x') << "\"}}\n" << std::flush;
    } else if(std::string(argv[1])=="normal") {
      std::cout << R"({"jsonrpc":"2.0","id":1,"result":{"supportedVersions":["2026-07-28"],"capabilities":{}}})"
                << "\n" << std::flush;
    } else {
      std::this_thread::sleep_for(10s);
    }
    return 0;
  }
  auto executable=std::filesystem::absolute(argv[0]).string();
  auto must_fail_fast=[&](std::string mode,mymcp::RequestOptions options,
                          mymcp::Json params=mymcp::json::object{}) {
    auto client=mymcp::Client::process(executable,{std::move(mode)});
    const auto begin=std::chrono::steady_clock::now();
    bool rejected=false;
    try {(void)client.request("server/discover",params,options);}
    catch(const std::exception& ex) {
      rejected=true;
      std::cout << "Expected transport error: " << ex.what() << "\n";
    }
    assert(rejected);
    assert(std::chrono::steady_clock::now()-begin < 5s);
  };
  mymcp::RequestOptions bounded;
  bounded.timeout=150ms;
  bounded.max_response_bytes=1024;
  must_fail_fast("silent",bounded);
  must_fail_fast("stubborn",bounded);
  must_fail_fast("no-read",bounded,
    mymcp::json::object{{"data",std::string(1024*1024,'z')}});
  auto canceled=bounded;
  canceled.cancelled=[] {return true;};
  must_fail_fast("silent",canceled);
  auto small_limit=bounded;
  small_limit.timeout=3s;
  must_fail_fast("large",small_limit);
  auto valid=mymcp::Client::process(executable,{"normal"});
  auto result=valid.discover(small_limit);
  assert(result.at("supportedVersions").is_array());
  // Strict hosts must not mistake arbitrary C++ notification or elicitation
  // callbacks for preemptible / deadline-bounded transport code.
  auto bounded_http=mymcp::Client::http("http://127.0.0.1:1/mcp");
  assert(bounded_http.has_bounded_transport());
  auto callbacks_http=mymcp::Client::http("http://127.0.0.1:1/mcp",{},
    [](const mymcp::Json&) {});
  assert(!callbacks_http.has_bounded_transport());
  bounded_http.on_input_required([](const mymcp::Json&){
    return mymcp::Json(mymcp::json::object{});
  });
  assert(!bounded_http.has_bounded_transport());
  std::cout << "MCP deadline and response preparse limits OK\n";
}
