#include "mymcp/schema.hpp"
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <exception>

namespace mymcp {
bool validate_schema(const json::value& schema, const json::value& instance,
                     std::string* error) {
  try {
    // jsoncons supports the JSON Schema 2020-12 dialect. Do not allow
    // network-resolved references from untrusted tool schemas.
    auto compiled = jsoncons::jsonschema::make_json_schema(schema);
    compiled.validate(instance);
    return true;
  } catch(const std::exception& e) {
    if(error) *error=e.what();
    return false;
  }
}
}
