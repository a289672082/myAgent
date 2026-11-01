// P0/P1/P2 多 Agent 回归：独立配置、委派、依赖、并行、预算、工作目录约束。
#include "myagent/multi_agent.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
using namespace myagent;
namespace j=myagent::json;
static Json reply(const std::string& text) {
  return j::object{{"status","completed"},
    {"usage",j::object{{"input_tokens",12},{"output_tokens",8},{"total_tokens",20}}},
    {"output",j::array{j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",text}}}}}}}};
}
static Json tool(const std::string& name, const std::string& arguments) {
  return j::object{{"status","completed"},{"output",j::array{j::object{
    {"type","function_call"},{"name",name},{"call_id","c1"},{"arguments",arguments}}}}};
}
struct Mock:Model {
  unsigned n=0;
  std::function<Json(const Json&,unsigned)> handle;
  Json create(const Json& req) override{return handle(req,++n);}
};
static Config config(const std::string& tag) {
  Config c;c.model=tag;c.api_key="example-"+tag;c.base_url="https://"+tag+".example.com";
  c.auto_discover_skills=false;c.auto_skill_selection=false;
  return c;
}
static void test_credential_context_and_delegation() {
  auto leader=std::make_shared<Mock>();
  leader->handle=[](const Json&,unsigned) {return reply("leader");};
  AgentTeam team(config("coordinator"),leader);
  auto a=std::make_shared<Mock>(),b=std::make_shared<Mock>();
  a->handle=[](const Json& req,unsigned) {
    assert(req.at("model").as_string()=="alpha");return reply("alpha done");
  };
  b->handle=[](const Json& req,unsigned) {
    assert(req.at("model").as_string()=="beta");return reply("beta done");
  };
  AgentSpec x;x.name="alpha";x.description="C++ coding";x.config=config("alpha");
  team.add_agent(x,a);
  x.name="beta";x.description="Review";x.config=config("beta");
  team.add_agent(x,b);
  assert(team.delegate("alpha","write code").status==LoopStatus::Completed);
  assert(team.delegate("beta","review").status==LoopStatus::Completed);
  assert(team.agent("alpha").history().size()==2);
  assert(team.agent("beta").history().size()==2);
  assert(team.agent("alpha").total_usage().total_tokens==20);
  assert(team.agent("beta").total_usage().total_tokens==20);
  try {team.add_agent(x,b);assert(false);}catch(const std::invalid_argument&){}
}
static void test_automatic_coordination_and_deps() {
  auto leader=std::make_shared<Mock>();
  leader->handle=[](const Json& req,unsigned n) {
    const auto request=j::serialize(req);
    assert(request.find("example-alpha")==std::string::npos); // 不泄露密钥
    if(n==1)return reply(R"JSON({"tasks":[
      {"id":"code","agent":"coder","description":"生成代码","depends_on":[],"mode":"react"},
      {"id":"review","agent":"reviewer","description":"审查代码","depends_on":["code"],"mode":"react"}]})JSON");
    return reply("协作完成");
  };
  AgentTeam team(config("boss"),leader);
  AgentSpec coder;coder.name="coder";coder.description="C++ coding";coder.config=config("alpha");
  auto c=std::make_shared<Mock>();
  c->handle=[](const Json&,unsigned){return reply("代码已生成");};
  team.add_agent(coder,c);
  coder.name="reviewer";coder.description="Review";coder.config=config("beta");
  auto r=std::make_shared<Mock>();
  r->handle=[](const Json& req,unsigned) {
    assert(j::serialize(req).find("代码已生成")!=std::string::npos);
    return reply("审查完成");
  };
  team.add_agent(coder,r);
  TeamOptions verify;
  verify.validate_task=[](const TeamTask& task,const DelegationResult& answer) {
    TaskValidation v;
    v.passed=!answer.answer.empty();
    v.reason="Empty sub Agent output";
    v.evidence=j::object{{"task",task.id},{"checked",true}};
    return v;
  };
  const auto result=team.run("完成 C++ 代码及代码审查",verify);
  assert(result.status==TeamStatus::Completed);
  assert(result.tasks.size()==2);
  assert(result.tasks[0].verification=="verified");
  assert(result.tasks[1].verification_evidence.at("checked").as<bool>());
  assert(result.usage_by_agent.at("coordinator").total_tokens==40);
  assert(result.usage_by_agent.at("coder").total_tokens==20);
  assert(result.usage_by_agent.at("reviewer").total_tokens==20);
  assert(result.model_calls==4);
}
static void test_parallel_and_dependency_budget() {
  AgentTeam team(config("boss"),std::make_shared<Mock>());
  std::atomic<int> running{0},peak{0};
  for(int k=0;k<3;++k) {
    AgentSpec worker;worker.name="worker"+std::to_string(k);
    worker.description="parallel worker";worker.config=config(worker.name);
    auto mock=std::make_shared<Mock>();
    mock->handle=[&](const Json&,unsigned){
      int n=++running;
      int old=peak.load();
      while(n>old && !peak.compare_exchange_weak(old,n)) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      --running;return reply("ok");
    };
    team.add_agent(worker,mock);
  }
  std::vector<TeamTask> graph={
    TeamTask{"t1","worker0","independent A",{},LoopMode::ReAct},
    TeamTask{"t2","worker1","independent B",{},LoopMode::ReAct},
    TeamTask{"t3","worker2","dependent C",{"t1","t2"},LoopMode::ReAct}
  };
  TeamOptions opts;opts.max_parallel_agents=2;
  auto result=team.collaborate(graph,opts);
  assert(result.status==TeamStatus::Completed);
  TeamOptions no_tools;
  no_tools.max_tool_calls=0; // 0 forbids tools, but pure model tasks still run.
  const auto model_only=team.collaborate(graph,no_tools);
  assert(model_only.status==TeamStatus::Completed && model_only.tool_calls==0);
  assert(result.model_calls==3&&result.usage.total_tokens==60);
  assert(peak.load()==2);
  TeamOptions budget;budget.max_parallel_agents=2;budget.max_model_calls=1;
  const auto limited=team.collaborate(graph,budget);
  assert(limited.status==TeamStatus::LimitReached);
  assert(limited.model_calls<=1);
  // 宿主验证失败时，不再把子 Agent 的口头承诺当成成功。
  // 后续有依赖的步骤应直接跳过，避免在不合格的代码上执行提交。
  TeamOptions strict;
  strict.validate_task=[](const TeamTask& t,const DelegationResult&) {
    TaskValidation v;
    v.passed=t.id!="t1";
    v.reason="external tests failed";
    return v;
  };
  const auto rejected=team.collaborate(graph,strict);
  assert(rejected.status==TeamStatus::Partial);
  assert(rejected.tasks[0].verification=="failed");
  assert(rejected.tasks[2].error.find("Skipped")!=std::string::npos);
  std::vector<TeamTask> invalid={TeamTask{"b","worker0","wrong dependency",{"a"}}};
  auto bad=team.collaborate(invalid);
  assert(bad.status==TeamStatus::Failed);
}
static void test_workspace_boundaries() {
  namespace fs=std::filesystem;
  const auto root=fs::temp_directory_path()/"myagent-team-test-workspaces";
  fs::create_directories(root/"one");
  fs::create_directories(root/"two");
  {std::ofstream f(root/"one"/"data.txt");f<<"inside"; }
  {std::ofstream f(root/"two"/"secret.txt");f<<"outside"; }
  auto model=std::make_shared<Mock>();
  model->handle=[](const Json&,unsigned n)->Json {
    if(n==1)return tool("local__workspace_read",R"({"relative":"data.txt"})");
    if(n==2)return reply("文件读取完成");
    if(n==3)return tool("local__workspace_read",R"({"relative":"../two/secret.txt"})");
    if(n==4)return reply("路径越界被拒绝");
    return reply("ok");
  };
  AgentTeam team(config("boss"));
  AgentSpec spec;spec.name="one";spec.description="read-only";spec.config=config("first");
  spec.workspace=root/"one";
  team.add_agent(spec,model);
  spec.name="two";spec.workspace=root/"two";
  spec.config=config("second");
  auto other=std::make_shared<Mock>();
  other->handle=[](const Json&,unsigned){return reply("other");};
  team.add_agent(spec,other);
  auto a=team.delegate("one","读取 data.txt");
  assert(a.status==LoopStatus::Completed&&a.tool_calls==1);
  auto b=team.delegate("one","读取 ../two/secret.txt");
  assert(b.status==LoopStatus::Completed&&b.tool_calls==1);
  // 框架把被拒绝的错误放在 tool_result 中，不让文件读取成功。
  const auto history=j::serialize(team.agent("one").history());
  // 拒绝方式由 MCP Server 决定；只强制验证越界文件的实际内容没有泄露。
  assert(history.find("outside")==std::string::npos);
  assert(fs::exists(root/"one"/"data.txt"));
  fs::remove_all(root);
}
static void test_team_partial_ledger_and_coordinator_limits() {
  auto coordinator=std::make_shared<Mock>();
  coordinator->handle=[](const Json&,unsigned) {
    return reply(R"({"tasks":[{"id":"first","agent":"worker","description":"one"},{"id":"second","agent":"worker","description":"two"}]})");
  };
  auto worker=std::make_shared<Mock>();
  worker->handle=[](const Json&,unsigned){return reply("completed");};
  AgentTeam team(config("coordinator"),coordinator);
  AgentSpec spec;spec.name="worker";spec.description="work";spec.config=config("worker");
  team.add_agent(spec,worker);

  TeamOptions capped;
  capped.max_delegations=1;
  const auto rejected=team.run("two tasks",capped);
  assert(rejected.status==TeamStatus::Failed);
  assert(worker->n==0);

  TeamOptions model_budget;
  model_budget.max_model_calls=1;
  const auto limited=team.run("two tasks",model_budget);
  assert(limited.status==TeamStatus::LimitReached);
  assert(limited.model_calls==1);

  std::vector<TeamTask> graph={
    TeamTask{"first","worker","one"},
    TeamTask{"second","worker","two",{"first"}}
  };
  TeamOptions interrupted;
  interrupted.on_event=[](const Json& event){
    if(event.at("event").as_string()=="task_started" &&
       event.at("task").as_string()=="second")
      throw std::runtime_error("host event handler failed");
  };
  const auto partial=team.collaborate(graph,interrupted);
  assert(partial.status==TeamStatus::Failed);
  assert(partial.tasks.size()==1 && partial.tasks.front().id=="first");
  assert(partial.model_calls==1 && partial.usage.total_tokens==20);
}
int main(){
  test_credential_context_and_delegation();
  test_automatic_coordination_and_deps();
  test_parallel_and_dependency_budget();
  test_team_partial_ledger_and_coordinator_limits();
  test_workspace_boundaries();
  std::cout<<"AgentTeam offline tests OK\n";
}
