// 综合 MRTR 回归：专用 Sampling/Roots/URL 回调、资源/提示词上下文请求。
#include "mymcp/mcp.hpp"
#include <iostream>
#include <stdexcept>
using namespace mymcp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("MRTR context failed: " #x); } while (false)

int main() {
  Server server("context-tests");
  server.prompt_context("approval", "需要确认", [](const Json& params)->Json {
    if (!params.contains("inputResponses"))
      return json::parse(R"({"resultType":"input_required","requestState":"prompt-v1","inputRequests":{"confirm":{"method":"elicitation/create","params":{"mode":"form","message":"继续？","requestedSchema":{"type":"object","properties":{"yes":{"type":"boolean"}},"required":["yes"]}}}}})");
    CHECK(params.at("requestState") == "prompt-v1");
    CHECK(params.at("inputResponses").at("confirm").at("content").at("yes") == true);
    return json::parse(R"({"messages":[{"role":"assistant","content":{"type":"text","text":"approved"}}]})");
  });
  server.resource_template_context("test://ids/{id}", "Resource with MRTR",
    [](const std::string& uri, const Json& vars, const Json& params)->Json {
      if (!params.contains("inputResponses"))
        return json::parse(R"({"resultType":"input_required","requestState":"resource-v1","inputRequests":{"confirm":{"method":"elicitation/create","params":{"mode":"form","message":"读取？","requestedSchema":{"type":"object","properties":{"yes":{"type":"boolean"}},"required":["yes"]}}}}})");
      CHECK(params.at("requestState") == "resource-v1");
      CHECK(params.at("inputResponses").at("confirm").at("content").at("yes") == true);
      return json::object{{"contents", json::array{json::object{
        {"uri", uri}, {"text", vars.at("id")}}}}};
    });
  server.add_tool_context("typed", "Three independent callbacks", json::object{{"type","object"}},
    [](const Json& params)->Json {
      if (!params.contains("inputResponses"))
        return json::parse(R"({"resultType":"input_required","requestState":"typed-v1","inputRequests":{"sample":{"method":"sampling/createMessage","params":{"messages":[],"maxTokens":16}},"roots":{"method":"roots/list","params":{}},"web":{"method":"elicitation/create","params":{"mode":"url","message":"授权确认","url":"https://example.org/authorize"}}}}})");
      CHECK(params.at("requestState") == "typed-v1");
      CHECK(params.at("inputResponses").at("sample").at("role") == "assistant");
      CHECK(params.at("inputResponses").at("roots").at("roots").is_array());
      CHECK(params.at("inputResponses").at("web").at("action") == "accept");
      return detail::make_tool_result("success");
    });
  Client client([&](const Json& req){return server.handle(req);});
  client.on_input_required([](const Json& requests)->Json {
    CHECK(requests.contains("confirm"));
    return json::parse(R"({"confirm":{"action":"accept","content":{"yes":true}}})");
  });
  CHECK(client.get_prompt("approval").at("messages")[0].at("content").at("text") == "approved");
  CHECK(client.read("test://ids/42").at("contents")[0].at("text") == "42");
  client.on_sampling([](const Json& req)->Json {
    CHECK(req.at("messages").is_array());
    return json::parse(R"({"role":"assistant","content":{"type":"text","text":"sampled"}})");
  });
  client.on_roots([](const Json&)->Json {
    return json::parse(R"({"roots":[{"uri":"file:///tmp","name":"tmp"}]})");
  });
  client.on_elicitation_url([](const Json& req)->Json {
    CHECK(req.at("url") == "https://example.org/authorize");
    return json::parse(R"({"action":"accept"})");
  });
  CHECK(client.call("typed").at("content")[0].at("text") == "success");
  std::cout << "MRTR context and explicit callback tests passed\n";
  return 0;
}
