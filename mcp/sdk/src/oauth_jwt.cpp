#include "mymcp/oauth_jwt.hpp"
#include <httplib.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mymcp::oauth {
namespace {
using Json = json::value;

std::string field(const Json& object, const char* name) {
  if (!object.is_object() || !object.contains(name) || !object.at(name).is_string())
    return {};
  return std::string(object.at(name).as_string());
}

std::string decode_base64url(std::string_view input) {
  if (input.empty() || input.size() > 1024 * 1024 || input.size() % 4 == 1)
    throw std::invalid_argument("Invalid base64url length");
  std::string encoded;
  encoded.reserve(input.size() + 3);
  for (unsigned char ch : input) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) encoded.push_back(static_cast<char>(ch));
    else if (ch == '-') encoded.push_back('+');
    else if (ch == '_') encoded.push_back('/');
    else throw std::invalid_argument("Invalid base64url alphabet");
  }
  const auto padding = (4 - encoded.size() % 4) % 4;
  encoded.append(padding, '=');
  std::string output(encoded.size() / 4 * 3, '\0');
  int count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                              reinterpret_cast<const unsigned char*>(encoded.data()),
                              static_cast<int>(encoded.size()));
  if (count < 0 || count < static_cast<int>(padding))
    throw std::invalid_argument("Invalid base64url encoding");
  output.resize(static_cast<std::size_t>(count) - padding);
  return output;
}

using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bignum = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using Rsa = std::unique_ptr<RSA, decltype(&RSA_free)>;
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

Pkey rsa_key(const Json& jwk) {
  if (field(jwk,"kty") != "RSA" ||
      (!field(jwk,"use").empty() && field(jwk,"use") != "sig") ||
      (!field(jwk,"alg").empty() && field(jwk,"alg") != "RS256"))
    throw std::invalid_argument("Unsupported JWK");
  auto n_bytes = decode_base64url(field(jwk,"n"));
  auto e_bytes = decode_base64url(field(jwk,"e"));
  if (n_bytes.size() < 256 || n_bytes.size() > 1024 || e_bytes.empty() || e_bytes.size() > 8)
    throw std::invalid_argument("Invalid RSA JWK key length");
  Bignum n(BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes.data()),
                      static_cast<int>(n_bytes.size()), nullptr), BN_free);
  Bignum e(BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes.data()),
                      static_cast<int>(e_bytes.size()), nullptr), BN_free);
  Rsa rsa(RSA_new(), RSA_free);
  if (!n || !e || !rsa || !BN_is_odd(e.get()) || BN_is_zero(e.get()) ||
      RSA_set0_key(rsa.get(), n.get(), e.get(), nullptr) != 1)
    throw std::runtime_error("Invalid RSA JWK components");
  n.release();
  e.release();
  Pkey key(EVP_PKEY_new(), EVP_PKEY_free);
  if (!key || EVP_PKEY_assign_RSA(key.get(), rsa.get()) != 1)
    throw std::runtime_error("Cannot import RSA JWK");
  rsa.release();
  if (EVP_PKEY_bits(key.get()) < 2048)
    throw std::invalid_argument("RSA key is too short");
  return key;
}

bool numeric_time(const Json& claims, const char* name, std::int64_t& seconds) {
  if (!claims.is_object() || !claims.contains(name)) return false;
  const auto& value=claims.at(name);
  if (value.is_int64()) {seconds=value.as_integer<std::int64_t>();return true;}
  if (value.is_uint64()) {
    const auto unsigned_value=value.as_integer<std::uint64_t>();
    if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return false;
    seconds=static_cast<std::int64_t>(unsigned_value);
    return true;
  }
  return false;
}

bool has_audience(const Json& claims, const std::string& audience) {
  if (!claims.is_object() || !claims.contains("aud")) return false;
  const auto& aud=claims.at("aud");
  if (aud.is_string()) return aud.as_string() == audience;
  if (!aud.is_array()) return false;
  for (const auto& candidate : aud.array_range())
    if (candidate.is_string() && candidate.as_string() == audience) return true;
  return false;
}

bool has_scopes(const Json& claims, const std::vector<std::string>& required) {
  if (required.empty()) return true;
  std::vector<std::string> scopes;
  if (const auto scope=field(claims,"scope"); !scope.empty()) {
    std::istringstream stream(scope);
    for (std::string name; stream >> name;) scopes.push_back(std::move(name));
  }
  if (claims.is_object() && claims.contains("scp") && claims.at("scp").is_array())
    for (const auto& entry : claims.at("scp").array_range())
      if (entry.is_string()) scopes.push_back(std::string(entry.as_string()));
  for (const auto& needed : required)
    if (std::find(scopes.begin(),scopes.end(),needed)==scopes.end()) return false;
  return true;
}
} // namespace

