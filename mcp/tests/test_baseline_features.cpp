#include "mymcp/mcp.hpp"
#include "mymcp/uri_template.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x))throw std::runtime_error("failed: " #x);}while(false)
using namespace mymcp;
static void expect_error(const std::function<void()>& callback, int code) {
  try {callback();} catch(const Error& e) {CHECK(e.code==code);return;}
  throw std::runtime_error("expected MCP error");
}
int main() {
  // RFC6570：保留字符、片段、路径、查询、分号、前缀以及列表/对象展开。
  const Json vars = json::object{{"id", "中 文"}, {"path", "docs/readme.md"},
    {"list", json::array{"a","b"}}, {"color","red"},
    {"map",json::object{{"x","1"},{"y","2"}}}};
  CHECK(expand_uri_template("test://books/{id}",vars)=="test://books/%E4%B8%AD%20%E6%96%87");
  CHECK(expand_uri_template("test://books/{+path}",vars)=="test://books/docs/readme.md");
  CHECK(expand_uri_template("x{#path}",vars)=="x#docs/readme.md");
  CHECK(expand_uri_template("x{/color}",vars)=="x/red");
  CHECK(expand_uri_template("x{?color}",vars)=="x?color=red");
  CHECK(expand_uri_template("x{;color}",vars)=="x;color=red");
  CHECK(expand_uri_template("x{?list*}",vars)=="x?list=a&list=b");
  CHECK(expand_uri_template("x{?map*}",vars)=="x?x=1&y=2");
  CHECK(expand_uri_template("x{.color}",vars)=="x.red");
  CHECK(expand_uri_template("x{color:2}",vars)=="xre");
  std::cerr << "checking RFC6570 reverse matches" << std::endl;
  CHECK(match_uri_template("test://books/{id}","test://books/abc").has_value());
  CHECK(match_uri_template("test://books{/id}","test://books/red").has_value());
  CHECK(!match_uri_template("test://books/{id}","test://books/%GG").has_value());

  std::cerr << "checking resources and prompts" << std::endl;
  Server server("baseline","1");
  server.resource_binary("test://logo","Logo",
    [] {return std::vector<std::uint8_t>{0,255,16,128};},"image/png");
  server.resource_template("test://books{/id}","Book",
    [](const std::string& uri,const Json& bindings)->Json {
      return json::object{{"contents",json::array{json::object{{"uri",uri},
        {"text",bindings.at("id")}}}}};
    });
  server.prompt_full("review","review a file",[](const Json& args)->Json {
    return json::object{{"messages",json::array{json::object{{"role","user"},
      {"content",json::object{{"type","text"},{"text",args.at("file")}}}}}}};
  },json::array{json::object{{"name","file"},{"required",true},{"description","文件名"}}},
    json::object{{"type","object"},{"required",json::array{"messages"}},
      {"properties",json::object{{"messages",json::object{{"type","array"},{"minItems",1}}}}}});
  server.prompt_full("bad_result","returns invalid role",[](const Json&)->Json {
    return json::object{{"messages",json::array{json::object{{"role","admin"},
      {"content",json::object{{"type","text"},{"text","x"}}}}}}};
  });
  Client cli([&](const Json& req){return server.handle(req);});
  CHECK(cli.read("test://logo").at("contents")[0].at("blob")=="AP8QgA==");
  CHECK(cli.read("test://logo").at("contents")[0].at("mimeType")=="image/png");
  CHECK(cli.read("test://books/red").at("contents")[0].at("text")=="red");
  CHECK(cli.get_prompt("review",json::object{{"file","a.cpp"}})
      .at("messages")[0].at("content").at("text")=="a.cpp");
  expect_error([&]{cli.get_prompt("review");},-32602);
  expect_error([&]{cli.get_prompt("review",json::object{{"file",123}});},-32602);
  expect_error([&]{cli.get_prompt("review",json::object{{"file","a"},{"extra","b"}});},-32602);
  expect_error([&]{cli.get_prompt("bad_result");},-32603);

  // 采样 + roots 通过 MRTR 往返，服务端不能直接向客户端发起反向 RPC。
  std::cerr << "checking MRTR" << std::endl;
  Server mrtr("mrtr-baseline");
  mrtr.add_tool_context("collect","request sampling + roots",
    json::object{{"type","object"}},[](const Json& params)->Json {
      if (params.contains("inputResponses")) {
        const auto& answers=params.at("inputResponses");
        if (params.at("requestState")!="opaque-v1" ||
            !answers.contains("sample") || !answers.contains("roots"))
          throw std::runtime_error("lost MRTR inputResponses");
        return detail::make_tool_result("done");
      }
      return json::object{{"resultType","input_required"},{"requestState","opaque-v1"},
        {"inputRequests",json::object{
          {"sample",json::object{{"method","sampling/createMessage"},
            {"params",json::object{{"messages",json::array{}},{"maxTokens",24}}}}},
          {"roots",json::object{{"method","roots/list"},{"params",json::object{}}}}
        }}};
    });
  Client mrtr_cli([&](const Json& req){return mrtr.handle(req);});
  mrtr_cli.on_input_required([](const Json& requests)->Json {
    CHECK(requests.contains("sample") && requests.contains("roots"));
    return json::object{
      {"sample",json::object{{"role","assistant"},{"content",json::object{{"type","text"},{"text","ok"}}}}},
      {"roots",json::object{{"roots",json::array{}}}}
    };
  });
  expect_error([&]{mrtr_cli.call("collect");},-32021); // 未授权采样/根目录。
  mrtr_cli.enable_mrtr_capabilities(false,true,true);
  CHECK(mrtr_cli.call("collect").at("content")[0].at("text")=="done");
  std::cerr << "baseline complete" << std::endl;
  std::cout << "MCP baseline resources/prompts/MRTR/RFC6570 tests passed\n";
}
