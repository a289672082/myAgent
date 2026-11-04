#include "mymcp/oauth.hpp"
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
  std::cout << "OAuth PKCE/issuer tests passed\n";
}
