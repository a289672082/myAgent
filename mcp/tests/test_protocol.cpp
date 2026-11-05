#include "mymcp/mcp.hpp"
#include <stdexcept>
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("Assertion failed: " #x); } while(false)
#include <iostream>
using namespace mymcp;
int main() {
  Server server("test", "1.0");
  server.tool("add", "Add integers", [](int a, int b){return a+b;}, arg<int>("a"), arg<int>("b"));
  server.resource("test://one", "One", []{return std::string("value");});
  Client client([&](const Json& req){return server.handle(req);});
  auto discovery = client.discover();
  REQUIRE(discovery.at("resultType") == "complete");
  REQUIRE(discovery.at("supportedVersions")[0] == ProtocolVersion);
  REQUIRE(discovery.at("_meta").contains("io.modelcontextprotocol/serverInfo"));
  REQUIRE(client.tools().at("tools").size() == 1);
  auto call = client.call("add", json::object{{"a", 2}, {"b", 3}});
  REQUIRE(call.at("resultType") == "complete");
  REQUIRE(call.at("content")[0].at("text") == "5");
  REQUIRE(client.read("test://one").at("contents")[0].at("text") == "value");
  try { client.call("add", json::object{{"a", "bad"}, {"b", 3}}); REQUIRE(false); }
  catch (const Error& e) { REQUIRE(e.code == -32602); }
  Json invalid = json::object{{"jsonrpc", "2.0"}, {"id", 88}, {"method", "tools/list"},
                  {"params", json::object{{"_meta", json::object{{"io.modelcontextprotocol/protocolVersion", "2099-01-01"},
                                          {"io.modelcontextprotocol/clientCapabilities", json::object{}},
                                          {"io.modelcontextprotocol/clientInfo", json::object{{"name", "test"}, {"version", "1"}}}}}}}};
  REQUIRE(server.handle(invalid).at("error").at("code") == -32022);
  // Legacy stdio era: initialize and notifications/initialized, without modern _meta.
  Server legacy_server("legacy-test", "1.0");
  legacy_server.tool("echo", "Echo", [](std::string s){ return s; }, arg<std::string>("text"));
  Client fallback([&](const Json& req) -> Json {
    if (req.at("method") == "server/discover")
      return json::object{{"jsonrpc", "2.0"}, {"id", req.at("id")},
                  {"error", json::object{{"code", -32601}, {"message", "Method not found"}}}};
    return legacy_server.handle(req);
  });
  REQUIRE(fallback.discover().at("protocolVersion") == LegacyProtocolVersion);
  REQUIRE(fallback.call("echo", json::object{{"text", "legacy"}}).at("content")[0].at("text") == "legacy");
  // HTTP modern discovery must not advertise legacy versions.
  Json modern_req = json::object{{"jsonrpc", "2.0"}, {"id", 19}, {"method", "server/discover"},
                     {"params", json::object{{"_meta", json::object{{"io.modelcontextprotocol/protocolVersion", ProtocolVersion},
                                            {"io.modelcontextprotocol/clientCapabilities", json::object{}},
                                          {"io.modelcontextprotocol/clientInfo", json::object{{"name", "test"}, {"version", "1"}}}}}}}};
  REQUIRE(server.handle(modern_req, false).at("result").at("supportedVersions").size() == 1);
  // MRTR: callback supplies an elicitation answer; client retries with requestState.
  Server mrtr("mrtr-server");
  mrtr.add_tool_context("ask", "Ask the user", json::object{{"type", "object"}},
      [](const Json& params) -> Json {
        if (const auto* responses = json::if_contains(params, "inputResponses")) {
          if (!responses->is_object() || !params.contains("requestState") ||
              params.at("requestState") != "opaque") throw std::runtime_error("Lost requestState");
          return json::object{{"content", json::array{json::object{{"type", "text"}, {"text", "accepted"}}}}, {"isError", false}};
        }
        Json req = json::object{{"method", "elicitation/create"},
                    {"params", json::object{{"mode", "form"}, {"message", "Continue?"},
                    {"requestedSchema", json::object{{"type", "object"}, {"properties", json::object{}}}}}}};
        return json::object{{"resultType", "input_required"},
                {"inputRequests", json::object{{"answer", std::move(req)}}},
                {"requestState", "opaque"}};
      });
  Client mrtr_client([&](const Json& req){ return mrtr.handle(req); });
  mrtr_client.on_input_required([](const Json& requests) -> Json {
    REQUIRE(requests.at("answer").at("method") == "elicitation/create");
    return json::object{{"answer", json::object{{"action", "accept"}, {"content", json::object{}}}}};
  });
  REQUIRE(mrtr_client.call("ask").at("content")[0].at("text") == "accepted");
  // MRTR：表单 JSON Schema 不能由客户端随意绕过。
  Server forms("forms");
  forms.add_tool_context("ask", "form validation", json::object{{"type", "object"}},
    [](const Json& params) -> Json {
      if (params.contains("inputResponses")) return detail::make_tool_result("ok");
      return json::object{{"resultType", "input_required"}, {"inputRequests", json::object{
        {"form", json::object{{"method", "elicitation/create"}, {"params", json::object{
          {"mode", "form"}, {"message", "Confirm"}, {"requestedSchema", json::object{
            {"type", "object"}, {"properties", json::object{{"ok", json::object{{"type", "boolean"}}}}},
            {"required", json::array{"ok"}}, {"additionalProperties", false}}}}}}}}}};
    });
  Client invalid_form([&](const Json& request) { return forms.handle(request); });
  invalid_form.on_input_required([](const Json&) -> Json {
    return json::object{{"form", json::object{{"action", "accept"},
      {"content", json::object{{"ok", "not-a-bool"}}}}}};
  });
  try { invalid_form.call("ask"); REQUIRE(false); }
  catch (const Error& e) { REQUIRE(e.code == -32602); }

  // MRTR：采样和 Roots 的响应必须先声明相应客户端能力。
  Server sampling("sampling");
  sampling.add_tool_context("sample", "sampling", json::object{{"type", "object"}},
    [](const Json& params) -> Json {
      if (params.contains("inputResponses")) return detail::make_tool_result("sampled");
      Json message = json::object{
        {"role", "user"},
        {"content", json::object{{"type", "text"}, {"text", "Hi"}}}
      };
      Json request = json::object{
        {"method", "sampling/createMessage"},
        {"params", json::object{{"messages", json::array{std::move(message)}}}}
      };
      return json::object{
        {"resultType", "input_required"},
        {"inputRequests", json::object{{"sample", std::move(request)}}}
      };
    });
  Client sampler([&](const Json& request) { return sampling.handle(request); });
  sampler.on_input_required([](const Json&) -> Json {
    return json::object{{"sample", json::object{{"role", "assistant"},
      {"content", json::object{{"type", "text"}, {"text", "Hello"}}}}}};
  });
  try { sampler.call("sample"); REQUIRE(false); }
  catch (const Error& e) { REQUIRE(e.code == -32602); }
  sampler.enable_sampling();
  REQUIRE(sampler.call("sample").at("content")[0].at("text") == "sampled");
  std::cout << "Protocol tests passed\n";
  return 0;
}
