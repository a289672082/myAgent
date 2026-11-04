// Streamable HTTP 服务端示例：快速请求返回 JSON，进度和订阅使用 SSE。
// No arguments: start a loopback-only HTTP MCP server on the port in demo_common.hpp.
// This program runs until stopped; run_http_client_demo in a second terminal.
#include "demo_common.hpp"
#include <chrono>
#include <thread>

int run_http_server_demo() {
  return demo::execute("run_http_server_demo", [] {
    // 创建 HTTP 服务端，地址和端口由 demo_common.hpp 统一管理。
    mymcp::Server server("http-demo");
    // 快速工具通过 HTTP POST 返回 JSON 结果。
    server.tool("hello", "Say hello over HTTP", [] { return "HTTP MCP is ready"; });
    // 注册可被读取和订阅更新的状态资源。
    server.resource(demo::status_uri, "Server status", [] { return std::string("online"); });
    // 此耗时工具可以在单次 HTTP 请求的 SSE 流中报告进度。
    server.add_tool("count_with_progress", "Stream progress updates using request-scoped SSE",
      mymcp::json::object{{"type", "object"}}, [&server](const mymcp::Json&) -> mymcp::Json {
        for (int i = 1; i <= 5; ++i) {
          if (server.is_cancelled()) break;
          // 报告第 i 批进度，最终仍需返回规范的工具结果。
          server.progress(i, 5, "Processing batch");
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return mymcp::detail::make_tool_result("count complete");
      });
    // 供订阅 Demo 调用的工具，用于发布资源更新事件。
    server.add_tool("emit_update", "Send a notification to SSE subscribers",
      mymcp::json::object{{"type", "object"}}, [&server](const mymcp::Json&) -> mymcp::Json {
        // 服务端只向订阅对应资源的客户端推送通知。
        server.publish("notifications/resources/updated", mymcp::json::object{{"uri", demo::status_uri}});
        return mymcp::detail::make_tool_result("event sent");
      });
    std::cout << "MCP HTTP server: " << demo::http_url << '\n' << std::flush;
    // 服务仅监听本地回环地址；此调用持续运行直至进程停止。
    server.run_http(demo::http_port); // blocks until termination
  });
}
int main() { return run_http_server_demo(); }
