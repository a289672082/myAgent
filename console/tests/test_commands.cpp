// 独立控制台命令和凭据保护回归，无需 FTXUI 运行 / 真实 API Key / 网络。
// 不属于 SDK 测试，也不向 SDK 的公共头或链接依赖引入 console 模块。
#include "session.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace myagent_console;
namespace {
void check(bool pass,const char* reason) {
  if(!pass)throw std::runtime_error(reason);
}
bool is(ConsoleSession& session,std::string_view command,CommandKind expected) {
  return session.handle(command).kind==expected;
}
}
int main() {
  try {
    const auto settings=std::filesystem::temp_directory_path()/
      ("myagent-console-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))/"console.json";
    ConsoleSession session({},settings);
    check(is(session,"/help",CommandKind::Info),"help failed");
    check(is(session,"/unknown",CommandKind::Error),"unknown command not rejected");
    check(is(session,"hello 你好",CommandKind::Send),"chat turn not forwarded");
    check(session.suggestions("/m").size()>0,"slash suggestion missing");
    check(session.suggestions("/Mo")==session.suggestions("/mo"),"suggestions should ignore case");
    check(is(session,"/HeLp",CommandKind::Info),"command should ignore case");
    check(is(session,"/apikey",CommandKind::SecretPrompt),
          "API Key must use masked editor");
    const auto direct_key=session.handle("/apikey my-secret");
    check(direct_key.kind==CommandKind::Info && session.has_credentials(),
          "direct API Key was not accepted");
    check(direct_key.message.find("my-secret")==std::string::npos &&
          session.status().message.find("my-secret")==std::string::npos,
          "direct API Key leaked in output");
    session.set_api_key("VERY-SECRET-DO-NOT-PRINT");
    check(session.has_credentials(),"API Key was not stored in memory");
    check(session.status().message.find("VERY-SECRET")==std::string::npos,
          "status leaked API Key");
    check(is(session,"/apikey clear",CommandKind::Info),"clear API Key failed");
    check(!session.has_credentials(),"API Key was not cleared");
    check(is(session,"/baseurl http://remote.example.com/v1",CommandKind::Error),
          "insecure remote API URL accepted");
    check(is(session,"/baseurl http://localhost:8888/v1",CommandKind::Info),
          "local proxy URL rejected");
    check(is(session,"/baseurl https://api.example.com/v1",CommandKind::Info),
          "HTTPS API URL rejected");
    check(is(session,"/baseurl https://user:key@host/v1",CommandKind::Error),
          "URL containing credentials accepted");
    check(is(session,"/protocol chat",CommandKind::Info),"Chat API mode rejected");
    check(session.config().model_protocol==myagent::ModelProtocol::ChatCompletions,
          "Chat protocol not applied");
    check(is(session,"/protocol responses",CommandKind::Info),"Responses protocol rejected");
    check(is(session,"/model test-model",CommandKind::Info),"model config rejected");
    check(session.config().model=="test-model","model config not applied");
    session.set_api_key("persistence-test-key");
    ConsoleSession reloaded({},settings);
    check(reloaded.config().model=="test-model" && reloaded.config().base_url=="https://api.example.com/v1" &&
          reloaded.config().api_key=="persistence-test-key","settings were not restored");
#ifdef _WIN32
    std::ifstream saved(settings,std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(saved)),{});
    saved.close();
    check(contents.find("persistence-test-key")==std::string::npos,"key was saved unencrypted");
#endif
    check(is(session,"/apikey clear",CommandKind::Info),"clear saved key failed");
    check(!ConsoleSession({},settings).has_credentials(),"cleared key returned after restart");
    check(is(session,"/mode react",CommandKind::Info),"ReAct mode unavailable");
    check(is(session,"/mode rewoo",CommandKind::Info),"ReWOO mode unavailable");
    check(is(session,"/mode execute-verify-repair",CommandKind::Error),
          "Untrusted fake verifier must not be offered");
    check(is(session,"/permissions deny",CommandKind::Info),"tool deny unavailable");
    check(session.permissions()==Permissions::Deny,"tool policy not applied");
    check(is(session,"/permissions ask",CommandKind::Info),"tool approval unavailable");

    check(is(session,"/mcp add data http http://remote.example.org/mcp",CommandKind::Error),
          "insecure HTTP MCP accepted");
    check(is(session,"/mcp add data http https://mcp.example.com/mcp MCP_TOKEN_ENV",
             CommandKind::Info),"HTTP MCP with token environment rejected");
    check(session.endpoints().size()==1 &&
          session.endpoints()[0].bearer_env=="MCP_TOKEN_ENV","MCP token env lost");
    check(session.handle("/mcp").message.find("MCP_TOKEN_ENV")!=std::string::npos,
          "MCP inventory not listed");
    check(is(session,"/mcp add data http https://other.example.com/mcp",CommandKind::Error),
          "duplicate MCP name accepted");
    check(is(session,"/mcp add stdio stdio \"C:\\Program Files\\mcp.exe\" --config test",
             CommandKind::Info),"Windows quoted stdio path rejected");
    check(session.endpoints().size()==2 &&
          session.endpoints()[1].target=="C:\\Program Files\\mcp.exe" &&
          session.endpoints()[1].args.size()==2,"stdio quoting/arguments lost");
    check(is(session,"/mcp add malicious http https://server.example/mcp raw-token=value",
             CommandKind::Error),"MCP inline token accepted");
    check(is(session,"/mcp refresh",CommandKind::Info),"MCP refresh failed");
    check(is(session,"/mcp remove data",CommandKind::Info),"MCP remove failed");
    check(session.endpoints().size()==1,"MCP removal not applied");
    check(is(session,"/mcp remove unknown",CommandKind::Error),
          "unknown MCP removal did not fail");
    check(is(session,"/tools",CommandKind::Info),"DeveloperTools status failed");
    check(is(session,"/tools off",CommandKind::Info),"DeveloperTools disable failed");
    check(is(session,"/tools on",CommandKind::Info),"DeveloperTools enable failed");
    check(is(session,"/new",CommandKind::Info),"new turn reset failed");
    check(is(session,"/clear",CommandKind::ClearView),"clear view failed");
    check(is(session,"/exit",CommandKind::Exit),"quit failed");

    std::filesystem::remove_all(settings.parent_path());
    std::cout<<"myagent_console_commands: slash commands, token privacy, MCP, protocol OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"myagent_console_commands failed: "<<e.what()<<'\n';
    return 1;
  }
}
