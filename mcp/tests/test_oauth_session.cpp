#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <string>
#include <chrono>
#include <thread>

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
    // 新增：模拟浏览器访问本机临时回调端口，实际走 state/issuer/PKCE 验证与 token 交换。
    auto query_value=[](const std::string& url, const std::string& name) {
      auto from=url.find(name+"=");
      CHECK(from!=std::string::npos);
      from+=name.size()+1;
      auto end=url.find('&',from);
      auto value=url.substr(from,end==std::string::npos?end:end-from);
      std::string decoded;
      auto hex=[](char c) {
        if(c>='0'&&c<='9')return int(c-'0');
        if(c>='A'&&c<='F')return int(c-'A'+10);
        if(c>='a'&&c<='f')return int(c-'a'+10);
        return -1;
      };
      for(std::size_t i=0;i<value.size();++i) {
        if(value[i]=='%' && i+2<value.size()) {
          int a=hex(value[i+1]),d=hex(value[i+2]);CHECK(a>=0&&d>=0);
          decoded+=char(a*16+d);i+=2;
        } else decoded+=value[i]=='+'?' ':value[i];
      }
      return decoded;
    };
    auto local_token=session.authorize_loopback([&](const std::string& auth_url) {
      const auto callback_uri=query_value(auth_url,"redirect_uri");
      const auto state=query_value(auth_url,"state");
      const std::string prefix="http://127.0.0.1:";
      CHECK(callback_uri.starts_with(prefix));
      const auto slash=callback_uri.find('/',prefix.size());
      CHECK(slash!=std::string::npos);
      const auto port=callback_uri.substr(prefix.size(),slash-prefix.size());
      httplib::Client browser(prefix+port);
      auto path=callback_uri.substr(slash)+"?code=sample-code&state="+
        mymcp::oauth::url_encode(state)+"&iss="+mymcp::oauth::url_encode(origin);
      bool received=false;
      for(int i=0;i<20;++i) {
        auto reply=browser.Get(path);
        if(reply&&reply->status==200) {received=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      CHECK(received);
    },{},std::chrono::seconds(8));
    CHECK(local_token.access_token=="token-1");
    CHECK(exchanges==3 && refreshes==1);
  } catch (...) { server.stop(); listener.join(); throw; }
  server.stop(); listener.join();
  std::cout << "OAuth session end-to-end test passed\n";
}
