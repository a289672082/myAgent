#include "mymcp/mcp.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (0)
using namespace mymcp;
int main() {
  Server server("baseline");
  server.resource_binary("app://pixel", "Pixel", [] {
    return std::string("a\0\xff", 3);
  }, "application/octet-stream");
  server.resource_template("app://asset/{+path}{?lang,version}", "Assets",
    [](const std::string& uri, const Json& vars) -> Json {
      return json::object{{"contents", json::array{json::object{
        {"uri", uri}, {"text", std::string(vars.at("path").as_string()) + ":" +
        std::string(vars.at("lang").as_string()) + ":" +
        std::string(vars.at("version").as_string())}}}}};
    });
  server.prompt_full("review", "Review a file", [](const Json& args) -> Json {
    return json::object{{"description", "A review"},
      {"messages", json::array{
        json::object{{"role","user"}, {"content", json::object{
          {"type","text"}, {"text",args.at("file")}}}},
        json::object{{"role","assistant"}, {"content", json::object{
          {"type","image"}, {"mimeType","image/png"}, {"data","YQ=="}}}}}}};
  }, json::array{json::object{{"name", "file"}, {"required", true},
                               {"title", "文件路径"}, {"description", "File path"}}});
  server.prompt_metadata("review", json::object{{"title", "代码审查"}});
  server.prompt_full("bad", "bad", [](const Json&) -> Json {
    return json::object{{"messages", json::array{
      json::object{{"role","invalid"}, {"content",json::object{{"type","text"},{"text","bad"}}}}}}};
  });
  server.add_tool_context("mrtr", "demo", json::object{{"type","object"}},
    [](const Json& params) -> Json {
      if (params.contains("inputResponses")) {
        const auto& result = params.at("inputResponses");
        CHECK(result.at("confirm").at("content").at("ok") == true);
        CHECK(result.at("sample").at("role") == "assistant");
        CHECK(result.at("roots").at("roots").size() == 1);
        CHECK(params.at("requestState") == "test-state");
        return detail::make_tool_result("done");
      }
      Json schema=json::object{{"type","object"},
        {"properties",json::object{{"ok",json::object{{"type","boolean"}}}}},
        {"required",json::array{"ok"}}};
      Json elicitation=json::object{{"method","elicitation/create"},
        {"params",json::object{{"mode","form"},{"message","Continue?"},
                               {"requestedSchema",std::move(schema)}}}};
      Json sampling=json::object{{"method","sampling/createMessage"},
        {"params",json::object{{"maxTokens",20},
          {"messages",json::array{json::object{{"role","user"},
             {"content",json::object{{"type","text"},{"text","Hello"}}}}}}}}};
      Json roots=json::object{{"method","roots/list"},{"params",json::object{}}};
      Json requests=json::object{{"confirm",std::move(elicitation)},
                                 {"sample",std::move(sampling)},
                                 {"roots",std::move(roots)}};
      return json::object{{"resultType","input_required"},
        {"requestState","test-state"},{"inputRequests",std::move(requests)}};
    });
  Client client([&](const Json& request) {return server.handle(request);});
  client.on_elicitation([](const Json&) {
    return json::object{{"action","accept"},{"content",json::object{{"ok",true}}}};
  });
  client.on_sampling([](const Json& params) -> Json {
    CHECK(params.at("messages").size() == 1);
    return json::object{{"role","assistant"}, {"model","local-test"},
      {"content",json::object{{"type","text"},{"text","Hello"}}}};
  });
  client.on_roots([](const Json&) {
    return json::object{{"roots",json::array{json::object{
      {"uri","file:///tmp"},{"name","tmp"}}}}};
  });
  CHECK(client.read("app://pixel").at("contents")[0].at("blob") == "YQD/");
  CHECK(client.read("app://asset/src%2Fmain.cpp?lang=zh&version=1")
            .at("contents")[0].at("text") == "src/main.cpp:zh:1");
  CHECK(client.prompts().at("prompts")[0].at("name") == "bad");
  Json available = client.prompts().at("prompts")[1];
  CHECK(available.at("title") == "代码审查");
  CHECK(available.at("arguments")[0].at("title") == "文件路径");
  CHECK(client.get_prompt("review",json::object{{"file","main.cpp"}})
              .at("messages").size() == 2);
  try { (void)client.get_prompt("review"); CHECK(false); }
  catch(const Error& e) { CHECK(e.code == -32602); }
  try { (void)client.get_prompt("bad"); CHECK(false); }
  catch(const Error& e) { CHECK(e.code == -32603); }
  CHECK(client.call("mrtr").at("content")[0].at("text") == "done");
  try {
    server.prompt_full("duplicate","bad",[](const Json&){return json::object{};},
      json::array{json::object{{"name","x"}},json::object{{"name","x"}}});
    CHECK(false);
  } catch(const std::invalid_argument&) {}
  std::cout << "MCP baseline resources/prompts/MRTR OK\n";
}
