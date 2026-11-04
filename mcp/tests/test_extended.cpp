#include "mymcp/mcp.hpp"
#include <iostream>
#include <stdexcept>
#define VERIFY(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while (0)
using namespace mymcp;
int main() {
  Server srv("extended", "0.2");
  srv.cache(120000, "public");
  srv.page_size(2);
  for (int i = 0; i < 5; ++i)
    srv.tool("tool" + std::to_string(i), "test", [i] {return i;});
  srv.resource_template("test://books/{id}", "Book", [](const std::string& uri, const Json& variables) -> Json {
      return json::object{{"contents", json::array{json::object{{"uri", uri}, {"text", std::string(variables.at("id").as_string())}}}}};
    }, "text/plain");
  srv.prompt_full("review", "review input", [](const Json& args)->Json {
    return json::object{{"messages", json::array{json::object{{"role", "assistant"}, {"content", json::object{{"type", "text"},
       {"text", args.at("topic")}}}}}}};
  }, json::array{json::object{{"name", "topic"}, {"required", true}}});
  srv.completion([](const Json& p) -> Json {
    return json::object{{"completion", json::object{{"values", json::array{p.at("argument").at("value")}}, {"hasMore", false}}}};
  });
  srv.extension("example.org/custom", json::object{{"enabled", true}});
  srv.method("example.org/echo", [](const Json& p)->Json {return json::object{{"echo", p.at("text")}};});
  Client client([&](const Json& req) {return srv.handle(req);});
  client.extension("example.org/custom");
  auto discovery = client.discover();
  VERIFY(discovery.at("ttlMs") == 120000);
  VERIFY(discovery.at("cacheScope") == "public");
  VERIFY(discovery.at("capabilities").at("completions").is_object());
  VERIFY(discovery.at("capabilities").at("extensions").contains("example.org/custom"));
  auto tools = client.tools();
  VERIFY(tools.at("tools").size() == 2);
  VERIFY(tools.at("nextCursor").is_string());
  VERIFY(client.list_all("tools/list", "tools").size() == 5);
  VERIFY(client.resource_templates().at("resourceTemplates").size() == 1);
  VERIFY(client.read("test://books/12").at("contents")[0].at("text") == "12");
  VERIFY(client.prompts().at("prompts")[0].at("arguments").size() == 1);
  VERIFY(client.get_prompt("review", json::object{{"topic", "cpp"}}).at("messages")[0].at("content").at("text") == "cpp");
  VERIFY(client.complete(json::object{{"type", "ref/prompt"}, {"name", "review"}}, "topic", "c++")
    .at("completion").at("values")[0] == "c++");
  VERIFY(client.request("example.org/echo", json::object{{"text", "hello"}}).at("echo") == "hello");
  try { client.request("tools/list", json::object{{"cursor", "tools/list:999"}}); VERIFY(false); }
  catch (const Error& error) { VERIFY(error.code == -32602); }
  auto sub = srv.subscribe("my-sub", json::object{{"toolsListChanged", true}, {"resourceSubscriptions", json::array{"test://books/12"}}});
  VERIFY(sub->messages.size() == 1);
  VERIFY(sub->messages.front().at("method") == "notifications/subscriptions/acknowledged");
  srv.publish("notifications/tools/list_changed");
  srv.publish("notifications/prompts/list_changed");
  srv.publish("notifications/resources/updated", json::object{{"uri", "test://books/13"}});
  srv.publish("notifications/resources/updated", json::object{{"uri", "test://books/12"}});
  VERIFY(sub->messages.size() == 3);
  VERIFY(sub->messages[1].at("method") == "notifications/tools/list_changed");
  VERIFY(sub->messages[2].at("params").at("uri") == "test://books/12");
  VERIFY(sub->messages[2].at("params").at("_meta").at("io.modelcontextprotocol/subscriptionId") == "my-sub");
  std::cout << "Extended protocol tests passed\n";
  return 0;
}
