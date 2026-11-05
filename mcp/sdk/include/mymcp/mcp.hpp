#pragma once

#include "mymcp/json.hpp"
#include <functional>
#include <map>
#include <cstddef>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mymcp {
namespace oauth {class Session;}
using Json = json::value;
inline constexpr const char* ProtocolVersion = "2026-07-28";
inline constexpr const char* LegacyProtocolVersion = "2025-11-25";

struct Error : std::runtime_error {
  int code;
  explicit Error(int c, std::string message) : std::runtime_error(std::move(message)), code(c) {}
};

template<class T> struct Arg {
  std::string name;
  std::string description;
  bool required = true;
};
template<class T> Arg<T> arg(std::string name, std::string description = "", bool required = true) {
  return {std::move(name), std::move(description), required};
}
namespace detail {
template<class T> struct optional_type : std::false_type {};
template<class T> struct optional_type<std::optional<T>> : std::true_type {};
template<class T> Json schema() {
  if constexpr (optional_type<T>::value) return schema<typename T::value_type>();
  else if constexpr (std::is_same_v<T, bool>) return json::object{{"type", "boolean"}};
  else if constexpr (std::is_integral_v<T>) return json::object{{"type", "integer"}};
  else if constexpr (std::is_floating_point_v<T>) return json::object{{"type", "number"}};
  else if constexpr (std::is_same_v<T, std::string>) return json::object{{"type", "string"}};
  else if constexpr (std::is_same_v<T, Json>) return json::object{};
  else return json::object{{"type", "object"}};
}
template<class T> T decode(const Json& j) {
  if constexpr (optional_type<T>::value) {
    if (j.is_null()) return std::nullopt;
    return decode<typename T::value_type>(j);
  } else if constexpr (std::is_same_v<T, Json>) return j;
  else if constexpr (std::is_same_v<T, std::string>) return std::string(j.as_string());
  else if constexpr (std::is_same_v<T, bool>) return j.as_bool();
  else if constexpr (std::is_integral_v<T>) {
    if (j.is_int64()) return static_cast<T>(j.as_integer<std::int64_t>());
    if (j.is_uint64()) return static_cast<T>(j.as_integer<std::uint64_t>());
    throw Error(-32602, "Expected integer argument");
  } else if constexpr (std::is_floating_point_v<T>) {
    if (j.is_double()) return static_cast<T>(j.as_double());
    if (j.is_int64()) return static_cast<T>(j.as_integer<std::int64_t>());
    if (j.is_uint64()) return static_cast<T>(j.as_integer<std::uint64_t>());
    throw Error(-32602, "Expected number argument");
  } else static_assert(!sizeof(T), "Unsupported argument type: use Json");
}
template<class T> T get_arg(const Json& args, const Arg<T>& spec) {
  const auto* p = json::if_contains(args, spec.name);
  if (!p || p->is_null()) {
    if constexpr (optional_type<T>::value) return std::nullopt;
    if (!spec.required) return T{};
    throw Error(-32602, "Missing argument: " + spec.name);
  }
  try { return decode<T>(*p); }
  catch (const Error&) { throw; }
  catch (const std::exception&) { throw Error(-32602, "Invalid argument: " + spec.name); }
}
Json make_tool_result(const Json& value);
Json make_tool_result(const std::string& value);
Json make_tool_result(const char* value);
Json make_tool_result(bool value);
Json make_tool_result(int value);
Json make_tool_result(long value);
Json make_tool_result(long long value);
Json make_tool_result(double value);
} // namespace detail

class Server {
public:
  explicit Server(std::string name, std::string version = "1.0.0");
  using ToolHandler = std::function<Json(const Json&)>;
  using TaskHandler = std::function<Json(const Json& input_responses, std::atomic_bool& cancelled)>;
  void add_tool(std::string name, std::string description, Json input_schema, ToolHandler handler);
  // Low-level advanced handler receives all tools/call params, including MRTR inputResponses.
  void add_tool_context(std::string name, std::string description, Json input_schema, ToolHandler handler);
  // Task-aware tool registration: only invokes handler when client advertises Tasks.
  void add_task_tool(std::string name, std::string description, Json input_schema,
                     TaskHandler handler);

