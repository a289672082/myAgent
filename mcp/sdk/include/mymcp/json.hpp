#pragma once
// jsoncons adapter: keep the concise object/array literal syntax of the SDK.
// No Boost runtime or headers are required.
#include <jsoncons/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace mymcp::json {
using value = jsoncons::json;
struct object : value {
    object() : value(jsoncons::json_object_arg) {}
    object(std::initializer_list<std::pair<std::string, value>> init)
      : value(jsoncons::json_object_arg, init) {}
    object(const value& v) : value(v) {}
    object(value&& v) : value(std::move(v)) {}
};
struct array : value {
    array() : value(jsoncons::json_array_arg) {}
    array(std::initializer_list<value> init)
      : value(jsoncons::json_array_arg, init) {}
    array(const value& v) : value(v) {}
    array(value&& v) : value(std::move(v)) {}
};
inline value parse(const std::string& text) { return value::parse(text); }
inline std::string serialize(const value& v) { return v.to_string(); }
inline const value* if_contains(const value& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.object_range().end() ? nullptr : &it->value();
}
inline value* if_contains(value& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.object_range().end() ? nullptr : &it->value();
}
}

// jsoncons's templated conversion constructor otherwise selects the generic
// reflection path for these light convenience wrappers instead of base copying.
namespace jsoncons {
template<class Json>
struct json_type_traits<Json, mymcp::json::object, void> {
  using allocator_type = typename Json::allocator_type;
  static Json to_json(const mymcp::json::object& v, const allocator_type&) {
    return static_cast<const mymcp::json::value&>(v);
  }
  static bool is(const Json& v) noexcept { return v.is_object(); }
  static mymcp::json::object as(const Json& v) { return mymcp::json::object(v); }
};
template<class Json>
struct json_type_traits<Json, mymcp::json::array, void> {
  using allocator_type = typename Json::allocator_type;
  static Json to_json(const mymcp::json::array& v, const allocator_type&) {
    return static_cast<const mymcp::json::value&>(v);
  }
  static bool is(const Json& v) noexcept { return v.is_array(); }
  static mymcp::json::array as(const Json& v) { return mymcp::json::array(v); }
};
}
