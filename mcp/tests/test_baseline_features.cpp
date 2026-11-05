#include "mymcp/mcp.hpp"
#include "mymcp/oauth_jwt.hpp"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if(!(x)) throw std::runtime_error("CHECK failed: " #x); } while(false)
using namespace mymcp;

static std::string b64url(const unsigned char* data, std::size_t size) {
  std::string text(((size + 2) / 3) * 4, '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()),data,static_cast<int>(size));
  while(!text.empty() && text.back()=='=')text.pop_back();
  for(auto& c:text){if(c=='+')c='-';else if(c=='/')c='_';}
  return text;
}
static std::string b64url(const std::string& data) {
  return b64url(reinterpret_cast<const unsigned char*>(data.data()),data.size());
}
static std::string bn64(const BIGNUM* number) {
  std::string bytes(BN_num_bytes(number),'\0');
  CHECK(BN_bn2bin(number,reinterpret_cast<unsigned char*>(bytes.data()))==static_cast<int>(bytes.size()));
  return b64url(bytes);
}
int main() {
  // 二进制资源支持嵌入零字节；URI 模板支持路径、保留字符等常用运算符。
  Server server("baseline");
  server.resource_binary("test://bytes","Binary",[] {
    return std::vector<std::uint8_t>{0,1,2,255};
  },"application/octet-stream");
  server.resource_template("test://docs/{+path}","Document",
    [](const std::string& uri,const Json& variables) -> Json {
      return json::object{{"contents",json::array{
        json::object{{"uri",uri},{"text",variables.at("path")}}}}};
    });
  server.prompt_full("review","审查文件",
    [](const Json& args) -> Json {
      return json::object{{"messages",json::array{
        json::object{{"role","user"},{"content",
          json::object{{"type","text"},{"text",args.at("topic")}}}}}}};
    },
    json::array{json::object{{"name","topic"},{"required",true},
      {"description","文件名称"},
      {"schema",json::object{{"type","string"},{"minLength",3}}}}});
  Client client([&](const Json& request){return server.handle(request);});
  CHECK(client.read("test://bytes").at("contents")[0].at("blob")=="AAEC/w==");
  CHECK(client.read("test://docs/parts/first").at("contents")[0].at("text")=="parts/first");
  CHECK(client.get_prompt("review",json::object{{"topic","demo.cpp"}}).at("messages")[0]
    .at("content").at("text")=="demo.cpp");
  for(const auto& bad : {Json(json::object{}),Json(json::object{{"topic","x"}}),
                          Json(json::object{{"topic",42}})}) {
    try { client.get_prompt("review",bad); CHECK(false); }
    catch(const Error& e){CHECK(e.code==-32602);}
  }
  try {server.prompt_full("invalid","",
    [](const Json&)->Json{return json::object{};},
    json::array{json::object{{"name","dup"}},json::object{{"name","dup"}}}); CHECK(false);}
  catch(const std::invalid_argument&) {}

  // MRTR：采样与根目录列表有独立能力声明、独立回调及严格 ID 配对。
  Server mrtr("mrtr");
  mrtr.add_tool_context("ask","采样与根目录",json::object{{"type","object"}},
    [](const Json& params)->Json {
      if(params.contains("inputResponses")){
        const auto& answers=params.at("inputResponses");
        if(!answers.contains("sampling") || !answers.contains("roots") ||
           params.at("requestState")!="opaque")throw std::runtime_error("MRTR context lost");
        return json::object{{"content",json::array{
          json::object{{"type","text"},{"text","accepted"}}}}};
      }
      Json message=json::object{{"role","user"},
        {"content",json::object{{"type","text"},{"text","Hello"}}}};
      Json sampling=json::object{{"method","sampling/createMessage"},
        {"params",json::object{{"messages",json::array{message}},{"maxTokens",64}}}};
      Json roots=json::object{{"method","roots/list"},{"params",json::object{}}};
      Json requests=json::object{{"sampling",sampling},{"roots",roots}};
      return json::object{{"resultType","input_required"},{"requestState","opaque"},
                          {"inputRequests",std::move(requests)}};
    });
  Client mrtr_client([&](const Json& request){return mrtr.handle(request);});
  mrtr_client.on_sampling([](const Json& p)->Json {
    CHECK(p.at("maxTokens")==64);
    return json::object{{"role","assistant"},
      {"content",json::object{{"type","text"},{"text","你好"}}}};
  });
  mrtr_client.on_roots([](const Json&)->Json {
    return json::object{{"roots",json::array{
      json::object{{"uri","file:///tmp"},{"name","demo"}}}}};
  });
  CHECK(mrtr_client.call("ask").at("content")[0].at("text")=="accepted");
  Client no_capabilities([&](const Json& request){return mrtr.handle(request);});
  try {no_capabilities.call("ask");CHECK(false);}
  catch(const Error& error){CHECK(error.code==-32021);}

  // URL 模式征询与普通表单分别声明能力；URL 授权由宿主决定。
  mrtr.add_tool_context("url-approval","模拟外部 URL 授权",
    json::object{{"type","object"}},
    [](const Json& params)->Json {
      if(params.contains("inputResponses")) {
        if(params.at("requestState")!="url-state" ||
           !params.at("inputResponses").contains("approval"))
          throw std::runtime_error("URL elicitation state lost");
        return json::object{{"content",json::array{
          json::object{{"type","text"},{"text","url-approved"}}}}};
      }
      Json approval=json::object{{"method","elicitation/create"},
        {"params",json::object{{"mode","url"},{"message","请在浏览器中完成授权"},
          {"url","https://auth.example/approval"},{"elicitationId","approval-demo"}}}};
      return json::object{{"resultType","input_required"},{"requestState","url-state"},
        {"inputRequests",json::object{{"approval",approval}}}};
    });
  Client url_client([&](const Json& request){return mrtr.handle(request);});
  url_client.on_elicitation_url([](const Json& input)->Json {
    CHECK(input.at("mode")=="url");
    CHECK(input.at("elicitationId")=="approval-demo");
    return json::object{{"action","accept"}};
  });
  url_client.on_input_required([](const Json&)->Json {return json::object{};});
  CHECK(url_client.call("url-approval").at("content")[0].at("text")=="url-approved");

  // JWT/JWKS：仅接受可信 JWKS 中的 RS256 公钥，核对 issuer/audience/scope/exp。
  using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
  using KeyCtx=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
  using Digest=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>;
  KeyCtx generation(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);
  CHECK(generation && EVP_PKEY_keygen_init(generation.get())==1);
  CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(generation.get(),2048)==1);
  EVP_PKEY* raw=nullptr;
  CHECK(EVP_PKEY_keygen(generation.get(),&raw)==1);
  Key key(raw,EVP_PKEY_free);
  const RSA* rsa=EVP_PKEY_get0_RSA(key.get());
  CHECK(rsa);
  const BIGNUM* n=nullptr;const BIGNUM* e=nullptr;
  RSA_get0_key(rsa,&n,&e,nullptr);
  CHECK(n && e);
  Json jwks=json::object{{"keys",json::array{
    json::object{{"kty","RSA"},{"kid","test-key"},{"alg","RS256"},{"use","sig"},
      {"n",bn64(n)},{"e",bn64(e)}}}}};
  const auto exp=std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count()+3600;
  Json header=json::object{{"typ","JWT"},{"alg","RS256"},{"kid","test-key"}};
  Json claims=json::object{{"iss","https://issuer.example"},
    {"aud","https://api.example/mcp"},{"exp",exp},{"scope","tools:read resources:read"}};
  const auto signed_data=b64url(json::serialize(header))+"."+b64url(json::serialize(claims));
  Digest signing(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  CHECK(signing && EVP_DigestSignInit(signing.get(),nullptr,EVP_sha256(),nullptr,key.get())==1);
  CHECK(EVP_DigestSignUpdate(signing.get(),signed_data.data(),signed_data.size())==1);
  std::size_t siglen=0;
  CHECK(EVP_DigestSignFinal(signing.get(),nullptr,&siglen)==1);
  std::vector<unsigned char> signature(siglen);
  CHECK(EVP_DigestSignFinal(signing.get(),signature.data(),&siglen)==1);
  signature.resize(siglen);
  auto token=signed_data+"."+b64url(signature.data(),signature.size());
  CHECK(oauth::verify_jwt_rs256(token,jwks,"https://issuer.example","https://api.example/mcp",
    {"tools:read"}));
  CHECK(!oauth::verify_jwt_rs256(token,jwks,"https://other.example","https://api.example/mcp"));
  CHECK(!oauth::verify_jwt_rs256(token,jwks,"https://issuer.example","https://other.example/mcp"));
  CHECK(!oauth::verify_jwt_rs256(token,jwks,"https://issuer.example","https://api.example/mcp",
    {"tools:write"}));
  auto forged=token;
  forged[forged.rfind('.')+1]=(forged[forged.rfind('.')+1]=='A'?'B':'A');
  CHECK(!oauth::verify_jwt_rs256(forged,jwks,"https://issuer.example","https://api.example/mcp"));
  // 将 JWKS 验证器直接挂接现有 Server::protect_http，实际拦截需要 HTTPS 代理。
  Server protected_server("protected");
  protected_server.protect_http(json::object{
    {"resource","https://api.example/mcp"},
    {"authorization_servers",json::array{"https://issuer.example"}}},
    [jwks](const std::string& candidate,const std::string& method,const std::string&) {
      return oauth::verify_jwt_rs256(candidate,jwks,"https://issuer.example",
        "https://api.example/mcp",method=="tools/call"
          ? std::vector<std::string>{"tools:read"} : std::vector<std::string>{});
    });
  CHECK(protected_server.verify_http(token,"tools/call","anything"));
  CHECK(!protected_server.verify_http(forged,"tools/call","anything"));
  std::cout<<"MCP baseline capability tests passed\n";
  return 0;
}
