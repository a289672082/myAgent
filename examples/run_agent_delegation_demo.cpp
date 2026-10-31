// P0：子 Agent 独立 baseUrl / apiKey / model；由父级显式选择目标进行任务委派。
// Mock 只验证配置模型被正确使用，不发起任何付费或网络请求。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <iostream>
#include <memory>

int run_agent_delegation_demo() {
  try {
    auto leader=loop_demo::config();
    leader.base_url="https://main.example.com";leader.api_key="leader-example-key";
    leader.model="coordinator-model";
    myagent::AgentTeam team(leader,std::make_shared<loop_demo::MockModel>());
    myagent::AgentSpec coder;
    coder.name="coder";coder.description="实现 C++ 源代码";
    coder.config=loop_demo::config();
    coder.config.base_url="https://provider-a.example.com/v1/responses";
    coder.config.append_responses_path=false;
    coder.config.api_key="coder-example-key";coder.config.model="coder-model";
    auto coder_model=std::make_shared<loop_demo::MockModel>();
    coder_model->handle=[](const myagent::Json& request,unsigned) {
      loop_demo::check(request.at("model").as_string()=="coder-model","coder model");
      return loop_demo::answer("已生成 C++ 示例代码");
    };
    team.add_agent(coder,coder_model);
    auto reviewer=coder;
    reviewer.name="reviewer";reviewer.description="检查代码是否符合现代 C++";
    reviewer.config.base_url="https://provider-b.example.com";
    reviewer.config.api_key="reviewer-example-key";
    reviewer.config.model="reviewer-model";
    auto reviewer_model=std::make_shared<loop_demo::MockModel>();
    reviewer_model->handle=[](const myagent::Json& request,unsigned) {
      loop_demo::check(request.at("model").as_string()=="reviewer-model","reviewer model");
      return loop_demo::answer("检查完成：使用 RAII 和边界检查");
    };
    team.add_agent(reviewer,reviewer_model);
    const auto code=team.delegate("coder","编写安全的 C++ 代码");
    const auto review=team.delegate("reviewer","审查刚才的代码");
    loop_demo::check(code.status==myagent::LoopStatus::Completed,"coder finished");
    loop_demo::check(review.status==myagent::LoopStatus::Completed,"reviewer finished");
    loop_demo::check(team.agent("coder").history().size()==2,"coder independent history");
    loop_demo::check(team.agent("reviewer").history().size()==2,"reviewer independent history");
    std::cout<<code.answer<<"\n"<<review.answer<<"\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_delegation_demo: "<<e.what()<<'\n';return 1;
  }
}
int main(){return run_agent_delegation_demo();}
