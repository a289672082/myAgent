// HTTP SSE 实时进度示例：同一个 POST 响应流包含进度通知和最终结果。
// Start run_http_server_demo first. No command-line arguments.
#include "demo_common.hpp"
#include <atomic>

int run_http_progress_demo() {
  return demo::execute("run_http_progress_demo", [] {
    // 计数器记录收到的进度事件总数。
    std::atomic<int> events{0};
    double last = 0;
    // 传入事件回调，SDK 负责解析请求级 SSE 数据帧。
    auto client = mymcp::Client::http(demo::http_url, {}, [&](const mymcp::Json& event) {
      if (!event.is_object() || !event.contains("method") ||
          // 只处理 notifications/progress，忽略其他类型的事件。
          event.at("method") != "notifications/progress") return;
      const auto& params = event.at("params");
      // 校验通知属于本次请求，避免并发时混淆。
      demo::check(params.at("progressToken") == "http-demo-progress", "progress token mismatch");
      const double value = params.at("progress").as<double>();
      // 检查进度单调增加，以保证进度条显示正确。
      demo::check(value > last, "progress not increasing");
      last = value;
      ++events;
      demo::print("HTTP SSE progress", params);
    });
    // 发起带 progressToken 的工具调用，等待 SSE 进度和最终结果。
    auto result = client.request_with_progress("tools/call",
      mymcp::json::object{{"name", "count_with_progress"},
                           {"arguments", mymcp::json::object{}}}, "http-demo-progress");
    demo::check(result.at("resultType") == "complete", "missing final result");
    // 确认全部五条进度事件都被消费。
    demo::check(events == 5, "not all progress SSE events arrived");
    demo::print("HTTP SSE final result", result);
  });
}
int main() { return run_http_progress_demo(); }
