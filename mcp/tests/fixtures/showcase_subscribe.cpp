// Run alongside mymcp_showcase_server --http. This example starts a subscriber,
// then invokes the event-emitting tool through a second HTTP client.
#include "mymcp/mcp.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mymcp;
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: mymcp_showcase_subscribe http://127.0.0.1:3000/mcp\n";
    return 2;
  }
  try {
    const std::string url = argv[1];
    std::thread producer;
    std::atomic_bool received{false};
    std::atomic_bool started{false};
    try {
      Client::listen_http(url, json::object{{"resourceSubscriptions", json::array{"demo://status"}}},
        [&](const Json& event) {
          std::cout << "subscription event: " << json::serialize(event) << '\n';
          const std::string method = event.at("method").as_string();
          if (method == "notifications/subscriptions/acknowledged" && !started.exchange(true)) {
            // Start after the acknowledgement, so we do not miss the event.
            producer = std::thread([url] {
              try {
                auto writer = Client::http(url);
                writer.call("emit_update");
              } catch (const std::exception& ex) {
                std::cerr << "event producer: " << ex.what() << '\n';
              }
            });
          }
          if (method == "notifications/resources/updated") {
            received = true;
            return false; // close SSE stream
          }
          return true;
        });
    } catch (...) {
      if (producer.joinable()) producer.join();
      throw;
    }
    if (producer.joinable()) producer.join();
    if (!received) throw std::runtime_error("Resource update was not received");
    std::cout << "SUBSCRIPTION_OK\n";
  } catch (const std::exception& ex) {
    std::cerr << "subscription example: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
