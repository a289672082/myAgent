// Offline regression tests for all loop modes, budgets, multimodal inputs,
// tool isolation, re-planning, cancellation, history and failure behavior.
#include "myagent/agent.hpp"
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

using namespace myagent;
namespace fs = std::filesystem;
namespace j = myagent::json;

static Json answer(const std::string& text) {
  return j::object{{"status", "completed"}, {"output", j::array{
    j::object{{"type", "message"}, {"role", "assistant"}, {"content", j::array{
      j::object{{"type", "output_text"}, {"text", text}}
    }}}}}};
}
static Json call(const std::string& name, const std::string& args) {
  return j::object{{"status", "completed"}, {"output", j::array{
    j::object{{"type", "function_call"}, {"name", name},
              {"call_id", "call-1"}, {"arguments", args}}}}};
}
static bool has(const std::string& text, const std::string& phrase) {
  return text.find(phrase) != std::string::npos;
}
struct MockModel : Model {
  unsigned calls = 0;
  std::function<Json(const Json&, unsigned)> handler;
  Json create(const Json& req) override { return handler(req, ++calls); }
};
static Config config() {
  Config c;
  c.auto_discover_skills = false;
  c.auto_skill_selection = false;
  return c;
}
static void test_react_and_images() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  agent.tool("sum", "Sum two integers", [](int a, int b) { return a + b; },
             mymcp::arg<int>("a"), mymcp::arg<int>("b"));
  model->handler = [](const Json& req, unsigned round) {
    if (round == 1) {
      const auto& parts = req.at("input")[0].at("content");
      assert(parts.size() == 3); // Text + 2 images.
      assert(parts[0].at("type").as_string() == "input_text");
      assert(parts[1].at("type").as_string() == "input_image");
      assert(parts[2].at("type").as_string() == "input_image");
      assert(has(std::string(parts[1].at("image_url").as_string()), "data:image/png;base64,"));
      return call("local__sum", R"({"a":20,"b":22})");
    }
    assert(has(std::string(req.at("input")[2].at("output").as_string()), "42"));
    return answer("Image task: answer 42");
  };
  LoopInput input;
  input.text = "Compare two images then calculate";
  input.images = {MYAGENT_LOOP_TEST_IMAGE, MYAGENT_LOOP_TEST_IMAGE};
  unsigned events = 0;
  LoopOptions opts;
  opts.on_event = [&](const Json&) { ++events; };
  auto r = agent.react(input, opts);
  assert(r.status == LoopStatus::Completed);
  assert(r.tool_calls == 1 && r.model_calls == 2);
  assert(r.iterations == 2 && events >= 3);
  assert(r.trace.size() >= 3);
  assert(agent.history().size() == 2);
  assert(agent.last_response().is_object());

  // A successful legacy ask() still uses the same tool runtime and history.
  model->calls = 0;
  model->handler = [](const Json&, unsigned) { return answer("Follow-up"); };
  assert(agent.ask("Follow-up?") == "Follow-up");
  assert(agent.history().size() == 4);

  // A total tool budget prevents executing a tool and does not modify history.
  model->calls = 0;
  model->handler = [](const Json&, unsigned) {
    return call("local__sum", R"({"a":1,"b":1})");
  };
  LoopOptions tool_cap;
  tool_cap.max_tool_calls = 0;
  const auto before = agent.history().size();
  r = agent.react("Trigger tool", tool_cap);
  assert(r.status == LoopStatus::LimitReached && r.tool_calls == 0);
  assert(agent.history().size() == before);
}
// 规划 → 执行 → 校验 → 安全重规划，验证“过时步骤不得继续执行”。
static void test_plan_replanning_and_isolation() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  int compile_exit_code = -1;
  bool code_fixed = false;
  int build_calls = 0;
  agent.tool("compile", "模拟编译并返回真实退出码", [&] {
    ++build_calls;
    compile_exit_code = code_fixed ? 0 : 1;
    return compile_exit_code;
  });
  agent.tool("fix", "修复模拟源文件", [&] { code_fixed = true; return true; });
  agent.tool("test", "模拟单元测试", [&] { return code_fixed && compile_exit_code == 0; });

  model->handler = [&](const Json& req, unsigned round) -> Json {
    switch (round) {
    case 1:
      assert(!j::if_contains(req, "tools"));
      assert(req.at("model").as_string() == "planner-model");
      return answer(R"JSON({"steps":[
        {"id":"compile","description":"编译项目","success_criteria":"退出码为 0"},
        {"id":"publish","description":"发布新版本","depends_on":["compile"]}
      ]})JSON");
    case 2:
      return call("local__compile", "{}");
    case 3:
      // 第一次编译失败。即使模型声称 completed，宿主验证器也必须拒绝。
      assert(build_calls == 1 && compile_exit_code == 1);
      return answer(R"({"status":"completed","summary":"模型认为构建成功"})");
    case 4:
      assert(!j::if_contains(req, "tools"));
      assert(has(std::string(req.at("input")[req.at("input").size()-1]
        .at("content").as_string()), "failed"));
      return answer(R"JSON({"steps":[
        {"id":"fix","description":"修复编译错误"},
        {"id":"compile2","description":"再次编译","depends_on":["fix"]},
        {"id":"tests","description":"执行测试","depends_on":["compile2"]}
      ]})JSON");
    case 5: return call("local__fix", "{}");
    case 6: return answer(R"({"status":"completed","summary":"已修复源文件"})");
    case 7: return call("local__compile", "{}");
    case 8: return answer(R"({"status":"completed","summary":"编译返回成功"})");
    case 9: return call("local__test", "{}");
    case 10: return answer(R"({"status":"completed","summary":"测试通过"})");
    case 11:
      assert(!j::if_contains(req, "tools"));
      return answer("编译失败后已经修复、重新编译并测试；未自动发布");
    default: assert(false && "Unexpected model request"); return answer("unexpected");
    }
  };
  LoopOptions options;
  options.max_plan_steps = 5;
  options.max_iterations = 6;
  options.max_replans = 1;
  options.planner_model = "planner-model";
  int validation_calls = 0;
  options.validate_step = [&](const Json& step, const Json& execution,
                              const Json& tool_results) {
    ++validation_calls;
    const std::string id(step.at("id").as_string());
    const std::string status(execution.at("status").as_string());
    assert(status == "completed");
    assert(tool_results.size() == 1);
    StepValidation validation;
    if (id == "compile" || id == "compile2") {
      validation.passed = compile_exit_code == 0;
      validation.reason = "编译退出码不等于零";
      validation.evidence = j::object{{"exit_code", compile_exit_code}};
    } else if (id == "fix") {
      validation.passed = code_fixed;
      validation.evidence = j::object{{"source_modified", code_fixed}};
    } else if (id == "tests") {
      validation.passed = code_fixed && compile_exit_code == 0;
      validation.evidence = j::object{{"tests_passed", validation.passed}};
    }
    return validation;
  };
  const auto result = agent.plan_and_execute("编译修复后运行测试", options);
  assert(result.status == LoopStatus::Completed);
  assert(result.plan.size() == 5);
  assert(result.plan[0].at("status").as_string() == "failed");
  assert(result.plan[0].at("verification").as_string() == "failed");
  assert(result.plan[1].at("status").as_string() == "superseded");
  assert(result.plan[2].at("status").as_string() == "completed");
  assert(result.plan[3].at("status").as_string() == "completed");
  assert(result.plan[4].at("status").as_string() == "completed");
  assert(result.plan[3].at("verification").as_string() == "verified");
  assert(result.plan[3].at("evidence").at("exit_code").as<int>() == 0);
  assert(build_calls == 2 && validation_calls == 4);
  assert(result.model_calls == 11 && result.tool_calls == 4);
  assert(agent.history().size() == 2);
  // commit_to_history=false 不提交临时步骤、工具结果和计划。
  auto isolated = std::make_shared<MockModel>();
  Agent second(config(), isolated);
  isolated->handler = [](const Json& req, unsigned round) {
    if (round == 1) return answer(R"({"steps":[{"id":"one","description":"核对文件"}]})");
    if (round == 2) return answer(R"({"status":"completed","summary":"已核对"})");
    return answer("检查完成（仅模型报告）");
  };
  LoopOptions no_commit;
  no_commit.commit_to_history = false;
  const auto r = second.plan_and_execute("保留当前历史", no_commit);
  assert(r.status == LoopStatus::Completed && second.history().empty());
  assert(r.plan[0].at("verification").as_string() == "unverified");
}

