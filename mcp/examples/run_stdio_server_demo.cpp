// stdio 服务端示例：供本地 MCP 客户端启动，通过 stdin/stdout 交换 JSON-RPC。
// Run this standalone stdio server without command line arguments.
// stdin/stdout are reserved for MCP JSON-RPC; logging goes to stderr only.
#include "mymcp/mcp.hpp"
#include <chrono>
#include <thread>

int run_stdio_server_demo() {
  // stdio 的标准输出专用于协议消息，请将日志写到 stderr。
  mymcp::Server server("stdio-demo");
  // 注册整数乘法工具，便于客户端验证参数传输。
  server.tool("multiply", "Multiply two integers", [](int lhs, int rhs) {
    return lhs * rhs;
  }, mymcp::arg<int>("lhs"), mymcp::arg<int>("rhs"));
  // 注册耗时工具，演示协作取消与进度通知。
  server.add_tool("count", "Report progress for an example long task",
    mymcp::json::object{{"type", "object"}},
    [&server](const mymcp::Json&) -> mymcp::Json {
      for (int n = 1; n <= 4; ++n) {
        // 在循环安全点检查客户端是否取消当前请求。
        if (server.is_cancelled()) return mymcp::detail::make_tool_result("cancelled");
        // 报告当前进度，客户端通过 progressToken 关联通知。
        server.progress(n, 4, "Demo step " + std::to_string(n));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return mymcp::detail::make_tool_result("completed");
    });
  // 进入阻塞的 stdio 协议循环，直到管道关闭。
  server.run_stdio();
  return 0;
}
int main() { return run_stdio_server_demo(); }
