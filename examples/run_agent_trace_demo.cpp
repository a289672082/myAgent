// P1-12：统一 Trace ID、Span ID、耗时和 OTEL JSONL 离线导出。
#include "advanced_demo_support.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace myagent;
using namespace advanced_demo;
namespace fs=std::filesystem;
int run_agent_trace_demo() {
 const auto file=fs::temp_directory_path()/("myagent-trace-demo-"+
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".jsonl");
 try {
  auto mock=std::make_shared<Mock>();
  mock->handler=[](const Json&){return answer("trace works");};
  Agent agent(config(),mock);
  LoopOptions options;
  options.otel_jsonl_path=file;
  unsigned spans=0;
  options.on_span=[&](const Json& span) {
    ensure(span.at("traceId").as_string_view().size()==32,"Trace ID 无效");
    ensure(span.at("spanId").as_string_view().size()==16,"Span ID 无效");
    ensure(span.at("status").as_string()=="ok","Span 未成功");
    ++spans;
  };
  const auto result=agent.react("sensitive example prompt",options);
  ensure(result.status==LoopStatus::Completed,"Trace Loop 未完成");
  ensure(result.trace_id.size()==32,"LoopResult 无 Trace ID");
  ensure(spans>=1,"没有输出模型 Span");
  std::ifstream in(file,std::ios::binary);
  ensure(static_cast<bool>(in),"没有创建 OTEL JSONL");
  const std::string raw((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  ensure(raw.find(result.trace_id)!=std::string::npos,"Trace 文件缺少关联 ID");
  ensure(raw.find("sensitive example prompt")==std::string::npos,
         "Trace 泄露了用户提示词");
  fs::remove(file);
  std::cout<<"run_agent_trace_demo: Trace/span/JSONL 和敏感内容保护通过\n";
  return 0;
 }catch(const std::exception& e){std::error_code ec;fs::remove(file,ec);
   std::cerr<<e.what()<<"\n";return 1;}
}
int main(){return run_agent_trace_demo();}
