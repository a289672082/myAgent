// 扩展示例：演示自定义能力标识与非核心 JSON-RPC 方法。
// No command line parameters: custom method and namespaced extension capability.
#include "demo_common.hpp"

int run_extensions_demo() {
  return demo::execute("run_extensions_demo", [] {
    mymcp::Server server("extensions-demo");
    // 向客户端声明自定义扩展的命名空间及版本。
    server.extension("example.com/demo", mymcp::json::object{{"revision", 1}});
    // 注册自定义 echo 方法，原样回传请求参数。
    server.method("example/echo", [](const mymcp::Json& params) -> mymcp::Json {
      return mymcp::json::object{{"echo", params}};
    });
    // 用进程内客户端执行能力发现和自定义请求。
    auto client = demo::in_process(server);
    // 查看服务端公布的扩展能力信息。
    demo::print("server extensions", client.discover());
    // 通过通用 request 接口调用自定义方法；不代表实现了官方 Apps 扩展。
    demo::print("custom method", client.request("example/echo", mymcp::json::object{{"message", "hello"}}));
    // Extensions advertised here are examples, NOT an implementation of the full Apps extension.
  });
}
int main() { return run_extensions_demo(); }
