// 资源示例：演示静态资源、资源模板和缓存提示。
// No command line parameters: static resources and parameterized resource templates.
#include "demo_common.hpp"

int run_resources_demo() {
  return demo::execute("run_resources_demo", [] {
    mymcp::Server server("resources-demo");
    // 缓存提示为 30 秒 public；这里并没有在本地自动缓存资源内容。
    server.cache(30000, "public");
    // 注册固定 URI 的文本资源。
    server.resource("demo://readme", "Readme", [] { return std::string("C++ MCP resources"); });
    // 使用 URI 模板动态解析 id，例如 demo://notes/17。
    server.resource_template("demo://notes/{id}", "Example note",
      [](const std::string& uri, const mymcp::Json& vars) -> mymcp::Json {
        return mymcp::json::object{{"contents", mymcp::json::array{mymcp::json::object{
          {"uri", uri}, {"mimeType", "text/plain"},
          {"text", std::string("Note #") + vars.at("id").as_string()}}}}};
      });
    // 创建进程内客户端，查看资源及模板目录。
    auto client = demo::in_process(server);
    demo::print("resource list", client.resources());
    demo::print("resource templates", client.resource_templates());
    demo::print("read fixed resource", client.read("demo://readme"));
    // 读取模板实例，核对响应的 contents 字段。
    auto note = client.read("demo://notes/17");
    demo::check(note.contains("contents"), "resource template did not resolve");
    demo::print("read template", note);
  });
}
int main() { return run_resources_demo(); }
