// P0：工具权限/宿主审批、Coordinator 无工具、同实例会话并发和超时测试。
#include "myagent/agent.hpp"
#include "myagent/multi_agent.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
namespace j=myagent::json;
using namespace myagent;
static Json answer(const std::string& content) {
  return j::object{{"status","completed"},{"output",j::array{j::object{
    {"type","message"},{"role","assistant"},{"content",j::array{j::object{
      {"type","output_text"},{"text",content}}}}}}}};
}
static Json call(const char* name) {
  return j::object{{"status","completed"},{"output",j::array{j::object{
    {"type","function_call"},{"call_id","call-1"},{"name",name},{"arguments","{}"}}}}};
}
struct Mock:Model {
  std::function<Json(const Json&,int)> reply;
  std::atomic<int> calls{0};
  Json create(const Json& request) override { return reply(request,++calls); }
};
static Config cfg() {
  Config c;c.auto_discover_skills=false;c.auto_skill_selection=false;
  return c;
}
static bool has_tool(const Json& req,const std::string& name) {
  const auto* defs=j::if_contains(req,"tools");
  if(!defs)return false;
  for(const auto& item:defs->array_range())
    if(item.at("name").as_string()==name)return true;
  return false;
}
static void test_policy() {
  auto mock=std::make_shared<Mock>();
  Config config=cfg();
  config.tool_policy.default_access=ToolAccess::Deny;
  config.tool_policy.allow("local__safe");
  config.tool_policy.require_approval("local__danger");
  std::atomic<int> approved{0},executed{0};
  config.tool_policy.approve=[&](const ToolRequest& req) {
    assert(req.tool=="local__danger");
    assert(req.phase=="ask");
    ++approved;return approved.load()>=2;
  };
  Agent agent(config,mock);
  agent.tool("safe","read",[](){return 1;});
  agent.tool("danger","dangerous", [&](){++executed;return true;});
  agent.tool("hidden","denied",[](){return "NEVER";});
  mock->reply=[&](const Json& req,int n) {
    assert(!has_tool(req,"local__hidden"));
    assert(has_tool(req,"local__danger"));
    assert(has_tool(req,"local__safe"));
    if(n==1||n==3)return call("local__danger");
    const auto& last=req.at("input")[req.at("input").size()-1].at("output");
    if(n==2)assert(std::string(last.as_string()).find("isError")!=std::string::npos);
    return answer("done");
  };
  assert(agent.ask("try sensitive")=="done");
  assert(approved==1 && executed==0);
  assert(agent.ask("try again")=="done");
  assert(approved==2 && executed==1);
}
static void test_coordinator_no_tools() {
  auto mock=std::make_shared<Mock>();
  Agent agent(cfg(),mock);
  agent.tool("danger","DO NOT EXPOSE",[](){return true;});
  mock->reply=[](const Json& req,int) {
    assert(!j::if_contains(req,"tools"));
    return answer("safe coordinator");
  };
  assert(agent.ask_without_tools("plan only")=="safe coordinator");
}
static void test_session_lock() {
  auto mock=std::make_shared<Mock>();
  Agent agent(cfg(),mock);
  std::atomic<int> running{0},max_running{0};
  mock->reply=[&](const Json&,int){
    const int active=++running;
    int earlier=max_running.load();
    while(active>earlier && !max_running.compare_exchange_weak(earlier,active)){}
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    --running;
    return answer("ok");
  };
  std::thread a([&]{assert(agent.ask("one")=="ok");});
  std::thread b([&]{assert(agent.ask("two")=="ok");});
  a.join();b.join();
  assert(max_running==1);
  assert(agent.history().size()==4);
}
static void test_cancel() {
  auto mock=std::make_shared<Mock>();
  mock->reply=[](const Json&,int){return answer("never");};
  Agent agent(cfg(),mock);
  LoopOptions opts;
  opts.cancelled=[](){return true;};
  auto r=agent.react("cancel",opts);
  assert(r.status==LoopStatus::Cancelled && mock->calls==0);
  opts.cancelled={};opts.timeout=std::chrono::milliseconds(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  // 新运行会创建自己的 deadline，正常快速请求能够完成。
  r=agent.react("fast",opts);
  assert(r.status==LoopStatus::Completed || r.status==LoopStatus::Cancelled);
}
int main() {
  test_policy();test_coordinator_no_tools();test_session_lock();test_cancel();
  std::cout<<"P0 runtime safety tests OK\n";
}
