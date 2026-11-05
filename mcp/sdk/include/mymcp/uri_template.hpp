#pragma once
#include "mymcp/mcp.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace mymcp {
// RFC 6570 展开：支持操作符 + # . / ; ? &、变量列表、:prefix 和 *，
// 以及字符串、数组与对象。解析资源 URI 时只提取无歧义的标量变量。
std::string expand_uri_template(std::string_view pattern, const Json& variables);
std::optional<Json> match_uri_template(std::string_view pattern, std::string_view uri);
} // namespace mymcp
