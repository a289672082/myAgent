#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "mymcp/json.hpp"

namespace mymcp::oauth {
using Json = json::value;

// JWT 验证策略由可信宿主提供。不要从 JWT header 的 jku/x5u 或未验证的
// 令牌正文动态选择密钥服务器，防止 SSRF、密钥替换和跨资源令牌复用。
struct JwtPolicy {
  std::string issuer;
  std::string audience;
  std::string jwks_uri;
  std::vector<std::string> default_scopes;
  // 优先查找 "method:name"，其次 "method"；每项列出的 scope 均为必需。
  std::map<std::string, std::vector<std::string>> required_scopes;
  std::chrono::seconds clock_skew{30};
  std::chrono::seconds cache_ttl{300};
};

// 仅接受受信任 JWKS 的 RS256（RSA >= 2048 位）及 ES256（P-256）。
// 拒绝 alg=none、HS256、过期、签发者/受众不符及权限不足的 token。
// HTTPS 强制验证服务端证书；只有开发用的显式本地回环 HTTP 例外。
class JwtVerifier {
public:
  explicit JwtVerifier(JwtPolicy policy);
  bool verify(const std::string& token, const std::string& method,
              const std::string& name) noexcept;
private:
  JwtPolicy policy_;
  std::mutex mutex_;
  Json keys_ = nullptr;
  std::chrono::steady_clock::time_point refreshed_at_{};
  Json trusted_keys(bool force_refresh);
};
} // namespace mymcp::oauth
