#pragma once
#include "mymcp/json.hpp"
#include <string>
#include <vector>

namespace mymcp::oauth {
// Initiates a public-client authorization code flow using PKCE S256, CSRF state
// and RFC 8707 resource indicator. The host app owns browser/redirect handling.
struct Authorization {
  std::string url;
  std::string state;
  std::string code_verifier; // Secret; never log or send except to the token endpoint.
  std::string expected_issuer;
  bool issuer_parameter_supported = false;
};
Authorization begin(const std::string& authorization_endpoint,
                    const std::string& expected_issuer,
                    const std::string& client_id,
                    const std::string& redirect_uri,
                    const std::string& resource,
                    const std::vector<std::string>& scopes = {},
                    bool issuer_parameter_supported = false);
// RFC 9207: check issuer and state BEFORE redeeming the code. Missing issuer
// is permitted only when AS metadata does not advertise support.
void validate_callback(const Authorization& flow,
                       const std::string& state,
                       const std::string& issuer);
std::string url_encode(const std::string& input);
 
// 校验 RS256 JWT 签名与访问控制字段。jwks 必须来自经过 TLS 验证、
 // 且与 expected_issuer 绑定的授权服务器；不能直接信任 JWT 内的 jku/jwk。
 // 基础实现仅支持 RSA RS256，拒绝 alg=none/HS256 和未知 kid。
bool verify_jwt_rs256(const std::string& token, const json::value& jwks,
                      const std::string& expected_issuer,
                      const std::string& expected_audience,
                      const std::vector<std::string>& required_scopes = {});
} // namespace mymcp::oauth
