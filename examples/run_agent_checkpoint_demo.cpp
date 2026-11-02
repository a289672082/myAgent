// 普通 Agent Loop 可恢复执行 Demo：完全离线、无 API Key、无命令行参数。
// 演示：Checkpoint -> 人工审批暂停 -> 恢复 -> 工具仅执行一次；
// 以及“工具开始但执行结果未知”的崩溃故障注入和人工核实。
#include "myagent/agent.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;
namespace j=myagent::json;
using myagent::Json;

namespace {
void ensure(bool passed,const std::string& message) {
  if(!passed)throw std::runtime_error("Checkpoint Demo: "+message);
}
// 模拟 Responses 模型的最终文字答复，整个示例不访问任何远端服务。
Json answer(const std::string& text) {
  return j::object{{"status","completed"},{"output",j::array{
    j::object{{"type","message"},{"role","assistant"},
      {"content",j::array{j::object{{"type","output_text"},{"text",text}}}}}
  }}};
}
class OfflineModel final:public myagent::Model {
public:
  std::atomic<unsigned> requests{0};
  Json create(const Json& request) override {
    ++requests;
    // 第一次模型会请求调用工具；拿到真实/核实的工具结果后才给出最终回复。
    const auto& input=request.at("input");
    for(const auto& item:input.array_range()) {
      if(!item.is_object())continue;
      const auto* type=j::if_contains(item,"type");
      if(type&&type->is_string()&&type->as_string()=="function_call_output")
        return answer("已经根据工具结果完成任务。");
    }
    return j::object{{"status","completed"},{"output",j::array{
      j::object{{"type","function_call"},{"call_id","demo-call-1"},
        {"name","local__write_example"},{"arguments","{}"}}
    }}};
  }
};
// 临时目录采用 RAII 清理：演示不会把含有工具参数的敏感 Checkpoint 留在磁盘。
struct ScopedDemoDirectory {
  fs::path root=fs::temp_directory_path()/
      ("myagent-checkpoint-demo-"+std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  ScopedDemoDirectory(){fs::create_directories(root);}
  ~ScopedDemoDirectory(){std::error_code ec;fs::remove_all(root,ec);}
};
myagent::Config settings() {
  myagent::Config config;
  config.model="offline-checkpoint-model";
  config.instructions="You are an offline demonstration assistant.";
  config.auto_discover_skills=false; // 不读取用户机器上的私有 Skills。
  config.auto_skill_selection=false;
  return config;
}
void register_example_tool(myagent::Agent& agent,int& side_effects) {
  agent.tool("write_example","仅增加示例内计数器，不写用户文件",
    [&side_effects]() {
      ++side_effects;
      std::cout<<"[工具] 模拟有副作用的操作，执行次数："<<side_effects<<"\n";
      return std::string("tool-result-confirmed");
    });
}
void demonstrate_approval(const fs::path& file) {
  std::cout<<"\n=== 场景一：暂停审批 → 恢复执行 ===\n";
  auto model=std::make_shared<OfflineModel>();
  int side_effects=0;
  myagent::Agent agent(settings(),model);
  register_example_tool(agent,side_effects);
  myagent::LoopOptions options;
  options.checkpoint_path=file;
  options.on_tool_approval=[](const myagent::ToolApprovalRequest& request) {
    std::cout<<"[审批] 工具 "<<request.name<<" 请求授权，先暂停任务。\n";
    return myagent::ToolDecision::Pause;
  };
  const auto paused=agent.react("请使用工具完成一次模拟写入",options);
  ensure(paused.status==myagent::LoopStatus::Paused,"没有进入暂停状态");
  ensure(side_effects==0,"审批前执行了工具");
  ensure(fs::exists(file),"暂停后未保存 Checkpoint");
  ensure(model->requests.load()==1,"首轮模型调用次数错误");
  std::cout<<"[Checkpoint] 已保存，任务未执行危险操作。\n";

  // GUI 收到人工确认后再次调用 resume；真实产品应根据用户操作返回 Approve。
  myagent::LoopOptions approved;
  approved.on_tool_approval=[](const myagent::ToolApprovalRequest& request) {
    std::cout<<"[审批] 用户已确认："<<request.name<<"\n";
    return myagent::ToolDecision::Approve;
  };
  const auto resumed=agent.resume(file,approved);
  ensure(resumed.status==myagent::LoopStatus::Completed,
         "恢复执行失败："+resumed.error);
  ensure(side_effects==1,"有副作用的工具执行次数不是 1");
  ensure(model->requests.load()==2,"恢复时不应再次请求模型生成相同工具调用");
  std::cout<<"[恢复] "<<resumed.answer<<"\n";
  ensure(agent.resume(file,approved).status==myagent::LoopStatus::Failed,
         "已完成的 Checkpoint 不应再次执行");
  ensure(side_effects==1,"完成后重复执行了工具");
}
void demonstrate_uncertain_outcome(const fs::path& file) {
  std::cout<<"\n=== 场景二：崩溃后工具结果未知 → 宿主核实 ===\n";
  auto model=std::make_shared<OfflineModel>();
  int side_effects=0;
  myagent::Agent agent(settings(),model);
  register_example_tool(agent,side_effects);
  myagent::LoopOptions options;
  options.checkpoint_path=file;
  options.on_tool_approval=[](const myagent::ToolApprovalRequest&) {
    return myagent::ToolDecision::Pause;
  };
  ensure(agent.react("模拟崩溃恢复",options).status==myagent::LoopStatus::Paused,
         "故障注入准备失败");

  // 仅用于演示的“故障注入”：将审批中的日志改为 pending。
  // 真实产品不得手工修改 Checkpoint；pending 是运行时在执行工具前写入的。
  std::ifstream in(file,std::ios::binary);
  ensure(static_cast<bool>(in),"无法读取测试快照");
  const std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  in.close();
  Json snapshot=j::parse(bytes);
  ensure(snapshot.at("events").size()>=2,"缺少工具日志");
  snapshot["events"][1]["status"]="pending";
  std::ofstream out(file,std::ios::binary|std::ios::trunc);
  ensure(static_cast<bool>(out),"无法写入故障注入快照");
  out<<j::serialize(snapshot);
  out.close();
  ensure(static_cast<bool>(out),"故障注入快照写盘失败");

  // 恢复时，系统必须“拒绝猜测”外部工具是否成功；不能重新执行。
  myagent::LoopOptions resume_options;
  auto unknown=agent.resume(file,resume_options);
  ensure(unknown.status==myagent::LoopStatus::Paused,
         "结果未知的工具没有被阻止");
  ensure(side_effects==0,"结果未知时重复执行工具");
  std::cout<<"[安全阻断] "<<unknown.error<<"\n";

  // 示例假设宿主已经从外部系统核实了结果，返回核实后的输出。
  resume_options.reconcile_tool=[](const myagent::ToolApprovalRequest& request)
      -> std::optional<std::string> {
    ensure(request.call_id=="demo-call-1","核实了错误工具调用");
    std::cout<<"[人工核实] 使用真实外部证据确认操作结果，不再执行原工具。\n";
    return std::string("externally-verified-outcome");
  };
  auto restored=agent.resume(file,resume_options);
  ensure(restored.status==myagent::LoopStatus::Completed,
         "核实结果后无法完成："+restored.error);
  ensure(side_effects==0,"恢复时不应重新执行有副作用的工具");
  std::cout<<"[安全恢复] "<<restored.answer<<"\n";
}
} // namespace

int run_agent_checkpoint_demo() {
  try {
    ScopedDemoDirectory demo;
    demonstrate_approval(demo.root/"approved.json");
    demonstrate_uncertain_outcome(demo.root/"uncertain.json");
    std::cout<<"\nrun_agent_checkpoint_demo: 全部检查通过。\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"run_agent_checkpoint_demo 失败："<<e.what()<<"\n";
    return 1;
  }
}
int main(){return run_agent_checkpoint_demo();}
