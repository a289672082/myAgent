#pragma once
#include "mymcp/oauth.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mymcp::oauth {
// OAuth is HTTP-only. Redirect/browser ownership stays with the host application.
// Session instances must be bound to one MCP resource/audience.
struct Metadata {
  std::string resource;
  std::string issuer;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::string registration_endpoint;
  bool issuer_parameter_supported = false;
  std::vector<std::string> scopes_supported;
};
struct Token {
  std::string access_token;
  std::string refresh_token;
  std::string token_type;
  std::string scope;
  std::chrono::system_clock::time_point expires_at{};
  bool expired(std::chrono::seconds margin = std::chrono::seconds(60)) const;
};

// Complete client-side Authorization Code + PKCE and refresh/token management.
// No confidential client secret is required. The host provides user interaction.
class Session {
public:
  explicit Session(std::string mcp_resource_url);
  // OAuth Token 仅允许用于此资源 URI；资源标识在 Session 构造后不可变。
  // HTTP 客户端必须在附加 Authorization 请求头前严格校验目标 URI。
  const std::string& resource_url() const noexcept { return resource_url_; }
  void discover(); // RFC 9728 protected resource metadata, RFC 8414 / OIDC
  const Metadata& metadata() const { return metadata_; }
  // Use a pre-registered client ID or Client ID Metadata Document URL.
  void client_id(std::string value);
  // Optional RFC 7591 dynamic registration for servers that explicitly support it.
  // "native" is the correct RFC 7591 / OIDC application_type for desktop and CLI clients.
  // Remote hosted web apps may select "web" explicitly.
  std::string register_dynamic(const std::vector<std::string>& redirect_uris,
                               std::string client_name = "myMcpTools",
                               std::string application_type = "native");
  Authorization begin_authorization(std::string redirect_uri,
                                     const std::vector<std::string>& scopes = {});
  // Validate state + issuer before using authorization code.
  Token exchange_code(const Authorization& flow, const std::string& code,
                      const std::string& callback_state, const std::string& callback_issuer,
                      const std::string& redirect_uri);
  // Accepts the complete callback URL. Strictly matches the registered redirect,
  // rejects duplicate/malformed parameters and authorization errors, then
  // validates state+issuer before exchanging the code.
  Token exchange_callback_url(const Authorization& flow, const std::string& callback_url,
                              const std::string& redirect_uri);
  // 安全本机浏览器授权回调：绑定随机临时回环端口，不开启公网监听；
  // 宿主提供打开系统浏览器的 navigate 回调。
  Token authorize_loopback(std::function<void(const std::string&)> navigate,
                           const std::vector<std::string>& scopes = {},
                           std::chrono::seconds timeout = std::chrono::seconds(120));
  Token refresh();
  std::string bearer_token(); // refresh if nearing expiry, else return current token
  bool has_token() const;
private:
  std::string resource_url_;
  std::string client_id_;
  std::string redirect_uri_;
  Metadata metadata_;
  mutable std::mutex mutex_;
  Token token_;
  bool token_available_ = false;
};
} // namespace mymcp::oauth