bool verify_jwt_rs256(const std::string& token, const Json& jwks,
                      const std::string& expected_issuer,
                      const std::string& expected_audience,
                      const std::vector<std::string>& required_scopes) {
  try {
    if (token.size() > 64 * 1024 || expected_issuer.empty() || expected_audience.empty() ||
        !jwks.is_object() || !jwks.contains("keys") || !jwks.at("keys").is_array())
      return false;
    const auto p1=token.find('.');
    if(p1==std::string::npos)return false;
    const auto p2=token.find('.',p1+1);
    if(p2==std::string::npos || token.find('.',p2+1)!=std::string::npos)return false;
    const Json header=Json::parse(decode_base64url(std::string_view(token).substr(0,p1)));
    const Json claims=Json::parse(decode_base64url(std::string_view(token).substr(p1+1,p2-p1-1)));
    const std::string kid=field(header,"kid");
    if (field(header,"alg")!="RS256" || kid.empty() ||
        header.contains("jku") || header.contains("x5u") || header.contains("crit"))
      return false;
    if(field(claims,"iss")!=expected_issuer || !has_audience(claims,expected_audience) ||
       !has_scopes(claims,required_scopes))return false;
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    constexpr std::int64_t skew=60;
    std::int64_t exp=0,nbf=0,iat=0;
    if(!numeric_time(claims,"exp",exp) || exp<=now-skew)return false;
    if(claims.contains("nbf") && (!numeric_time(claims,"nbf",nbf) || nbf>now+skew))return false;
    if(claims.contains("iat") && (!numeric_time(claims,"iat",iat) || iat>now+skew))return false;
    const Json* matching=nullptr;
    for(const auto& candidate : jwks.at("keys").array_range()) {
      if(field(candidate,"kid")!=kid)continue;
      if(matching) return false; // 同名 kid 不允许选择性验证
      matching=&candidate;
    }
    if(!matching)return false;
    auto key=rsa_key(*matching);
    const auto signature=decode_base64url(std::string_view(token).substr(p2+1));
    const auto signed_data=std::string_view(token).substr(0,p2);
    Digest context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!context || EVP_DigestVerifyInit(context.get(),nullptr,EVP_sha256(),
                                      nullptr,key.get())!=1)return false;
    return EVP_DigestVerify(context.get(),
             reinterpret_cast<const unsigned char*>(signature.data()),signature.size(),
             reinterpret_cast<const unsigned char*>(signed_data.data()),signed_data.size())==1;
  } catch (...) {return false;}
}

Json fetch_jwks(const std::string& trusted_jwks_uri) {
  const auto scheme_end=trusted_jwks_uri.find("://");
  if(scheme_end==std::string::npos || trusted_jwks_uri.find('#')!=std::string::npos)
    throw std::invalid_argument("Invalid JWKS URL");
  const auto scheme=trusted_jwks_uri.substr(0,scheme_end);
  const auto authority_begin=scheme_end+3;
  const auto path_begin=trusted_jwks_uri.find('/',authority_begin);
  const auto authority=trusted_jwks_uri.substr(authority_begin,
      path_begin==std::string::npos?std::string::npos:path_begin-authority_begin);
  if(authority.empty() || authority.find('@')!=std::string::npos ||
     authority.find('?')!=std::string::npos)
    throw std::invalid_argument("Invalid JWKS authority");
  const auto host=authority.substr(0,authority.find(':'));
  const bool local=(host=="127.0.0.1" || host=="localhost" || authority.starts_with("[::1]"));
  if(scheme!="https" && !(scheme=="http" && local))
    throw std::invalid_argument("JWKS endpoint requires HTTPS");
  auto c=std::make_unique<httplib::Client>(scheme+"://"+authority);
  c->enable_server_certificate_verification(true);
  c->set_follow_location(false);
  c->set_connection_timeout(5,0);
  c->set_read_timeout(10,0);
  std::string content;
  auto response=c->Get(path_begin==std::string::npos?"/":trusted_jwks_uri.substr(path_begin),
    [&](const char* bytes,std::size_t len) {
      if(len>1024*1024 || content.size()>1024*1024-len)return false;
      content.append(bytes,len);
      return true;
    });
  if(!response || response->status!=200 || content.empty())
    throw std::runtime_error("JWKS retrieval failed");
  Json jwks=Json::parse(content);
  if(!jwks.is_object() || !jwks.contains("keys") || !jwks.at("keys").is_array())
    throw std::runtime_error("Invalid JWKS document");
  return jwks;
}
} // namespace mymcp::oauth
