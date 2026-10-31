// P0 安全示例：工具最小权限、危险操作审批与 Coordinator 无工具模式。
// 离线 Mock Model，不连接网络、不写入真实项目。
#include "loop_demo_support.hpp"
#include "myagent/agent.hpp"
#include <iostream>
#include <memory>

int run_runtime_hardening_demo() {
  try {
    auto cfg=loop_demo::config();
    cfg.agent_id="coder";
    cfg.tool_policy.default_access=myagent::ToolAccess::Deny;
    cfg.tool_policy.allow("local__read");
    cfg.tool_policy.require_approval("local__write");
    bool approved=false;
    cfg.tool_policy.approve=[&](const myagent::ToolRequest& request) {
      // 宿主审批器应弹确认窗口或校验用户策略；示例拒绝写文件。
      loop_demo::check(request.agent=="coder","必须有可信 Agent ID");
      loop_demo::check(request.tool=="local__write","敏感工具必须明确审批");
      approved=true;return false;
    };
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[](const myagent::Json& request,unsigned round) {
      const auto& tools=request.at("tools");
      for(const auto& item:tools.array_range())
        loop_demo::check(item.at("name").as_string()!="local__hidden",
                         "禁止的工具不应出现在模型工具列表");
      if(round==1)return loop_demo::tool_call("local__write",R"({"text":"danger"})");
      loop_demo::check(myagent::json::serialize(request).find("Tool permission denied")!=std::string::npos,
                       "拒绝审批后应该返回明确的工具错误");
      return loop_demo::answer("写入被宿主拒绝，未修改文件。");
    };
    myagent::Agent agent(cfg,model);
    agent.tool("read","只读工具",[](){return "OK";});
    agent.tool("write","敏感写入工具",[](std::string){return true;},
               mymcp::arg<std::string>("text"));
    agent.tool("hidden","不允许模型发现的工具",[](){return true;});
    const auto result=agent.ask("尝试写入并说明权限结果");
    loop_demo::check(approved,"应触发一次宿主审批");
    std::cout<<result<<"\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_runtime_hardening_demo: "<<e.what()<<'\n';return 1;
  }
}
int main(){return run_runtime_hardening_demo();}
