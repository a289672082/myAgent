#include "mymcp/jwt.hpp"
#include <httplib.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <set>

namespace mymcp::oauth {
namespace {
std::string field(const Json& j, std::string_view name) {
  if (!j.is_object()) return {};
  const Json* p = json::if_contains(j, name);
  return p && p->is_string() ? std::string(p->as_string()) : std::string{};
}
std::vector<unsigned char> b64url(std::string_view encoded) {
  if (encoded.empty() || encoded.size() > 1024 * 1024 ||
      encoded.size() % 4 == 1) throw std::invalid_argument("Invalid base64url length");
  std::string normal;
  normal.reserve(encoded.size()+3);
  for (unsigned char c : encoded) {
    if (std::isalnum(c)) normal += static_cast<char>(c);
    else if (c == '-') normal += '+';
    else if (c == '_') normal += '/';
    else throw std::invalid_argument("Invalid base64url alphabet");
  }
  const auto padding = (4-normal.size()%4)%4;
  normal.append(padding, '=');
  std::vector<unsigned char> bytes(normal.size()*3/4+3);
  const int size = EVP_DecodeBlock(bytes.data(),
      reinterpret_cast<const unsigned char*>(normal.data()),
      static_cast<int>(normal.size()));
  if (size < 0 || static_cast<std::size_t>(size) < padding)
    throw std::invalid_argument("Invalid base64url payload");
  bytes.resize(static_cast<std::size_t>(size)-padding);
  return bytes;
}
bool numeric_time(const Json& claims, std::string_view key, std::int64_t& value) {
  if (!claims.is_object()) return false;
  const Json* p = json::if_contains(claims,key);
  if (!p || (!p->is_int64() && !p->is_uint64())) return false;
  if (p->is_uint64() && p->as_integer<std::uint64_t>() >
                          static_cast<std::uint64_t>(INT64_MAX)) return false;
  value = p->as_integer<std::int64_t>();
  return true;
}
bool has_scope(const Json& claims, const std::string& required) {
  const auto scope = field(claims,"scope");
  std::size_t from=0;
  while (from < scope.size()) {
    while (from < scope.size() && scope[from]==' ') ++from;
    auto end=scope.find(' ',from);
    if (end==std::string::npos) end=scope.size();
    if (scope.substr(from,end-from)==required) return true;
    from=end+1;
  }
  if (claims.is_object() && claims.contains("scp") && claims.at("scp").is_array()) {
    for (const auto& item : claims.at("scp").array_range())
      if (item.is_string() && item.as_string()==required) return true;
  }
  return false;
}
bool audience_matches(const Json& claims, const std::string& expected) {
  if (!claims.is_object() || !claims.contains("aud")) return false;
  const Json& aud=claims.at("aud");
  if (aud.is_string()) return aud.as_string()==expected;
  if (aud.is_array()) for (const auto& entry : aud.array_range())
    if (entry.is_string() && entry.as_string()==expected) return true;
  return false;
}
struct KeyDeleter {void operator()(EVP_PKEY* key) const {EVP_PKEY_free(key);}};
struct RsaDeleter {void operator()(RSA* rsa) const {RSA_free(rsa);}};
struct BnDeleter {void operator()(BIGNUM* b) const {BN_free(b);}};
struct ContextDeleter {void operator()(EVP_MD_CTX* ctx) const {EVP_MD_CTX_free(ctx);}};
using Pkey = std::unique_ptr<EVP_PKEY,KeyDeleter>;
using Rsa = std::unique_ptr<RSA,RsaDeleter>;
using Bn = std::unique_ptr<BIGNUM,BnDeleter>;
using Context = std::unique_ptr<EVP_MD_CTX,ContextDeleter>;
Pkey rsa_public_key(const Json& jwk) {
  if (field(jwk,"kty")!="RSA") return {};
  const std::string n=field(jwk,"n"), e=field(jwk,"e");
  if (n.empty()||e.empty()) return {};
  const auto modulus=b64url(n), exponent=b64url(e);
  if (modulus.size() < 256 || modulus.size() > 1024 ||
      exponent.empty() || exponent.size() > 8) return {};
  Bn bn_n(BN_bin2bn(modulus.data(),static_cast<int>(modulus.size()),nullptr));
  Bn bn_e(BN_bin2bn(exponent.data(),static_cast<int>(exponent.size()),nullptr));
  if (!bn_n || !bn_e || BN_num_bits(bn_n.get()) < 2048 ||
      !BN_is_odd(bn_e.get()) || BN_is_one(bn_e.get())) return {};
  Rsa rsa(RSA_new());
  if (!rsa || RSA_set0_key(rsa.get(), bn_n.get(), bn_e.get(), nullptr)!=1)
    return {};
  bn_n.release();bn_e.release(); // RSA owns both BIGNUMs.
  Pkey key(EVP_PKEY_new());
  if (!key || EVP_PKEY_assign_RSA(key.get(),rsa.get())!=1) return {};
  rsa.release();
  return key;
}
bool verify_signature(std::string_view signed_data,
                      const std::vector<unsigned char>& signature, EVP_PKEY* key) {
  if (!key || signature.empty()) return false;
  Context ctx(EVP_MD_CTX_new());
  if (!ctx ||
      EVP_DigestVerifyInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key)!=1 ||
      EVP_DigestVerifyUpdate(ctx.get(),signed_data.data(),signed_data.size())!=1)
    return false;
  return EVP_DigestVerifyFinal(ctx.get(),signature.data(),signature.size())==1;
}
} // namespace

