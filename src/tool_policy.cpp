#include "myagent/tool_policy.hpp"
#include <stdexcept>
namespace myagent {
ToolAccess ToolPolicy::access(const std::string& tool) const {
  const auto direct = rules.find(tool);
  if (direct != rules.end()) return direct->second;
  std::size_t longest = 0;
  ToolAccess selected = default_access;
  for (const auto& [pattern, mode] : rules) {
    if (pattern.size() <= 1 || pattern.back() != '*') continue;
    const auto length = pattern.size() - 1;
    if (length >= longest && tool.compare(0, length, pattern, 0, length) == 0) {
      longest = length;
      selected = mode;
    }
  }
  return selected;
}
bool ToolPolicy::authorize(const ToolRequest& request) const {
  const auto permission = access(request.tool);
  if (permission == ToolAccess::Deny) return false;
  if (permission == ToolAccess::Allow) return true;
  // 缺失宿主审批时一律拒绝，不能让模型自我批准。
  return approve && approve(request);
}
} // namespace myagent
