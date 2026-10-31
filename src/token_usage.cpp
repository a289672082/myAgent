#include "myagent/token_usage.hpp"
#include <limits>
#include <stdexcept>
#include <utility>

namespace myagent {
namespace json = mymcp::json;
namespace {
// 必须是非负整型计数；不能把字符串、浮点数或负数当作实际 Token。
bool read_uint(const Json& value, std::uint64_t& out) {
  if (value.is_uint64()) {
    out = value.as<std::uint64_t>();
    return true;
  }
  if (value.is_int64() && value.as<std::int64_t>() >= 0) {
    out = static_cast<std::uint64_t>(value.as<std::int64_t>());
    return true;
  }
  return false;
}
bool get_counter(const Json& obj, const char* name, std::uint64_t& out) {
  if (!obj.is_object()) return false;
  const auto* field = json::if_contains(obj, name);
  return field && read_uint(*field, out);
}
// optional details 只有明确提供时才读取；未提供时计为 0，但不宣称平台没有消耗。
bool optional_counter(const Json& obj, const char* parent, const char* name,
                      std::uint64_t& out) {
  const auto* details = json::if_contains(obj, parent);
  if (!details || details->is_null()) return true;
  if (!details->is_object()) return false;
  const auto* value = json::if_contains(*details, name);
  return !value || value->is_null() || read_uint(*value, out);
}
void checked_add(std::uint64_t& dst, std::uint64_t src) {
  if (src > std::numeric_limits<std::uint64_t>::max() - dst)
    throw std::overflow_error("TokenUsage counters overflow");
  dst += src;
}
} // namespace

void TokenUsage::add(const TokenUsage& other) {
  checked_add(input_tokens, other.input_tokens);
  checked_add(output_tokens, other.output_tokens);
  checked_add(total_tokens, other.total_tokens);
  checked_add(cached_input_tokens, other.cached_input_tokens);
  checked_add(reasoning_output_tokens, other.reasoning_output_tokens);
  checked_add(reported_calls, other.reported_calls);
  checked_add(unreported_calls, other.unreported_calls);
}

TokenUsage parse_token_usage(const Json& response) {
  TokenUsage usage;
  // 没有响应统计的模型兼容网关不影响回答；但统计并非完整。
  const auto* raw = response.is_object() ? json::if_contains(response, "usage") : nullptr;
  if (!raw || !raw->is_object() ||
      !get_counter(*raw, "input_tokens", usage.input_tokens) ||
      !get_counter(*raw, "output_tokens", usage.output_tokens)) {
    return TokenUsage{.unreported_calls = 1};
  }
  // 按平台实际 total_tokens 记账；仅在 total 缺省时用两部分相加。
  const auto* total = json::if_contains(*raw, "total_tokens");
  if (total) {
    if (!read_uint(*total, usage.total_tokens))
      return TokenUsage{.unreported_calls = 1};
  } else {
    if (usage.input_tokens > std::numeric_limits<std::uint64_t>::max()
                               - usage.output_tokens)
      return TokenUsage{.unreported_calls = 1};
    usage.total_tokens = usage.input_tokens + usage.output_tokens;
  }
  if (!optional_counter(*raw, "input_tokens_details", "cached_tokens",
                        usage.cached_input_tokens) ||
      !optional_counter(*raw, "output_tokens_details", "reasoning_tokens",
                        usage.reasoning_output_tokens) ||
      usage.cached_input_tokens > usage.input_tokens ||
      usage.reasoning_output_tokens > usage.output_tokens) {
    return TokenUsage{.unreported_calls = 1};
  }
  usage.reported_calls = 1;
  return usage;
}
} // namespace myagent