// 验证计划依赖：不允许前向引用、不存在的步骤、重复 ID、自引用或环依赖。
static void test_plan_dependency_validation() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  const std::vector<std::string> invalid = {
    R"({"steps":[{"id":"a","description":"a","depends_on":["missing"]}]})",
    R"({"steps":[{"id":"a","description":"a","depends_on":["a"]}]})",
    R"({"steps":[{"id":"a","description":"a"},{"id":"a","description":"duplicated"}]})",
    R"({"steps":[{"id":"a","description":"a","depends_on":["b"]},{"id":"b","description":"b"}]})",
    R"({"steps":[{"id":"a","description":"a","depends_on":"other"}]})"
  };
  for (const auto& plan : invalid) {
    model->handler = [plan](const Json&, unsigned) { return answer(plan); };
    const auto r = agent.plan_and_execute("拒绝非法依赖");
    assert(r.status == LoopStatus::Failed);
    assert(agent.history().empty());
  }
}

// 失败步骤默认不自动重试：如果已调用工具，不应让写文件/命令重复执行。
// 当重试策略未明确允许工具副作用时，应通过重规划处理。
static void test_tool_failure_and_retry_guard() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  int writes = 0;
  agent.tool("write_once", "模拟有副作用的写入", [&] { ++writes; return false; });
  model->handler = [](const Json&, unsigned round) {
    if (round == 1) return answer(R"({"steps":[{"id":"write","description":"写入文件"}]})");
    if (round == 2) return call("local__write_once", "{}");
    return answer(R"({"status":"failed","summary":"写入失败"})");
  };
  LoopOptions options;
  options.max_step_retries = 3;
  options.max_replans = 0;
  const auto r = agent.plan_and_execute("写文件失败测试", options);
  assert(r.status == LoopStatus::LimitReached);
  assert(writes == 1); // 已发生工具调用，因此默认不做可能有副作用的重试。
  assert(r.plan.size() == 1 && r.plan[0].at("attempts").as<int>() == 1);
  assert(r.plan[0].at("status").as_string() == "failed");
  assert(agent.history().empty());

  // 没有工具调用的阶段可以按用户设定的上限做有限重试。
  auto retryModel = std::make_shared<MockModel>();
  Agent retryAgent(config(), retryModel);
  retryModel->handler = [](const Json&, unsigned round) {
    if (round == 1) return answer(R"({"steps":[{"id":"calc","description":"解释公式"}]})");
    if (round == 2) return answer(R"({"status":"failed","summary":"信息不足"})");
    if (round == 3) return answer(R"({"status":"completed","summary":"已补充公式"})");
    return answer("最终答复");
  };
  LoopOptions retry;
  retry.max_step_retries = 1;
  retry.max_replans = 0;
  const auto rr = retryAgent.plan_and_execute("重新解释公式", retry);
  assert(rr.status == LoopStatus::Completed && rr.plan[0].at("attempts").as<int>() == 2);
  assert(rr.plan[0].at("verification").as_string() == "unverified");
}
static void test_reflection_and_evaluation() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  model->handler = [](const Json& req, unsigned step) {
    if (step == 1) return answer("unsafe draft");
    if (step == 2) {
      assert(!j::if_contains(req, "tools"));
      return answer(R"({"needs_revision":true,"feedback":"fix safety"})");
    }
    if (step == 3) return answer("safe draft");
    return answer(R"({"needs_revision":false,"feedback":"good"})");
  };
  LoopOptions options;
  options.evaluator_model = "reviewer-model";
  auto r = agent.reflect("Write a safe example", options);
  assert(r.status == LoopStatus::Completed && r.answer == "safe draft");
  assert(r.evaluations.size() == 2 && r.model_calls == 4);
  assert(agent.history().size() == 2);

  model->calls = 0;
  model->handler = [](const Json& req, unsigned step) {
    if (step == 1) return answer("bad candidate");
    if (step == 2) {
      assert(req.at("model").as_string() == "reviewer-model");
      assert(!j::if_contains(req, "tools"));
      return answer(R"({"score":0.5,"passed":false,"feedback":"incorrect","issues":["bug"]})");
    }
    if (step == 3) return answer("good candidate");
    return answer(R"({"score":0.95,"passed":true,"feedback":"good","issues":[]})");
  };
  options.acceptance_score = 0.9;
  r = agent.evaluate_and_optimize("Optimize safely", options);
  assert(r.status == LoopStatus::Completed && r.answer == "good candidate");
  assert(r.evaluations.size() == 2 && r.iterations == 2);
  assert(agent.history().size() == 4);

  // Revision exhaustion returns the last candidate but never commits it.
  model->calls = 0;
  model->handler = [](const Json&, unsigned step) {
    return (step % 2) ? answer("draft") :
           answer(R"({"needs_revision":true,"feedback":"not enough"})");
  };
  LoopOptions limit;
  limit.max_revisions = 0;
  const auto old = agent.history().size();
  r = agent.reflect("Improve", limit);
  assert(r.status == LoopStatus::LimitReached && r.answer == "draft");
  assert(agent.history().size() == old);
}
static void test_errors_and_cancellation() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  model->handler = [](const Json&, unsigned) {
    return answer("plain text instead of a valid plan");
  };
  auto r = agent.plan_and_execute("Invalid planner output");
  assert(r.status == LoopStatus::Failed && has(r.error, "Planner"));
  assert(agent.history().empty());

  LoopOptions cancelled;
  cancelled.cancelled = [] { return true; };
  r = agent.react("Cancelled", cancelled);
  assert(r.status == LoopStatus::Cancelled && r.model_calls == 0);
  assert(agent.history().empty());

  // A model-only test cannot transcribe audio; error is explicit, not simulated.
  LoopInput audio;
  audio.text = "Transcribe this";
  audio.audio = "not-a-real-file.wav";
  r = agent.reflect(audio);
  assert(r.status == LoopStatus::Failed && has(r.error, "transcription backend"));

  LoopInput empty;
  r = agent.react(empty);
  assert(r.status == LoopStatus::Failed && has(r.error, "requires text"));

  // Speech synthesis must not produce a fake MP3 with a mock model.
  model->calls = 0;
  model->handler = [](const Json&, unsigned) { return answer("done"); };
  LoopOptions speech;
  speech.speech_output = "no-such-speech.mp3";
  r = agent.react("Speak", speech);
  assert(r.status == LoopStatus::Failed && has(r.error, "requires OpenAIResponses"));
  assert(agent.history().empty());

  LoopOptions max_calls;
  max_calls.max_model_calls = 1;
  model->calls = 0;
  model->handler = [](const Json&, unsigned) {
    return call("agent_skill_load", R"({"name":"missing"})");
  };
  r = agent.react("Tool budget", max_calls);
  assert(r.status == LoopStatus::Failed || r.status == LoopStatus::LimitReached);
  assert(agent.history().empty());
}
int main() {
  test_react_and_images();
  test_plan_replanning_and_isolation();
  test_plan_dependency_validation();
  test_tool_failure_and_retry_guard();
  test_reflection_and_evaluation();
  test_errors_and_cancellation();
  std::cout << "Unified loop regression tests OK\n";
}
