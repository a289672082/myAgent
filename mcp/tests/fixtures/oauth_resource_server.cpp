// DEVELOPMENT-ONLY OAuth protected-resource example.
// Demonstrates MCP Protected Resource Metadata and Bearer verification hook.
// Fixed token comparison is NOT an OAuth/JWT verifier. For production, replace
// the callback with verified JWT/JWKS or trusted token introspection and check
// issuer, resource/audience, expiration, and per-method scopes.
#include "mymcp/mcp.hpp"
#include <openssl/crypto.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    const std::string issuer = argc >= 2 ? argv[1] : "";
    const unsigned short port = argc >= 3 ? static_cast<unsigned short>(std::stoi(argv[2])) : 3100;
    const char* demo_token = std::getenv("MYMCP_DEMO_TOKEN");
    if (issuer.empty() || !demo_token || !*demo_token)
      throw std::invalid_argument("Usage: MYMCP_DEMO_TOKEN=<test-token> mymcp_oauth_resource_server <https://issuer> [port]");
    const std::string expected(demo_token);
    mymcp::Server server("protected-demo", "1.0.0");
    server.tool("private_hello", "Return a protected greeting", [] {return "Authorized demo request";});
    // This server listens on loopback; real deployments MUST use HTTPS and a
    // trusted identity provider/JWT verifier. Do not embed a token in source.
    server.protect_http(mymcp::json::object{
      {"resource", "http://127.0.0.1:" + std::to_string(port) + "/mcp"},
      {"authorization_servers", mymcp::json::array{issuer}}},
      [expected](const std::string& candidate, const std::string&, const std::string&) {
        return candidate.size() == expected.size() &&
          CRYPTO_memcmp(candidate.data(), expected.data(), candidate.size()) == 0;
      });
    std::cout << "Development-only protected MCP listening on 127.0.0.1:" << port << '\n';
    server.run_http(port);
  } catch (const std::exception& ex) {
    std::cerr << "OAuth protected server example: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
