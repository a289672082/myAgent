// P0 Skill YAML 专项 Demo：无需模型 API Key、环境变量或命令行参数。
// 使用真实的 SKILL.md / agents/openai.yaml 样例，以及离线 Mock Model。
// 可直接运行 run_skill_yaml_demo；独立于 run_agent_demo。
#include "myagent/agent.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using myagent::Json;
namespace json = myagent::json;

namespace {

void ensure(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("Skill YAML Demo: " + message);
}
bool contains(const std::string& value, const std::string& fragment) {
  return value.find(fragment) != std::string::npos;
}

// 模拟 Responses API 返回的模型文本答复。
Json reply_text(const std::string& text) {
  return json::object{
    {"status", "completed"},
    {"output", json::array{
      json::object{
        {"type", "message"}, {"role", "assistant"},
        {"content", json::array{
          json::object{{"type", "output_text"}, {"text", text}}
        }}
      }
    }}
  };
}

// 模拟模型主动调用 Agent 的 Skill 工具，不需要网络请求。
Json reply_tool(const std::string& name, const std::string& call_id,
                const std::string& arguments) {
  return json::object{
    {"status", "completed"},
    {"output", json::array{
      json::object{
        {"type", "function_call"}, {"name", name},
        {"call_id", call_id}, {"arguments", arguments}
      }
    }}
  };
}

class SkillDemoModel final : public myagent::Model {
public:
  Json create(const Json& request) override {
    ++round_;
    const std::string instructions(request.at("instructions").as_string());
    const Json& input = request.at("input");

    if (round_ == 1) {
      // 只把自动可调用 Skill 的名称和描述发送给模型。
      ensure(contains(instructions, "cpp-code-review:"),
             "自动 Skill 目录没有包含 cpp-code-review");
      ensure(!contains(instructions, "claude-manual:") &&
             !contains(instructions, "codex-manual:"),
             "标记为仅手动调用的 Skill 不应出现在自动目录");
      ensure(!contains(instructions, "CHECK_MEMORY_BOUNDS"),
             "完整 Skill 正文不应在发现阶段注入");
      bool has_load = false;
      bool has_read = false;
      for (const auto& tool : request.at("tools").array_range()) {
        const std::string name(tool.at("name").as_string());
        has_load |= name == "agent_skill_load";
        has_read |= name == "agent_skill_read_file";
      }
      ensure(has_load && has_read, "缺少 Skill 自动加载/附件读取工具");
      std::cout << "[模型模拟] 任务匹配 cpp-code-review，调用 agent_skill_load\n";
      return reply_tool("agent_skill_load", "skill-load-1",
                        R"({"name":"cpp-code-review"})");
    }

    if (round_ == 2) {
      // Agent 已根据工具调用按需载入完整 SKILL.md。
      ensure(input.size() == 3, "首次工具调用没有加入会话输入");
      const std::string output(input[2].at("output").as_string());
      ensure(contains(output, "CHECK_MEMORY_BOUNDS"),
             "选中 Skill 后没有获得完整 SKILL.md");
      ensure(contains(output, "references/review-checklist.md"),
             "没有列出 Skill 参考文件");
      std::cout << "[模型模拟] 按需读取 references/review-checklist.md\n";
      return reply_tool(
        "agent_skill_read_file", "skill-read-2",
        R"({"name":"cpp-code-review","path":"references/review-checklist.md"})");
    }

    if (round_ == 3) {
      ensure(input.size() == 5, "第二次工具调用没有加入会话输入");
      const std::string output(input[4].at("output").as_string());
      ensure(contains(output, "CHECK_NULL_POINTERS"),
             "未能读取参考文件正文");
      return reply_text("已自动应用 cpp-code-review，并读取参考检查清单。");
    }

    if (round_ == 4 || round_ == 5) {
      // 仅手动调用的 Claude / Codex Skill，可以由 C++ 显式启用。
      ensure(contains(instructions, "# Explicit Skill: claude-manual") &&
             contains(instructions, "CLAUDE_MANUAL_CHECK"),
             "Claude 手动 Skill 没有生效");
      ensure(contains(instructions, "# Explicit Skill: codex-manual") &&
             contains(instructions, "CODEX_MANUAL_CHECK"),
             "Codex 手动 Skill 没有生效");
      if (round_ == 5)
        ensure(input.size() == 1, "clear_history() 后仍保留旧对话输入");
      return reply_text("两个手动 Skill 均已生效。");
    }
    throw std::runtime_error("Demo Mock Model 收到意外的请求轮次");
  }

