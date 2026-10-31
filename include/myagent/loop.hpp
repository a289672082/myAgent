#pragma once
// Unified, synchronous Agent loops. Uses one Model/MCP/Skills runtime.
#include "mymcp/mcp.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace myagent {
using mymcp::Json;

enum class LoopMode { ReAct, PlanAndExecute, Reflection, EvaluatorOptimizer };
enum class LoopStatus { Completed, LimitReached, Cancelled, Failed };

// Audio is transcribed once before entering the loop. Images are kept as
// Responses input_image parts, subject to the model's vision capabilities.
struct LoopInput {
  std::string text;
  std::vector<std::filesystem::path> images;
  std::filesystem::path audio;
};

struct LoopOptions {
  unsigned max_iterations = 8;
  unsigned max_model_calls = 32; // Total across all phases and tool calls.
  unsigned max_tool_calls = 32;  // Total across all phases.
  unsigned max_plan_steps = 8;
  unsigned max_replans = 1;
  unsigned max_revisions = 3;
  double acceptance_score = 0.85;
  std::string planner_model;   // Empty uses Config::model on the same backend.
  std::string evaluator_model; // Empty uses Config::model on the same backend.
  bool commit_to_history = true;
  // Optional. Requires an OpenAIResponses backend and a speech-capable gateway.
  std::filesystem::path speech_output;
  std::string speech_voice = "alloy";
  // Both callbacks run synchronously on the calling thread. Not reentrant.
  std::function<bool()> cancelled;
  std::function<void(const Json&)> on_event;
};

struct LoopResult {
  LoopStatus status = LoopStatus::Failed;
  std::string answer;
  std::string error;
  std::string audio_transcript;
  std::filesystem::path speech_output;
  Json plan = mymcp::json::array{};
  Json evaluations = mymcp::json::array{};
  Json trace = mymcp::json::array{};
  unsigned model_calls = 0;
  unsigned tool_calls = 0;
  unsigned iterations = 0;
};
} // namespace myagent
