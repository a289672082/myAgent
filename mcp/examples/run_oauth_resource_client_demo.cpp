// OAuth 资源客户端示例：通过固定测试 Bearer Token 调用本机受保护工具。
// No arguments: talk to the development-only protected resource demo server.
// The hard-coded token is intentionally NOT a valid OAuth token. NEVER use in production.
#include "demo_common.hpp"

int run_oauth_resource_client_demo() {
  return demo::execute("run_oauth_resource_client_demo", [] {
    // 先启动 run_oauth_resource_demo，再连接它的本地端口。
    constexpr const char* local_url = "http://127.0.0.1:33868/mcp";
    // 仅供教学测试，实际项目不能在源码中硬编码访问令牌。
    constexpr const char* local_test_token = "local-demo-token-not-a-real-oauth-token";
    // SDK 会在 HTTP 请求头添加 Authorization: Bearer ...。
    auto authorized = mymcp::Client::http(local_url, local_test_token);
    // 使用已认证客户端完成能力发现。
    demo::print("authenticated discovery", authorized.discover());
    // 调用受保护工具，验证认证通过后的返回结果。
    demo::print("protected tool", authorized.call("private_hello"));
  });
}
int main() { return run_oauth_resource_client_demo(); }
