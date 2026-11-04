// OAuth 资源服务端示例：演示受保护资源元数据与 Bearer Token 验证回调。
// Fixed development-only token. No command line parameters, binds loopback only.
// Never copy this verifier into production: a string comparison is NOT token validation.
#include "demo_common.hpp"
#include <openssl/crypto.h>

int run_oauth_resource_demo() {
  return demo::execute("run_oauth_resource_demo", [] {
    constexpr unsigned short port = 33868;
    // 这是硬编码的本地演示 Token，绝不能作为真实 OAuth 凭证。
    const std::string token = "local-demo-token-not-a-real-oauth-token";
    mymcp::Server server("oauth-resource-demo");
    // 注册需要认证才能调用的工具。
    server.tool("private_hello", "Only available with the demo Bearer token",
                [] { return "protected demo result"; });
    // 配置资源元数据与自定义令牌验证函数，未经认证的请求会被拒绝。
    server.protect_http(mymcp::json::object{
        {"resource", "http://127.0.0.1:33868/mcp"},
        // 生产环境必须改成可信授权服务器地址，并校验 issuer、audience 和 scopes。
        {"authorization_servers", mymcp::json::array{"https://auth.example.invalid"}}},
      [token](const std::string& candidate, const std::string&, const std::string&) {
        return candidate.size() == token.size() &&
          // 常数时间字符串比较只是演示；不等于 JWT/JWKS 验签或 Token Introspection。
          CRYPTO_memcmp(candidate.data(), token.data(), candidate.size()) == 0;
      });
    std::cout << "LOCAL TEST ONLY: http://127.0.0.1:33868/mcp\n"
              << "Fixed token is intentionally untrusted demo data.\n" << std::flush;
    // 仅在回环地址运行演示服务；对外部署需要 HTTPS 和完整权限控制。
    server.run_http(port);
  });
}
int main() { return run_oauth_resource_demo(); }
