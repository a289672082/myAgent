// Demonstrates discovery, pagination, typed tools, schema, resources,
// prompts, completion, MRTR, tasks and stdio progress.
#include "mymcp/mcp.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mymcp;

int main(int argc, char** argv) {
  try {
    const bool http = argc >= 2 && std::string(argv[1]) == "--http";
    const bool stdio = argc >= 3 && std::string(argv[1]) == "--stdio";
    if (!http && !stdio) {
      std::cerr << "Usage: mymcp_showcase_client --stdio <server-executable> | --http <http://127.0.0.1:port/mcp>\n";
      return 2;
    }
    if (http && argc < 3) return 2;
    Client client = http ? Client::http(argv[2]) : Client::process(argv[2], {},
      [](const Json& event) {
        if (event.is_object() && event.contains("method") &&
            event.at("method") == "notifications/progress")
          std::cout << "progress: " << json::serialize(event.at("params")) << '\n';
      });
    client.enable_tasks(); // advertise capability before calling discover()
    client.on_input_required([](const Json& inputs) -> Json {
      if (!inputs.contains("approval")) throw std::runtime_error("Unexpected input request");
      // Real applications SHOULD ask the user. This is a non-destructive demo.
      return json::object{{"approval", json::object{{"action", "accept"},
                   {"content", json::object{{"approved", true}}}}}};
    });

    std::cout << "discovery: " << json::serialize(client.discover()) << '\n';
    std::cout << "tools: " << json::serialize(client.list_all("tools/list", "tools")) << '\n';
    std::cout << "add: " << json::serialize(client.call("add", json::object{{"a", 3}, {"b", 4}})) << '\n';
    std::cout << "schema check: " << json::serialize(client.call("rate", json::object{{"value", 87}})) << '\n';
    std::cout << "resources: " << json::serialize(client.list_all("resources/list", "resources")) << '\n';
    std::cout << "read: " << json::serialize(client.read("demo://status")) << '\n';
    std::cout << "template: " << json::serialize(client.read("demo://notes/42")) << '\n';
    std::cout << "templates: " << json::serialize(client.resource_templates()) << '\n';
    std::cout << "prompts: " << json::serialize(client.prompts()) << '\n';
    std::cout << "prompt: " << json::serialize(client.get_prompt("review", json::object{{"topic", "demo.cpp"}})) << '\n';
    std::cout << "completion: " << json::serialize(client.complete(
      json::object{{"type", "ref/prompt"}, {"name", "review"}}, "topic", "c")) << '\n';
    std::cout << "MRTR: " << json::serialize(client.call("confirm_release")) << '\n';
    std::cout << "extension: " << json::serialize(client.request("example/echo", json::object{{"message", "hi"}})) << '\n';
    if (!http) std::cout << "progress result: " << json::serialize(client.request_with_progress(
      "tools/call", json::object{{"name", "count"}, {"arguments", json::object{}}}, "demo-progress-1")) << '\n';

    const Json task = client.call("generate_report");
    if (!task.contains("taskId") || !task.at("taskId").is_string())
      throw std::runtime_error("No taskId in task tool result");
    const std::string task_id = task.at("taskId").as_string();
    bool done = false;
    for (int n = 0; n < 100; ++n) {
      Json state = client.get_task(task_id);
      const std::string status = state.at("status").as_string();
      if (status == "completed") {
        std::cout << "task: " << json::serialize(state) << '\n';
        done = true;
        break;
      }
      if (status == "failed" || status == "cancelled")
        throw std::runtime_error("Task failed: " + json::serialize(state));
      std::this_thread::sleep_for(std::chrono::milliseconds(35));
    }
    if (!done) throw std::runtime_error("Task polling timed out");
    std::cout << "SHOWCASE_OK\n";
  } catch (const std::exception& ex) {
    std::cerr << "showcase client: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
