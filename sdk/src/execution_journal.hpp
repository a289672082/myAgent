#pragma once
// 普通 Agent Loop 的写前日志。模型响应可确定性回放；工具状态必须先持久化。
// 崩溃后的 pending 工具绝不自动重试，必须让宿主核实真实副作用。
#include "myagent/loop.hpp"
#include "file_io.hpp"
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace myagent::detail {
class ExecutionJournal {
public:
  struct Descriptor {
    std::string task;
    LoopMode mode;
  };
  static Descriptor inspect(const std::filesystem::path& path) {
    const Json doc=json::parse(read_regular_file(path,16*1024*1024));
    if(!doc.is_object() || !doc.contains("version") || doc.at("version").as<int>()!=1 ||
       !doc.contains("task") || !doc.at("task").is_string() ||
       !doc.contains("mode") || !doc.at("mode").is_int64())
      throw std::runtime_error("Invalid Agent checkpoint header");
    const auto mode=doc.at("mode").as<int>();
    // 保留旧四种枚举的 0..3 编号，新模式追加 4/5，保证旧快照可恢复。
    if(mode<0 || mode>static_cast<int>(LoopMode::ExecuteVerifyRepair))
      throw std::runtime_error("Invalid checkpoint LoopMode");
    return {std::string(doc.at("task").as_string()),static_cast<LoopMode>(mode)};
  }
  ExecutionJournal(const std::filesystem::path& path,bool resume,
                   const std::string& task,LoopMode mode,
                   const Json& initial_history,const std::string& model)
      :path_(path),require_new_(!resume) {
    if(resume) {
      persisted_bytes_=read_regular_file(path_,16*1024*1024);
      doc_=json::parse(persisted_bytes_);
      if(!doc_.is_object() || !doc_.contains("version") ||
         doc_.at("version").as<int>()!=1 ||
         !doc_.contains("task") || !doc_.at("task").is_string() ||
         doc_.at("task").as_string()!=task ||
         !doc_.contains("mode") || doc_.at("mode").as<int>()!=static_cast<int>(mode) ||
         !doc_.contains("model") || doc_.at("model").as_string()!=model ||
         !doc_.contains("history") || doc_.at("history")!=initial_history ||
         !doc_.contains("events") || !doc_.at("events").is_array() ||
         !doc_.contains("state") || doc_.at("state").as_string()=="completed")
        throw std::runtime_error("Agent checkpoint mismatch or already completed");
    } else {
      doc_=json::object{{"version",1},{"task",task},{"mode",static_cast<int>(mode)},
                        {"model",model},{"history",initial_history},
                        {"events",json::array{}},{"state","running"}};
      persist();
    }
  }
  bool replay_model(const Json& request,Json& response) {
    if(cursor_>=events().size())return false;
    const auto& row=events()[cursor_];
    verify(row,"model",request);
    if(!row.contains("response"))throw std::runtime_error("Incomplete model response journal");
    response=row.at("response");
    ++cursor_;
    return true;
  }
  void record_model(const Json& request,const Json& response) {
    events().push_back(json::object{{"kind","model"},{"request",request},{"response",response}});
    ++cursor_;persist();
  }
  enum class ToolState { New, Complete, ApprovalPending, Unknown };
  ToolState inspect_tool(const Json& request,std::string& output) {
    if(cursor_==events().size())return ToolState::New;
    const auto& row=events()[cursor_];
    verify(row,"tool",request);
    const std::string status(row.at("status").as_string());
    if(status=="complete") {
      output=std::string(row.at("output").as_string());
      ++cursor_;
      return ToolState::Complete;
    }
    if(status=="approval")return ToolState::ApprovalPending;
    if(status=="pending")return ToolState::Unknown;
    throw std::runtime_error("Unknown tool journal state");
  }
  void awaiting_approval(const Json& request) {
    if(cursor_==events().size())
      events().push_back(json::object{{"kind","tool"},{"request",request},{"status","approval"}});
    else verify(events()[cursor_],"tool",request);
    persist();
  }
  void begin_tool(const Json& request) {
    if(cursor_==events().size())
      events().push_back(json::object{{"kind","tool"},{"request",request},{"status","pending"}});
    else {
      auto& row=events()[cursor_];verify(row,"tool",request);
      if(row.at("status").as_string()!="approval")
        throw std::runtime_error("Refusing automatic replay of uncertain tool");
      row["status"]="pending";
    }
    // 必须在真正调用有副作用的工具之前成功写盘。
    persist();
  }
  void complete_tool(const std::string& output) {
    if(cursor_>=events().size() || events()[cursor_].at("status").as_string()!="pending")
      throw std::logic_error("Tool is not pending");
    events()[cursor_]["status"]="complete";
    events()[cursor_]["output"]=output;
    persist();++cursor_;
  }
  void reconcile(const std::string& output) {
    if(cursor_>=events().size() || events()[cursor_].at("status").as_string()!="pending")
      throw std::logic_error("No uncertain tool to reconcile");
    complete_tool(output);
  }
  void mark(const char* state) {
    doc_["state"]=state;
    persist();
  }
private:
  Json& events() {return doc_["events"];}
  void verify(const Json& row,const char* kind,const Json& request) const {
    if(!row.is_object() || !row.contains("kind") ||
       row.at("kind").as_string()!=kind || !row.contains("request") ||
       row.at("request")!=request)
      throw std::runtime_error("Agent checkpoint replay mismatch; refusing unsafe replay");
  }
  void persist() {
    const std::string serialized=json::serialize(doc_);
    if(serialized.size()>16*1024*1024)
      throw std::runtime_error("Agent checkpoint size limit exceeded");
    // 首次创建必须拒绝覆盖任何已有日志；恢复后则检查文件版本，
    // 阻止另一个进程在本 Agent 运行期间修改状态造成双重工具执行。
    atomic_replace(path_,serialized,true,
                   require_new_?nullptr:&persisted_bytes_,require_new_);
    persisted_bytes_=serialized;
    require_new_=false;
  }
  std::filesystem::path path_;
  Json doc_;
  std::string persisted_bytes_;
  bool require_new_=false;
  std::size_t cursor_=0;
};
} // namespace myagent::detail
