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
    if(call==1) {
      assert(!body.at("tools").empty());
      res.set_content(j::serialize(j::object{
        {"choices",j::array{j::object{
          {"finish_reason","tool_calls"},{"message",j::object{
            {"content",nullptr},{"tool_calls",j::array{j::object{
              {"id","chat-call-1"},{"type","function"},{"function",j::object{
                {"name","local__plus"},{"arguments",R"({"a":2,"b":3})"}}}}}}}}}},
        {"usage",j::object{{"prompt_tokens",12},{"completion_tokens",8},{"total_tokens",20}}}
      }),"application/json");
    } else {
      assert(body.at("messages")[body.at("messages").size()-1].at("role").as_string()=="tool");
      res.set_content(j::serialize(j::object{
        {"choices",j::array{j::object{{"finish_reason","stop"},
          {"message",j::object{{"content","5"}}}}}},
        {"usage",j::object{{"prompt_tokens",14},{"completion_tokens",6},{"total_tokens",20}}}
      }),"application/json");
    }
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
int main() {
  const auto root=fs::temp_directory_path()/"myagent-p1-p2-fixtures";
  fs::create_directories(root);
  test_yaml_config(root);
  test_checkpoint(root);
  test_memory(root);
  test_chat_adapter();
  fs::remove_all(root);
  std::cout<<"P1/P2 integration tests OK\n";
}
