#include "mymcp/uri_template.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <stdexcept>
#include <vector>

namespace mymcp {
namespace {
struct Operator {
  std::string first, separator;
  bool named = false, reserved = false;
  char empty = '=';
};
Operator operation(char ch) {
  switch (ch) {
    case '+': return {"", ",", false, true, '='};
    case '#': return {"#", ",", false, true, '='};
    case '.': return {".", ".", false, false, '='};
    case '/': return {"/", "/", false, false, '='};
    case ';': return {";", ";", true, false, '\0'};
    case '?': return {"?", "&", true, false, '='};
    case '&': return {"&", "&", true, false, '='};
    default:  return {"", ",", false, false, '='};
  }
}
bool is_operator(char c) { return std::string("+#./;?&").find(c) != std::string::npos; }
bool unreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
bool reserved(unsigned char c) {
  return std::string(":/?#[]@!$&'()*+,;=").find(static_cast<char>(c)) != std::string::npos;
}
std::string encode(std::string_view input, bool allow_reserved) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : input) {
    if (unreserved(c) || (allow_reserved && reserved(c)) || c == '%') {
      // 原始 '%' 必须编码，除非它是来自规范化输入的已编码序列。
      if (c != '%') {out += static_cast<char>(c); continue;}
    }
    out += '%'; out += hex[c >> 4]; out += hex[c & 15];
  }
  return out;
}
int from_hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
std::optional<std::string> decode(std::string_view value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {out += value[i]; continue;}
    if (i + 2 >= value.size()) return std::nullopt;
    const int a = from_hex(value[i+1]), b = from_hex(value[i+2]);
    if (a < 0 || b < 0) return std::nullopt;
    out += static_cast<char>((a << 4) | b);
    i += 2;
  }
  return out;
}
struct Spec {std::string name; bool explode = false; std::size_t prefix = 0;};
Spec parse_spec(std::string_view raw) {
  Spec spec;
  auto end = raw.find_first_of("*:");
  spec.name = std::string(raw.substr(0, end));
  if (spec.name.empty() || spec.name.size() > 64 ||
      !std::all_of(spec.name.begin(), spec.name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == '%';
      })) throw std::invalid_argument("Invalid RFC6570 template variable");
  if (end != std::string_view::npos) {
    auto modifier = raw.substr(end);
    if (modifier == "*") spec.explode = true;
    else if (modifier.size() > 1 && modifier.front() == ':') {
      auto digits = modifier.substr(1);
      if (digits.size() > 4 || digits.front() == '0' ||
          !std::all_of(digits.begin(), digits.end(), [](unsigned char c){return std::isdigit(c);}))
        throw std::invalid_argument("Invalid RFC6570 prefix modifier");
      std::size_t len = 0;
      const auto result = std::from_chars(digits.data(), digits.data()+digits.size(), len);
      if (result.ec != std::errc{} || len == 0 || len > 10000)
        throw std::invalid_argument("Invalid RFC6570 prefix length");
      spec.prefix = len;
    } else throw std::invalid_argument("Invalid RFC6570 template modifier");
  }
  return spec;
}
struct Expression {char op = '\0'; std::vector<Spec> vars;};
Expression parse_expression(std::string_view exp) {
  Expression out;
  if (exp.empty()) throw std::invalid_argument("Empty RFC6570 expression");
  if (is_operator(exp.front())) {out.op = exp.front();exp.remove_prefix(1);}
  for (std::size_t from = 0; from <= exp.size();) {
    const auto comma = exp.find(',', from);
    auto raw = exp.substr(from, comma == std::string_view::npos ? comma : comma-from);
    out.vars.push_back(parse_spec(raw));
    if (comma == std::string_view::npos) break;
    from = comma + 1;
  }
  return out;
}
std::string scalar(const Json& value) {
  if (value.is_string()) return std::string(value.as_string());
  if (value.is_bool() || value.is_number()) return json::serialize(value);
  throw std::invalid_argument("Unsupported RFC6570 scalar");
}
std::string prefix_unicode(std::string value, std::size_t count) {
  if (!count) return value;
  std::size_t pos=0, chars=0;
  while (pos < value.size() && chars < count) {
    const unsigned char c = static_cast<unsigned char>(value[pos]);
    const std::size_t n = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
        (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (n == 0 || pos+n > value.size()) throw std::invalid_argument("Invalid UTF-8 prefix");
    for (std::size_t j=1;j<n;++j)
      if ((static_cast<unsigned char>(value[pos+j]) & 0xc0) != 0x80)
        throw std::invalid_argument("Invalid UTF-8 prefix");
    pos += n; ++chars;
  }
  value.resize(pos);
  return value;
}
std::string join(const std::vector<std::string>& items, std::string_view separator) {
  std::string out;
  for (const auto& x : items) {
    if (!out.empty()) out += separator;
    out += x;
  }
  return out;
}
std::string expand_expression(const Expression& expression, const Json& variables) {
  const Operator op = operation(expression.op);
  std::vector<std::string> members;
  for (const auto& spec : expression.vars) {
    const Json* value = json::if_contains(variables, spec.name);
    if (!value || value->is_null()) continue;
    const auto name = encode(spec.name, false);
    if (value->is_array()) {
      if (spec.prefix) throw std::invalid_argument("Prefix modifier requires a scalar");
      std::vector<std::string> parts;
      for (const auto& v : value->array_range()) {
        const auto item = encode(scalar(v), op.reserved);
        parts.push_back(spec.explode && op.named ? name + (item.empty() && op.empty == '\0' ? "" : "=" + item) : item);
      }
      if (parts.empty()) continue;
      if (spec.explode) members.push_back(join(parts, op.separator));
      else members.push_back((op.named ? name + "=" : "") + join(parts, ","));
    } else if (value->is_object()) {
      if (spec.prefix) throw std::invalid_argument("Prefix modifier requires a scalar");
      std::vector<std::string> parts;
      for (const auto& item : value->object_range()) {
        const auto key = encode(item.key(), op.reserved);
        const auto val = encode(scalar(item.value()), op.reserved);
        if (spec.explode) parts.push_back(key + (val.empty() && op.empty == '\0' ? "" : "=" + val));
        else {parts.push_back(key);parts.push_back(val);}
      }
      if (parts.empty()) continue;
      members.push_back((op.named && !spec.explode ? name + "=" : "") +
          join(parts, spec.explode ? op.separator : ","));
    } else {
      const auto value_string = encode(prefix_unicode(scalar(*value), spec.prefix), op.reserved);
      members.push_back(op.named ? name + (value_string.empty() && op.empty == '\0' ? "" : "=" + value_string)
                                 : value_string);
    }
  }
  return members.empty() ? "" : op.first + join(members, op.separator);
}
void append_regex_literal(std::string& regex, std::string_view literal) {
  for (char c : literal) {
    if (std::string(R"(\.[]{}()+*?^$|)").find(c) != std::string::npos)
      regex += '\\';
    regex += c;
  }
}
} // namespace

