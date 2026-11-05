#include "mymcp/oauth_jwt.hpp"
#include <httplib.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rsa.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace mymcp::oauth {
namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
const Json& field(const Json& j, std::string_view key) {
  static const Json null_value = nullptr;
  if (const auto* value = json::if_contains(j, key)) return *value;
  return null_value;
}
std::string str(const Json& j, std::string_view key) {
  const auto& v = field(j, key);
  return v.is_string() ? std::string(v.as_string()) : std::string{};
}
std::string decode_url64(std::string_view encoded) {
  if (encoded.empty() || encoded.size() > 16384 || encoded.size() % 4 == 1)
    throw std::invalid_argument("Invalid JWT base64url length");
  std::string input(encoded);
  for (char& c : input) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
    else if (!std::isalnum(static_cast<unsigned char>(c)))
      throw std::invalid_argument("Invalid JWT base64url character");
  }
  const auto pad = (4 - input.size() % 4) % 4;
  input.append(pad, '=');
  std::string output(input.size() / 4 * 3, '\0');
  const int written = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
    reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
  if (written < static_cast<int>(pad)) throw std::invalid_argument("Invalid JWT base64url encoding");
  output.resize(static_cast<std::size_t>(written) - pad);
  return output;
}
struct Url {std::string scheme, host, port, path;};
Url parse_trusted_url(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) throw std::invalid_argument("Invalid JWKS URI");
  Url out; out.scheme = url.substr(0, scheme_end);
  auto path_pos = url.find('/', scheme_end + 3);
  const auto authority = url.substr(scheme_end + 3,
        path_pos == std::string::npos ? path_pos : path_pos - scheme_end - 3);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority.find('#') != std::string::npos) throw std::invalid_argument("Invalid JWKS authority");
  if (authority.front() == '[') {
    auto close = authority.find(']');
    if (close == std::string::npos ||
        (close+1 < authority.size() && authority[close+1] != ':'))
      throw std::invalid_argument("Invalid JWKS IPv6 address");
    out.host = authority.substr(1, close - 1);
    out.port = close+1 == authority.size() ? (out.scheme == "https"?"443":"80") :
                                             authority.substr(close+2);
  } else {
    const auto colon = authority.find(':');
    out.host = authority.substr(0, colon);
    out.port = colon == std::string::npos ? (out.scheme == "https"?"443":"80") :
                                            authority.substr(colon+1);
  }
  out.path = path_pos == std::string::npos ? "/" : url.substr(path_pos);
  if (out.host.empty() || out.port.empty() || out.path.find('#') != std::string::npos ||
      (out.scheme != "https" && !(out.scheme == "http" &&
       (out.host == "127.0.0.1" || out.host == "::1" || out.host == "localhost"))))
    throw std::invalid_argument("JWKS URI requires HTTPS outside loopback");
  return out;
}
Json load_keys(const std::string& uri) {
  const auto url = parse_trusted_url(uri);
  const auto authority = url.host.find(':') != std::string::npos ?
      "[" + url.host + "]" : url.host;
  httplib::Client client(url.scheme + "://" + authority + ":" + url.port);
  client.enable_server_certificate_verification(true);
  client.set_follow_location(false);
  client.set_connection_timeout(5,0);
  client.set_read_timeout(10,0);
  std::string body;
  auto response = client.Get(url.path, [&](const char* bytes, std::size_t length) {
    if (length > 512*1024 - body.size()) return false;
    body.append(bytes,length);
    return true;
  });
  if (!response || response->status != 200 || body.empty())
    throw std::runtime_error("JWKS download failed");
  auto keys = Json::parse(body);
  if (!keys.is_object() || !field(keys,"keys").is_array() ||
      field(keys,"keys").size() > 64) throw std::runtime_error("Invalid JWKS");
  return keys;
}
Key rsa_key(const Json& jwk) {
  if (str(jwk,"kty") != "RSA") return {nullptr,EVP_PKEY_free};
  auto n = decode_url64(str(jwk,"n")), e = decode_url64(str(jwk,"e"));
  if (n.size() < 256 || e.empty() || e.size() > 8)
    throw std::runtime_error("Unsafe RSA JWK modulus/exponent");
  std::unique_ptr<BIGNUM,decltype(&BN_free)> bn_n(BN_bin2bn(
    reinterpret_cast<const unsigned char*>(n.data()),static_cast<int>(n.size()),nullptr),BN_free);
  std::unique_ptr<BIGNUM,decltype(&BN_free)> bn_e(BN_bin2bn(
    reinterpret_cast<const unsigned char*>(e.data()),static_cast<int>(e.size()),nullptr),BN_free);
  if (!bn_n || !bn_e || !BN_is_odd(bn_e.get()) || BN_is_one(bn_e.get()))
    throw std::runtime_error("Invalid RSA key");
  RSA* rsa=RSA_new();
  if (!rsa) throw std::runtime_error("Cannot allocate RSA");
  if (RSA_set0_key(rsa,bn_n.get(),bn_e.get(),nullptr)!=1) {
    RSA_free(rsa);throw std::runtime_error("Invalid RSA JWK");
  }
  bn_n.release();bn_e.release();
  Key key(EVP_PKEY_new(),EVP_PKEY_free);
  if (!key || EVP_PKEY_assign_RSA(key.get(),rsa)!=1) {
    RSA_free(rsa);throw std::runtime_error("Cannot import RSA JWK");
  }
  return key;
}
Key ec_key(const Json& jwk) {
  if (str(jwk,"kty") != "EC" || str(jwk,"crv") != "P-256")
    return {nullptr,EVP_PKEY_free};
  auto x=decode_url64(str(jwk,"x")), y=decode_url64(str(jwk,"y"));
  if(x.size()!=32||y.size()!=32)throw std::runtime_error("Invalid P-256 coordinates");
  EC_KEY* ec=EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if(!ec)throw std::runtime_error("Cannot allocate EC key");
  const EC_GROUP* group=EC_KEY_get0_group(ec);
  EC_POINT* point=EC_POINT_new(group);
  BIGNUM* bx=BN_bin2bn(reinterpret_cast<const unsigned char*>(x.data()),32,nullptr);
  BIGNUM* by=BN_bin2bn(reinterpret_cast<const unsigned char*>(y.data()),32,nullptr);
  bool success=point&&bx&&by&&
    EC_POINT_set_affine_coordinates(group,point,bx,by,nullptr)==1 &&
    EC_KEY_set_public_key(ec,point)==1 && EC_KEY_check_key(ec)==1;
  if(bx)BN_free(bx);if(by)BN_free(by);if(point)EC_POINT_free(point);
  if(!success){EC_KEY_free(ec);throw std::runtime_error("Invalid EC JWK");}
  Key key(EVP_PKEY_new(),EVP_PKEY_free);
  if(!key||EVP_PKEY_assign_EC_KEY(key.get(),ec)!=1){
    EC_KEY_free(ec);throw std::runtime_error("Cannot import EC JWK");
  }
  return key;
}
bool verify_signature(const Json& jwk, const std::string& alg,
                      std::string_view data, std::string signature) {
  if (!str(jwk,"use").empty() && str(jwk,"use") != "sig") return false;
  if (!str(jwk,"alg").empty() && str(jwk,"alg") != alg) return false;
  Key key = alg=="RS256" ? rsa_key(jwk) : ec_key(jwk);
  if(!key) return false;
  if(alg=="ES256"){
    if(signature.size()!=64)return false;
    ECDSA_SIG* sig=ECDSA_SIG_new();
    if(!sig)return false;
    BIGNUM* r=BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()),32,nullptr);
    BIGNUM* s=BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()+32),32,nullptr);
    if(!r||!s||ECDSA_SIG_set0(sig,r,s)!=1){
      if(r)BN_free(r);if(s)BN_free(s);ECDSA_SIG_free(sig);return false;
    }
    const int len=i2d_ECDSA_SIG(sig,nullptr);
    if(len<=0){ECDSA_SIG_free(sig);return false;}
    std::string der(len,'\0');
    auto* buffer=reinterpret_cast<unsigned char*>(der.data());
    i2d_ECDSA_SIG(sig,&buffer);
    ECDSA_SIG_free(sig);
    signature=std::move(der);
  }
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  return ctx && EVP_DigestVerifyInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key.get())==1 &&
         EVP_DigestVerifyUpdate(ctx.get(),data.data(),data.size())==1 &&
         EVP_DigestVerifyFinal(ctx.get(),reinterpret_cast<const unsigned char*>(signature.data()),
                               signature.size())==1;
}
bool valid_claims(const Json& claims, const JwtPolicy& p,
                  const std::string& method,const std::string& name) {
  if(!claims.is_object() || str(claims,"iss")!=p.issuer) return false;
  bool audience_match=false;
  const auto& aud=field(claims,"aud");
  if(aud.is_string()) audience_match=std::string(aud.as_string())==p.audience;
  if(aud.is_array()) for(const auto& a:aud.array_range())
    if(a.is_string() && a.as_string()==p.audience) audience_match=true;
  if(!audience_match)return false;
  const auto now=std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  const auto skew=p.clock_skew.count();
  const auto& exp=field(claims,"exp");
  if(!(exp.is_int64()||exp.is_uint64()) || exp.as_integer<std::int64_t>() <= now-skew)
    return false;
  for(const char* claim : {"nbf","iat"}){
    const auto& value=field(claims,claim);
    if(!value.is_null() && (!(value.is_int64()||value.is_uint64()) ||
       value.as_integer<std::int64_t>() > now+skew)) return false;
  }
  std::set<std::string> token_scopes;
  if(field(claims,"scope").is_string()){
    std::istringstream words(str(claims,"scope"));std::string word;
    while(words>>word)token_scopes.insert(word);
  }
  if(field(claims,"scp").is_array())for(const auto& scope:field(claims,"scp").array_range())
    if(scope.is_string())token_scopes.insert(scope.as_string());
  std::vector<std::string> required=p.default_scopes;
  auto found=p.required_scopes.find(method+":"+name);
  if(found==p.required_scopes.end())found=p.required_scopes.find(method);
  if(found!=p.required_scopes.end())required.insert(required.end(),found->second.begin(),found->second.end());
  for(const auto& scope:required)if(!token_scopes.contains(scope))return false;
  return true;
}
} // namespace

