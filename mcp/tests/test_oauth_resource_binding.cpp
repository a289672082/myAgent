// OAuth 资源绑定回归：Token 只能与创建它的 MCP Resource URI 配对。
// 本测试不需要外网，也不会获取真实 OAuth 凭据。
// 正例：同一 HTTPS 资源、本机 HTTP 资源；
// 反例：同域其他路径、端口、主机、协议降级、URL 片段、空 Session。
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
template <class F>
void rejected(F&& fn,const std::string& message) {
  bool blocked=false;
  try {fn();}
  catch(const std::invalid_argument&) {blocked=true;}
  if(!blocked)throw std::runtime_error(message);
}
}

int main() {
  try {
    const auto resource=std::make_shared<mymcp::oauth::Session>(
      "https://mcp.example.test/mcp");
    if(resource->resource_url()!="https://mcp.example.test/mcp")
      throw std::runtime_error("OAuth resource identifier changed");
    // Session 创建后不可改写资源地址；客户端目标必须完全一致。
    auto valid=mymcp::Client::http_oauth(resource->resource_url(),resource);
    if(valid.has_bounded_transport())
      throw std::runtime_error("OAuth refresh must not claim bounded transport");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "https://mcp.example.test/admin",resource); },
      "Different MCP resource path was accepted");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "https://mcp.example.test:444/mcp",resource); },
      "Different port was accepted");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "https://other.example.test/mcp",resource); },
      "Different OAuth audience/host was accepted");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "http://mcp.example.test/mcp",resource); },
      "HTTP downgrade was accepted");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "https://mcp.example.test/mcp#fragment",resource); },
      "URL fragment was accepted");
    rejected([&] { (void)mymcp::Client::http_oauth(
      "https://mcp.example.test/mcp",{}); },
      "Null OAuth Session was accepted");
    // 本机 loopback HTTP 仍支持桌面客户端/开发环境授权流。
    const auto local=std::make_shared<mymcp::oauth::Session>(
      "http://127.0.0.1:47891/mcp");
    (void)mymcp::Client::http_oauth(local->resource_url(),local);
    const auto localhost=std::make_shared<mymcp::oauth::Session>(
      "http://localhost:47891/mcp");
    (void)mymcp::Client::http_oauth(localhost->resource_url(),localhost);

    std::cout<<"OAuth resource binding and HTTPS boundary OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"OAuth binding regression failed: "<<e.what()<<'\n';
    return 1;
  }
}
