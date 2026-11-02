// AGENTS.md 显式加载专项示例：不需要 API Key、网络、环境变量或命令行参数。
// 演示文件读取、字符串替换、清空以及原始系统指令的保留。
#include "myagent/agent.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;
using myagent::Json;
namespace json=myagent::json;

namespace {
void ensure(bool condition,const std::string& message) {
  if(!condition)throw std::runtime_error("AGENTS.md Demo: "+message);
}

// 离线模型仅检查 Agent 实际发送的 instructions，而不调用真实 API。
class AgentsMdDemoModel final:public myagent::Model {
public:
  Json create(const Json& request) override {
    const std::string instructions(request.at("instructions").as_string());
    ensure(instructions.find("BASE_SYSTEM_INSTRUCTION")!=std::string::npos,
           "原始 Config::instructions 丢失");
    if(stage_==0) {
      ensure(instructions.find("RULE_FROM_AGENTS_MD_FILE")!=std::string::npos,
             "未从 AGENTS.md 文件注入规则");
      ensure(instructions.find("RULE_FROM_STRING")==std::string::npos,
             "文件阶段错误包含字符串规则");
    } else if(stage_==1) {
      ensure(instructions.find("RULE_FROM_STRING")!=std::string::npos,
             "字符串规则未注入");
      ensure(instructions.find("RULE_FROM_AGENTS_MD_FILE")==std::string::npos,
             "新规则没有替换旧文件规则");
    } else if(stage_==2) {
      ensure(instructions.find("RULE_FROM_STRING")==std::string::npos &&
             instructions.find("RULE_FROM_AGENTS_MD_FILE")==std::string::npos,
             "空字符串未清空 AGENTS.md 规则");
    } else {
      throw std::runtime_error("模拟模型收到额外的请求");
    }
    ++stage_;
    return json::object{
      {"status","completed"},
      {"output",json::array{json::object{
        {"type","message"},{"role","assistant"},
        {"content",json::array{json::object{
          {"type","output_text"},{"text","AGENTS.md 规则已正确应用"}
        }}}
      }}}
    };
  }
  int stages()const{return stage_;}
private:
  int stage_=0;
};
} // namespace

int run_agents_md_demo() {
  try {
    // CMake 注入源码中的样例资源路径，运行目录可任意更换。
    const fs::path rules_file=fs::path(MYAGENT_AGENTS_MD_DEMO_DIR)/"AGENTS.md";
    myagent::Config config;
    config.instructions="BASE_SYSTEM_INSTRUCTION";
    config.auto_discover_skills=false;
    config.auto_skill_selection=false;
    auto model=std::make_shared<AgentsMdDemoModel>();
    myagent::Agent agent(config,model);

    // 第一种：从实际 AGENTS.md 文件加载项目规则。
    agent.LoadAgentsMdFile(rules_file);
    std::cout<<"[文件加载] "<<agent.ask("请确认项目规则")<<"\n";

    // 第二种：直接从字符串加载；会替换文件内容，不会叠加旧规则。
    agent.LoadAgentsMdFileFormString(
      "# 字符串规则\nRULE_FROM_STRING\n- 公共接口添加中文注释");
    std::cout<<"[字符串替换] "<<agent.ask("请确认新规则")<<"\n";

    // 空内容清空已经加载的 AGENTS.md，但保留基础系统指令。
    agent.LoadAgentsMdFileFormString("");
    std::cout<<"[规则清空] "<<agent.ask("请确认规则已清空")<<"\n";
    ensure(model->stages()==3,"预期执行三次离线模型检查");
    std::cout<<"run_agents_md_demo: 全部检查通过。\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"run_agents_md_demo 失败: "<<e.what()<<"\n";
    return 1;
  }
}
int main(){return run_agents_md_demo();}
