// stdio 客户端示例：启动独立服务端进程，完成协议发现与工具调用。
// A zero-argument process transport example. The server binary path is provided by CMake.
#include "demo_common.hpp"

// 服务端路径由 CMake 编译时提供，无需命令行参数。
#ifndef MYMCP_DEMO_STDIO_SERVER
#error "CMake must provide MYMCP_DEMO_STDIO_SERVER"
#endif
int run_stdio_client_demo() {
  return demo::execute("run_stdio_client_demo", [] {
    // SDK 负责启动并连接目标 stdio 子进程。
    auto client = mymcp::Client::process(MYMCP_DEMO_STDIO_SERVER);
    // 执行 MCP 发现/版本协商。
    demo::print("stdio discovery", client.discover());
    // 以 JSON 具名参数调用 multiply 工具。
    auto result = client.call("multiply", mymcp::json::object{{"lhs", 6}, {"rhs", 7}});
    // 打印工具结果，便于查看 MCP 内容结构。
    demo::print("multiply", result);
  });
}
int main() { return run_stdio_client_demo(); }
