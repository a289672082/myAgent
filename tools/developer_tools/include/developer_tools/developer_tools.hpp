#pragma once
// 23 个开发工具共用一份 C++ 实现和 JSON Schema，独立于 myAgent SDK 公开 ABI。
#include "mymcp/mcp.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
namespace myagent {class Agent;}
namespace developer_tools {
struct ToolContext {
  // 必须是已经存在的工作区；工具仅允许工作区相对路径（非 OS 沙箱）。
  std::filesystem::path workspace;
};
struct ToolOptions {
  bool filesystem_read=true;   // 文件查询及目录树
  bool filesystem_write=true;  // 文件创建、追加和创建目录
  bool search=true;            // 文件名和内容搜索
  bool editor=true;            // replace_text, apply_patch
  bool git=true;               // 只读 Git
  bool command=true;           // 仅版本查询和受限样例编译
  bool http=true;              // 仅 https://example.com/，禁用重定向
  bool cpp_project=true;       // 工程信息、构建系统和符号搜索
  static ToolOptions ReadOnly() {
    ToolOptions o;
    o.filesystem_write=false;o.editor=false;o.command=false;o.http=false;
    return o;
  }
  static ToolOptions All() {return {};}
};
struct ToolDefinition {
  std::string name;
  std::string description;
  mymcp::Json schema;
  mymcp::Server::ToolHandler handler;
};
std::vector<ToolDefinition> CreateToolDefinitions(
    ToolContext context,ToolOptions options=ToolOptions::All());
std::size_t RegisterToMcp(mymcp::Server& server,ToolContext context,
                          ToolOptions options=ToolOptions::All());
std::size_t RegisterToAgent(myagent::Agent& agent,ToolContext context,
                            ToolOptions options=ToolOptions::All());
} // namespace developer_tools
