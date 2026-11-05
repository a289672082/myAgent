#include "mymcp/jwt.hpp"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <ctime>
#define CHECK(x) do{if(!(x))throw std::runtime_error("JWT assertion: " #x);}while(false)
using namespace mymcp;
namespace {
struct KeyFree {void operator()(EVP_PKEY* p) const {EVP_PKEY_free(p);}};
struct ContextFree {void operator()(EVP_PKEY_CTX* p) const {EVP_PKEY_CTX_free(p);}};
struct MdFree {void operator()(EVP_MD_CTX* p) const {EVP_MD_CTX_free(p);}};
struct RsaFree {void operator()(RSA* p) const {RSA_free(p);}};
using Key = std::unique_ptr<EVP_PKEY,KeyFree>;
using Context = std::unique_ptr<EVP_PKEY_CTX,ContextFree>;
using Digest = std::unique_ptr<EVP_MD_CTX,MdFree>;
using Rsa = std::unique_ptr<RSA,RsaFree>;
std::string encode(const unsigned char* bytes,std::size_t len) {
  std::string s(4*((len+2)/3),'\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(s.data()),bytes,static_cast<int>(len));
  for(char& c:s){if(c=='+')c='-';else if(c=='/')c='_';}
  while(!s.empty()&&s.back()=='=')s.pop_back();
  return s;
}
std::string encode(const std::string& v) {
  return encode(reinterpret_cast<const unsigned char*>(v.data()),v.size());
}
std::string number(const BIGNUM* b) {
  std::string bytes(BN_num_bytes(b),'\0');
  BN_bn2bin(b,reinterpret_cast<unsigned char*>(bytes.data()));
  return encode(bytes);
}
Key make_rsa() {
  Context ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr));
  CHECK(ctx && EVP_PKEY_keygen_init(ctx.get())==1 &&
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(),2048)==1);
  EVP_PKEY* key=nullptr;
  CHECK(EVP_PKEY_keygen(ctx.get(),&key)==1);
  return Key(key);
}
Json jwks(EVP_PKEY* key) {
  Rsa rsa(EVP_PKEY_get1_RSA(key));
  CHECK(rsa);
  const BIGNUM *n=nullptr,*e=nullptr,*d=nullptr;
  RSA_get0_key(rsa.get(),&n,&e,&d);
  CHECK(n&&e);
  return json::object{{"keys",json::array{
    json::object{{"kty","RSA"},{"kid","kid1"},{"alg","RS256"},{"use","sig"},
      {"n",number(n)},{"e",number(e)}}
  }}};
}
std::string sign(EVP_PKEY* key, Json claims, std::string alg="RS256", std::string kid="kid1") {
  Json header=json::object{{"typ","JWT"},{"alg",alg},{"kid",kid}};
  auto prefix=encode(json::serialize(header))+"."+encode(json::serialize(claims));
  Digest ctx(EVP_MD_CTX_new());
  CHECK(ctx && EVP_DigestSignInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key)==1 &&
        EVP_DigestSignUpdate(ctx.get(),prefix.data(),prefix.size())==1);
  std::size_t size=0;
  CHECK(EVP_DigestSignFinal(ctx.get(),nullptr,&size)==1);
  std::string signature(size,'\0');
  CHECK(EVP_DigestSignFinal(ctx.get(),
        reinterpret_cast<unsigned char*>(signature.data()),&size)==1);
  signature.resize(size);
  return prefix+"."+encode(signature);
}
} // namespace
int main() {
  const auto key=make_rsa();
  const auto pub=jwks(key.get());
  oauth::JwtPolicy policy;
  policy.issuer="https://issuer.example";
  policy.audience="https://resource.example/mcp";
  policy.required_scopes={"tools:read"};
  oauth::JwtVerifier verifier(pub,policy);
  const auto now=static_cast<std::int64_t>(std::time(nullptr));
  auto claims=json::object{{"iss",policy.issuer},{"aud",policy.audience},
    {"exp",now+120},{"iat",now-10},{"nbf",now-10},{"scope","tools:read tools:write"}};
  CHECK(verifier.verify(sign(key.get(),claims)));
  CHECK(verifier.verify(sign(key.get(),claims),{"tools:write"}));
  CHECK(!verifier.verify(sign(key.get(),claims),{"admin"}));
  auto wrong=claims;
  wrong["aud"]="https://different.example/mcp";
  CHECK(!verifier.verify(sign(key.get(),wrong)));
  wrong=claims;wrong["iss"]="https://evil.example";
  CHECK(!verifier.verify(sign(key.get(),wrong)));
  wrong=claims;wrong["exp"]=now-600;
  CHECK(!verifier.verify(sign(key.get(),wrong)));
  wrong=claims;wrong["nbf"]=now+600;
  CHECK(!verifier.verify(sign(key.get(),wrong)));
  wrong=claims;wrong["scope"]="tools:write";
  CHECK(!verifier.verify(sign(key.get(),wrong)));
  CHECK(!verifier.verify(sign(key.get(),claims,"none")));
  CHECK(!verifier.verify(sign(key.get(),claims,"RS256","not-found")));
  auto tampered=sign(key.get(),claims);
  tampered[tampered.size()-3]=(tampered[tampered.size()-3]=='A'?'B':'A');
  CHECK(!verifier.verify(tampered));
  try { (void)oauth::fetch_jwks_https("http://issuer.example/jwks");CHECK(false); }
  catch(const std::invalid_argument&){}
}