  // C++ equivalent of Kotlin's addTool, including automatic JSON schema and argument conversion.
  template<class F, class... Ts>
  void tool(std::string name, std::string description, F fn, Arg<Ts>... arguments) {
    static_assert(std::is_invocable_v<F, Ts...>, "Handler argument types must match arg<T> declarations");
    json::object properties;
    json::array required;
    (void)std::initializer_list<int>{([&] {
      Json p = detail::schema<Ts>();
      if (!arguments.description.empty()) p["description"] = arguments.description;
      properties[arguments.name] = std::move(p);
      if (arguments.required && !detail::optional_type<Ts>::value) required.emplace_back(arguments.name);
    }(), 0)...};
    Json schema = json::object{{"type", "object"}, {"properties", std::move(properties)},
                   {"required", std::move(required)}, {"additionalProperties", false}};
    add_tool(std::move(name), std::move(description), std::move(schema),
      [f = std::move(fn), arguments...](const Json& values) -> Json {
        try {
          if constexpr (std::is_void_v<std::invoke_result_t<F, Ts...>>) {
            std::invoke(f, detail::get_arg<Ts>(values, arguments)...);
            return detail::make_tool_result("OK");
          } else {
            return detail::make_tool_result(std::invoke(f, detail::get_arg<Ts>(values, arguments)...));
          }
        } catch (const std::exception& e) {
          return json::object{{"content", json::array{json::object{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}};
        }
      });
  }
  void resource(std::string uri, std::string name, std::function<std::string()> reader, std::string mime_type = "text/plain");
  // 基础二进制资源：读取原始字节，并在 resources/read 中返回 base64 blob。
  // reader 不能返回文件路径；必须返回实际的二进制字节。
  void resource_binary(std::string uri, std::string name,
                       std::function<std::vector<std::uint8_t>()> reader,
                       std::string mime_type = "application/octet-stream");
  void prompt(std::string name, std::string description, std::function<std::string(const Json&)> reader);
  // Higher-level APIs preserve the full MCP JSON result representation.
  void resource_template(std::string uri_template, std::string name,
                         std::function<Json(const std::string&, const Json&)> reader,
                         std::string mime_type = "text/plain", std::string description = "");
  // arguments 为 MCP PromptArgument 列表（name/description/required）。
  // 若给出 type/schema 字段，则为扩展的可选参数校验，不会修改线上的参数名。
  void prompt_full(std::string name, std::string description,
                   std::function<Json(const Json&)> handler,
                   Json arguments = json::array{});
  void completion(std::function<Json(const Json&)> handler);
  void method(std::string method_name, std::function<Json(const Json&)> handler);
  void extension(std::string extension_id, Json settings = json::object{});
  void cache(std::uint64_t ttl_ms, std::string scope = "public");
  void page_size(std::size_t size);
  // Tasks extension (opt-in). Task IDs are unguessable; registry is process-local.
  void enable_tasks();
  // Call from a tools/call handler to return a task handle immediately.
  Json start_task(TaskHandler handler, std::uint64_t ttl_ms = 3600000, std::uint64_t poll_ms = 1000);
  // Returns a snapshot of task state, for programmatic inspection.
  Json task_state(const std::string& id);
  struct Subscription {
    Json id, notifications;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Json> messages;
    bool closed = false;
  };
  // Raw subscription stream primitives for transports (notification filters are enforced here).
  std::shared_ptr<Subscription> subscribe(Json id, Json filter);
  void publish(const std::string& method, Json params = json::object{});
  void close_subscription(const std::shared_ptr<Subscription>& sub);
  void instructions(std::string value) { instructions_ = std::move(value); }
  Json handle(const Json& request, bool allow_legacy = true); // JSON-RPC request -> response; null for notifications
  // Only stdio currently supports request-scoped progress/cancellation.
  void progress(double value, std::optional<double> total = std::nullopt,
                std::string message = {});
  bool is_cancelled() const;
  void run_stdio();
  // Optional OAuth resource-server boundary: the callback MUST validate the
  // token's issuer, audience, expiration and scopes; the SDK never trusts raw JWT claims.
  using TokenVerifier = std::function<bool(const std::string& token,
                                          const std::string& method,
                                          const std::string& name)>;
  void protect_http(Json protected_resource_metadata, TokenVerifier verifier);
  bool verify_http(const std::string& token, const std::string& method,
                   const std::string& name) const;
  Json protected_resource_metadata() const { return protected_resource_metadata_; }
  bool http_requires_auth() const { return static_cast<bool>(token_verifier_); }
  void run_http(unsigned short port = 3000, std::string host = "127.0.0.1");
  const Json* tool_schema(const std::string& name) const;
private:
  struct Tool { std::string description; Json schema; ToolHandler handler; bool with_context = false; bool requires_tasks = false; };
  struct Resource {
    std::string name, mime;
    std::function<std::string()> reader;
    std::function<std::vector<std::uint8_t>()> binary_reader;
  };
  struct Prompt { std::string description; std::function<std::string(const Json&)> reader;
                  std::function<Json(const Json&)> full_reader; Json arguments = json::array{}; };
  struct ResourceTemplate {
    std::string name, mime, description;
    std::function<Json(const std::string&, const Json&)> reader;
  };
  std::string name_, version_, instructions_;
  std::map<std::string, Tool> tools_;
  std::map<std::string, Resource> resources_;
  std::map<std::string, Prompt> prompts_;
  std::map<std::string, ResourceTemplate> templates_;
  std::map<std::string, std::function<Json(const Json&)>> methods_;
  std::function<Json(const Json&)> completion_;
  json::object extensions_;
  std::uint64_t ttl_ms_ = 0;
  std::string cache_scope_ = "public";
  std::size_t page_size_ = 100;
  std::mutex subscriber_mutex_;
  std::vector<std::weak_ptr<Subscription>> subscribers_;
  struct TaskState {
    std::mutex mutex;
    std::atomic_bool cancel_requested{false};
    Json details;
    std::chrono::steady_clock::time_point expires_at;
    TaskHandler handler;
    json::object responses;
    std::set<std::string> used_input_ids;
  };
  void run_task(const std::shared_ptr<TaskState>& task, Json input_responses);
  std::mutex task_mutex_;
  std::map<std::string, std::shared_ptr<TaskState>> tasks_;
  TokenVerifier token_verifier_;
  Json protected_resource_metadata_ = nullptr;
  bool legacy_initialized_ = false;
};

// 每次 MCP 请求的资源约束。响应字节数 0 表示 SDK 默认上限；超时默认 30 秒，显式 0 表示不设截止时间。
// Cancel callback 必须能安全地由调用线程读取；不可信进程建议使用 process() 隔离。
struct RequestOptions {
  std::chrono::milliseconds timeout{std::chrono::seconds(30)}; // 默认硬截止时间；0=显式无限制
  std::size_t max_response_bytes = 0;
  std::function<bool()> cancelled;
};

class Client {
public:
  explicit Client(std::function<Json(const Json&)> send, std::string name = "mymcp-client");
  static Client process(const std::string& executable, std::vector<std::string> args = {},
                        std::function<void(const Json&)> on_notification = {});
  // HTTPS verifies server certificates and hostnames against system trust roots.
  // Callback receives request-scoped SSE notifications (including progress).
  // Version discovery automatically falls back to legacy Streamable HTTP.
  static Client http(const std::string& url, std::string bearer_token = {},
                     std::function<void(const Json&)> on_notification = {});
  // OAuth HTTP 传输：Session 已经获取某个 MCP Resource 的访问令牌。
  // url 必须与 session->resource_url() 完全一致；远程资源只能是 HTTPS，
  // 本地 loopback 允许 HTTP，禁止无意间把 Bearer Token 发给其他目标。
  static Client http_oauth(const std::string& url, std::shared_ptr<oauth::Session> session,
                           std::function<void(const Json&)> on_notification = {});
  // Blocks while a long-lived HTTP subscription is open. Return false to disconnect.
  // 2026-07-28 不提供 Last-Event-ID 流恢复；仅重新建立无副作用的订阅。
  // max_reconnects=0 表示只建立一次连接，不自动重连。
  static void listen_http(const std::string& url, Json notifications,
                          std::function<bool(const Json&)> on_event,
                          unsigned max_reconnects = 3);
  // true 仅对 SDK 自带的 stdio/HTTP 传输成立；自定义进程内 lambda
  // 无法保证回调支持强制超时，严格隔离场景必须拒绝它们。
  bool has_bounded_transport() const noexcept {
    return static_cast<bool>(send_bounded_) && !requires_oauth_refresh_ &&
           !has_untrusted_callback_ && !input_handler_;
  }
  Json discover(const RequestOptions& options = {});
  Json request(const std::string& method, Json params = json::object{},
               const RequestOptions& options = {});
  Json request_with_progress(const std::string& method, Json params, Json progress_token);
  Json tools();
  // HTTP tools/call：首次调用先完整读取 tools/list 全部分页，
  // 缓存 inputSchema 中 x-mcp-header 并映射到 Mcp-Param-* 请求头；
  // 目录失效后重载，stdio 工具调用行为不变。
  Json call(const std::string& name, Json arguments = json::object{},
            const RequestOptions& options = {});
  Json resources();
  Json read(const std::string& uri);
  Json prompts();
  Json get_prompt(const std::string& name, Json arguments = json::object{});
  Json resource_templates();
  void enable_tasks();
  Json get_task(const std::string& task_id);
  Json update_task(const std::string& task_id, Json input_responses);
  Json cancel_task(const std::string& task_id);
  Json complete(Json ref, std::string argument_name, std::string value,
                Json context = json::object{});
  Json list_all(const std::string& method, const std::string& array_field,
                const RequestOptions& options = {});
  void extension(std::string name, Json settings = json::object{});
  Json capabilities() const { return capabilities_; }
  // Callback receives inputRequests and returns a JSON object keyed by request IDs.
  // The SDK retries the original request with inputResponses and the opaque requestState.
  void on_input_required(std::function<Json(const Json&)> handler) {
    input_handler_ = std::move(handler);
    Json cap = capabilities_.contains("elicitation") ? capabilities_["elicitation"] : Json(json::object{});
    if (input_handler_) cap["form"] = json::object{};
    else cap.erase("form");
    capabilities_["elicitation"] = std::move(cap);
  }
  // MRTR 专项回调仅在显式注册后向服务端声明相应客户端能力。
  // handler 接收 inputRequest.params，返回该请求对应的 MCP 响应结果。
  // URL 模式的征询单独显式授权，不与 form 数据征询混用。
  void on_elicitation_url(std::function<Json(const Json&)> handler) {
    elicitation_url_handler_ = std::move(handler);
    Json cap = capabilities_.contains("elicitation") ? capabilities_["elicitation"] : Json(json::object{});
    if (elicitation_url_handler_) cap["url"] = json::object{};
    else cap.erase("url");
    capabilities_["elicitation"] = std::move(cap);
  }
  void on_sampling(std::function<Json(const Json&)> handler) {
    sampling_handler_ = std::move(handler);
    if (sampling_handler_) capabilities_["sampling"] = json::object{};
    else capabilities_.erase("sampling");
  }
  void on_roots(std::function<Json(const Json&)> handler) {
    roots_handler_ = std::move(handler);
    if (roots_handler_) capabilities_["roots"] = json::object{{"listChanged", false}};
    else capabilities_.erase("roots");
  }
private:
  std::function<Json(const Json&)> send_;
  std::function<Json(const Json&, const RequestOptions&)> send_bounded_;
  std::string name_;
  long long next_id_ = 1;
  Json capabilities_ = json::object{};
  bool connected_ = false;
  bool legacy_ = false;
  bool allow_legacy_ = true;
  Json discovery_ = nullptr;
  bool http_ = false;
  bool tools_known_ = false;
  // HTTP 工具目录缓存是否完整且未被 list_changed 通知失效。
  // 用于防止分页未加载完成、目录更新后带着过期 x-mcp-header 调用工具。
  std::function<bool()> http_catalog_ready_;
  bool requires_oauth_refresh_ = false; // OAuth token acquisition currently has separate I/O.
  bool has_untrusted_callback_ = false; // C++ notification callback may never return.
  std::function<Json(const Json&)> input_handler_;
  std::function<Json(const Json&)> elicitation_url_handler_;
  std::function<Json(const Json&)> sampling_handler_;
  std::function<Json(const Json&)> roots_handler_;
};

} // namespace mymcp
