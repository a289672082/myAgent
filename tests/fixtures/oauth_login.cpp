// Native OAuth 2.1 Authorization Code + PKCE login for a real MCP endpoint.
// Requires an actual OAuth authorization server; use --client-id for an
// existing registration or --register for servers supporting RFC 7591.
// Tokens are held in memory and never printed or written to disk.
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: mymcp_oauth_login <https://mcp-host/mcp> (--client-id <ID> | --register) [--scope <SCOPE>]\n"
              << "For locally registered test providers, http://127.0.0.1 is also supported.\n";
    return 2;
  }
  try {
    const std::string resource = argv[1];
    std::string client_id;
    bool register_dynamic = false;
    std::vector<std::string> scopes;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--client-id" && i + 1 < argc) client_id = argv[++i];
      else if (option == "--register") register_dynamic = true;
      else if (option == "--scope" && i + 1 < argc) scopes.emplace_back(argv[++i]);
      else throw std::invalid_argument("Invalid OAuth option: " + option);
    }
    if ((client_id.empty() && !register_dynamic) || (!client_id.empty() && register_dynamic))
      throw std::invalid_argument("Choose exactly one of --client-id or --register");

    auto session = std::make_shared<mymcp::oauth::Session>(resource);
    session->discover(); // RFC 9728, RFC 8414 / OIDC metadata
    std::cout << "Authorization server: " << session->metadata().issuer << '\n';

    // Local loopback callback; OS chooses an available port. No client secret.
    httplib::Server receiver;
    const int port = receiver.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("Cannot bind local OAuth callback receiver");
    const std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/callback";
    if (register_dynamic) session->register_dynamic({redirect}, "myMcpTools C++ example");
    else session->client_id(client_id);
    const auto flow = session->begin_authorization(redirect, scopes);

    std::mutex mutex;
    std::condition_variable wake;
    bool received = false;
    std::string callback_url;
    receiver.Get("/callback", [&](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard lock(mutex);
        if (!received) {
          callback_url = "http://127.0.0.1:" + std::to_string(port) + req.target;
          received = true;
        }
      }
      res.set_content("<h3>Authorization callback received</h3><p>You may close this tab.</p>",
                      "text/html; charset=utf-8");
      wake.notify_one();
    });
    std::thread callback_thread([&] { receiver.listen_after_bind(); });
    std::cout << "\nOpen this authorization URL in your browser:\n" << flow.url << "\n\n"
              << "Waiting up to 180 seconds for the localhost callback...\n";
    bool success = false;
    {
      std::unique_lock lock(mutex);
      success = wake.wait_for(lock, std::chrono::seconds(180), [&] { return received; });
    }
    receiver.stop();
    callback_thread.join();
    if (!success) throw std::runtime_error("OAuth callback timed out");

    // Reject duplicate query parameters, wrong state/issuer, and redirect mismatches.
    session->exchange_callback_url(flow, callback_url, redirect);
    auto client = mymcp::Client::http_oauth(resource, session);
    std::cout << "MCP discovery: " << mymcp::json::serialize(client.discover()) << '\n';
    std::cout << "Authorized tools: " << mymcp::json::serialize(client.tools()) << '\n';
    std::cout << "OAuth login completed. Tokens were not printed or persisted.\n";
  } catch (const std::exception& ex) {
    std::cerr << "OAuth login example: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
