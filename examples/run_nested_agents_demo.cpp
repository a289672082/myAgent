// P1-11：多层子智能体白名单与共享调用预算（完全离线）。
#include "advanced_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <iostream>
using namespace myagent;
using namespace advanced_demo;
int run_nested_agents_demo() {
 try {
  auto coordinator=std::make_shared<Mock>();
  coordinator->handler=[](const Json&){return answer("coordinator idle");};
  AgentTeam team(config(),coordinator);
  auto root_model=std::make_shared<Mock>();
  auto child_model=std::make_shared<Mock>();
  auto leaf_model=std::make_shared<Mock>();
  root_model->handler=[](const Json& request) {
    if(has_tool_result(request))return answer("root received child result");
    return tool_call("local__agent_delegate","root-to-child",
                     R"({"child":"child","task":"delegate to leaf"})");
  };
  child_model->handler=[](const Json& request) {
    if(has_tool_result(request))return answer("child received leaf result");
    return tool_call("local__agent_delegate","child-to-leaf",
                     R"({"child":"leaf","task":"report from leaf"})");
  };
  leaf_model->handler=[](const Json&) {
    return answer("leaf completed");
  };
  AgentSpec root;root.name="root";root.description="orchestrator";
  root.config=config();root.allowed_children={"child"};
  AgentSpec child;child.name="child";child.description="secondary worker";
  child.config=config();child.allowed_children={"leaf"};
  AgentSpec leaf;leaf.name="leaf";leaf.description="terminal specialist";
  leaf.config=config();
  team.add_agent(root,root_model);
  team.add_agent(child,child_model);
  team.add_agent(leaf,leaf_model);
  const auto result=team.delegate_tree("root","start nested delegation",3,8,4);
  ensure(result.status==LoopStatus::Completed,"嵌套委派失败："+result.error);
  ensure(root_model->calls==2 && child_model->calls==2 && leaf_model->calls==1,
         "3 层委派没有执行预期路径");
  ensure(result.model_calls==5,"嵌套模型消耗没有汇总");
  // 缩小层数后：leaf 不得再次被调用。模型可能观察到工具拒绝并作出最终答复。
  const auto before=leaf_model->calls;
  auto limited=team.delegate_tree("root","depth-limited task",2,8,4);
  ensure(leaf_model->calls==before,"层级上限没有阻止三级委派");
  ensure(limited.model_calls<=8 && limited.tool_calls<=4,
         "嵌套任务预算超出上限");
  std::cout<<"run_nested_agents_demo: 白名单 / 3 层委派 / 全局预算通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_nested_agents_demo();}
