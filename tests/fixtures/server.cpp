#include "mymcp/mcp.hpp"
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <openssl/crypto.h>
int main(int argc, char** argv) {
  using namespace mymcp;
  Server server("demo-server", "1.0.0");
  server.instructions("Tools for arithmetic and welcome messages.");
  server.tool("add", "Add two integers", [](int a, int b) { return a+b; },
              arg<int>("a", "First integer"), arg<int>("b", "Second integer"));
  server.tool("greet", "Say hello", [](std::string name) { return "Hello, " + name; },
              arg<std::string>("name", "Person's name"));
  server.resource("demo://status", "Status", [] { return std::string("healthy"); });
  server.prompt("welcome", "A welcome prompt", [](const mymcp::Json&) { return "Welcome to the MCP server."; });
  server.add_tool("count_with_progress", "Count while reporting progress", json::object{{"type", "object"}},
     [&server](const Json&) -> Json {
       for (int i = 1; i <= 50; ++i) {
         if (server.is_cancelled()) break;
         server.progress(i, 50, "Counting");
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
       }
       return detail::make_tool_result("done");
     });
  if (argc >= 2 && std::string(argv[1]) == "--http-auth") {
    const auto* secret = std::getenv("MYMCP_TEST_TOKEN");
    if (!secret || !*secret) return 2;
    unsigned short port = argc >= 3 ? static_cast<unsigned short>(std::stoi(argv[2])) : 3000;
    const std::string expected(secret);
    server.protect_http(json::object{{"resource", "http://127.0.0.1:" + std::to_string(port) + "/mcp"},
                         {"authorization_servers", json::array{"https://auth.example"}}},
       [expected](const std::string& token, const std::string&, const std::string&) {
         return token.size() == expected.size() &&
             CRYPTO_memcmp(token.data(), expected.data(), token.size()) == 0;
       });
    server.run_http(port);
  } else if (argc >= 2 && std::string(argv[1]) == "--http")
    server.run_http(argc >= 3 ? static_cast<unsigned short>(std::stoi(argv[2])) : 3000);
  else server.run_stdio();
}
