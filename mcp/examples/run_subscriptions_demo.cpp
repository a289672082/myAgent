// SSE 订阅示例：确认订阅成功后，由另一客户端发布资源变更通知。
// No arguments: SSE subscription with a publish request from another client.
// Start run_http_server_demo first; this program connects to its fixed loopback URL.
#include "demo_common.hpp"
#include <atomic>
#include <thread>

int run_subscriptions_demo() {
  return demo::execute("run_subscriptions_demo", [] {
    // 用独立线程执行变更请求，避免阻塞 SSE 读取回调。
    std::thread producer;
    std::atomic_bool updated{false};
    std::atomic_bool acknowledged{false};
    try {
      // 向服务端发送 subscriptions/listen，并持续读取事件流。
      mymcp::Client::listen_http(demo::http_url,
          // 过滤器只订阅指定 URI 的资源变更。
          mymcp::json::object{{"resourceSubscriptions", mymcp::json::array{demo::status_uri}}},
          [&](const mymcp::Json& message) -> bool {
            const std::string method = message.at("method").as_string();
            // 收到订阅确认后才能开始发布更新事件。
            if (method == "notifications/subscriptions/acknowledged" && !acknowledged.exchange(true)) {
              producer = std::thread([] {
                try {
                  auto client = mymcp::Client::http(demo::http_url);
                  // 另一个客户端调用 emit_update，促使服务端向订阅者推送通知。
                  client.call("emit_update");
                } catch (const std::exception& e) { std::cerr << "publish failed: " << e.what() << '\n'; }
              });
            }
            // 收到资源变更通知，输出事件并结束本次订阅。
            if (method == "notifications/resources/updated") {
              demo::print("resource updated", message);
              updated = true;
              // 返回 false 表示主动关闭 SSE 流。
              return false; // close SSE stream cleanly
            }
            return true;
          });
    } catch (...) {
      if (producer.joinable()) producer.join();
      throw;
    }
    if (producer.joinable()) producer.join();
    // 检查订阅确认和更新通知两个阶段均成功。
    demo::check(acknowledged && updated, "no subscription acknowledgement/update received");
  });
}
int main() { return run_subscriptions_demo(); }
