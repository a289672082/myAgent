// P1-10：MCP tools/list 的 TTL 缓存和显式失效（离线 SDK Loopback）。
#include "advanced_demo_support.hpp"
#include <iostream>
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
  std::cout<<"run_mcp_catalog_cache_demo: TTL 命中与手动失效通过\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_mcp_catalog_cache_demo();}
