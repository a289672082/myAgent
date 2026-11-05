#include "mymcp/oauth.hpp"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <ctime>
#include <memory>
#include <set>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <string_view>
#include <algorithm>

namespace mymcp::oauth {
namespace {
std::string base64url(const unsigned char* bytes, std::size_t size) {
  std::string b64(4 * ((size + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64.data()), bytes, static_cast<int>(size));
  while (!b64.empty() && b64.back() == '=') b64.pop_back();
  for (auto& c : b64) {if (c == '+') c = '-'; if (c == '/') c = '_';}
  return b64;
}
std::string nonce(std::size_t bytes) {
  std::string random(bytes, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), static_cast<int>(bytes)) != 1)
    throw std::runtime_error("CSPRNG failure");
  return base64url(reinterpret_cast<const unsigned char*>(random.data()), bytes);
}
void require_https(const std::string& url) {
  if (url.find('#') != std::string::npos) throw std::invalid_argument("OAuth endpoint must not have a fragment");
  if (url.starts_with("https://")) return;
  // OAuth local-only development/testing is permitted over HTTP, but not any
  // non-loopback host, and not a deceptive name such as localhost.example.com.
  if (url.starts_with("http://")) {
    const auto end = url.find_first_of("/?#", 7);
    const std::string authority = url.substr(7, end == std::string::npos ? end : end - 7);
    auto is_loopback_authority = [&](const std::string& host) {
      if (authority == host) return true;
      if (!authority.starts_with(host + ":")) return false;
      const auto port = authority.substr(host.size() + 1);
      return !port.empty() && std::all_of(port.begin(), port.end(), [](unsigned char c){ return c >= '0' && c <= '9'; });
    };
    if (is_loopback_authority("127.0.0.1") || is_loopback_authority("localhost") ||
        is_loopback_authority("[::1]")) return;
  }
  throw std::invalid_argument("OAuth endpoints must use HTTPS (except loopback)");
}
}
std::string url_encode(const std::string& input) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (unsigned char c : input) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') out << char(c);
    else out << '%' << std::setw(2) << std::setfill('0') << int(c);
  }
  return out.str();
}
Authorization begin(const std::string& authorization_endpoint,
                    const std::string& expected_issuer,
                    const std::string& client_id,
                    const std::string& redirect_uri,
                    const std::string& resource,
                    const std::vector<std::string>& scopes,
                    bool issuer_parameter_supported) {
  require_https(authorization_endpoint);
  require_https(expected_issuer);
  if (client_id.empty() || redirect_uri.empty() || resource.empty())
    throw std::invalid_argument("OAuth requires client_id, redirect_uri and resource");
  // Avoid prefix matches such as http://localhost.attacker.example.
  require_https(resource);
  Authorization flow;
  flow.state = nonce(32);
  flow.code_verifier = nonce(48);
  flow.expected_issuer = expected_issuer;
  flow.issuer_parameter_supported = issuer_parameter_supported;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (EVP_Digest(flow.code_verifier.data(), flow.code_verifier.size(),
                 digest, &len, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("PKCE SHA-256 failure");
  const auto challenge = base64url(digest, len);
  std::string scope;
  for (const auto& item : scopes) {
    if (!scope.empty()) scope += ' ';
    scope += item;
  }
  flow.url = authorization_endpoint;
  flow.url += (flow.url.find('?') == std::string::npos ? '?' : '&');
  flow.url += "response_type=code&client_id=" + url_encode(client_id) +
              "&redirect_uri=" + url_encode(redirect_uri) +
              "&code_challenge_method=S256&code_challenge=" + url_encode(challenge) +
              "&state=" + url_encode(flow.state) + "&resource=" + url_encode(resource);
  if (!scope.empty()) flow.url += "&scope=" + url_encode(scope);
  return flow;
}
void validate_callback(const Authorization& flow, const std::string& state, const std::string& issuer) {
  if (state.size() != flow.state.size() ||
      CRYPTO_memcmp(state.data(), flow.state.data(), state.size()) != 0)
    throw std::invalid_argument("OAuth callback state mismatch");
  if ((flow.issuer_parameter_supported && issuer.empty()) ||
      (!issuer.empty() && issuer != flow.expected_issuer))
    throw std::invalid_argument("OAuth callback issuer mismatch or missing issuer");
}

namespace {
std::optional<std::string> decode_url64(std::string_view s) {
  if (s.empty() || s.size() > 32768 || s.size() % 4 == 1) return std::nullopt;
  std::string normalized(s);
  for (char& c : normalized) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
    else if (!std::isalnum(static_cast<unsigned char>(c))) return std::nullopt;
  }
  const auto rem = normalized.size() % 4;
  if (rem) normalized.append(4 - rem, '=');
  std::string decoded(normalized.size() / 4 * 3, '\0');
  const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                reinterpret_cast<const unsigned char*>(normalized.data()),
                                static_cast<int>(normalized.size()));
  if (n < 0) return std::nullopt;
  decoded.resize(static_cast<std::size_t>(n) - (rem ? 4 - rem : 0));
  return decoded;
}
std::string get_string(const json::value& j, std::string_view k) {
  if (!j.is_object()) return {};
  auto* v = json::if_contains(j, k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string{};
}
bool has_audience(const json::value& payload, const std::string& audience) {
  auto* aud = json::if_contains(payload, "aud");
  if (!aud) return false;
  if (aud->is_string()) return aud->as_string() == audience;
  if (aud->is_array()) {
    for (const auto& item : aud->array_range())
      if (item.is_string() && item.as_string() == audience) return true;
  }
  return false;
}
bool claims_valid(const json::value& payload, const std::string& issuer,
                  const std::string& audience, const std::vector<std::string>& scopes) {
  if (!payload.is_object() || issuer.empty() || audience.empty() ||
      get_string(payload, "iss") != issuer || !has_audience(payload, audience))
    return false;
  const auto* exp = json::if_contains(payload, "exp");
  if (!exp || !(exp->is_int64() || exp->is_uint64())) return false;
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  const std::int64_t expiry = exp->as_integer<std::int64_t>();
  if (expiry <= now) return false;
  if (auto* nbf = json::if_contains(payload, "nbf"); nbf) {
    if (!(nbf->is_int64() || nbf->is_uint64()) ||
        nbf->as_integer<std::int64_t>() > now + 30) return false;
  }
  if (auto* iat = json::if_contains(payload, "iat"); iat) {
    if (!(iat->is_int64() || iat->is_uint64()) ||
        iat->as_integer<std::int64_t>() > now + 30) return false;
  }
  const auto* scope = json::if_contains(payload, "scope");
  std::set<std::string> granted;
  if (scope && scope->is_string()) {
    std::istringstream in(std::string(scope->as_string()));
    std::string token;
    while (in >> token) granted.insert(token);
  }
  for (const auto& required : scopes) if (!granted.contains(required)) return false;
  return true;
}
} // namespace

bool verify_jwt_rs256(const std::string& token, const json::value& jwks,
                      const std::string& expected_issuer,
                      const std::string& expected_audience,
                      const std::vector<std::string>& required_scopes) {
  // 仅从可信、明确传入的 JWKS 选择密钥。绝不访问 JWT 自带 URL。
  try {
    if (token.size() > 65536 || !jwks.is_object()) return false;
    auto first = token.find('.');
    if (first == std::string::npos) return false;
    auto second = token.find('.', first + 1);
    if (second == std::string::npos || token.find('.', second + 1) != std::string::npos) return false;
    auto header_bytes = decode_url64(std::string_view(token).substr(0, first));
    auto claims_bytes = decode_url64(std::string_view(token).substr(first + 1, second - first - 1));
    auto signature = decode_url64(std::string_view(token).substr(second + 1));
    if (!header_bytes || !claims_bytes || !signature) return false;
    auto header = json::parse(*header_bytes);
    auto claims = json::parse(*claims_bytes);
    if (get_string(header, "alg") != "RS256" ||
        !claims_valid(claims, expected_issuer, expected_audience, required_scopes))
      return false;
    const auto kid = get_string(header, "kid");
    if (kid.empty()) return false; // 防止多密钥情况下的密钥混淆
    auto* keys = json::if_contains(jwks, "keys");
    if (!keys || !keys->is_array() || keys->size() > 128) return false;
    for (const auto& key : keys->array_range()) {
      if (get_string(key, "kid") != kid || get_string(key, "kty") != "RSA") continue;
      if (auto* alg = json::if_contains(key, "alg"); alg &&
          (!alg->is_string() || alg->as_string() != "RS256")) return false;
      if (auto* use = json::if_contains(key, "use"); use &&
          (!use->is_string() || use->as_string() != "sig")) return false;
      auto modulus = decode_url64(get_string(key, "n"));
      auto exponent = decode_url64(get_string(key, "e"));
      if (!modulus || !exponent || modulus->size() < 256 ||
          modulus->size() > 1024 || exponent->empty() || exponent->size() > 8) return false;
      std::unique_ptr<BIGNUM, decltype(&BN_free)> n(
        BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus->data()),
                  static_cast<int>(modulus->size()), nullptr), BN_free);
      std::unique_ptr<BIGNUM, decltype(&BN_free)> e(
        BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent->data()),
                  static_cast<int>(exponent->size()), nullptr), BN_free);
      if (!n || !e || !BN_is_odd(e.get()) || BN_is_one(e.get())) return false;
      std::unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
      if (!rsa || RSA_set0_key(rsa.get(), n.get(), e.get(), nullptr) != 1) return false;
      n.release(); e.release();
      std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(EVP_PKEY_new(), EVP_PKEY_free);
      if (!public_key || EVP_PKEY_assign_RSA(public_key.get(), rsa.get()) != 1) return false;
      rsa.release();
      std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                                      public_key.get()) != 1) return false;
      const auto signed_data = token.substr(0, second);
      if (EVP_DigestVerifyUpdate(ctx.get(), signed_data.data(), signed_data.size()) != 1)
        return false;
      return EVP_DigestVerifyFinal(ctx.get(),
                reinterpret_cast<const unsigned char*>(signature->data()),
                signature->size()) == 1;
    }
    return false;
  } catch (...) {
    return false; // 未通过验证的凭据一律拒绝
  }
}

}