  int rounds() const { return round_; }

private:
  int round_ = 0;
};

} // namespace

int run_skill_yaml_demo() {
  try {
    // 示例资源目录使用源码绝对路径定位；可以从任意工作目录运行。
    const fs::path demo_root = MYAGENT_SKILL_YAML_DEMO_DIR;
    const fs::path skills_dir = demo_root / "skills";
    const fs::path invalid_dir = demo_root / "invalid-skills";

    // 1. 解析 P0 新增的完整 YAML 元数据（含多行字符串和嵌套 map）。
    myagent::SkillLibrary library;
    library.load(skills_dir);
    ensure(library.all().size() == 3, "示例目录应包含三个 Skill");
    const auto& review = library.get("cpp-code-review");
    ensure(review.content.empty(), "发现阶段不应该加载 SKILL.md 正文");
    ensure(contains(review.description, "C++"), "多行 description 解析错误");
    ensure(review.license == "MIT", "license 解析错误");
    ensure(contains(review.compatibility, "C++20"), "compatibility 解析错误");
    ensure(review.metadata.at("author") == "myAgent examples",
           "metadata.author 解析错误");
    ensure(review.metadata.at("version") == "1.0", "metadata.version 解析错误");
    ensure(review.allowed_tools == "Read", "allowed-tools 解析错误");
    ensure(review.auto_invocable, "自动 Skill 不应被禁止调用");
    ensure(!library.get("claude-manual").auto_invocable,
           "Claude disable-model-invocation: true 未生效");
    ensure(!library.get("codex-manual").auto_invocable,
           "Codex allow_implicit_invocation: false 未生效");

    std::cout << "[YAML 元数据] " << review.name << "\n";
    std::cout << "  description: " << review.description << "\n";
    std::cout << "  license: " << review.license << "\n";
    std::cout << "  compatibility: " << review.compatibility << "\n";
    std::cout << "  author: " << review.metadata.at("author") << "\n";
    std::cout << "  allowed-tools: " << review.allowed_tools
              << "（仅为元数据，不授予工具权限）\n";

    // 2. 演示检测错误 Skill 文件：应给出文件名与错误字段。
    myagent::SkillLibrary invalid_library;
    bool rejected = false;
    try {
      invalid_library.load(invalid_dir);
    } catch (const std::runtime_error& e) {
      const std::string diagnostic = e.what();
      ensure(contains(diagnostic, "SKILL.md") &&
             contains(diagnostic, "description"),
             "格式错误应包含文件名和 description 字段");
      std::cout << "[校验失败（预期行为）] " << diagnostic << "\n";
      rejected = true;
    }
    ensure(rejected, "错误的 Skill 描述竟然被接受");

    // 3. 离线模拟模型按照任务自动选择 Skill，再读取 references 文本。
    myagent::Config config;
    config.auto_discover_skills = false; // 专项 Demo 只使用自身示例资源，避免扫描用户目录。
    config.auto_skill_selection = true;
    const auto mock_model = std::make_shared<SkillDemoModel>();
    myagent::Agent agent(config, mock_model);
    agent.load_skills(skills_dir);
    std::cout << "[自动选择] "
              << agent.ask("请使用适合的 Skill 检查 C++ 代码中的内存问题") << "\n";
    ensure(mock_model->rounds() == 3, "自动加载流程应执行三次 Mock 请求");

    // 4. 自动禁用不影响 use_skill() 的手动启用行为。
    agent.use_skill("claude-manual");
    agent.use_skill("codex-manual");
    std::cout << "[手动启用] " << agent.ask("使用手动 Skill 进行审核") << "\n";

    // 5. 清空对话历史后，已经注册/手动启用的 Skill 仍然有效。
    agent.clear_history();
    ensure(agent.history().empty(), "对话历史没有清空");
    std::cout << "[清空对话] " << agent.ask("开始新会话") << "\n";
    ensure(mock_model->rounds() == 5, "模拟流程调用次数异常");

    std::cout << "run_skill_yaml_demo: 全部检查通过。\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_skill_yaml_demo 失败: " << e.what() << '\n';
    return 1;
  }
}

int main() { return run_skill_yaml_demo(); }
