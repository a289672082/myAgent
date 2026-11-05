#pragma once
#include "mymcp/mcp.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace mymcp::oauth {
struct JwtPolicy {
  std::string issuer;                      // 必须与授权服务签发者精确一致
  std::string audience;                    // MCP Resource URI，禁止跨资源复用
  std::vector<std::string> required_scopes;
  std::chrono::seconds clock_skew{30};
};

// 只接受 RS256 + RSA >= 2048 位的 JWT；对不可信 alg、kid、声明一律拒绝。
// JWKS 应从可信的 HTTPS 端点读取，并由宿主按需刷新、轮换和缓存。
class JwtVerifier {
public:
  JwtVerifier(Json jwks, JwtPolicy policy);
  bool verify(const std::string& token,
              const std::vector<std::string>& additional_scopes = {}) const noexcept;
private:
  Json jwks_;
  JwtPolicy policy_;
};

// 仅 HTTPS，无重定向，验证证书和主机名；限制响应大小为 1 MiB。
// 不自动跟随 metadata 中不受信任的 JWKS URL。
Json fetch_jwks_https(const std::string& url);
} // namespace mymcp::oauth
