// Regression cases for audit issues that were not covered by earlier suites.
#include "myagent/multi_agent.hpp"
#include "httplib.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace myagent;
namespace j=mymcp::json;
namespace fs=std::filesystem;

static Config settings() {
  Config c;
  c.auto_discover_skills=false;
  c.auto_skill_selection=false;
  c.model="offline-demo";
  return c;
}
static Json answer(std::string text) {
  return j::object{{"status","completed"},{"output",j::array{j::object{
    {"type","message"},{"role","assistant"},{"content",j::array{j::object{
      {"type","output_text"},{"text",std::move(text)}}}}}}}};
}
struct Mock final: Model {
  std::function<Json(const Json&)> reply;
  std::atomic<unsigned> calls{0};
  Json create(const Json& request) override {++calls;return reply(request);}
};
static void test_token_usage_atomicity() {
  TokenUsage a;
  a.input_tokens=7;
  a.output_tokens=std::numeric_limits<std::uint64_t>::max();
  TokenUsage b;
  b.input_tokens=1;b.output_tokens=1;
  try{a.add(b);assert(false);}catch(const std::overflow_error&){}
  assert(a.input_tokens==7);
  assert(a.output_tokens==std::numeric_limits<std::uint64_t>::max());
}
static void test_chat_targets_and_finish() {
  httplib::Server server;
  server.Post(R"(/.*)",[](const httplib::Request& req,httplib::Response& res) {
    assert(req.target=="/gateway/v1/chat/completions?version=2");
    res.set_content(R"({"choices":[{"finish_reason":"content_filter","message":{"content":"filtered"}}]})",
                    "application/json");
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  assert(port>0);
  std::thread thread([&]{server.listen_after_bind();});
  for(int n=0;n<200 && !server.is_running();++n)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  try {
    auto c=settings();
    c.api_key="test";
    c.base_url="http://127.0.0.1:"+std::to_string(port)+"/gateway?version=2";
    ChatCompletionsModel model(c);
    Json request=j::object{{"model","offline-demo"},{"input",j::array{
      j::object{{"role","user"},{"content","hello"}}}}};
    auto result=model.create(request);
    assert(result.at("status").as_string()=="incomplete");
    assert(result.at("output").size()==1);
  }catch(...){server.stop();thread.join();throw;}
  server.stop();thread.join();
}
static void test_empty_completion() {
  auto model=std::make_shared<Mock>();
  model->reply=[](const Json&) {
    return Json(j::object{{"status","completed"},{"output",j::array{}}});
  };
  Agent agent(settings(),model);
  try{agent.ask("hello");assert(false);}catch(const std::runtime_error&){}
  assert(agent.history().empty());
}
static void test_history_format(const fs::path& root) {
  auto model=std::make_shared<Mock>();
  model->reply=[](const Json&){return answer("ok");};
  Agent agent(settings(),model);
  assert(agent.ask("hello")=="ok");
  auto file=root/"history.json";
  agent.save_history(file);
  std::ifstream input(file);
  const std::string saved(std::istreambuf_iterator<char>{input},{});
  input.close(); // Windows must release the read handle before overwriting.
  auto data=j::parse(saved);
  assert(data.at("format").as_string()=="myagent-history-v2");
  Agent restored(settings(),model);
  restored.load_history(file);
  assert(restored.history().size()==agent.history().size());
  // A second session must not overwrite a newer revision that another Agent
  // saved after both sessions had loaded the old version.
  Agent newer(settings(),model);
  newer.load_history(file);
  assert(newer.ask("new conversation turn")=="ok");
  newer.save_history(file);
  try {
    restored.save_history(file);
    assert(false);
  }catch(const std::runtime_error&){}
  assert(restored.history().size()==agent.history().size());
  // Reject malformed restored messages without destroying the prior history.
  data["messages"][0]["role"]="system";
  {std::ofstream out(file);out<<j::serialize(data);}
  try{restored.load_history(file);assert(false);}catch(const std::runtime_error&){}
  assert(restored.history().size()==agent.history().size());
  data["messages"][0]["role"]="user";
  data["model"]="different-model";
  {std::ofstream out(file);out<<j::serialize(data);}
  try{restored.load_history(file);assert(false);}catch(const std::runtime_error&){}
  // Legacy arrays are still supported after validation.
  {std::ofstream out(file);out<<j::serialize(data.at("messages"));}
  restored.load_history(file);
}
static void test_worker_cancellation_and_deadline() {
  auto worker=std::make_shared<Mock>();
  worker->reply=[](const Json&){return answer("worker");};
  auto coordinator=std::make_shared<Mock>();
  coordinator->reply=[](const Json&){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return answer(R"({"tasks":[{"id":"t1","agent":"worker","description":"do work"}]})");
  };
  AgentTeam team(settings(),coordinator);
  AgentSpec spec;
  spec.name="worker";spec.description="test worker";spec.config=settings();
  team.add_agent(spec,worker);
  TeamTask task;task.id="t1";task.agent="worker";task.description="do work";
  TeamOptions options;
  options.worker_loop.cancelled=[](){return true;};
  const auto cancelled=team.collaborate({task},options);
  assert(cancelled.status!=TeamStatus::Completed);
  assert(worker->calls==0);
  options.worker_loop.cancelled={};
  options.timeout=std::chrono::milliseconds(1);
  const auto timed=team.run("do work",options);
  assert(timed.status==TeamStatus::Cancelled);
  assert(worker->calls==0);
  // A 1 ms deadline can expire before dispatch (0 calls) or while the
  // mock is sleeping (1 call). Count actual attempts, not a scheduling guess.
  assert(timed.model_calls==coordinator->calls.load());
  assert(timed.model_calls<=1);
}
static void test_noncooperative_model_deadline() {
  auto model=std::make_shared<Mock>();
  model->reply=[](const Json&){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return answer("late completion");
  };
  Agent agent(settings(),model);
  LoopOptions options;
  options.timeout=std::chrono::milliseconds(1);
  auto result=agent.react("timeout must not commit history",options);
  assert(result.status==LoopStatus::Cancelled);
  assert(agent.history().empty());
  try {
    (void)agent.ask_without_tools("late request",1,std::chrono::milliseconds(1));
    assert(false);
  }catch(const std::runtime_error&) {}
  assert(agent.history().empty());
}
static void test_config_transaction_and_workspace(const fs::path& root) {
  auto dir=root/"team-config";
  fs::create_directories(dir);
  auto config_file=dir/"agents.yaml";
  const auto write_config=[&](bool duplicate) {
    std::ofstream out(config_file);
    out<<"agents:\n"
       <<"  - name: worker\n"
       <<"    description: test worker\n"
       <<"    workspace: nested-work\n"
       <<"    config:\n"
       <<"      base_url: http://127.0.0.1:1\n"
       <<"      model: dummy\n"
       <<"      api_key_ref: worker-key\n";
    if(duplicate)
      out<<"  - name: worker\n"
         <<"    description: duplicate worker\n"
         <<"    config:\n"
         <<"      base_url: http://127.0.0.1:1\n"
         <<"      model: dummy\n"
         <<"      api_key_ref: worker-key\n";
  };
  AgentTeam team(settings(),std::make_shared<Mock>());
  write_config(true);
  try{
    team.add_agents_from_file(config_file,[](const std::string&){return "offline";});
    assert(false);
  }catch(const std::exception&){}
  try{(void)team.agent("worker");assert(false);}catch(const std::out_of_range&){}
  assert(!fs::exists(dir/"nested-work")); // Newly created empty dirs rolled back.
  write_config(false);
  team.add_agents_from_file(config_file,[](const std::string&){return "offline";});
  assert(fs::is_directory(dir/"nested-work"));
}
static void test_replay_guard() {
  std::atomic<unsigned> effects{0};
  auto model=std::make_shared<Mock>();
  model->reply=[&](const Json&) {
    const unsigned round=model->calls.load();
    if(round<=2)
      return Json(j::object{{"status","completed"},{"output",j::array{j::object{
        {"type","function_call"},{"name","local__write"},
        {"call_id",round==1?"call-1":"call-2"},{"arguments",R"({"path":"same"})"}
      }}}});
    return answer("finished");
  };
  Agent agent(settings(),model);
  agent.tool("write","non-idempotent write",[&](std::string){++effects;return true;},
             mymcp::arg<std::string>("path"));
  LoopOptions options;
  // Protect unknown tool names by default; callers opt safe read-only tools out.
  assert(options.protect_unclassified_tools);
  options.max_iterations=6;
  const auto result=agent.react("write once",options);
  assert(result.status==LoopStatus::Completed);
  assert(effects==1);
  bool blocked=false;
  for(const auto& e:result.trace.array_range()) {
    const auto* event=j::if_contains(e,"event");
    if(event && event->is_string() && event->as_string()=="tool_replay_blocked")
      blocked=true;
  }
  assert(blocked);
}
static void test_memory_save_and_metadata(const fs::path& root) {
  MemoryStore memory;
  memory.remember({"note","Hello memory",j::object{{"topic","test"}}});
  const auto file=root/"memory.json";
  memory.save(file);
  memory.remember({"more","Extra item",j::object{}});
  memory.save(file); // Existing destination must be replaceable on Windows.
  MemoryStore loaded;
  loaded.load(file);
  assert(loaded.all().size()==2);
  try {
    memory.remember({"huge","body",j::object{{"blob",std::string(17000,'a')}}});
    assert(false);
  }catch(const std::invalid_argument&){}
  assert(memory.all().size()==2);
  // Replacements and deletions must also update the exact save-size ledger.
  memory.remember({"note","updated",j::object{}});
  assert(memory.forget("more"));
  memory.save(file);
  loaded.load(file);
  assert(loaded.all().size()==1);
  assert(loaded.all().front().content=="updated");

  // A 32 MiB in-memory state is always serializable. The *next* record is
  // rejected before it can poison subsequent save() operations.
  MemoryStore capped;
  const std::string body(16300,'x');
  bool hit_limit=false;
  for(unsigned i=0;i<2300;++i) {
    try {capped.remember({std::to_string(i),body,j::object{}});}
    catch(const std::runtime_error&) {hit_limit=true;break;}
  }
  assert(hit_limit);
  const auto capped_file=root/"capped-memory.json";
  capped.save(capped_file);
  MemoryStore restored;
  restored.load(capped_file);
  assert(restored.all().size()==capped.all().size());

  // Two processes can load a common version; only the first update may save.
  MemoryStore first_writer,stale_writer;
  first_writer.load(file);
  stale_writer.load(file);
  first_writer.remember({"winner","saved",j::object{}});
  first_writer.save(file);
  stale_writer.remember({"loser","stale",j::object{}});
  try {
    stale_writer.save(file);
    assert(false);
  }catch(const std::runtime_error&){}
  MemoryStore verified;
  verified.load(file);
  assert(verified.all().size()==2);

  // The load operation must be transactional, rejecting duplicate IDs
  // rather than silently replacing a previous record.
  {
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    out << R"([{"id":"duplicate","content":"first","metadata":{}},{"id":"duplicate","content":"second","metadata":{}}])";
  }
  try {
    loaded.load(file);
    assert(false);
  }catch(const std::runtime_error&){}
  assert(loaded.all().size()==1 && loaded.all().front().id=="note");

  // A concurrent writer must be refused instead of replacing the file.
  const auto lock_path=fs::path(file.string()+".lock");
  fs::create_directory(lock_path);
  try {
    memory.save(file);
    assert(false);
  }catch(const std::runtime_error&){}
  fs::remove(lock_path);
  // This instance has a stale revision: save must still reject that overwrite.
  try {
    memory.save(file);
    assert(false);
  }catch(const std::runtime_error&){}
  const auto separate=root/"new-memory.json";
  memory.save(separate);
  loaded.load(separate);
  assert(loaded.all().size()==1);
}
static void test_handoff_budget(const fs::path& root) {
  auto model=std::make_shared<Mock>();
  std::atomic<std::size_t> longest{0};
  model->reply=[&](const Json& request) {
    const auto& input=request.at("input");
    for(const auto& entry:input.array_range()) {
      if(!entry.is_object())continue;
      const auto* role=j::if_contains(entry,"role");
      const auto* content=j::if_contains(entry,"content");
      if(role && role->is_string() && role->as_string()=="user" &&
         content && content->is_string()) {
        const auto value=std::string(content->as_string());
        longest=std::max(longest.load(),value.size());
      }
    }
    return answer(std::string(800,'x'));
  };
  AgentTeam team(settings(),std::make_shared<Mock>());
  AgentSpec spec;spec.name="worker";spec.description="worker";spec.config=settings();
  team.add_agent(spec,model);
  TeamTask first;first.id="first";first.agent="worker";first.description="First";
  TeamTask second;second.id="second";second.agent="worker";
  second.description="Second";second.depends_on={"first"};
  second.context.goal="A very long goal: "+std::string(700,'x');
  second.context.relevant_context=j::object{{"blob",std::string(5000,'z')}};
  TeamOptions opts;opts.max_handoff_chars=115;
  const auto result=team.collaborate({first,second},opts);
  assert(result.status==TeamStatus::Completed);
  assert(longest.load()<=opts.max_handoff_chars);
}
static void test_mcp_catalog_and_result_limits() {
  auto mock=std::make_shared<Mock>();
  mock->reply=[](const Json&){return answer("should not call model");};
  Config config=settings();
  config.max_mcp_tool_bytes=256;
  Agent limited(config,mock);
  limited.tool("huge_schema",std::string(2000,'d'),[](){return true;});
  try {
    (void)limited.ask("enumerate");
    assert(false);
  }catch(const std::runtime_error&){}
  assert(mock->calls==0);

  Config count_config=settings();
  count_config.max_mcp_tools=1;
  Agent count_limited(count_config,mock);
  count_limited.tool("first","first",[](){return true;});
  count_limited.tool("second","second",[](){return true;});
  try {
    (void)count_limited.ask("enumerate");
    assert(false);
  }catch(const std::runtime_error&){}
  assert(mock->calls==0);

  auto large=std::make_shared<Mock>();
  large->reply=[&](const Json& request) -> Json {
    if(large->calls==1)
      return j::object{{"status","completed"},{"output",j::array{
        j::object{{"type","function_call"},{"call_id","big1"},
          {"name","local__large"},{"arguments","{}"}}}}};
    const auto& items=request.at("input");
    const auto output=std::string(items[items.size()-1].at("output").as_string());
    assert(output.find("isError")!=std::string::npos);
    assert(output.size()<4096);
    return answer("bounded result");
  };
  Agent agent(settings(),large);
  agent.tool("large","large tool result",[](){return std::string(350000,'x');});
  assert(agent.ask("Call the large tool")=="bounded result");
  assert(large->calls==2);
}
static void test_explicit_agents_md(const fs::path& root) {
  auto model=std::make_shared<Mock>();
  std::string observed;
  model->reply=[&](const Json& request) {
    observed=j::serialize(request);
    return answer("ok");
  };
  Agent agent(settings(),model);
  const auto file=root/"AGENTS.md";
  {
    std::ofstream out(file);
    out<<"RULE_FROM_DISK";
  }
  agent.LoadAgentsMdFile(file);
  assert(agent.ask("test")=="ok");
  assert(observed.find("RULE_FROM_DISK")!=std::string::npos);
  agent.LoadAgentsMdFileFormString("RULE_FROM_TEXT");
  assert(agent.ask("test2")=="ok");
  assert(observed.find("RULE_FROM_TEXT")!=std::string::npos);
  assert(observed.find("RULE_FROM_DISK")==std::string::npos);
  agent.LoadAgentsMdFileFormString("");
  assert(agent.ask("test3")=="ok");
  assert(observed.find("RULE_FROM_TEXT")==std::string::npos);
  try {
    agent.LoadAgentsMdFileFormString(std::string(128*1024+1,'x'));
    assert(false);
  } catch(const std::invalid_argument&) {}
  try {
    agent.LoadAgentsMdFile(root/"missing.md");
    assert(false);
  } catch(const std::exception&) {}
  agent.LoadAgentsMdFileFormString("VALID_RULE");
  assert(agent.ask("test4")=="ok");
  assert(observed.find("VALID_RULE")!=std::string::npos);
}
int main() {
  const auto root=fs::temp_directory_path()/
      ("myagent-full-audit-"+std::to_string(std::chrono::steady_clock::now()
          .time_since_epoch().count()));
  fs::create_directories(root);
  try {
    test_token_usage_atomicity();
    test_chat_targets_and_finish();
    test_empty_completion();
    test_history_format(root);
    test_worker_cancellation_and_deadline();
    test_noncooperative_model_deadline();
    test_config_transaction_and_workspace(root);
    test_replay_guard();
    test_memory_save_and_metadata(root);
    test_mcp_catalog_and_result_limits();
    test_handoff_budget(root);
    test_explicit_agents_md(root);
  }catch(...){fs::remove_all(root);throw;}
  fs::remove_all(root);
  std::cout<<"Full audit regression tests OK\n";
}
