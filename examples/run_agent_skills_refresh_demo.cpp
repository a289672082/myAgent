// Skills 热更新 Demo：增加、修改、删除和失败回滚，完全离线且不执行脚本。
// 不修改用户工作目录：所有演示 Skill 只写入自动清理的临时目录。
#include "loop_demo_support.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace fs=std::filesystem;
using myagent::Json;
namespace j=myagent::json;

struct ScopedDirectory {
  fs::path path=fs::temp_directory_path()/
      ("myagent-skills-refresh-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
  ScopedDirectory(){fs::create_directories(path);}
  ~ScopedDirectory(){std::error_code ec;fs::remove_all(path,ec);}
};

void write_text(const fs::path& path,const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path,std::ios::binary|std::ios::trunc);
  if(!output)throw std::runtime_error("Cannot write demo SKILL.md");
  output<<content;
  if(!output)throw std::runtime_error("Failed writing demo Skill content");
}

bool contains(const std::string& text,const std::string& part) {
  return text.find(part)!=std::string::npos;
}
} // namespace

int run_agent_skills_refresh_demo() {
  try {
    ScopedDirectory temp;
    const auto root=temp.path/"skills";
    const auto review=root/"code-review"/"SKILL.md";
    const auto extra=root/"extra-tips"/"SKILL.md";

    // 阶段 1：初始 Skill 仅加载元数据；完整内容按 agent_skill_load 获取。
    write_text(review,
      "---\nname: code-review\ndescription: First review description\n---\n"
      "REVIEW_V1: 检查空指针和数组越界。\n");

    auto cfg=loop_demo::config();
    cfg.auto_discover_skills=false; // 演示目录与用户的全局 Skills 隔离。
    cfg.auto_skill_selection=true;  // 让模型通过内置工具按需读取 Skill。
    auto model=std::make_shared<loop_demo::MockModel>();
    model->handle=[&](const Json& request,unsigned)->Json {
      const auto* defs=j::if_contains(request,"tools");
      bool has_skill_loader=false;
      if(defs && defs->is_array())
        for(const auto& def:defs->array_range())
          if(def.at("name").as_string()=="agent_skill_load")
            has_skill_loader=true;

      if(!has_skill_loader)
        return loop_demo::answer("没有可自动调用的 Skill");

      // 工具返回必须来自运行时实际加载的文件，而不是模型自行编造。
      for(const auto& entry:request.at("input").array_range()) {
        const auto* type=j::if_contains(entry,"type");
        if(!type || !type->is_string() ||
           type->as_string()!="function_call_output")continue;
        const std::string output(entry.at("output").as_string());
        if(contains(output,"REVIEW_V2"))
          return loop_demo::answer("已按第二版规则检查");
        if(contains(output,"REVIEW_V1"))
          return loop_demo::answer("已按第一版规则检查");
        throw std::runtime_error("Skill 工具未返回预期的真实内容");
      }
      return loop_demo::tool_call("agent_skill_load",R"({"name":"code-review"})");
    };

    myagent::Agent agent(cfg,model);
    agent.load_skills(root);
    loop_demo::check(agent.skills().all().size()==1,"初始 Skill 未发现");
    loop_demo::check(agent.skills().get("code-review").content.empty(),
                     "发现时不应急切加载完整 SKILL.md");
    loop_demo::check(agent.ask("按 Skill 检查代码")=="已按第一版规则检查",
                     "第一次按需加载失败");

    // 阶段 2：更新 SKILL.md 后主动刷新；旧对话清空，避免旧工具输出干扰。
    write_text(review,
      "---\nname: code-review\ndescription: Updated review description\n---\n"
      "REVIEW_V2: 检查资源所有权、异常安全和取消语义。\n");
    agent.refresh_skills();
    loop_demo::check(agent.skills().get("code-review").description==
                     "Updated review description","修改后元数据未刷新");
    agent.clear_history();
    loop_demo::check(agent.ask("使用最新 Skill 再检查一次")=="已按第二版规则检查",
                     "刷新后未读取第二版指令");

    // 阶段 3：运行时添加另一个 Skill，不重新构造 Agent。
    write_text(extra,
      "---\nname: extra-tips\ndescription: Additional checks\n---\n"
      "新增静态检查建议。\n");
    agent.refresh_skills();
    loop_demo::check(agent.skills().all().size()==2,"新 Skill 未被动态发现");

    // 阶段 4：无效 YAML 会让整个刷新失败，但旧的有效目录仍可使用。
    write_text(extra,"---\nname: extra-tips\n---\n缺少 description。\n");
    bool rejected=false;
    try {agent.refresh_skills();}
    catch(const std::runtime_error&){rejected=true;}
    loop_demo::check(rejected,"无效 Skill 必须被拒绝");
    loop_demo::check(agent.skills().all().size()==2 &&
                     agent.skills().get("code-review").description==
                       "Updated review description",
                     "失败刷新不得破坏原有 Skill 目录");

    // 阶段 5：删除两个 Skill 后刷新，模型不应继续看到旧工具。
    fs::remove_all(root/"extra-tips");
    fs::remove_all(root/"code-review");
    agent.refresh_skills();
    loop_demo::check(agent.skills().all().empty(),"删除的 Skill 仍在目录中");
    agent.clear_history();
    loop_demo::check(agent.ask("现在还能自动加载 Skill 吗？")==
                     "没有可自动调用的 Skill","删除后仍暴露旧 Skill 工具");

    std::cout<<"run_agent_skills_refresh_demo: 初版加载、更新、新增、"
                "无效 YAML 回滚和删除全部通过。\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_skills_refresh_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main() {return run_agent_skills_refresh_demo();}
