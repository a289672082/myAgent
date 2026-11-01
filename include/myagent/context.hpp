#pragma once
// 上下文压缩：旧轮次结构化摘要 + 新轮次原样保留。
// 触发判断只使用保守近似 Token；计费仍以 Responses usage 为准。
#include "mymcp/mcp.hpp"
#include "myagent/token_usage.hpp"
#include <cstddef>
#include <functional>
#include <string>
namespace myagent {
using mymcp::Json;

struct ContextOptions {
  bool auto_compress = false;          // 未显式指定模型容量时不自动假定 128k。
  std::size_t max_context_tokens = 0; // 0 表示未知；自动触发需显式提供。
  double compress_threshold = 0.80;
  std::size_t reserve_output_tokens = 4096;
  std::size_t keep_recent_turns = 2;
  std::size_t max_summary_chars = 8192;
  std::string summary_model;           // 空则使用 Agent 默认模型及相同 endpoint/key。
};
struct ContextStats {
  std::size_t estimated_tokens = 0; // 非精确计数，不用于计费。
  std::size_t compression_count = 0;
  std::size_t removed_messages = 0;
  TokenUsage compression_usage; // 所有已返回的摘要模型 usage，包括未采用的摘要。
};
struct CompressionResult {
  bool compressed = false;
  std::size_t original_messages = 0;
  std::size_t remaining_messages = 0;
  std::size_t estimated_tokens_before = 0;
  std::size_t estimated_tokens_after = 0;
  std::string reason;
};
class ContextManager {
public:
  explicit ContextManager(ContextOptions options = {});
  const ContextOptions& options() const { return options_; }
  ContextStats stats(const Json& history) const;
  // extra_tokens 用于工具 Schema 与系统指令等非 history 内容的预算保留。
  bool needs_compression(const Json& history, std::size_t extra_tokens = 0) const;
  // 提供一个 callback：将完整摘要请求交给 Model，返回 Responses JSON。
  // 只删除完整的旧用户轮次，保留最近 N 轮（含其所有 tool call/output）。
  // 如果摘要失败，history 一字不改；不在工具调用与结果中间裁剪。
  CompressionResult compress(Json& history, const std::function<Json(const Json&)>& summarize,
                              bool force = false);
  void clear_stats() { stats_ = {}; }
private:
  ContextOptions options_;
  ContextStats stats_;
};
} // namespace myagent
