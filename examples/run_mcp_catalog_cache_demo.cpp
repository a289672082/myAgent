// P1-10：MCP tools/list 的 TTL 缓存和显式失效（离线 SDK Loopback）。
#include "advanced_demo_support.hpp"
#include <iostream>
#include <chrono>
#include <thread>
using namespace myagent;
using namespace advanced_demo;
int run_mcp_catalog_cache_demo() {
 try {
  mymcp::Server server("catalog-server","1.0");
  server.tool("echo","Echo test",[](std::string value){return value;},
              mymcp::arg<std::string>("value"));
  unsigned catalog_calls=0;
  auto client=std::make_shared<mymcp::Client>(
      [&server,&catalog_calls](const Json& request) {
        const auto* method=j::if_contains(request,"method");
        if(method&&method->is_string()&&method->as_string()=="tools/list")
          ++catalog_calls;
        return server.handle(request);
      },"cached-client");
  auto mock=std::make_shared<Mock>();
  mock->handler=[](const Json&){return answer("catalog checked");};
  auto cfg=config();
  cfg.mcp_catalog_ttl_seconds=3600;
  Agent agent(cfg,mock);
  agent.add_mcp("cached",client);
  ensure(agent.ask("first")=="catalog checked","首次工具发现失败");
  ensure(agent.ask("second")=="catalog checked","第二次调用失败");
  ensure(catalog_calls==1,"缓存未命中，重复查询了 tools/list");
  agent.invalidate_mcp_catalog("cached");
  ensure(agent.ask("third")=="catalog checked","工具目录刷新失败");
  ensure(catalog_calls==2,"明确刷新未重新请求 tools/list");
  agent.set_mcp_catalog_version("cached","2.0");
  ensure(agent.ask("version changed")=="catalog checked","版本刷新失败");
  ensure(catalog_calls==3,"变更服务端版本未触发失效");
  agent.set_mcp_catalog_version("cached","2.0");
  ensure(agent.ask("same version")=="catalog checked","同版本缓存调用失败");
  ensure(catalog_calls==3,"同版本不应重复刷新");

  // 恶意或损坏的 MCP 目录可能跨多页返回超大 Schema。
  // 必须在第一页校验失败，不能继续拉取余下页面后再做总体检查。
  mymcp::Server oversized_server("oversized-catalog","1.0");
  oversized_server.page_size(1);
  oversized_server.tool("a_large",std::string(4096,'x'),
                        [](){return 1;});
  oversized_server.tool("z_small","Normal tool",
                        [](){return 2;});
  unsigned oversized_pages=0;
  auto oversized_client=std::make_shared<mymcp::Client>(
      [&oversized_server,&oversized_pages](const Json& request) {
        const auto* method=j::if_contains(request,"method");
        if(method&&method->is_string()&&method->as_string()=="tools/list")
          ++oversized_pages;
        return oversized_server.handle(request);
      },"oversized-client");
  auto bounded_cfg=config();
  bounded_cfg.max_mcp_tool_bytes=512;
  bounded_cfg.max_mcp_catalog_bytes=8192;
  bounded_cfg.mcp_catalog_ttl_seconds=3600;
  Agent bounded(bounded_cfg,mock);
  bounded.add_mcp("oversized",oversized_client);
  bool rejected=false;
  try { (void)bounded.ask("reject oversized schema"); }
  catch(const std::exception&) { rejected=true; }
  ensure(rejected && oversized_pages==1,
         "MCP Schema 必须在第一页越界时立即拒绝，不能继续请求后续分页");

  // MCP 服务可能把一个目录拆成多页。每页分别给予 1 秒会使总耗时
  // 随分页数量线性增长；全目录应共享一次 1 秒预算，避免无限拖延。
  mymcp::Server slow_server("slow-catalog","1.0");
  slow_server.page_size(1);
  slow_server.tool("a","one",[](){return 1;});
  slow_server.tool("b","two",[](){return 2;});
  slow_server.tool("c","three",[](){return 3;});
  slow_server.tool("d","four",[](){return 4;});
  unsigned slow_pages=0;
  auto slow_client=std::make_shared<mymcp::Client>(
      [&slow_server,&slow_pages](const Json& req) {
        const auto* method=j::if_contains(req,"method");
        if(method && method->is_string() && method->as_string()=="tools/list") {
          ++slow_pages;
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        return slow_server.handle(req);
      },"slow-catalog-client");
  auto slow_cfg=config();
  slow_cfg.mcp_timeout_seconds=1;
  Agent slow_agent(slow_cfg,mock);
  slow_agent.add_mcp("slow",slow_client);
  bool slow_rejected=false;
  try { (void)slow_agent.ask("bounded paginated discovery"); }
  catch(const std::exception&){slow_rejected=true;}
  ensure(slow_rejected && slow_pages>=1 && slow_pages<=2,
         "多页 MCP 工具目录不得为每页重新分配完整超时");

  std::cout<<"run_mcp_catalog_cache_demo: TTL、失效与分页前置预算校验通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_mcp_catalog_cache_demo();}
