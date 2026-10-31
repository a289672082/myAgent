// Fully offline skill discovery and progressive disclosure tests.
#include "myagent/agent.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace myagent;
namespace fs = std::filesystem;

struct SkillModel : Model {
  std::function<Json(const Json&)> handler;
  int round = 0;
  Json create(const Json& request) override { ++round; return handler(request); }
};
static Json msg(const std::string& text) {
  return json::object{{"status", "completed"},
    {"output", json::array{json::object{{"type", "message"}, {"role", "assistant"},
      {"content", json::array{json::object{{"type", "output_text"}, {"text", text}}}}}}}};
}
static Json call(const char* name, const char* args) {
  return json::object{{"status", "completed"}, {"output", json::array{
    json::object{{"type", "function_call"}, {"name", name}, {"call_id", "skill-call"},
                 {"arguments", args}}}}};
}
static bool has(const std::string& s, const std::string& part) {
  return s.find(part) != std::string::npos;
}
static void write(const fs::path& file, const std::string& content) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  assert(bool(out));
  out << content;
}
int main() {
  const auto old = fs::current_path();
  const auto temp = fs::temp_directory_path() / "myagent_skills_offline_test";
  fs::remove_all(temp);
  const auto project = temp / "project";
  const auto reviews = project / ".agents" / "skills" / "code-review";
  const auto review_file = reviews / "SKILL.md";
  const auto secondary = project / ".claude" / "skills" / "rewrite";
  write(review_file,
    "---\r\nname: code-review\r\ndescription: Review C++ code for correctness\r\n---\r\n"
    "CHECK FOR MEMORY SAFETY ISSUES.\r\nRead references/checklist.md when needed.\r\n");
  write(reviews / "references" / "checklist.md", "CHECK RETURN VALUES.\n");
  write(reviews / "scripts" / "check.py", "print('do not execute')\n");
  write(reviews / "assets" / "template.txt", "example template\n");
  write(reviews / "assets" / "image.bin", std::string("a\0b", 3));
  write(secondary / "SKILL.md",
    "---\nname: rewrite\ndescription: Rewrite natural-language text\n---\n"
    "Make text clear and concise.\n");
  // Duplicate global-like skill should lose to the first project discovery root.
  write(project / ".claude" / "skills" / "code-review" / "SKILL.md",
    "---\nname: code-review\ndescription: Alternate reviewer\n---\nDUPLICATE\n");
  fs::current_path(project);

  // Loading metadata does not eagerly read the full instructions.
  SkillLibrary library;
  library.load(project / ".agents" / "skills");
  assert(library.all().size() == 1);
  assert(library.get("code-review").content.empty());
  assert(!has(library.get("code-review").description, "MEMORY SAFETY"));
  assert(has(library.read("code-review"), "CHECK FOR MEMORY SAFETY"));
  assert(has(library.read_file("code-review", "references/checklist.md"), "CHECK RETURN VALUES"));
  assert(has(library.read_file("code-review", "scripts/check.py"), "do not execute"));
  assert(has(library.read_file("code-review", "assets/template.txt"), "example template"));
  assert(library.supporting_files("code-review").size() == 4);
  try { library.read_file("code-review", "../outside.txt"); assert(false); }
  catch (const std::invalid_argument&) {}
  try { library.read_file("code-review", "references/../../outside.txt"); assert(false); }
  catch (const std::invalid_argument&) {}
  try { library.read_file("code-review", "/etc/passwd"); assert(false); }
  catch (const std::invalid_argument&) {}
  try { library.read_file("code-review", "SKILL.md"); assert(false); }
  catch (const std::invalid_argument&) {}
  try { library.read_file("code-review", "assets/image.bin"); assert(false); }
  catch (const std::invalid_argument&) {}

  Config config;
  config.auto_discover_skills = true;
  config.auto_skill_selection = true;
  auto model = std::make_shared<SkillModel>();
  Agent agent(config, model); // No OpenAI key required for offline mock.
  assert(agent.skills().get("code-review").description ==
         "Review C++ code for correctness");
  assert(agent.skills().get("rewrite").description ==
         "Rewrite natural-language text");
  model->handler = [&](const Json& request) -> Json {
    const std::string instructions(request.at("instructions").as_string());
    assert(has(instructions, "code-review: Review C++ code for correctness"));
    assert(has(instructions, "rewrite: Rewrite natural-language text"));
    assert(!has(instructions, "CHECK FOR MEMORY SAFETY")); // No eager content injection.
    bool found_load = false, found_read = false;
    for (const auto& tool : request.at("tools").array_range()) {
      const std::string name(tool.at("name").as_string());
      found_load |= name == "agent_skill_load";
      found_read |= name == "agent_skill_read_file";
    }
    assert(found_load && found_read);
    const auto& input = request.at("input");
    if (model->round == 1)
      return call("agent_skill_load", R"({"name":"code-review"})");
    if (model->round == 2) {
      assert(has(std::string(input[2].at("output").as_string()),
                 "CHECK FOR MEMORY SAFETY"));
      assert(has(std::string(input[2].at("output").as_string()),
                 "references/checklist.md"));
      return call("agent_skill_read_file",
                  R"({"name":"code-review","path":"references/checklist.md"})");
    }
    assert(has(std::string(input[4].at("output").as_string()), "CHECK RETURN VALUES"));
    return msg("I applied the code-review skill.");
  };
  assert(agent.ask("Review this C++ code") == "I applied the code-review skill.");
  assert(model->round == 3);
  assert(agent.skills().all().size() >= 2);

  // Reset conversation but preserve automatic Skill registration.
  agent.clear_history();
  assert(agent.history().empty());
  assert(agent.skills().get("code-review").name == "code-review");
  model->round = 0;
  model->handler = [&](const Json& request) -> Json {
    const auto& input = request.at("input");
    if (model->round == 1) {
      // A previous request's Skill activation does not auto-inject into a new turn.
      assert(!has(std::string(request.at("instructions").as_string()),
                  "CHECK FOR MEMORY SAFETY"));
      return call("agent_skill_read_file",
                  R"({"name":"code-review","path":"references/checklist.md"})");
    }
    assert(has(std::string(input[2].at("output").as_string()), "isError"));
    return msg("Skill reading requires activation.");
  };
  assert(agent.ask("Read the review checklist") == "Skill reading requires activation.");

  // Explicit user activation still injects instructions, and stays enabled.
  model->round = 0;
  agent.use_skill("code-review");
  model->handler = [&](const Json& req) -> Json {
    assert(has(std::string(req.at("instructions").as_string()),
               "CHECK FOR MEMORY SAFETY"));
    return msg("Manual activation retained.");
  };
  assert(agent.ask("Review again") == "Manual activation retained.");

  // Selection can be disabled without disabling explicit use_skill().
  Config manual_config;
  manual_config.auto_discover_skills = false;
  manual_config.auto_skill_selection = false;
  auto manual_model = std::make_shared<SkillModel>();
  Agent manual(manual_config, manual_model);
  manual.load_skills(project / ".agents" / "skills");
  manual.use_skill("code-review");
  manual_model->handler = [](const Json& request) {
    assert(has(std::string(request.at("instructions").as_string()),
               "CHECK FOR MEMORY SAFETY"));
    const auto* tools = json::if_contains(request, "tools");
    if (tools) for (const auto& item : tools->array_range())
      assert(item.at("name").as_string() != "agent_skill_load");
    return msg("Manual only");
  };
  assert(manual.ask("Review") == "Manual only");

  fs::current_path(old);
  fs::remove_all(temp);
  std::cout << "Skill auto-discovery tests OK\n";
}
