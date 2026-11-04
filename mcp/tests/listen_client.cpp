#include "mymcp/mcp.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  bool received = false;
  mymcp::Client::listen_http(argv[1], mymcp::json::object{{"toolsListChanged", true}}, [&](const mymcp::Json& event) {
    if (event.at("method") != "notifications/subscriptions/acknowledged")
      throw std::runtime_error("Expected subscription acknowledgement");
    received = true;
    return false;
  });
  if (!received) return 2;
  std::cout << "SSE client passed\n";
}
