#pragma once
// 仅供独立控制台程序使用：不属于 myAgent SDK，不安装、不导出头文件。
// 所有会话与 MCP 操作均通过 include/myagent/agent.hpp 的公开接口完成。
#include "myagent/agent.hpp"
#include <atomic>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myagent_console {

enum class CommandKind { Info, Error, Send, SecretPrompt, ClearView, Exit };
struct CommandResult {
  CommandKind kind = CommandKind::Info;
  std::string message;
};

struct McpEntry {
  std::string name;
  std::string transport;             // stdio / http
  std::string target;                // 可执行文件路径，或 HTTP URL
  std::vector<std::string> args;     // stdio 参数（直接传给进程，不经 shell）
  std::string bearer_env;            // HTTP Bearer 仅从环境变量加载，不保存密钥
};

enum class Permissions { Ask, Deny, Allow };

// 独立应用的状态控制器。单线程调用配置和命令接口；对话则可以在独立工作线程执行。
// 修改 /model、/baseurl、/apikey、/protocol 或 MCP 时重建 Agent；原对话会被清空。
// 新增功能若需 SDK 特性，应通过公开 API 实现，不为控制台增加 SDK 专用接口。
class ConsoleSession {
 public:
  // 可注入 Model，仅供控制台自身的离线聊天回归测试使用，不修改 SDK。
  explicit ConsoleSession(std::shared_ptr<myagent::Model> test_model = {},
                          std::filesystem::path settings_path = {});

  CommandResult handle(std::string_view command);
  void set_api_key(std::string value);
  CommandResult status() const;
  std::vector<std::string> suggestions(std::string_view prefix) const;

  // 同步执行一次对话，由 UI 调用方安排线程。on_delta 从工作线程调用，
  // on_approval 在执行任何 MCP 工具前等待 UI 人工确认（除非显式 /permissions allow）。
  myagent::LoopResult chat(const std::string& message,
      const std::function<void(const std::string&,const std::string&)>& on_delta,
      const std::function<bool(const myagent::ToolApprovalRequest&)>& on_approval,
      const std::function<void(const std::string&)>& on_activity,
      const std::function<bool()>& cancelled);

  const myagent::Config& config() const noexcept { return config_; }
  const std::vector<McpEntry>& endpoints() const noexcept { return endpoints_; }
  Permissions permissions() const noexcept { return permissions_; }
  myagent::LoopMode loop_mode() const noexcept { return mode_; }
  bool has_credentials() const noexcept { return !config_.api_key.empty(); }

 private:
  std::string mcp_command(const std::vector<std::string>& tokens,CommandKind& kind);
  void reset_agent();
  void ensure_agent();
  void load_settings();
  void save_settings() const;
  std::filesystem::path settings_path_;
  std::optional<std::string> saved_key_, saved_base_url_, saved_model_;
  myagent::Config config_;
  std::shared_ptr<myagent::Model> test_model_;
  std::unique_ptr<myagent::Agent> agent_;
  std::vector<McpEntry> endpoints_;
  bool native_tools_enabled_=true;  // 仅供可选 DeveloperTools 编译开关；默认启用
  Permissions permissions_=Permissions::Ask;
  myagent::LoopMode mode_=myagent::LoopMode::ReAct;
};

} // namespace myagent_console
