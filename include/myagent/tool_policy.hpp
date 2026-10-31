#pragma once
// 工具权限是执行器强制检查，不依赖模型提示词或 Skill.allowed-tools 元数据。
#include "mymcp/mcp.hpp"
#include <functional>
#include <map>
#include <string>
namespace myagent {
using mymcp::Json;
enum class ToolAccess { Allow, Deny, RequireApproval };
struct ToolRequest {
  std::string agent;
  std::string tool;
  Json arguments = mymcp::json::object{};
  std::string phase;
};
struct ToolPolicy {
  // 工具名称支持精确匹配或末尾 '*' 前缀匹配（例如 git__*）。
  // 默认 Allow 是保持旧版兼容；生产环境建议设置 default_access=Deny。
  ToolAccess default_access = ToolAccess::Allow;
  std::map<std::string, ToolAccess> rules;
  std::function<bool(const ToolRequest&)> approve;
  void allow(const std::string& name) { rules[name] = ToolAccess::Allow; }
  void deny(const std::string& name) { rules[name] = ToolAccess::Deny; }
  void require_approval(const std::string& name) { rules[name] = ToolAccess::RequireApproval; }
  ToolAccess access(const std::string& qualified_name) const;
  // 拒绝时返回 false；批准动作必须由可信宿主同步授权，不允许模型伪造。
  bool authorize(const ToolRequest& request) const;
};
} // namespace myagent
