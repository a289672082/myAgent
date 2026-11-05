#include "mymcp/oauth.hpp"
#include "mymcp/mcp.hpp"
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <memory>
#include <vector>
#include <ctime>
#include <stdexcept>
#include <iostream>
using namespace mymcp::oauth;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("assertion failed: " #x); } while(0)
int main() {
  auto flow = begin("https://auth.example/authorize", "https://auth.example", "https://app.example/client-id.json",
                    "http://127.0.0.1:3201/callback", "https://mcp.example/mcp", {"files:read", "files:write"}, true);
  CHECK(flow.state.size() >= 32 && flow.code_verifier.size() >= 43);
  CHECK(flow.url.find("code_challenge_method=S256") != std::string::npos);
  CHECK(flow.url.find("resource=https%3A%2F%2Fmcp.example%2Fmcp") != std::string::npos);
  CHECK(flow.url.find("scope=files%3Aread%20files%3Awrite") != std::string::npos);
  validate_callback(flow, flow.state, "https://auth.example");
  try {validate_callback(flow, "wrong", "https://auth.example"); CHECK(false);} catch (const std::invalid_argument&) {}
  try {validate_callback(flow, flow.state, "https://evil.example"); CHECK(false);} catch (const std::invalid_argument&) {}
  try {validate_callback(flow, flow.state, ""); CHECK(false);} catch (const std::invalid_argument&) {}
  flow.issuer_parameter_supported = false;
  validate_callback(flow,flow.state,"");
  try {begin("http://evil.example", "https://auth.example", "id", "http://127.0.0.1", "https://mcp.example"); CHECK(false);} catch(const std::invalid_argument&){}
  try {begin("https://auth.example/authorize", "https://auth.example", "id",
      "http://127.0.0.1/callback", "http://localhost.attacker.example/mcp"); CHECK(false);
  } catch(const std::invalid_argument&){}
  // 使用测试进程生成的 RSA 密钥对验证 JWT/JWKS，绝不依赖外部凭据。
  auto keygen = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
    EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  CHECK(keygen && EVP_PKEY_keygen_init(keygen.get()) == 1);
  CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(keygen.get(), 2048) == 1);
  EVP_PKEY* raw_key = nullptr;
  CHECK(EVP_PKEY_keygen(keygen.get(), &raw_key) == 1);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
  std::unique_ptr<RSA, decltype(&RSA_free)> rsa(EVP_PKEY_get1_RSA(key.get()), RSA_free);
  CHECK(rsa != nullptr);
  const BIGNUM *n = nullptr, *e = nullptr;
  RSA_get0_key(rsa.get(), &n, &e, nullptr);
  auto b64 = [](const unsigned char* data, std::size_t size) {
    std::string text(((size + 2) / 3) * 4, '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()), data, static_cast<int>(size));
    while (!text.empty() && text.back() == '=') text.pop_back();
    for (auto& c : text) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    return text;
  };
  auto bn64 = [&](const BIGNUM* value) {
    std::vector<unsigned char> data(static_cast<std::size_t>(BN_num_bytes(value)));
    CHECK(BN_bn2bin(value, data.data()) == static_cast<int>(data.size()));
    return b64(data.data(), data.size());
  };
  const mymcp::Json jwks = mymcp::json::object{{"keys", mymcp::json::array{
    mymcp::json::object{{"kid", "test-2026"}, {"kty", "RSA"}, {"alg", "RS256"},
                         {"use", "sig"}, {"n", bn64(n)}, {"e", bn64(e)}}}}};
  const auto header = b64(reinterpret_cast<const unsigned char*>(
    R"({"alg":"RS256","kid":"test-2026","typ":"JWT"})"),
    std::string(R"({"alg":"RS256","kid":"test-2026","typ":"JWT"})").size());
  const std::string claims = mymcp::json::serialize(mymcp::json::object{
    {"iss", "https://auth.example"}, {"aud", "https://mcp.example/mcp"},
    {"scope", "files:read files:write"}, {"exp", static_cast<std::int64_t>(std::time(nullptr)) + 3600}});
  const std::string signing_input = header + "." +
    b64(reinterpret_cast<const unsigned char*>(claims.data()), claims.size());
  auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  CHECK(ctx && EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1);
  CHECK(EVP_DigestSignUpdate(ctx.get(), signing_input.data(), signing_input.size()) == 1);
  std::size_t signature_size = 0;
  CHECK(EVP_DigestSignFinal(ctx.get(), nullptr, &signature_size) == 1);
  std::vector<unsigned char> signature(signature_size);
  CHECK(EVP_DigestSignFinal(ctx.get(), signature.data(), &signature_size) == 1);
  const auto jwt = signing_input + "." + b64(signature.data(), signature_size);
  CHECK(verify_jwt_rs256(jwt, jwks, "https://auth.example", "https://mcp.example/mcp", {"files:read"}));
  CHECK(!verify_jwt_rs256(jwt, jwks, "https://wrong.example", "https://mcp.example/mcp"));
  CHECK(!verify_jwt_rs256(jwt, jwks, "https://auth.example", "https://wrong.example"));
  CHECK(!verify_jwt_rs256(jwt, jwks, "https://auth.example", "https://mcp.example/mcp", {"files:admin"}));
  std::string tampered = jwt; tampered[tampered.size()-2] = tampered[tampered.size()-2] == 'A' ? 'B' : 'A';
  CHECK(!verify_jwt_rs256(tampered, jwks, "https://auth.example", "https://mcp.example/mcp"));
  CHECK(!verify_jwt_rs256("eyJhbGciOiJub25lIn0.e30.", jwks, "https://auth.example", "https://mcp.example/mcp"));

  mymcp::Server protected_server("oauth-protected");
  protected_server.protect_http_jwks(
    mymcp::json::object{{"resource", "https://mcp.example/mcp"},
      {"authorization_servers", mymcp::json::array{"https://auth.example"}}},
    jwks, "https://auth.example", "https://mcp.example/mcp", {"files:read"});
  CHECK(protected_server.verify_http(jwt, "tools/call", "read_file"));
  CHECK(!protected_server.verify_http(tampered, "tools/call", "read_file"));
  std::cout << "OAuth PKCE/issuer tests passed\n";
}
