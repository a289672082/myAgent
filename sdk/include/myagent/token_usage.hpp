#pragma once
// Responses API 的实际 Token 用量。所有值均来自模型响应中的 usage 字段，
// 不根据文本长度、字节数或工具输出大小估算。
#include "mymcp/mcp.hpp"
#include <cstdint>

namespace myagent {
using mymcp::Json;

// 注意：cached_input_tokens 是 input_tokens 的子集；
// reasoning_output_tokens 是 output_tokens 的子集，不能再次累加到 total_tokens。
struct TokenUsage {
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
  std::uint64_t total_tokens = 0;
  std::uint64_t cached_input_tokens = 0;
  std::uint64_t reasoning_output_tokens = 0;
  std::uint64_t reported_calls = 0;   // 成功读取 usage 的 Responses 请求次数
  std::uint64_t unreported_calls = 0; // 无 usage 或无有效计数的 Responses 请求次数

  void add(const TokenUsage& other);
  bool complete() const { return unreported_calls == 0; }
};

// 读取单个 Responses API 返回的 usage，包括 input_tokens_details.cached_tokens
// 和 output_tokens_details.reasoning_tokens。字段缺失或格式不正确不估算：
// 将该次模型响应记为 unreported_calls=1，不抛出解析异常阻断正常回答。
TokenUsage parse_token_usage(const Json& response);
} // namespace myagent
