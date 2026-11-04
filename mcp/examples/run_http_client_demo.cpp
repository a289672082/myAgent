// HTTP 客户端示例：依次执行发现、工具列表、工具调用和资源读取。
// No arguments: connect to run_http_server_demo on localhost.
#include "demo_common.hpp"

int run_http_client_demo() {
  return demo::execute("run_http_client_demo", [] {
    // HTTP 地址来自共享配置；先运行 run_http_server_demo。
    auto client = mymcp::Client::http(demo::http_url);
    // 进行 MCP 版本和能力发现。
    demo::print("HTTP discovery", client.discover());
    // 查询服务端工具列表及参数 Schema。
    demo::print("tools", client.tools());
    // 执行快速工具 hello，通常得到 JSON 响应。
    demo::print("call", client.call("hello"));
    // 读取服务端注册的状态资源。
    demo::print("resource", client.read(demo::status_uri));
  });
}
int main() { return run_http_client_demo(); }
