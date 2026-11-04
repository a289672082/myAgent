// 工具示例：演示类型安全的工具注册、JSON Schema 验证与自动分页。
// No command line parameters: run_tools_demo demonstrates typed tools, JSON Schema and pagination.
#include "demo_common.hpp"
#include <string>

int run_tools_demo() {
  return demo::execute("run_tools_demo", [] {
    // 创建服务端，本示例直接在进程内调用，不启动 HTTP 或 stdio。
    mymcp::Server server("tools-demo");
    // 每页只返回一个工具，以便演示客户端的自动分页。
    server.page_size(1); // each page has one tool
    // 使用 arg<int> 声明参数，SDK 自动生成整数参数的 JSON Schema。
    server.tool("add", "Add two numbers", [](int a, int b) { return a + b; },
                mymcp::arg<int>("a"), mymcp::arg<int>("b"));
    server.tool("hello", "Say hello", [](std::string name) { return "Hello, " + name; },
                mymcp::arg<std::string>("name"));
    // 手写 Schema，要求 value 是 0~100 的整数。
    server.add_tool("percentage", "Only accept 0..100",
      mymcp::json::object{{"type", "object"},
        {"properties", mymcp::json::object{{"value", mymcp::json::object{
          {"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}},
        {"required", mymcp::json::array{"value"}}},
      [](const mymcp::Json& args) -> mymcp::Json {
        return mymcp::detail::make_tool_result(args.at("value").as_integer<int>());
      });

    // 创建进程内客户端，直接把 JSON-RPC 消息交给服务端处理。
    auto client = demo::in_process(server);
    demo::print("discovery", client.discover());
    // list_all 自动跟踪 nextCursor 并合并所有工具分页。
    auto tools = client.list_all("tools/list", "tools");
    demo::check(tools.size() == 3, "pagination failed");
    demo::print("all tools", tools);
    auto answer = client.call("add", mymcp::json::object{{"a", 10}, {"b", 32}});
    demo::print("add(10,32)", answer);
    demo::print("hello", client.call("hello", mymcp::json::object{{"name", "C++"}}));
    demo::print("valid schema", client.call("percentage", mymcp::json::object{{"value", 42}}));
    try {
      // 故意传入 110，验证越界参数会触发 JSON-RPC -32602。
      (void)client.call("percentage", mymcp::json::object{{"value", 110}});
      throw std::runtime_error("invalid JSON Schema input was accepted");
    } catch (const mymcp::Error& e) {
      demo::check(e.code == -32602, "incorrect JSON-RPC error for schema violation");
      std::cout << "invalid percentage rejected as expected\n";
    }
  });
}
int main() { return run_tools_demo(); }
