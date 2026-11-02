#pragma once
// 轻量 OpenTelemetry Span JSONL 导出（无 SDK、无网络上传）。
// 只记录白名单元数据，不包含 API Key、prompt、工具参数或模型响应。
#include "myagent/loop.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
namespace myagent::detail {
inline std::string trace_hex(std::size_t bytes) {
  std::random_device rd;
  std::ostringstream out;
  out<<std::hex<<std::setfill('0');
  for(std::size_t i=0;i<bytes;++i)out<<std::setw(2)<<(rd()&0xffu);
  return out.str();
}
inline std::uint64_t unix_nanos() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
inline void export_span(const LoopOptions& options,Json span) {
  if(options.on_span)options.on_span(span);
  if(!options.otel_jsonl_path.empty()) {
    std::ofstream out(options.otel_jsonl_path,std::ios::binary|std::ios::app);
    if(!out)throw std::runtime_error("Cannot append OpenTelemetry span JSONL");
    out<<mymcp::json::serialize(span)<<"\n";
    out.flush();
    if(!out)throw std::runtime_error("OpenTelemetry JSONL write failed");
  }
}
} // namespace myagent::detail
