#pragma once
#include "myagent/loop.hpp"
#include <stdexcept>
#include <string>
namespace myagent::detail {
struct LoopAbort : std::runtime_error {
  LoopStatus status;
  LoopAbort(LoopStatus why, const std::string& message)
      : std::runtime_error(message), status(why) {}
};
} // namespace myagent::detail
