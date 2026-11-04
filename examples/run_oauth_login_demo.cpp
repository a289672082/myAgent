// 真实 OAuth 登录示例：在源码配置资源地址，通过浏览器与本机回调完成授权。
// Real provider: set configuration constants in this file; no CLI parsing or secret storage.
// The callback port is OS assigned. Your provider must permit native loopback redirects.
#include "demo_common.hpp"
#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace configuration {
// 在这里填写真实 HTTPS MCP 资源地址，不要使用占位域名。
constexpr const char* mcp_resource = "https://replace-with-your-mcp-server.example/mcp";
// 使用授权服务器分配的公开客户端 ID；不要把 client_secret 写入代码。
constexpr const char* public_client_id = "REPLACE_WITH_PUBLIC_CLIENT_ID";
constexpr bool use_dynamic_registration = false;
constexpr const char* requested_scope = "tools:read";
} // namespace configuration

int run_oauth_login_demo() {
  return demo::execute("run_oauth_login_demo", [] {
    const std::string resource = configuration::mcp_resource;
    // 配置仍是占位内容时直接退出，避免误认为登录成功。
    if (resource.find("replace-with-your-mcp-server.example") != std::string::npos ||
        (!configuration::use_dynamic_registration &&
         std::string(configuration::public_client_id) == "REPLACE_WITH_PUBLIC_CLIENT_ID")) {
      std::cout << "Edit the configuration constants at the top of run_oauth_login_demo.cpp first.\n";
      return; // not an error: example is intentionally not configured with credentials
    }
    auto session = std::make_shared<mymcp::oauth::Session>(resource);
    // 客户端先发现受保护资源元数据和 OAuth 授权服务器。
    session->discover();
    httplib::Server callback_server;
    // 本机回调服务动态选择可用端口，减少端口冲突。
    const int port = callback_server.bind_to_any_port("127.0.0.1");
    demo::check(port > 0, "cannot open loopback OAuth callback receiver");
    const std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/callback";
    // 按配置选择动态客户端注册或使用已有公开客户端 ID。
    if (configuration::use_dynamic_registration)
      session->register_dynamic({redirect}, "myMcpTools desktop demo");
    else session->client_id(configuration::public_client_id);
    // 生成带 PKCE、state 等校验参数的授权地址。
    const auto flow = session->begin_authorization(redirect, {configuration::requested_scope});

    std::mutex mutex;
    std::condition_variable wake;
    std::string callback_url;
    // 只接收本地浏览器回调，具体授权信息交给 Session 统一验证。
    callback_server.Get("/callback", [&](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard lock(mutex);
        if (callback_url.empty()) callback_url = redirect.substr(0, redirect.find("/callback")) + req.target;
      }
      res.set_content("OAuth callback received. You may close this page.", "text/plain");
      wake.notify_one();
    });
    std::thread listener([&] { callback_server.listen_after_bind(); });
    bool got_callback = false;
    try {
      std::cout << "Open this URL in your browser to authorize:\n" << flow.url << "\n";
      std::unique_lock lock(mutex);
      // 等待用户完成授权，最多 180 秒，超时就停止监听。
      got_callback = wake.wait_for(lock, std::chrono::seconds(180), [&] { return !callback_url.empty(); });
      lock.unlock();
      callback_server.stop();
      listener.join();
    } catch (...) { callback_server.stop(); listener.join(); throw; }
    demo::check(got_callback, "authorization callback timed out");
    // 校验回调地址中的 state、issuer 等信息，再交换 Token。
    session->exchange_callback_url(flow, callback_url, redirect);
    // 建立附加 Bearer Token 的 MCP HTTP 客户端。
    auto client = mymcp::Client::http_oauth(resource, session);
    demo::print("authorized discovery", client.discover());
    demo::print("authorized tools", client.tools());
    // 示例 Token 仅保存在内存，不打印令牌、不进行持久化。
    std::cout << "OAuth access/refresh tokens were kept only in memory.\n";
  });
}
int main() { return run_oauth_login_demo(); }
