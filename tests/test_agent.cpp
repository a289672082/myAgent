// Fully offline regression tests for Responses orchestration and existing MCP reuse.
#include "myagent/agent.hpp"
#include "mymcp/oauth_client.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace mymcp;
using namespace myagent;
namespace fs = std::filesystem;

struct MockModel : Model {
  std::function<Json(const Json&)> handler;
  int count = 0;
  Json create(const Json& request) override { ++count; return handler(request); }
};
static Json assistant(const std::string& text) {
  return json::object{{"status", "completed"},
    {"output", json::array{json::object{{"type", "message"}, {"role", "assistant"},
      {"content", json::array{json::object{{"type", "output_text"}, {"text", text}}}}}}}};
}
static Json call(const std::string& name, const std::string& args) {
  return json::object{{"status", "completed"},
    {"output", json::array{json::object{{"type", "function_call"},
      {"call_id", "call_one"}, {"name", name}, {"arguments", args}}}}};
}
static bool contains(const std::string& text, const std::string& pattern) {
  return text.find(pattern) != std::string::npos;
}
int main() {
  // Explicit credentials/endpoint: no environment-variable fallback.
  {
    Config missing_key;
    missing_key.base_url = "https://api.openai.com";
    try { OpenAIResponses backend(missing_key); assert(false); }
    catch (const std::invalid_argument&) {}

    Config missing_url;
    missing_url.api_key = "sk-placeholder-for-offline-test";
    try { OpenAIResponses backend(missing_url); assert(false); }
    catch (const std::invalid_argument&) {}

    Config invalid_url;
    invalid_url.api_key = "sk-placeholder-for-offline-test";
    invalid_url.base_url = "api.openai.com";
    try { OpenAIResponses backend(invalid_url); assert(false); }
    catch (const std::invalid_argument&) {}

    Config explicit_config;
    explicit_config.api_key = "sk-placeholder-for-offline-test";
    explicit_config.base_url = "http://127.0.0.1:8765";
    OpenAIResponses backend(explicit_config); // Does not contact network.
    Agent configured(explicit_config);        // Explicit config reaches OpenAI backend.
  }
  Config cfg;
  cfg.instructions = "offline test agent";
  auto mock = std::make_shared<MockModel>();
  Agent agent(cfg, mock);
  // Typed C++ registration reuses the existing Server::tool and JSON Schema.
  agent.tool("add", "Add two integers", [](int a, int b) { return a + b; },
             arg<int>("a", "first"), arg<int>("b", "second"));

  // An independent MCP server can be attached with an ordinary Client.
  Server remote("remote", "1.0");
  remote.tool("echo", "Echo text", [](std::string value) { return value; },
              arg<std::string>("value"));
  auto remote_client = std::make_shared<Client>(
    [&remote](const Json& r) { return remote.handle(r); });
  agent.add_mcp("remote", remote_client);

  mock->handler = [&](const Json& request) {
    assert(request.at("model").as_string() == "gpt-4.1-mini");
    const Json& defs = request.at("tools");
    assert(defs.size() == 2);
    assert(defs[0].at("name").as_string() == "local__add");
    assert(defs[1].at("name").as_string() == "remote__echo");
    const Json& input = request.at("input");
    if (mock->count == 1)
      return call("local__add", R"({"a":19,"b":23})");
    if (mock->count == 2) {
      assert(input.size() == 3); // user, function_call, function_call_output
      assert(contains(std::string(input[2].at("output").as_string()), "42"));
      return assistant("The answer is 42.");
    }
    if (mock->count == 3) {
      assert(input.size() == 5); // previous round + new user + answer
      return assistant("Follow-up works.");
    }
    return assistant("Image received.");
  };
  assert(agent.ask("Please add 19 and 23") == "The answer is 42.");
  assert(agent.history().size() == 4);
  assert(agent.ask("Can you recall the result?") == "Follow-up works.");
  assert(agent.history().size() == 6);

  const fs::path tmp = fs::temp_directory_path() / "mymcp_agent_test";
  fs::create_directories(tmp / "skills" / "translator");
  {
    std::ofstream out(tmp / "skills" / "translator" / "SKILL.md");
    out << "---\nname: translator\ndescription: Translation instructions\n---\n"
           "Always translate faithfully.\n";
  }
  agent.load_skills(tmp / "skills");
  agent.use_skill("translator");
  assert(agent.skills().get("translator").description == "Translation instructions");

  const auto image = tmp / "sample.png";
  {
    std::ofstream out(image, std::ios::binary);
    out << "abc"; // Fake bytes: offline transport-shape test only.
  }
  mock->handler = [&](const Json& request) {
    assert(contains(std::string(request.at("instructions").as_string()), "Always translate"));
    const auto& input = request.at("input");
    const auto& parts = input[input.size() - 1].at("content");
    assert(parts.size() == 2);
    assert(parts[1].at("type").as_string() == "input_image");
    assert(parts[1].at("image_url").as_string() == "data:image/png;base64,YWJj");
    return assistant("Image received.");
  };
  assert(agent.ask_image("Describe", image) == "Image received.");

  agent.save_history(tmp / "history.json");
  assert(fs::exists(tmp / "history.json"));
  const auto before = agent.history().size();
  agent.clear_history();
  assert(agent.history().empty());
  agent.load_history(tmp / "history.json");
  assert(agent.history().size() == before);

  // Failed model calls leave previously committed history intact.
  mock->handler = [](const Json&) -> Json { throw std::runtime_error("fake offline failure"); };
  try { agent.ask("Will fail"); assert(false); }
  catch (const std::runtime_error&) {}
  assert(agent.history().size() == before);

  // Invalid/skipped skill and duplicate MCP namespaces are rejected.
  try { agent.use_skill("missing"); assert(false); }
  catch (const std::out_of_range&) {}
  try { agent.add_mcp("remote", remote_client); assert(false); }
  catch (const std::invalid_argument&) {}
  // Friendly transport APIs require no explicit Client / shared_ptr construction.
  // HTTP registration is lazy, so these tests do not contact the network.
  {
    auto mocked_transport_model = std::make_shared<MockModel>();
    Agent transports(cfg, mocked_transport_model);
    transports.add_mcp_http("public", "http://127.0.0.1:1/mcp");
    transports.add_mcp_http("bearer", "http://127.0.0.1:2/mcp", "test-only-token");
    auto oauth_session = std::make_shared<oauth::Session>("http://127.0.0.1:3/mcp");
    transports.add_mcp_oauth("secured", "http://127.0.0.1:3/mcp", oauth_session);

    // Duplicate name must be rejected before attempting a connection.
    try { transports.add_mcp_http("public", "http://127.0.0.1:4/mcp"); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_stdio("public", "nonexistent"); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_oauth("public", "http://127.0.0.1:5/mcp", oauth_session); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_http("bad prefix", "http://127.0.0.1:6/mcp"); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_http("empty_url", ""); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_stdio("empty_binary", ""); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_oauth("missing_session", "http://127.0.0.1:7/mcp", {}); assert(false); }
    catch (const std::invalid_argument&) {}
    try { transports.add_mcp_oauth("missing_url", "", oauth_session); assert(false); }
    catch (const std::invalid_argument&) {}
  }

  // Real child-process integration: tool discovery, invocation, and response
  // are exercised against the vendored myMcpTools stdio server executable.
  {
    auto stdio_model = std::make_shared<MockModel>();
    Agent stdio_agent(cfg, stdio_model);
    stdio_agent.add_mcp_stdio("worker", MYAGENT_TEST_STDIO_SERVER);
    stdio_model->handler = [&](const Json& request) -> Json {
      const Json& defs = request.at("tools");
      bool found_multiply = false;
      for (const auto& definition : defs.array_range()) {
        if (definition.at("name").as_string() == "worker__multiply")
          found_multiply = true;
      }
      assert(found_multiply);
      const auto& input = request.at("input");
      if (stdio_model->count == 1)
        return call("worker__multiply", R"({"lhs":6,"rhs":7})");
      assert(input.size() == 3);
      assert(contains(std::string(input[2].at("output").as_string()), "42"));
      return assistant("MCP stdio worked.");
    };
    assert(stdio_agent.ask("Multiply 6 by 7") == "MCP stdio worked.");
    assert(stdio_model->count == 2);
  }

  fs::remove_all(tmp);
  std::cout << "agent tests OK\n";
}
