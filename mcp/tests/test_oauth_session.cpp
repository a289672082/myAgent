#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while (0)

int main() {
  httplib::Server server;
  int port = server.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  const std::string origin = "http://127.0.0.1:" + std::to_string(port);
  std::atomic<int> exchanges{0}, refreshes{0};
  server.Get("/.well-known/oauth-protected-resource", [origin](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"resource\":\""+origin+"/mcp\",\"authorization_servers\":[\""+origin+"\"],\"scopes_supported\":[\"tools:read\"]}","application/json");
  });
  server.Get("/.well-known/oauth-authorization-server", [origin](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"issuer\":\""+origin+"\",\"authorization_endpoint\":\""+origin+"/authorize\",\"token_endpoint\":\""+origin+"/token\",\"registration_endpoint\":\""+origin+"/register\",\"authorization_response_iss_parameter_supported\":true}","application/json");
  });
  server.Post("/register", [](const httplib::Request& req, httplib::Response& res) {
    CHECK(req.body.find("redirect_uris") != std::string::npos);
    CHECK(req.body.find("\"application_type\":\"native\"") != std::string::npos);
    res.set_content("{\"client_id\":\"local-client\"}","application/json");
    res.status = 201;
  });
  server.Post("/token", [&](const httplib::Request& req, httplib::Response& res) {
    if(req.body.find("grant_type=authorization_code") != std::string::npos) {
      CHECK(req.body.find("code_verifier=") != std::string::npos);
      ++exchanges;
      res.set_content("{\"access_token\":\"token-1\",\"token_type\":\"Bearer\",\"refresh_token\":\"refresh-1\",\"expires_in\":1}", "application/json");
    } else {
      CHECK(req.body.find("grant_type=refresh_token") != std::string::npos);
      CHECK(req.body.find("refresh_token=refresh-1") != std::string::npos);
      ++refreshes;
      res.set_content("{\"access_token\":\"token-2\",\"token_type\":\"Bearer\",\"expires_in\":3600}", "application/json");
    }
  });
  std::thread listener([&] { server.listen_after_bind(); });
  try {
    mymcp::oauth::Session session(origin+"/mcp");
    session.discover();
    CHECK(session.metadata().issuer==origin);
    CHECK(session.register_dynamic({"http://127.0.0.1:49593/callback"})=="local-client");
    auto flow = session.begin_authorization("http://127.0.0.1:49593/callback");
    CHECK(flow.url.find("code_challenge_method=S256")!=std::string::npos);
    auto token = session.exchange_code(flow,"sample-code",flow.state,origin,"http://127.0.0.1:49593/callback");
    CHECK(token.access_token=="token-1");
    CHECK(session.has_token());
    const auto redirect = "http://127.0.0.1:49593/callback";
    auto next_flow = session.begin_authorization(redirect);
    auto callback = std::string(redirect) + "?code=sample-code&state=" +
                    mymcp::oauth::url_encode(next_flow.state) + "&iss=" +
                    mymcp::oauth::url_encode(origin);
    try {
      session.exchange_callback_url(next_flow, callback + "&state=duplicate", redirect);
      CHECK(false);
    } catch (const std::invalid_argument&) {}
    try {
      session.exchange_callback_url(next_flow, std::string(redirect)+
        "?code=sample-code&state=wrong&iss="+mymcp::oauth::url_encode(origin), redirect);
      CHECK(false);
    } catch (const std::invalid_argument&) {}
    try {
      session.exchange_callback_url(next_flow, std::string(redirect)+
        "?error=access_denied&state="+next_flow.state, redirect);
      CHECK(false);
    } catch (const std::runtime_error&) {}
    auto callback_token = session.exchange_callback_url(next_flow, callback, redirect);
    CHECK(callback_token.access_token=="token-1");
    CHECK(session.bearer_token()=="token-2");
    CHECK(session.bearer_token()=="token-2");
    CHECK(exchanges==2 && refreshes==1);
  } catch (...) { server.stop(); listener.join(); throw; }
  server.stop(); listener.join();
  std::cout << "OAuth session end-to-end test passed\n";
}
