// 对源码审查发现的问题做离线回归，验证真实行为而非只检查字符串。
#include "myagent/multi_agent.hpp"
#include "workspace_io.hpp"
#include "httplib.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace myagent;
namespace j=myagent::json;
namespace fs=std::filesystem;
static Json answer(const std::string& text,int input=12,int output=8) {
  return j::object{{"status","completed"},
    {"usage",j::object{{"input_tokens",input},{"output_tokens",output},
                         {"total_tokens",input+output}}},
    {"output",j::array{j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",text}}}}}}}};
}
struct Mock:Model {
  std::atomic<unsigned> calls{0};
  std::function<Json(const Json&)> handler;
  Json create(const Json& request) override {
    ++calls;return handler(request);
  }
};
static Config config() {
  Config c;
  c.auto_discover_skills=false;
  c.auto_skill_selection=false;
  return c;
}
static void test_checkpoint_failover_ledger(const fs::path& root) {
  AgentTeam team(config(),std::make_shared<Mock>());
  auto first=std::make_shared<Mock>();
  first->handler=[](const Json&) {
    auto response=answer("failed",20,10);
    response["status"]="incomplete";
    return response;
  };
  auto backup=std::make_shared<Mock>();
  backup->handler=[](const Json&){return answer("success",40,10);};
  AgentSpec a;a.name="primary";a.description="original";a.config=config();
  team.add_agent(a,first);
  a.name="backup";a.description="fallback";team.add_agent(a,backup);
  TeamTask task{"task1","primary","do one thing"};
  task.fallback_agents={"backup"};
  TeamOptions opt;
  opt.checkpoint_path=root/"checkpoint-v2.json";
  opt.validate_task=[](const TeamTask&,const DelegationResult&) {
    return TaskValidation{true,"verified",j::object{{"checked",true}}};
  };
  const auto initial=team.collaborate({task},opt);
  assert(initial.status==TeamStatus::Completed);
  assert(initial.model_calls==2 && initial.usage.total_tokens==80);
  assert(initial.usage_by_agent.at("primary").total_tokens==30);
  assert(initial.usage_by_agent.at("backup").total_tokens==50);
  const auto data=j::parse([&]{
    std::ifstream in(opt.checkpoint_path);
    return std::string(std::istreambuf_iterator<char>(in),{});
  }());
  assert(data.at("format").as_string()=="myagent-team-checkpoint-v2");
  assert(data.at("prior_attempts").size()==1);
  assert(data.at("prior_attempts")[0].at("agent").as_string()=="primary");
  opt.resume_checkpoint=true;
  const auto resumed=team.collaborate({task},opt);
  assert(resumed.status==TeamStatus::Completed);
  assert(resumed.model_calls==2 && resumed.usage.total_tokens==80);
  assert(resumed.usage_by_agent.at("primary").total_tokens==30);
  assert(resumed.usage_by_agent.at("backup").total_tokens==50);
  assert(first->calls==1 && backup->calls==1);
}
static void test_chat_multi_tool_calls() {
  httplib::Server server;
  std::atomic<unsigned> rounds{0};
  server.Post("/v1/chat/completions",[&](const httplib::Request& req,
                                        httplib::Response& res) {
    const auto body=j::parse(req.body);
    assert(body.at("parallel_tool_calls").is_bool());
    assert(!body.at("parallel_tool_calls").as_bool());
    const unsigned n=++rounds;
    Json message;
    if(n==1) {
      Json calls=j::array{};
      for(int i=0;i<2;++i)
        calls.push_back(j::object{{"id",std::string("call-")+std::to_string(i)},
          {"type","function"},{"function",j::object{
            {"name","local__sum"},{"arguments",R"({"a":1,"b":2})"}}}});
      message=j::object{{"content",nullptr},{"tool_calls",calls}};
    } else {
      const auto& items=body.at("messages");
      int assistants=0,tools=0;
      for(const auto& item:items.array_range()) {
        if(item.at("role").as_string()=="assistant" &&
           j::if_contains(item,"tool_calls")) {
          ++assistants;
          assert(item.at("tool_calls").size()==2);
        }
        if(item.at("role").as_string()=="tool") ++tools;
      }
      assert(assistants==1 && tools==2);
      message=j::object{{"content","both tools returned"}};
    }
    res.set_content(j::serialize(j::object{
      {"choices",j::array{j::object{{"message",message},
        {"finish_reason",n==1?"tool_calls":"stop"}}}}}),"application/json");
  });
  const auto port=server.bind_to_any_port("127.0.0.1");
  assert(port>0);
  std::thread worker([&]{server.listen_after_bind();});
  for(int i=0;i<200 && !server.is_running();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  try {
    auto c=config();
    c.base_url="http://127.0.0.1:"+std::to_string(port);
    c.api_key="not-a-real-key";c.model_protocol=ModelProtocol::ChatCompletions;
    Agent agent(c);
    agent.tool("sum","sum",[](int a,int b){return a+b;},
      mymcp::arg<int>("a"),mymcp::arg<int>("b"));
    assert(agent.ask("1+2 twice")=="both tools returned");
    assert(rounds==2);
  }catch(...){server.stop();worker.join();throw;}
  server.stop();worker.join();
}
static void test_coordinator_hard_budget() {
  auto co=std::make_shared<Mock>();
  co->handler=[](const Json& req) {
    const auto* ins=j::if_contains(req,"instructions");
    if(ins && ins->is_string() &&
       std::string(ins->as_string()).find("Summarize older conversation")!=std::string::npos)
      return answer("earlier conversation summary");
    return answer(R"({"tasks":[{"id":"job","agent":"w","description":"work"}]})");
  };
  auto cfg=config();
  cfg.context.auto_compress=true;
  cfg.context.max_context_tokens=750;
  cfg.context.reserve_output_tokens=50;
  cfg.context.compress_threshold=0.55;
  AgentTeam team(cfg,co);
  // 在团队正式运行之前生成几轮完整历史，确保 Coordinator 需要压缩。
  auto& coordinator=const_cast<Agent&>(team.coordinator());
  coordinator.ask_without_tools("Earlier conversation details to retain.");
  coordinator.ask_without_tools("An earlier context to compact.");
  AgentSpec a;a.name="w";a.description="worker";a.config=config();
  auto worker=std::make_shared<Mock>();
  worker->handler=[](const Json&){return answer("done");};
  team.add_agent(a,worker);
  TeamOptions options;options.max_model_calls=1;
  const auto before=co->calls.load();
  const auto r=team.run("Do some coordination now",options);
  // 压缩+正式规划不能突破团队只允许一次模型调用的硬预算。
  assert(co->calls.load()-before<=1);
  assert(r.model_calls<=1);
  assert(worker->calls==0);
}
static void test_workspaces(const fs::path& root) {
  const auto workspace=root/"workspace",outside=root/"outside";
  fs::create_directories(workspace/"sub");
  fs::create_directories(outside);
  {std::ofstream f(outside/"secret.txt");f<<"TOP_SECRET"; }
  {std::ofstream f(workspace/"sub"/"ok.txt");f<<"old"; }
  assert(detail::workspace_read(workspace,"sub/ok.txt")=="old");
  assert(detail::workspace_write(workspace,"sub/ok.txt","new"));
  assert(detail::workspace_read(workspace,"sub/ok.txt")=="new");
  assert(detail::workspace_list(workspace,"sub").find("ok.txt")!=std::string::npos);
  try{detail::workspace_read(workspace,"../outside/secret.txt");assert(false);}
  catch(const std::exception&){}
#ifndef _WIN32
  fs::create_directory_symlink(outside,workspace/"jump");
  try{detail::workspace_read(workspace,"jump/secret.txt");assert(false);}
  catch(const std::exception&){}
  try{detail::workspace_write(workspace,"jump/new.txt","escaped");assert(false);}
  catch(const std::exception&){}
  assert(!fs::exists(outside/"new.txt"));
  fs::create_symlink(outside/"secret.txt",workspace/"sub"/"link.txt");
  try{detail::workspace_read(workspace,"sub/link.txt");assert(false);}
  catch(const std::exception&){}
#endif
}
static void test_loop_compaction_history_and_hard_limit() {
  auto model=std::make_shared<Mock>();
  std::atomic<unsigned> summaries{0};
  model->handler=[&](const Json& request) {
    const auto* ins=j::if_contains(request,"instructions");
    if(ins && ins->is_string() &&
       std::string(ins->as_string()).find("Summarize older conversation")!=std::string::npos) {
      ++summaries;
      return answer("PRESERVED_COMPACT_SUMMARY");
    }
    return answer("ok");
  };
  auto c=config();
  c.context.auto_compress=true;
  c.context.max_context_tokens=6000;
  c.context.reserve_output_tokens=100;
  c.context.compress_threshold=0.45;
  c.context.keep_recent_turns=1;
  Agent agent(c,model);
  for(int i=0;i<3;++i)
    assert(agent.ask("Earlier history "+std::to_string(i)+" "+std::string(350,'A'))=="ok");
  const auto before=summaries.load();
  const auto result=agent.react("Large current task "+std::string(9000,'B'));
  assert(result.status==LoopStatus::Completed);
  assert(summaries>before);
  assert(agent.history().size()>=3);
  assert(std::string(agent.history()[0].at("content").as_string()).find(
    "PRESERVED_COMPACT_SUMMARY")!=std::string::npos);
  // 继续新任务不应重新看到已被压缩的原文。
  assert(agent.history()[0].at("content").as_string().find(
    "Earlier history 0")==std::string::npos);

  auto hard=config();
  hard.context.auto_compress=true;
  hard.context.max_context_tokens=3000;
  hard.context.reserve_output_tokens=100;
  hard.context.compress_threshold=0.7;
  hard.context.keep_recent_turns=1;
  Agent limited(hard,model);
  for(int i=0;i<2;++i)
    assert(limited.ask("Old conversation "+std::to_string(i)+" "+std::string(400,'C'))=="ok");
  const auto history_before=j::serialize(limited.history());
  const auto count=summaries.load();
  const auto rejected=limited.react("Too-large uncompressible goal "+std::string(12000,'D'));
  assert(rejected.status==LoopStatus::LimitReached);
  assert(summaries>count);
  // 压缩成功但最终输入仍超出硬预算，必须 fail-closed 且不写入历史。
  assert(j::serialize(limited.history())==history_before);
}
int main() {
  auto root=fs::temp_directory_path()/("myagent-audit-"+std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  try {
    std::cerr<<"AUDIT checkpoint\n";
    test_checkpoint_failover_ledger(root);
    std::cerr<<"AUDIT chat\n";
    test_chat_multi_tool_calls();
    std::cerr<<"AUDIT coordinator\n";
    test_coordinator_hard_budget();
    std::cerr<<"AUDIT workspace\n";
    test_workspaces(root);
    std::cerr<<"AUDIT compaction\n";
    test_loop_compaction_history_and_hard_limit();
  } catch(...) {fs::remove_all(root);throw;}
  fs::remove_all(root);
  std::cout<<"Audit regression suite passed\n";
}