JwtVerifier::JwtVerifier(Json jwks, JwtPolicy policy)
    : jwks_(std::move(jwks)), policy_(std::move(policy)) {
  if (policy_.issuer.empty() || policy_.audience.empty() ||
      policy_.clock_skew < std::chrono::seconds(0) ||
      policy_.clock_skew > std::chrono::minutes(5) ||
      !jwks_.is_object() || !jwks_.contains("keys") || !jwks_.at("keys").is_array())
    throw std::invalid_argument("JWT verification needs pinned issuer, audience and a JWKS keys array");
}
bool JwtVerifier::verify(const std::string& token,
                         const std::vector<std::string>& additional_scopes) const noexcept {
  try {
    if (token.size() > 1024*1024) return false;
    const auto first=token.find('.'),second=token.find('.',first==std::string::npos?first:first+1);
    if (first==std::string::npos || second==std::string::npos ||
        token.find('.',second+1)!=std::string::npos) return false;
    const auto hd=b64url(std::string_view(token).substr(0,first));
    const auto pl=b64url(std::string_view(token).substr(first+1,second-first-1));
    const auto signature=b64url(std::string_view(token).substr(second+1));
    const Json header=Json::parse(std::string(hd.begin(),hd.end()));
    const Json claims=Json::parse(std::string(pl.begin(),pl.end()));
    if (field(header,"alg")!="RS256" || field(header,"kid").empty() ||
        field(header,"kid").size()>128 || field(claims,"iss")!=policy_.issuer ||
        !audience_matches(claims,policy_.audience)) return false;
    std::int64_t exp=0,nbf=0,iat=0;
    const auto now=static_cast<std::int64_t>(std::time(nullptr));
    const auto skew=policy_.clock_skew.count();
    if (!numeric_time(claims,"exp",exp) || exp <= now-skew) return false;
    if (claims.contains("nbf") && (!numeric_time(claims,"nbf",nbf) || nbf > now+skew))
      return false;
    if (claims.contains("iat") && (!numeric_time(claims,"iat",iat) || iat > now+skew))
      return false;
    if (!std::all_of(policy_.required_scopes.begin(),policy_.required_scopes.end(),
                     [&](const auto& s){return has_scope(claims,s);}) ||
        !std::all_of(additional_scopes.begin(),additional_scopes.end(),
                     [&](const auto& s){return has_scope(claims,s);})) return false;
    const Json* match=nullptr;
    for (const auto& key : jwks_.at("keys").array_range()) {
      if (field(key,"kid")==field(header,"kid")) {
        if (match) return false; // Duplicate key ID is ambiguous.
        match=&key;
      }
    }
    if (!match || (match->contains("alg") && field(*match,"alg")!="RS256") ||
        (match->contains("use") && field(*match,"use")!="sig")) return false;
    const auto key=rsa_public_key(*match);
    return verify_signature(std::string_view(token).substr(0,second),signature,key.get());
  } catch (...) {return false;}
}
Json fetch_jwks_https(const std::string& url) {
  if (!url.starts_with("https://") || url.size()>8192 ||
      url.find('#')!=std::string::npos)
    throw std::invalid_argument("JWKS must use HTTPS");
  const auto start=std::string_view("https://").size();
  const auto slash=url.find('/',start);
  const auto authority=url.substr(start,slash==std::string::npos?slash:slash-start);
  if (authority.empty() || authority.find('@')!=std::string::npos ||
      authority.find('\\')!=std::string::npos)
    throw std::invalid_argument("Invalid JWKS HTTPS origin");
  const auto path=slash==std::string::npos?std::string{"/"}:url.substr(slash);
  httplib::Client client("https://"+authority);
  client.enable_server_certificate_verification(true);
  client.set_follow_location(false);
  client.set_connection_timeout(5,0);
  client.set_read_timeout(10,0);
  std::string received;
  bool too_large=false;
  auto response=client.Get(path,[&](const char* data,std::size_t n) {
    if (n>1024*1024-received.size()) {too_large=true;return false;}
    received.append(data,n);return true;
  });
  if (too_large || !response || response->status!=200)
    throw std::runtime_error("HTTPS JWKS retrieval failed");
  auto jwks=Json::parse(received);
  if (!jwks.is_object() || !jwks.contains("keys") || !jwks.at("keys").is_array())
    throw std::runtime_error("Invalid JWKS JSON document");
  return jwks;
}
} // namespace mymcp::oauth
