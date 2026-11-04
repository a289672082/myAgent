#include "mymcp/mcp.hpp"
#include <iostream>
int main(int argc, char** argv) {
  using namespace mymcp;
  try {
    Client client = argc > 1 ? Client::http(argv[1]) : Client::process("./mymcp_example_server");
    std::cout << "Discover: " << json::serialize(client.discover()) << '\n';
    std::cout << "Tools: " << json::serialize(client.tools()) << '\n';
    std::cout << "2 + 3: " << json::serialize(client.call("add", json::object{{"a", 2}, {"b", 3}})) << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
