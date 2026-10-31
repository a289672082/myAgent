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
static void test_plan_replanning_and_isolation() {
  auto model = std::make_shared<MockModel>();
  Agent agent(config(), model);
  agent.tool("check", "Stub check", [](std::string task) { return task + " verified"; },
             mymcp::arg<std::string>("task"));
  model->handler = [](const Json& req, unsigned step) {
    if (step == 1) {
      assert(!j::if_contains(req, "tools"));
      assert(req.at("model").as_string() == "planner-model");
      return answer(R"({"steps":[{"id":"first","description":"initial task"}]})");
    }
    if (step == 2) {
      assert(j::if_contains(req, "tools"));
      return answer(R"({"status":"blocked","reason":"Need alternate check"})");
    }
    if (step == 3) {
      assert(!j::if_contains(req, "tools"));
      return answer(R"({"steps":[{"id":"second","description":"alternate task"}]})");
    }
    if (step == 4) return call("local__check", R"({"task":"alternate"})");
    if (step == 5) {
      assert(has(std::string(req.at("input")[8].at("output").as_string()), "verified"));
      return answer("alternate step completed");
    }
    assert(!j::if_contains(req, "tools"));
    return answer("Plan finished after replanning");
  };
  LoopOptions options;
  options.max_plan_steps = 2;
  options.max_replans = 1;
  options.planner_model = "planner-model";
  auto r = agent.plan_and_execute("Run a two-stage recovery", options);
  assert(r.status == LoopStatus::Completed);
  assert(r.plan.size() == 2 && r.tool_calls == 1);
  assert(r.plan[0].at("status").as_string() == "blocked");
  assert(r.plan[1].at("status").as_string() == "completed");
  assert(r.model_calls == 6);
  assert(agent.history().size() == 2);

  // When history commits are disabled, no phase messages leak.
  model->calls = 0;
  model->handler = [](const Json& req, unsigned step) {
    if (step == 1) return answer(R"({"steps":[{"id":"one","description":"inspect"}]})");
    if (step == 2) return answer("done");
    return answer("summary");
  };
  LoopOptions no_commit;
  no_commit.commit_to_history = false;
  const auto previous_size = agent.history().size();
  r = agent.plan_and_execute("Keep working transcript private", no_commit);
  assert(r.status == LoopStatus::Completed && agent.history().size() == previous_size);
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
  test_reflection_and_evaluation();
  test_errors_and_cancellation();
  std::cout << "Unified loop regression tests OK\n";
}
