#include "mymcp/mcp.hpp"
#include "mymcp/oauth_jwt.hpp"
#include <httplib.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#define CHECK(x) do { if(!(x))throw std::runtime_error("failed: " #x); } while(0)
using namespace mymcp;
namespace {
std::string b64url(const std::string& input) {
  std::string out(4*((input.size()+2)/3),'\0');
  int len=EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
      reinterpret_cast<const unsigned char*>(input.data()),static_cast<int>(input.size()));
  out.resize(len);
  while(!out.empty()&&out.back()=='=')out.pop_back();
  for(char& c:out){if(c=='+')c='-';if(c=='/')c='_';}
  return out;
}
std::string bn64(const BIGNUM* b) {
  std::string bytes(BN_num_bytes(b),'\0');
  BN_bn2bin(b,reinterpret_cast<unsigned char*>(bytes.data()));
  return b64url(bytes);
}
std::string sign(EVP_PKEY* key,const Json& header,const Json& payload) {
  auto input=b64url(json::serialize(header))+"."+b64url(json::serialize(payload));
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  CHECK(ctx!=nullptr);
  CHECK(EVP_DigestSignInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key)==1);
  CHECK(EVP_DigestSignUpdate(ctx.get(),input.data(),input.size())==1);
  std::size_t length=0;CHECK(EVP_DigestSignFinal(ctx.get(),nullptr,&length)==1);
  std::string bytes(length,'\0');
  CHECK(EVP_DigestSignFinal(ctx.get(),reinterpret_cast<unsigned char*>(bytes.data()),&length)==1);
  bytes.resize(length);
  return input+"."+b64url(bytes);
}
}
int main() {
  EVP_PKEY_CTX* ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr);
  CHECK(ctx!=nullptr);
  CHECK(EVP_PKEY_keygen_init(ctx)==1);
  CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx,2048)==1);
  EVP_PKEY* raw=nullptr;CHECK(EVP_PKEY_keygen(ctx,&raw)==1);
  EVP_PKEY_CTX_free(ctx);
  std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(raw,EVP_PKEY_free);
  RSA* rsa=EVP_PKEY_get1_RSA(key.get());CHECK(rsa);
  const BIGNUM* n=nullptr;const BIGNUM* e=nullptr;
  RSA_get0_key(rsa,&n,&e,nullptr);
  const std::string n_value=bn64(n),e_value=bn64(e);
  RSA_free(rsa);
  httplib::Server mock;
  const int port=mock.bind_to_any_port("127.0.0.1");
  CHECK(port>0);
  const std::string origin="http://127.0.0.1:"+std::to_string(port);
  mock.Get("/keys",[&](const httplib::Request&,httplib::Response& response){
    Json jwks=json::object{{"keys",json::array{json::object{
      {"kty","RSA"},{"kid","test-key"},{"use","sig"},{"alg","RS256"},
      {"n",n_value},{"e",e_value}}}}};
    response.set_content(json::serialize(jwks),"application/json");
  });
  std::thread worker([&]{mock.listen_after_bind();});
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  try{
    oauth::JwtPolicy policy;
    policy.issuer=origin;
    policy.audience=origin+"/mcp";
    policy.jwks_uri=origin+"/keys";
    policy.default_scopes={"tools:read"};
    policy.required_scopes["tools/call:delete"]={"tools:write"};
    Server server("protected");
    server.protect_http_jwt(json::object{
      {"resource",policy.audience},{"authorization_servers",json::array{origin}}},policy);
    Json header=json::object{{"alg","RS256"},{"kid","test-key"},{"typ","at+jwt"}};
    Json claims=json::object{{"iss",origin},{"aud",origin+"/mcp"},
      {"exp",std::time(nullptr)+3600},{"scope","tools:read"}};
    auto token=sign(key.get(),header,claims);
    CHECK(server.verify_http(token,"tools/list",""));
    CHECK(!server.verify_http(token,"tools/call","delete"));
    CHECK(server.verify_http(token,"tools/call","read"));
    claims["aud"]=origin+"/different";
    CHECK(!server.verify_http(sign(key.get(),header,claims),"tools/list",""));
    claims["aud"]=origin+"/mcp";
    claims["exp"]=std::time(nullptr)-3600;
    CHECK(!server.verify_http(sign(key.get(),header,claims),"tools/list",""));
    claims["exp"]=std::time(nullptr)+3600;
    header["alg"]="none";
    CHECK(!server.verify_http(sign(key.get(),header,claims),"tools/list",""));
    CHECK(!server.verify_http(token+"tampered","tools/list",""));
    std::cout<<"JWT JWKS verification passed\n";
  }catch(...){mock.stop();worker.join();throw;}
  mock.stop();worker.join();
}
