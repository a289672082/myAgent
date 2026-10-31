// 子 Agent Skills / MCP 配置演示：全部离线运行，逐步演示两个开关。
//
// ① auto_discover_skills：Agent 构造时是否扫描项目/用户 Skill 目录。
//    false → 仅使用本 Agent 显式 load_skills() 指定的目录，更适合严格角色隔离。
//    true  → 在构造时自动发现 .agents/skills、.claude/skills 等目录。
// ② auto_skill_selection：是否把可调用 Skill 清单和 agent_skill_load 工具交给模型。
//    true  → 模型在需要时按需加载 SKILL.md；false → 只能由 C++ 调 use_skill()。
// ③ MCP 工具按 Agent 单独注册，不共享；本例用 in-process Client/Server 离线验证。
//    生产可将 add_mcp() 改为 add_mcp_http()、add_mcp_stdio() 或 add_mcp_oauth()。
// ④ 子 Agent 模型使用不同 base_url / api_key / model；Mock 模型不发真实请求。
#include "loop_demo_support.hpp"
#include "myagent/multi_agent.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}
bool has_tool(const myagent::Json& request, const std::string& name) {
  const auto* defs = myagent::json::if_contains(request, "tools");
  if (!defs) return false;
  for (const auto& item : defs->array_range())
    if (item.at("name").as_string() == name) return true;
  return false;
}
// 测试自动发现时临时切换至固定示例工作目录，并确保异常情况下恢复原目录。
// 注意：current_path() 是进程级状态，实际多线程应用不建议这样修改。
struct ScopedCurrentDirectory {
  explicit ScopedCurrentDirectory(const fs::path& path) : previous(fs::current_path()) {
    fs::current_path(path);
  }
  ~ScopedCurrentDirectory() { fs::current_path(previous); }
  fs::path previous;
};
} // namespace

