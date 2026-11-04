// A self-contained MCP feature showcase. All demo data stays in memory.
// Run with no arguments for stdio or with --http [port] for loopback HTTP.
#include "mymcp/mcp.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mymcp;

int main(int argc, char** argv) {
  try {
    Server server("showcase", "1.0.0");
    server.instructions("Example MCP server: typed tools, resources, prompts, pagination, tasks and subscriptions.");
    server.cache(15000, "public");
    server.page_size(2); // demonstrate cursor pagination in the client

    // Typed, Kotlin-like registration automatically derives inputSchema.
    server.tool("add", "Add two integers", [](int a, int b) { return a + b; },
                arg<int>("a", "First integer"), arg<int>("b", "Second integer"));
    server.tool("hello", "Personal greeting", [](std::string name) {
      return "Hello, " + name + "!";
    }, arg<std::string>("name", "Recipient"));

    // Hand-authored schemas can constrain values beyond C++ types.
    server.add_tool("rate", "Evaluate a percentage from 0 through 100",
      json::object{{"type", "object"}, {"properties", json::object{{"value", json::object{
        {"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}},
        {"required", json::array{"value"}}, {"additionalProperties", false}},
      [](const Json& args) -> Json {
        return detail::make_tool_result("Rated " + std::to_string(args.at("value").as_integer<int>()) + "%");
      });

    server.resource("demo://status", "Server status", [] {return std::string("ready");});
    server.resource("demo://version", "Server version", [] {return std::string("1.0.0");});
    server.resource_template("demo://notes/{id}", "Note by ID",
      [](const std::string& uri, const Json& vars) -> Json {
        const std::string id = vars.at("id").as_string();
        return json::object{{"contents", json::array{json::object{
          {"uri", uri}, {"mimeType", "text/plain"}, {"text", "Example note #" + id}}}}};
      }, "text/plain", "Read an in-memory note by ID");

    server.prompt_full("review", "Generate a code review request",
      [](const Json& args) -> Json {
        const std::string topic = args.at("topic").as_string();
        return json::object{{"description", "Sample review prompt"}, {"messages", json::array{
          json::object{{"role", "user"}, {"content", json::object{{"type", "text"},
            {"text", "Please review: " + topic}}}}}}};
      }, json::array{json::object{{"name", "topic"}, {"description", "Review subject"}, {"required", true}}});

    server.completion([](const Json& params) -> Json {
      std::string prefix;
      if (const auto* arg = json::if_contains(params, "argument")) {
        if (const auto* value = json::if_contains(*arg, "value")) {
          if (value->is_string()) prefix = value->as_string();
        }
      }
      json::array matches;
      for (const std::string& candidate : {"cpp", "cmake", "clang"})
        if (candidate.starts_with(prefix)) matches.push_back(candidate);
      return json::object{{"completion", json::object{{"values", std::move(matches)},
                {"hasMore", false}}}};
    });

    // Progress is currently emitted on stdio, with cooperative cancellation.
    server.add_tool("count", "Count with progress and cancellation",
      json::object{{"type", "object"}, {"additionalProperties", false}},
      [&server](const Json&) -> Json {
        for (int n = 1; n <= 5; ++n) {
          if (server.is_cancelled()) return detail::make_tool_result("cancelled");
          server.progress(n, 5, "Counting items");
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return detail::make_tool_result("count finished");
      });

    // Multi round-trip tool: the client must explicitly approve before completion.
    server.add_tool_context("confirm_release", "Ask for release approval via MRTR",
      json::object{{"type", "object"}, {"additionalProperties", false}},
      [](const Json& params) -> Json {
        const auto* answers = json::if_contains(params, "inputResponses");
        if (!answers) {
          return json::object{{"resultType", "input_required"}, {"requestState", "confirm-v1"},
            {"inputRequests", json::object{{"approval", json::object{
              {"method", "elicitation/create"}, {"params", json::object{
                {"mode", "form"}, {"message", "Approve this example release?"},
                {"requestedSchema", json::object{{"type", "object"},
                  {"properties", json::object{{"approved", json::object{{"type", "boolean"}}}}},
                  {"required", json::array{"approved"}}}}}}}}}}};
        }
        const auto* choice = json::if_contains(*answers, "approval");
        bool approved = false;
        if (choice && choice->is_object()) {
          const auto* content = json::if_contains(*choice, "content");
          if (content && content->is_object()) {
            const auto* value = json::if_contains(*content, "approved");
            approved = value && value->is_bool() && value->as_bool();
          }
        }
        return detail::make_tool_result(approved ? "approved" : "denied");
      });

    server.enable_tasks();
    server.add_task_tool("generate_report", "Generate a report asynchronously",
      json::object{{"type", "object"}, {"additionalProperties", false}},
      [](const Json&, std::atomic_bool& cancelled) -> Json {
        for (int i = 0; i < 12; ++i) {
          if (cancelled.load()) return detail::make_tool_result("cancelled");
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return detail::make_tool_result("report complete");
      });

    server.add_tool("emit_update", "Publish a resource update to active subscribers",
      json::object{{"type", "object"}, {"additionalProperties", false}},
      [&server](const Json&) -> Json {
        server.publish("notifications/resources/updated", json::object{{"uri", "demo://status"}});
        return detail::make_tool_result("update published");
      });
    server.extension("example.com/showcase", json::object{});
    server.method("example/echo", [](const Json& params) -> Json {
      return json::object{{"echo", params}};
    });

    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--stdio")) {
      server.run_stdio();
    } else if (argc >= 2 && std::string(argv[1]) == "--http") {
      const unsigned short port = argc >= 3 ? static_cast<unsigned short>(std::stoi(argv[2])) : 3000;
      server.run_http(port); // loopback only
    } else {
      std::cerr << "Usage: mymcp_showcase_server [--stdio | --http [port]]\n";
      return 2;
    }
  } catch (const std::exception& ex) {
    std::cerr << "showcase server: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
