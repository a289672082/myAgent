// Regression: C++ client falls back to pre-2026 initialize over HTTP.
#include "mymcp/mcp.hpp"
#include <stdexcept>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  auto client = mymcp::Client::http(argv[1]);
  auto info = client.discover();
  if (info.at("protocolVersion") != "2025-11-25") return 3;
  auto tools = client.tools();
  if (tools.at("tools").size() != 1) return 4;
  auto answer = client.call("ping");
  if (answer.at("content")[0].at("text") != "pong") return 5;
  return 0;
}
