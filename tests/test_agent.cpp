// Fully offline regression tests for Responses orchestration and existing MCP reuse.
#include "myagent/agent.hpp"
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
  fs::remove_all(tmp);
  std::cout << "agent tests OK\n";
}
