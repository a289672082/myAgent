// 全程离线：验证一份 ToolDefinition 在 Agent 与 MCP Server 两条路径可用。
// 不请求网络、不运行构建或任意命令，不写工作区外内容。
#include "developer_tools/developer_tools.hpp"
#include "myagent/agent.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <set>

using mymcp::Json;
using mymcp::json::object;
using mymcp::json::array;

namespace {
void expect(bool yes,const char* message) {
  if(!yes)throw std::runtime_error(message);
}
struct TemporaryWorkspace {
  std::filesystem::path root;
  TemporaryWorkspace() {
    namespace fs=std::filesystem;
    root=fs::temp_directory_path()/("myagent-devtools-test-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    std::ofstream(root/"hello.cpp")<<"int found_symbol = 42;\n";
    std::ofstream(root/"CMakeLists.txt")<<"cmake_minimum_required(VERSION 3.20)\n";
  }
  ~TemporaryWorkspace() {
    std::error_code ec;std::filesystem::remove_all(root,ec);
  }
};
Json request(const std::string& method,Json params,int id=1) {
  return object{{"jsonrpc","2.0"},{"id",id},{"method",method},
                {"params",std::move(params)}};
}
struct CatalogModel final:myagent::Model {
  bool found=false;
  Json create(const Json& payload) override {
    if(payload.contains("tools")&&payload.at("tools").is_array()) {
      for(const auto& t:payload.at("tools").array_range())
        if(t.at("name").as_string()=="local__read_file")found=true;
    }
    const auto msg=object{{"type","message"},{"role","assistant"},
        {"content",array{object{{"type","output_text"},{"text","OK"}}}}};
    return object{{"status","completed"},{"output",array{msg}}};
  }
};
}
int main() {
  try {
    TemporaryWorkspace w;
    const developer_tools::ToolContext ctx{w.root};

    // 外部调用者期望的七个类别开关都能够单独指定。
    developer_tools::ToolOptions options;
    options.filesystem=true;
    options.search=true;
    options.editor=true;
    options.git=true;
    options.command=true;
    options.http=true;
    options.cpp_project=true;

    const auto tools=developer_tools::CreateToolDefinitions(ctx,options);
    expect(tools.size()==23,"Expected exactly 23 shared tools");
    std::set<std::string> names;
    for(const auto& t:tools) {
      expect(t.schema.is_object(),"Tool schema must be a JSON object");
      names.insert(t.name);
    }
    expect(names.size()==23&&names.count("find_symbol")==1,
           "Tool names must be unique and include singular find_symbol");
    // 类别级开关：filesystem=false 必须关闭全部 7 个文件/目录工具，
    // 而不能让细粒度 filesystem_read/write 的默认 true 意外启用它们。
    struct CategoryCase {
      bool developer_tools::ToolOptions::*flag;
      std::size_t removed;
      const char* example_name;
    };
    const std::array<CategoryCase,7> categories{{
      {&developer_tools::ToolOptions::filesystem,7,"read_file"},
      {&developer_tools::ToolOptions::search,4,"search_files"},
      {&developer_tools::ToolOptions::editor,2,"apply_patch"},
      {&developer_tools::ToolOptions::git,3,"git_status"},
      {&developer_tools::ToolOptions::command,2,"run_command"},
      {&developer_tools::ToolOptions::http,2,"http_get"},
      {&developer_tools::ToolOptions::cpp_project,3,"project_info"}
    }};
    for(const auto& category:categories) {
      auto subset=options;
      subset.*(category.flag)=false;
      const auto filtered=developer_tools::CreateToolDefinitions(ctx,subset);
      expect(filtered.size()==23-category.removed,"Category switch exposed wrong number of tools");
      expect(std::none_of(filtered.begin(),filtered.end(),[&](const auto& item) {
        return item.name==category.example_name;
      }),"Disabled category still exposed its tool");
    }

    const auto safe=developer_tools::CreateToolDefinitions(
      ctx,developer_tools::ToolOptions::ReadOnly());
    expect(safe.size()==14,"ReadOnly must expose exactly 14 safe tools");

    mymcp::Server mcp("test-developer-tools","1.0.0");
    expect(developer_tools::RegisterToMcp(mcp,ctx,options)==23,
           "MCP must receive all 23 tools");
    const auto initialized=mcp.handle(request("initialize",
      object{{"protocolVersion","2025-11-25"}}));
    expect(initialized.contains("result"),"MCP initialize must succeed");
    const auto listing=mcp.handle(request("tools/list",object{}));
    expect(listing.contains("result")&&listing.at("result").contains("tools"),
           "MCP tools/list must succeed");
    std::set<std::string> mcp_names;
    for(const auto& item:listing.at("result").at("tools").array_range())
      mcp_names.insert(item.at("name").as_string());
    expect(mcp_names==names,"MCP definitions and C++ registry diverged");

    const auto read=mcp.handle(request("tools/call",
      object{{"name","read_file"},
             {"arguments",object{{"path","hello.cpp"}}}},2));
    expect(read.contains("result") &&
           mymcp::json::serialize(read).find("found_symbol")!=std::string::npos,
           "MCP tools/call read_file failed");

    const auto denied=mcp.handle(request("tools/call",
      object{{"name","read_file"},
             {"arguments",object{{"path","../outside.txt"}}}},3));
    expect(mymcp::json::serialize(denied).find("isError")!=std::string::npos,
           "Workspace traversal must produce tool error");

    auto model=std::make_shared<CatalogModel>();
    myagent::Config config;
    config.agent_id="developer-tools-regression";
    config.auto_discover_skills=false;
    myagent::Agent agent(config,model);
    expect(developer_tools::RegisterToAgent(agent,ctx,options)==23,
           "Agent must receive all 23 tools");
    const auto result=agent.ask("List the available tools");
    expect(model->found&&result=="OK","Agent must discover native tool local__read_file");
    std::cout<<"developer_tools: 23 shared definitions, MCP tools/list, MCP call, "
                "workspace boundary, Agent native discovery OK\n";
    return 0;
  }catch(const std::exception& e) {
    std::cerr<<"developer_tools test failed: "<<e.what()<<"\n";
    return 1;
  }
}
