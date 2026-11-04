// stdio 进度示例：通过客户端通知回调接收 notifications/progress。
// No arguments: stdio progress notifications from a long-running tool call.
#include "demo_common.hpp"
#include <atomic>

#ifndef MYMCP_DEMO_STDIO_SERVER
#error "CMake must provide MYMCP_DEMO_STDIO_SERVER"
#endif

int run_progress_demo() {
  return demo::execute("run_progress_demo", [] {
    // 原子计数器用于记录进度通知数量。
    std::atomic<int> notifications{0};
    // 创建 stdio 客户端，注册异步通知处理回调。
    auto client = mymcp::Client::process(MYMCP_DEMO_STDIO_SERVER, {},
      [&](const mymcp::Json& message) {
        if (message.is_object() && message.contains("method") &&
            // 筛选进度通知，忽略其他事件。
            message.at("method") == "notifications/progress") {
          ++notifications;
          demo::print("progress event", message.at("params"));
        }
      });
    // 调用耗时工具并指定唯一 progressToken 以匹配通知。
    auto result = client.request_with_progress("tools/call",
      mymcp::json::object{{"name", "count"}, {"arguments", mymcp::json::object{}}},
      // 实际并发请求必须使用不同 Token，避免进度串线。
      "progress-demo-token");
    demo::print("finished", result);
    // 确认调用过程中确实收到了进度通知。
    demo::check(notifications > 0, "no progress notifications observed");
  });
}
int main() { return run_progress_demo(); }