int run_agent_delegation_demo() {
  try {
    const fs::path demo_root = MYAGENT_TEAM_DEMO_SKILLS_DIR;

    // ========== 主 Agent：本例只做显式任务委派 ==========
    auto leader = loop_demo::config();
    leader.base_url = "https://coordinator.example.com";
    leader.api_key = "example-leader-key";
    leader.model = "coordinator-model";
    leader.auto_discover_skills = false;
    leader.auto_skill_selection = false;
    myagent::AgentTeam team(leader, std::make_shared<loop_demo::MockModel>());

    // ========== 子 Agent A：Coder，仅开放自己的 Skill 和文件 MCP ==========
    myagent::AgentSpec coder;
    coder.name = "coder";
    coder.description = "负责 C++20 开发；可查看项目文件";
    coder.config = loop_demo::config();
    coder.config.base_url = "https://provider-a.example.com/v1/responses";
    coder.config.append_responses_path = false;
    coder.config.api_key = "example-coder-key";
    coder.config.model = "coder-model";
    coder.config.auto_discover_skills = false; // 禁止把用户/项目全局 Skills 混入 Coder。
    coder.config.auto_skill_selection = true;  // 允许模型主动使用 agent_skill_load。

    auto coder_model = std::make_shared<loop_demo::MockModel>();
    coder_model->handle = [](const myagent::Json& request, unsigned round) {
      loop_demo::check(request.at("model").as_string() == "coder-model",
                       "Coder 使用自己的模型");
      const std::string instructions(request.at("instructions").as_string());
      loop_demo::check(contains(instructions, "cpp-develop"),
                       "Coder 应看到自己的 Skill 描述");
      loop_demo::check(!contains(instructions, "cpp-review"),
                       "Coder 不应看到 Reviewer 的 Skill");
      loop_demo::check(has_tool(request, "files__inspect"),
                       "Coder 应拥有专用 files MCP");
      loop_demo::check(!has_tool(request, "review__audit"),
                       "Coder 不应拥有 Reviewer 的 MCP");
      loop_demo::check(has_tool(request, "agent_skill_load"),
                       "auto_skill_selection=true 应允许按需加载 Skill");

      if (round == 1) {
        // 模拟模型根据当前任务主动选择 cpp-develop。
        return loop_demo::tool_call("agent_skill_load", R"({"name":"cpp-develop"})");
      }
      if (round == 2) {
        const std::string output(request.at("input")[2].at("output").as_string());
        loop_demo::check(contains(output, "CODER_SKILL_ACTIVE"),
                         "Skill 按需加载未得到完整指令");
        return loop_demo::tool_call("files__inspect", R"({"relative":"src/demo.cpp"})");
      }
      if (round == 3) {
        const std::string output(request.at("input")[4].at("output").as_string());
        loop_demo::check(contains(output, "src/demo.cpp"),
                         "文件 MCP 执行结果未回传给 Coder");
        return loop_demo::answer("Coder 已使用 cpp-develop 和 files MCP。");
      }
      throw std::runtime_error("Coder Mock 收到意外轮次");
    };
    team.add_agent(coder, coder_model);

    // 只加载 Coder 的专用 Skill 目录；不会自动授予任何文件/命令权限。
    auto& coder_agent = team.agent("coder");
    coder_agent.load_skills(demo_root / "coder");
    loop_demo::check(coder_agent.skills().all().size() == 1,
                     "Coder 应只有一个专属 Skill");

    // 创建独立的内存 MCP Server + Client，真实执行 tools/list 和 tools/call。
    mymcp::Server files_mcp("files-server", "1.0.0");
    files_mcp.tool("inspect", "查看演示项目文件",
      [](const std::string& relative) { return "文件已读取: " + relative; },
      mymcp::arg<std::string>("relative"));
    auto files_client = std::make_shared<mymcp::Client>(
      [&files_mcp](const myagent::Json& request) {
        return files_mcp.handle(request);
      });
    coder_agent.add_mcp("files", files_client);

    // 正式接入真实 MCP 服务时使用下面的简易接口（不要同时注册相同 prefix）：
    // coder_agent.add_mcp_http("files", "https://your-mcp.example.com/mcp", "mcp-bearer-token");
    // coder_agent.add_mcp_stdio("git", "./your-git-mcp-server");
    // coder_agent.add_mcp_oauth("private", "https://example.com/mcp", authorized_oauth_session);

    // ========== 子 Agent B：Reviewer，手动固定 Skill + 只开放审查 MCP ==========
    myagent::AgentSpec reviewer;
    reviewer.name = "reviewer";
    reviewer.description = "负责 C++ 代码审查，不开放 Coder 的文件 MCP";
    reviewer.config = loop_demo::config();
    reviewer.config.base_url = "https://provider-b.example.com";
    reviewer.config.api_key = "example-reviewer-key";
    reviewer.config.model = "reviewer-model";
    reviewer.config.auto_discover_skills = false;
    reviewer.config.auto_skill_selection = false; // 不给模型 agent_skill_load 工具。

    auto reviewer_model = std::make_shared<loop_demo::MockModel>();
    reviewer_model->handle = [](const myagent::Json& request, unsigned round) {
      loop_demo::check(request.at("model").as_string() == "reviewer-model",
                       "Reviewer 使用独立模型");
      const std::string instructions(request.at("instructions").as_string());
      loop_demo::check(contains(instructions, "# Explicit Skill: cpp-review") &&
                       contains(instructions, "REVIEWER_SKILL_ACTIVE"),
                       "Reviewer 应通过 use_skill() 固定完整审查指令");
      loop_demo::check(!contains(instructions, "CODER_SKILL_ACTIVE"),
                       "Reviewer 不应读取 Coder 的 Skill");
      loop_demo::check(!has_tool(request, "agent_skill_load"),
                       "auto_skill_selection=false 时不应提供 Skill 加载工具");
      loop_demo::check(!has_tool(request, "files__inspect"),
                       "Reviewer 不应拥有 Coder 的文件 MCP");
      loop_demo::check(has_tool(request, "review__audit"),
                       "Reviewer 应拥有审查 MCP");
      if (round == 1)
        return loop_demo::tool_call("review__audit", R"({"file":"src/demo.cpp"})");
      if (round == 2) {
        const std::string output(request.at("input")[2].at("output").as_string());
        loop_demo::check(contains(output, "检查结果"),
                         "审查 MCP 的结果未回传给 Reviewer");
        return loop_demo::answer("Reviewer 使用 cpp-review 和 review MCP 完成审查。");
      }
      throw std::runtime_error("Reviewer Mock 收到意外轮次");
    };
    team.add_agent(reviewer, reviewer_model);

    auto& reviewer_agent = team.agent("reviewer");
    reviewer_agent.load_skills(demo_root / "reviewer");
    reviewer_agent.use_skill("cpp-review"); // 自动选择关闭后，显式固定该 Skill。
    loop_demo::check(reviewer_agent.skills().all().size() == 1,
                     "Reviewer 应只看到自己的 Skill");

    mymcp::Server review_mcp("review-server", "1.0.0");
    review_mcp.tool("audit", "进行代码审查，不执行写文件操作",
      [](const std::string& file) { return "检查结果: " + file + " 通过"; },
      mymcp::arg<std::string>("file"));
    auto review_client = std::make_shared<mymcp::Client>(
      [&review_mcp](const myagent::Json& request) {
        return review_mcp.handle(request);
      });
    reviewer_agent.add_mcp("review", review_client);

    // ========== 子 Agent C：演示 auto_discover_skills = true ==========
    // auto_discover_skills 在 Agent 构造时执行，因此只在 add_agent() 周围
    // 将工作目录切换为包含 .agents/skills 的示例目录。
    myagent::AgentSpec automatic;
    automatic.name = "automatic";
    automatic.description = "演示项目 Skill 自动发现（无需 load_skills）";
    automatic.config = loop_demo::config();
    automatic.config.model = "discovery-model";
    automatic.config.auto_discover_skills = true;
    automatic.config.auto_skill_selection = true;
    auto discovery_model = std::make_shared<loop_demo::MockModel>();
    discovery_model->handle = [](const myagent::Json& request, unsigned) {
      loop_demo::check(contains(std::string(request.at("instructions").as_string()),
                           "auto-discovered"),
                       "自动发现 Skill 应出现在模型可选的目录中");
      return loop_demo::answer("已自动发现 auto-discovered Skill");
    };
    {
      ScopedCurrentDirectory in_fixture(demo_root / "auto-root");
      team.add_agent(automatic, discovery_model); // 构造时进行项目目录搜索。
    }
    loop_demo::check(team.agent("automatic").skills().get("auto-discovered").name ==
                     "auto-discovered", "auto_discover_skills=true 没有生效");

    // ========== 运行并验证：三套 Skill/MCP 不会混用 ==========
    const auto code = team.delegate("coder", "用 cpp-develop 查看 C++ 示例源文件");
    const auto review = team.delegate("reviewer", "用 cpp-review 检查 C++ 示例代码");
    const auto discovered = team.delegate("automatic", "列出可用的项目 Skill");

    loop_demo::check(code.status == myagent::LoopStatus::Completed &&
                     code.tool_calls == 2, "Coder 自动 Skill + MCP 调用");
    loop_demo::check(review.status == myagent::LoopStatus::Completed &&
                     review.tool_calls == 1, "Reviewer 手动 Skill + MCP 调用");
    loop_demo::check(discovered.status == myagent::LoopStatus::Completed,
                     "自动发现 Agent 执行失败");
    loop_demo::check(coder_agent.history().size() == 2 &&
                     reviewer_agent.history().size() == 2,
                     "各子 Agent 独立对话历史");

    std::cout << code.answer << '\n'
              << review.answer << '\n'
              << discovered.answer << '\n'
              << "run_agent_delegation_demo：Skill 自动发现 / 自动选择 / 手动启用 / MCP 隔离均通过。\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "run_agent_delegation_demo 失败: " << e.what() << '\n';
    return 1;
  }
}
int main() { return run_agent_delegation_demo(); }
