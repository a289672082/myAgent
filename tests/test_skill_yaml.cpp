// P0: Agent Skills YAML specification and Codex/Claude policy compatibility.
#include "myagent/agent.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace myagent;

static void write(const fs::path& file, const std::string& value) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  assert(out.good());
  out << value;
}
static bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}
static void expect_invalid(const fs::path& root,
                           const std::string& yaml,
                           const std::string& fragment) {
  fs::remove_all(root);
  write(root / "bad-skill" / "SKILL.md", yaml);
  SkillLibrary lib;
  try { lib.load(root); assert(false && "Expected a rejected Skill"); }
  catch (const std::runtime_error& e) {
    const std::string message = e.what();
    assert(contains(message, "SKILL.md"));
    assert(contains(message, fragment));
  }
  assert(lib.all().empty());
}
static Json message(const std::string& body) {
  return json::object{{"status", "completed"}, {"output", json::array{
    json::object{{"type", "message"}, {"role", "assistant"},
                 {"content", json::array{
                   json::object{{"type", "output_text"}, {"text", body}}
                 }}}}}};
}
struct Mock final : Model {
  std::function<Json(const Json&)> reply;
  Json create(const Json& request) override { return reply(request); }
};

int main() {
  const auto temp = fs::temp_directory_path() / "myagent_skill_yaml_spec_tests";
  fs::remove_all(temp);
  const auto root = temp / "valid-skills";
  const auto rich = root / "rich-skill";
  write(rich / "SKILL.md",
    "\xef\xbb\xbf---\r\n"
    "name: rich-skill\r\n"
    "description: >-\r\n"
    "  Review source code and configuration files:\r\n"
    "  use when security or portability matters.\r\n"
    "license: 'Apache-2.0'\r\n"
    "compatibility: |\r\n"
    "  Requires a C++20 toolchain.\r\n"
    "  Internet access is optional.\r\n"
    "metadata:\r\n"
    "  author: 'Example: Organization'\r\n"
    "  version: \"1.0\"\r\n"
    "allowed-tools: 'Read Bash(git:*)'\r\n"
    "custom-extension:\r\n"
    "  hooks:\r\n"
    "    stop: true\r\n"
    "---\r\n"
    "# Review rules\r\n"
    "Remember to check bounds.\r\n");
  write(root / "manual-claude" / "SKILL.md",
    "---\nname: manual-claude\n"
    "description: Run only when explicitly requested.\n"
    "disable-model-invocation: true\n"
    "user-invocable: true\n"
    "---\nManual rules.\n");
  write(root / "manual-codex" / "SKILL.md",
    "---\nname: manual-codex\n"
    "description: Useful but requires explicit invocation.\n"
    "---\nCodex manual rules.\n");
  write(root / "manual-codex" / "agents" / "openai.yaml",
    "interface:\n"
    "  display_name: \"Manual Codex\"\n"
    "  short_description: >-\n"
    "    A manually invoked example\n"
    "policy:\n"
    "  allow_implicit_invocation: false\n"
    "dependencies:\n"
    "  tools:\n"
    "    - type: mcp\n"
    "      value: filesystem\n");

  SkillLibrary lib;
  lib.load(root);
  assert(lib.all().size() == 3);
  const auto& skill = lib.get("rich-skill");
  assert(skill.content.empty()); // Discovery never injected the Markdown body.
  assert(skill.name == "rich-skill");
  assert(contains(skill.description, "configuration files: use when"));
  assert(skill.license == "Apache-2.0");
  assert(contains(skill.compatibility, "C++20 toolchain."));
  assert(skill.metadata.at("author") == "Example: Organization");
  assert(skill.metadata.at("version") == "1.0");
  assert(skill.allowed_tools == "Read Bash(git:*)");
  assert(skill.auto_invocable);
  assert(!lib.get("manual-claude").auto_invocable);
  assert(!lib.get("manual-codex").auto_invocable);
  assert(contains(lib.read("rich-skill"), "Remember to check bounds."));

  Config config;
  config.auto_discover_skills = false;
  config.auto_skill_selection = true;
  auto model = std::make_shared<Mock>();
  Agent agent(config, model);
  agent.load_skills(root);
  model->reply = [](const Json& request) {
    const std::string instructions(request.at("instructions").as_string());
    assert(contains(instructions, "rich-skill"));
    assert(!contains(instructions, "manual-claude:"));
    assert(!contains(instructions, "manual-codex:"));
    return message("catalog honors policies");
  };
  assert(agent.ask("Describe available tools") == "catalog honors policies");
  agent.use_skill("manual-codex");
  model->reply = [](const Json& request) {
    const std::string instructions(request.at("instructions").as_string());
    assert(contains(instructions, "Codex manual rules."));
    return message("manual activation preserved");
  };
  assert(agent.ask("Follow manual procedure") == "manual activation preserved");

  const auto negative = temp / "negative";
  expect_invalid(negative, "No YAML frontmatter\n", "must start");
  expect_invalid(negative, "---\nname: bad-skill\n", "closing");
  expect_invalid(negative, "---\nname: bad-skill\n---\n", "description");
  expect_invalid(negative, "---\ndescription: A test\n---\n", "name");
  expect_invalid(negative,
    "---\nname: BadSkill\ndescription: A test\n---\n", "name");
  expect_invalid(negative,
    "---\nname: wrong-folder\ndescription: A test\n---\n", "parent folder");
  expect_invalid(negative,
    "---\nname: bad--skill\ndescription: A test\n---\n", "name");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: []\n---\n", "description");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\nmetadata: []\n---\n", "metadata");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\nmetadata:\n  version: [1,2]\n---\n",
    "metadata");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\nmetadata:\n  author: a\n  author: b\n---\n",
    "duplicate YAML key");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\ndescription: Repeated\n---\n",
    "duplicate YAML key");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\ncompatibility: []\n---\n",
    "compatibility");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\ncompatibility: ''\n---\n",
    "compatibility");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\nallowed-tools: [Read]\n---\n",
    "allowed-tools");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\n"
    "disable-model-invocation: nope\n---\n", "disable-model-invocation");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\nmetadata: [unterminated\n---\n",
    "invalid YAML");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: " + std::string(1025, 'd') + "\n---\n",
    "description");
  expect_invalid(negative,
    "---\nname: bad-skill\ndescription: A test\ncompatibility: " +
    std::string(501, 'x') + "\n---\n", "compatibility");

  // A YAML mapping that mixes valid and invalid entries must roll back entirely.
  const auto mixed = temp / "mixed";
  write(mixed / "first-skill" / "SKILL.md",
    "---\nname: first-skill\ndescription: Fine.\n---\n");
  write(mixed / "second-skill" / "SKILL.md",
    "---\nname: second-skill\nmetadata: [invalid]\n---\n");
  SkillLibrary atomic;
  try { atomic.load(mixed); assert(false); }
  catch (const std::runtime_error&) {}
  assert(atomic.all().empty());

  // Invalid optional Codex policies give a precise path and field name.
  const auto codex = temp / "codex-invalid";
  write(codex / "policy-skill" / "SKILL.md",
    "---\nname: policy-skill\ndescription: A test\n---\n");
  write(codex / "policy-skill" / "agents" / "openai.yaml",
    "policy:\n  allow_implicit_invocation: not-a-bool\n");
  SkillLibrary broken;
  try { broken.load(codex); assert(false); }
  catch (const std::runtime_error& ex) {
    assert(contains(ex.what(), "openai.yaml"));
    assert(contains(ex.what(), "allow_implicit_invocation"));
  }

  fs::remove_all(temp);
  std::cout << "Agent Skills YAML spec tests OK\n";
}
