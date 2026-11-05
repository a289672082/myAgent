#pragma once
#include "mymcp/json.hpp"
#include <string>
#include <vector>

namespace mymcp::oauth {
// 仅验证受信任发行者签发的 RS256 JWT；不读取 token 指定的 jku/x5u URL。
// JWKS 必须由宿主从可信发行者元数据获取并周期性刷新。验证失败返回 false。
bool verify_jwt_rs256(const std::string& token, const json::value& jwks,
                      const std::string& expected_issuer,
                      const std::string& expected_audience,
                      const std::vector<std::string>& required_scopes = {});

// 使用显式配置的 HTTPS JWKS URL（或本机回环 HTTP）获取公钥集合。
// 调用者在授权处理之外主动刷新，不在每个请求内访问网络。
json::value fetch_jwks(const std::string& trusted_jwks_uri);
} // namespace mymcp::oauth