std::string expand_uri_template(std::string_view pattern, const Json& variables) {
  if (!variables.is_object() || pattern.size() > 8192)
    throw std::invalid_argument("Invalid URI template or variables");
  std::string out;
  for (std::size_t pos = 0; pos < pattern.size();) {
    if (pattern[pos] == '}') throw std::invalid_argument("Unexpected closing template brace");
    if (pattern[pos] != '{') {out += pattern[pos++];continue;}
    const auto end = pattern.find('}',pos+1);
    if (end == std::string_view::npos) throw std::invalid_argument("Unclosed URI template");
    out += expand_expression(parse_expression(pattern.substr(pos+1,end-pos-1)),variables);
    pos=end+1;
  }
  return out;
}

std::optional<Json> match_uri_template(std::string_view pattern, std::string_view uri) {
  // RFC6570 对复杂列表/映射不可逆；资源读取只能安全提取无歧义的标量变量。
  if (pattern.size() > 8192 || uri.size() > 8192) return std::nullopt;
  std::string regex = "^";
  std::vector<Spec> names;
  try {
    for (std::size_t pos = 0; pos < pattern.size();) {
      if (pattern[pos] != '{') {
        const auto end = pattern.find('{',pos);
        append_regex_literal(regex,pattern.substr(pos,end == std::string_view::npos ? end : end-pos));
        if (end == std::string_view::npos) break;
        pos=end;continue;
      }
      const auto end = pattern.find('}',pos+1);
      if (end == std::string_view::npos) return std::nullopt;
      auto exp = parse_expression(pattern.substr(pos+1,end-pos-1));
      const auto op = operation(exp.op);
      append_regex_literal(regex,op.first);
      for (std::size_t i = 0; i < exp.vars.size(); ++i) {
        const auto& spec = exp.vars[i];
        if (spec.explode) return std::nullopt;
        if (i) append_regex_literal(regex,op.separator);
        if (op.named) {
          append_regex_literal(regex,spec.name);
          regex += "=?";
        }
        regex += "([^&#]*?)";
        names.push_back(spec);
      }
      pos=end+1;
    }
    regex += "$";
    std::smatch m;
    const std::string text(uri);
    if (!std::regex_match(text,m,std::regex(regex))) return std::nullopt;
    json::object variables;
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto decoded = decode(m[i+1].str());
      if (!decoded || variables.contains(names[i].name)) return std::nullopt;
      variables[names[i].name] = *decoded;
    }
    // 禁止宽松匹配导致编码变体或歧义模板错误指向另一个资源。
    if (expand_uri_template(pattern,variables) != uri) return std::nullopt;
    return Json(std::move(variables));
  } catch (const std::exception&) {return std::nullopt;}
}
} // namespace mymcp
