// P1/P2 集成测试：Checkpoint 安全恢复、声明式 YAML、长期记忆、Chat API 适配。
#include "myagent/multi_agent.hpp"
#include "httplib.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
namespace fs=std::filesystem;
namespace j=myagent::json;
using namespace myagent;
static Json response(const std::string& text) {
  return j::object{{"status","completed"},
    {"usage",j::object{{"input_tokens",8},{"output_tokens",12},{"total_tokens",20}}},
    {"output",j::array{j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",text}}}}}}}};
}
struct Mock:Model {
  std::atomic<unsigned> count{0};
  std::function<Json(const Json&)> handler;
  Json create(const Json& request) override {++count;return handler(request);}
};
static Config config() {
  Config c;c.auto_discover_skills=false;c.auto_skill_selection=false;
  return c;
}
static void test_yaml_config(const fs::path& root) {
  const auto dir=root/"skills"/"cpp-review";
  fs::create_directories(dir);
  {std::ofstream f(dir/"SKILL.md");
    f<<"---\nname: cpp-review\ndescription: C++ review skill\n---\n";
    f<<"Always check integer boundaries.\n";}
  const auto yaml=root/"team.yaml";
  {std::ofstream out(yaml);
    out<<"agents:\n"
       <<"  - name: reviewer\n"
       <<"    description: C++ 审查 Agent\n"
       <<"    config:\n"
       <<"      base_url: http://127.0.0.1:1\n"
       <<"      model: review-model\n"
       <<"      api_key_ref: review-secret\n"
       <<"      auto_discover_skills: false\n"
       <<"      auto_skill_selection: false\n"
       <<"    skills: [skills]\n"
       <<"    use_skills: [cpp-review]\n"
       <<"    mcp:\n"
       <<"      - prefix: files\n"
       <<"        transport: http\n"
       <<"        url: http://127.0.0.1:1/mcp\n"
       <<"        bearer_token_ref: files-secret\n";}
  AgentTeam team(config(),std::make_shared<Mock>());
  unsigned resolved=0;
  team.add_agents_from_file(yaml,[&](const std::string& key) {
    ++resolved;
    assert(key=="review-secret"||key=="files-secret");
    return "dummy-"+key;
  });
  assert(resolved==2);
  assert(team.agent("reviewer").skills().get("cpp-review").name=="cpp-review");
  // 不能把明文 API Key 放在配置文件里。
  const auto invalid=root/"invalid.yaml";
  {std::ofstream out(invalid);
   out<<"agents:\n  - name: bad\n    description: bad\n    config:\n"
      <<"      base_url: http://localhost\n      model: x\n"
      <<"      api_key: secret-in-plaintext\n      api_key_ref: secret\n";}
  try {
    team.add_agents_from_file(invalid,[](const std::string&){return "secret";});
    assert(false);
  } catch(const std::invalid_argument&) {}
}
static void test_checkpoint(const fs::path& root) {
  AgentTeam team(config(),std::make_shared<Mock>());
  auto mock=std::make_shared<Mock>();
  mock->handler=[](const Json&){return response("ok");};
  AgentSpec spec;spec.name="worker";spec.description="worker";spec.config=config();
  team.add_agent(spec,mock);
  std::vector<TeamTask> tasks={
    TeamTask{"one","worker","first"},
    TeamTask{"two","worker","second",{"one"}}
  };
  TeamOptions options;
  options.checkpoint_path=root/"checkpoint.json";
  options.validate_task=[](const TeamTask&,const DelegationResult& r) {
    return TaskValidation{r.status==LoopStatus::Completed,"verified",
      j::object{{"validated",true}}};
  };
  const auto first=team.collaborate(tasks,options);
  assert(first.status==TeamStatus::Completed&&mock->count==2);
  assert(fs::exists(options.checkpoint_path));
  assert(first.tasks[0].verification=="verified");
  options.resume_checkpoint=true;
  const auto resumed=team.collaborate(tasks,options);
  assert(resumed.status==TeamStatus::Completed);
  assert(mock->count==2); // 已验证并重新确认的任务，不得重新执行副作用。
  assert(resumed.model_calls==first.model_calls);
  tasks[0].description="changed";
  const auto mismatch=team.collaborate(tasks,options);
  assert(mismatch.status==TeamStatus::Failed);
  tasks[0].description="first";
  // 模拟中途崩溃：存在 in-flight 任务，不可直接重放。
  const auto checkpoint=options.checkpoint_path;
  std::ifstream in(checkpoint,std::ios::binary);
  const auto bytes=std::string(std::istreambuf_iterator<char>(in),{});
  auto saved=j::parse(bytes);
  saved["inflight"]=j::array{"one"};
  {std::ofstream out(checkpoint,std::ios::trunc);out<<j::serialize(saved);}
  const auto unsafe=team.collaborate(tasks,options);
  assert(unsafe.status==TeamStatus::Failed);
  assert(mock->count==2);
}
static void test_memory(const fs::path& root) {
  MemoryStore index;
  index.remember({"lang","Use modern C++20 RAII.",j::object{}});
  index.remember({"ui","Prefer responsive layouts.",j::object{}});
  assert(index.search("C++20 RAII").size()==1);
  assert(index.search("C++20 RAII")[0].id=="lang");
  index.save(root/"memory.json");
  MemoryStore loaded;loaded.load(root/"memory.json");
  assert(loaded.search("RAII").size()==1);
  assert(loaded.forget("lang"));
  assert(loaded.search("RAII").empty());

  auto mock=std::make_shared<Mock>();
  auto c=config();c.memory_retrieval=true;
  mock->handler=[](const Json& req) {
    assert(std::string(req.at("instructions").as_string()).find("C++20 RAII")!=std::string::npos);
    return response("memory used");
  };
  Agent agent(c,mock);
  agent.remember({"guide","C++20 RAII is mandatory.",j::object{}});
  assert(agent.ask("C++20 RAII guide")=="memory used");
  agent.save_memory(root/"agent_memory.json");
  assert(agent.recall("RAII").size()==1);
}
static void test_chat_adapter() {
  httplib::Server server;
  std::atomic<int> hit{0};
  server.Post("/v1/chat/completions",[&](const httplib::Request& req,httplib::Response& res) {
    assert(req.get_header_value("Authorization")=="Bearer example-chat-key");
    const auto body=j::parse(req.body);
    assert(body.at("model").as_string()=="chat-model");
    const int call=++hit;
    Json msg=j::object{};
    Json choice=j::object{};
    if(call==1) {
      assert(!body.at("tools").empty());
      Json fn=j::object{{"name","local__plus"},{"arguments",R"({"a":2,"b":3})"}};
      Json invocation=j::object{{"id","chat-call-1"},{"type","function"},{"function",fn}};
      msg["content"]=nullptr;
      msg["tool_calls"]=j::array{invocation};
      choice["finish_reason"]="tool_calls";
    } else {
      assert(body.at("messages")[body.at("messages").size()-1].at("role").as_string()=="tool");
      msg["content"]="5";
      choice["finish_reason"]="stop";
    }
    choice["message"]=msg;
    Json use=j::object{{"prompt_tokens",call==1?12:14},
                       {"completion_tokens",call==1?8:6},{"total_tokens",20}};
    res.set_content(j::serialize(j::object{{"choices",j::array{choice}},{"usage",use}}),
                    "application/json");
  });
  const auto port=server.bind_to_any_port("127.0.0.1");
  assert(port>0);
  std::thread worker([&]{server.listen_after_bind();});
  for(int n=0;n<100&&!server.is_running();++n)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(server.is_running());
  try {
    auto c=config();
    c.base_url="http://127.0.0.1:"+std::to_string(port);
    c.api_key="example-chat-key";
    c.model="chat-model";
    c.model_protocol=ModelProtocol::ChatCompletions;
    Agent agent(c);
    agent.tool("plus","add",[](int a,int b){return a+b;},
               mymcp::arg<int>("a"),mymcp::arg<int>("b"));
    assert(agent.ask("compute 2+3")=="5");
    assert(hit==2&&agent.last_usage().total_tokens==40);
  } catch(...) {
    server.stop();worker.join();throw;
  }
  server.stop();worker.join();
}
static void test_failover_retry_and_priority() {
  // 重分配仅在没有工具调用时允许，避免重复写文件、推送或发布。
  AgentTeam team(config(),std::make_shared<Mock>());
  auto failed=std::make_shared<Mock>();
  failed->handler=[](const Json&){
    auto r=response("failed worker");
    r["status"]="incomplete";
    return r;
  };
  AgentSpec spec;spec.name="primary";spec.description="original";spec.config=config();
  team.add_agent(spec,failed);
  auto fallback=std::make_shared<Mock>();
  fallback->handler=[](const Json&){return response("fallback completed");};
  spec.name="backup";spec.description="backup";team.add_agent(spec,fallback);
  TeamTask task{"x","primary","delegate this"};
  task.fallback_agents={"backup"};
  TeamOptions options;
  const auto reassigned=team.collaborate({task},options);
  assert(reassigned.status==TeamStatus::Completed);
  assert(reassigned.tasks[0].agent=="backup");
  assert(failed->count==1 && fallback->count==1);
  assert(reassigned.model_calls==2);

  auto retry_model=std::make_shared<Mock>();
  std::atomic<int> attempts{0};
  retry_model->handler=[&](const Json&) {
    if(++attempts==1){auto r=response("temporary");r["status"]="incomplete";return r;}
    return response("recovered without tools");
  };
  spec.name="retry";spec.description="retry";team.add_agent(spec,retry_model);
  TeamOptions retries;retries.max_task_retries=1;
  const auto retried=team.collaborate({TeamTask{"r","retry","try twice"}},retries);
  assert(retried.status==TeamStatus::Completed);
  assert(retried.tasks[0].attempts==2 && retried.tasks[0].model_calls==2);

  std::vector<std::string> order;
  auto low=std::make_shared<Mock>();
  low->handler=[&](const Json&){order.push_back("low");return response("low");};
  spec.name="low";spec.description="low";team.add_agent(spec,low);
  auto high=std::make_shared<Mock>();
  high->handler=[&](const Json&){order.push_back("high");return response("high");};
  spec.name="high";spec.description="high";team.add_agent(spec,high);
  TeamTask low_task{"lowid","low","low priority"};
  TeamTask high_task{"highid","high","high priority"};
  low_task.priority=1;high_task.priority=10;
  TeamOptions serial;serial.max_parallel_agents=1;
  const auto prioritized=team.collaborate({low_task,high_task},serial);
  assert(prioritized.status==TeamStatus::Completed);
  assert(order.size()==2 && order[0]=="high" && order[1]=="low");
}
static void test_workspace_atomic_write(const fs::path& root) {
  const auto workspace=root/"workspaces"/"writer";
  fs::create_directories(workspace);
  const auto file=workspace/"target.txt";
  {std::ofstream out(file);out<<"original";}
  AgentTeam team(config(),std::make_shared<Mock>());
  auto model=std::make_shared<Mock>();
  model->handler=[](const Json& req) {
    const auto* input=j::if_contains(req,"input");
    bool tool_done=false;
    if(input&&input->is_array())
      for(const auto& item:input->array_range()) {
        const auto* type=j::if_contains(item,"type");
        if(type&&type->is_string()&&type->as_string()=="function_call_output")
          tool_done=true;
      }
    if(tool_done)return response("write completed");
    return j::object{{"status","completed"},{"output",j::array{
      j::object{{"type","function_call"},{"call_id","write-1"},
        {"name","local__workspace_write"},
        {"arguments",R"({"relative":"target.txt","content":"replaced"})"}}}}};
  };
  AgentSpec spec;spec.name="writer";spec.description="safe workspace writer";
  spec.config=config();spec.workspace=workspace;spec.allow_workspace_write=true;
  team.add_agent(spec,model);
  const auto result=team.delegate("writer","replace an existing test file");
  assert(result.status==LoopStatus::Completed);
  std::ifstream in(file);std::string text;std::getline(in,text);
  assert(text=="replaced");
}
int main() {
  const auto root=fs::temp_directory_path()/"myagent-p1-p2-fixtures";
  fs::remove_all(root);
  fs::create_directories(root);
  test_yaml_config(root);
  test_checkpoint(root);
  test_memory(root);
  test_chat_adapter();
  test_failover_retry_and_priority();
  test_workspace_atomic_write(root);
  fs::remove_all(root);
  std::cout<<"P1/P2 integration tests OK\n";
}
