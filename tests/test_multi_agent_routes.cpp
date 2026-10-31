// 双 HTTP Provider 离线集成测试：每个子 Agent 的 base_url、API Key、模型名独立生效。
// 与 Mock Model 不同：该测试会经过真正的 OpenAIResponses HTTP 客户端。
#include "myagent/multi_agent.hpp"
#include "httplib.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace myagent;
namespace j=myagent::json;
struct Provider {
  httplib::Server server;
  std::thread thread;
  int port=0;
  std::string expected_key,expected_model,expected_path,response_name;
  std::atomic<int> hits{0};
  Provider(std::string key,std::string model,std::string path,std::string name)
    :expected_key(std::move(key)),expected_model(std::move(model)),
     expected_path(std::move(path)),response_name(std::move(name)) {
    server.Post(R"(/.*)",[this](const httplib::Request& req,httplib::Response& res) {
      if(req.path!=expected_path ||
         req.get_header_value("Authorization")!="Bearer "+expected_key) {
        res.status=403;res.set_content("Wrong provider or credentials","text/plain");return;
      }
      const auto payload=j::parse(req.body);
      if(payload.at("model").as_string()!=expected_model) {
        res.status=400;res.set_content("Wrong model","text/plain");return;
      }
      ++hits;
      res.set_content(j::serialize(j::object{
        {"status","completed"},
        {"usage",j::object{{"input_tokens",11},{"output_tokens",9},{"total_tokens",20}}},
        {"output",j::array{j::object{{"type","message"},{"role","assistant"},
          {"content",j::array{j::object{{"type","output_text"},{"text",response_name}}}}}}}
      }),"application/json");
    });
    port=server.bind_to_any_port("127.0.0.1");
    assert(port>0);
    thread=std::thread([this]{server.listen_after_bind();});
    for(int i=0;i<200&&!server.is_running();++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(server.is_running());
  }
  ~Provider(){server.stop();if(thread.joinable())thread.join();}
  std::string origin()const{return "http://127.0.0.1:"+std::to_string(port);}
};
int main() {
  Provider a("only-a-key","alpha-model","/v1/responses","provider A");
  Provider b("only-b-key","beta-model","/custom/responses","provider B");
  Config root;root.auto_discover_skills=false;root.auto_skill_selection=false;
  root.base_url=a.origin();root.api_key="unused-coordinator-key";root.model="coordinator-model";
  // 仅测试显式分工，不发起 Coordinator 模型请求。
  AgentTeam team(root,std::shared_ptr<Model>{});
  AgentSpec left;left.name="alpha";left.description="Coder";
  left.config=root;left.config.base_url=a.origin();left.config.api_key="only-a-key";
  left.config.model="alpha-model";left.config.append_responses_path=true;
  team.add_agent(left);
  AgentSpec right;right.name="beta";right.description="Reviewer";
  right.config=root;right.config.base_url=b.origin()+"/custom/responses";
  right.config.api_key="only-b-key";right.config.model="beta-model";
  right.config.append_responses_path=false;
  team.add_agent(right);
  TeamOptions options;options.max_parallel_agents=2;
  const auto result=team.collaborate({
    TeamTask{"implement","alpha","实现"},
    TeamTask{"review","beta","独立审查"}
  },options);
  assert(result.status==TeamStatus::Completed);
  assert(a.hits==1&&b.hits==1);
  assert(result.tasks.size()==2);
  assert(result.tasks[0].answer=="provider A");
  assert(result.tasks[1].answer=="provider B");
  assert(result.usage_by_agent.at("alpha").total_tokens==20);
  assert(result.usage_by_agent.at("beta").total_tokens==20);
  std::cout<<"Independent provider base URL/API Key routing OK\n";
}
