// 供所有 run_xxxx_demo 使用的共享配置与辅助函数，不属于 SDK 公共 API。
#pragma once
// Shared helpers for the no-argument examples, not part of the public SDK.
#include "mymcp/mcp.hpp"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace demo {
// 所有 HTTP 示例共用此端口，服务端与客户端保持一致。
inline constexpr unsigned short http_port = 33867;
inline constexpr const char* http_url = "http://127.0.0.1:33867/mcp";
// 供资源读取与 SSE 订阅共同使用的演示资源 URI。
inline constexpr const char* status_uri = "demo://current-status";

// 进程内客户端直接调用服务端处理函数，不启用网络或子进程。
inline mymcp::Client in_process(mymcp::Server& server) {
  return mymcp::Client([&server](const mymcp::Json& request) { return server.handle(request); });
}
// 统一断言：失败时抛出异常，由 execute 输出错误信息。
inline void check(bool ok, std::string_view what) {
  if (!ok) throw std::runtime_error(std::string(what));
}
// 将 MCP JSON 响应序列化打印，方便查看协议结构。
inline void print(std::string_view label, const mymcp::Json& result) {
  std::cout << label << ": " << mymcp::json::serialize(result) << '\n';
}
template<class Function>
// 统一运行入口：捕获异常并返回成功/失败退出码。
int execute(std::string_view name, Function&& body) {
  try {
    body();
    std::cout << name << " OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << name << " failed: " << error.what() << '\n';
    return 1;
  }
}
} // namespace demo
