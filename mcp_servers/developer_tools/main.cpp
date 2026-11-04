// 独立 stdio MCP Server，暴露同一套 developer_tools C++ Handler。
// 默认只创建 demo_workspace，不默认操作调用者的源码工作区。
// 若要指定工作区，请设置 MYAGENT_TOOLS_WORKSPACE（必须已存在）。
#include "developer_tools/developer_tools.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
  try {
    namespace fs=std::filesystem;
    fs::path root;
    if(const char* env=std::getenv("MYAGENT_TOOLS_WORKSPACE");env && *env) {
      root=env;
    } else {
      root=fs::current_path()/"demo_workspace";
      fs::create_directories(root);
    }
    mymcp::Server server("myAgent-developer-tools","1.0.0");
    server.instructions(
      "23 个受限开发工具。所有文件路径相对显式工作区。"
      "写入、补丁和编译必须经宿主确认；不存在操作系统沙箱。"
      "命令仅支持 git_version/cmake_version，HTTP 仅允许 https://example.com/。");
    const auto count=developer_tools::RegisterToMcp(
      server,{root},developer_tools::ToolOptions::All());
    std::cerr<<"developer-tools MCP: "<<count<<" tools, workspace="
             <<fs::canonical(root).string()<<"\n";
    // stdout 留给 JSON-RPC，禁止在此输出人类可读信息。
    server.run_stdio();
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"developer-tools MCP: "<<e.what()<<"\n";
    return 1;
  }
}