JwtVerifier::JwtVerifier(JwtPolicy policy):policy_(std::move(policy)){
  if(policy_.issuer.empty()||policy_.audience.empty()||policy_.jwks_uri.empty() ||
     policy_.clock_skew.count()<0||policy_.clock_skew.count()>300 ||
     policy_.cache_ttl.count()<10||policy_.cache_ttl.count()>3600)
    throw std::invalid_argument("Invalid JWT verification policy");
  (void)parse_trusted_url(policy_.jwks_uri);
}
Json JwtVerifier::trusted_keys(bool force_refresh){
  std::lock_guard lock(mutex_);
  const auto now=std::chrono::steady_clock::now();
  if(!keys_.is_null() && now-refreshed_at_ < (force_refresh ?
       std::chrono::seconds(10) : policy_.cache_ttl)) return keys_;
  keys_=load_keys(policy_.jwks_uri);
  refreshed_at_=now;
  return keys_;
}
bool JwtVerifier::verify(const std::string& token, const std::string& method,
                         const std::string& name) noexcept {
  try {
    if(token.size()>16384)return false;
    const auto first=token.find('.'),second=token.find('.',first+1);
    if(first==std::string::npos||second==std::string::npos||
       token.find('.',second+1)!=std::string::npos)return false;
    const auto header=Json::parse(decode_url64(std::string_view(token).substr(0,first)));
    const auto claims=Json::parse(decode_url64(std::string_view(token).substr(first+1,second-first-1)));
    const auto algorithm=str(header,"alg"),kid=str(header,"kid");
    if((algorithm!="RS256"&&algorithm!="ES256")||kid.empty()||kid.size()>256)return false;
    if(!valid_claims(claims,policy_,method,name))return false;
    const auto signature=decode_url64(std::string_view(token).substr(second+1));
    for(int attempt=0;attempt<2;++attempt){
      Json jwks=trusted_keys(attempt!=0);
      bool found=false;
      for(const auto& key:field(jwks,"keys").array_range()){
        if(str(key,"kid")!=kid)continue;
        found=true;
        // 不允许签名失败后改用其他 kid 或非可信 header 提供的密钥。
        return verify_signature(key,algorithm,std::string_view(token).substr(0,second),signature);
      }
      if(found)return false;
    }
  } catch (...) {return false;}
  return false;
}
} // namespace mymcp::oauth
