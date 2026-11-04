#pragma once
#include "mymcp/json.hpp"
#include <string>
namespace mymcp {
// jsoncons JSON Schema 2020-12 (in-process references; external resolution disabled).
bool validate_schema(const json::value& schema, const json::value& instance,
                     std::string* error = nullptr);
}
