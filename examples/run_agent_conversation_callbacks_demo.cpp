// 统一对话回调 Demo：ask、ReAct、工具事件、失败及回调异常隔离。
// 完全离线 MockModel；不使用 API Key。子 Agent 可逐个注册同一回调。
#include "loop_demo_support.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace j=myagent::json;
int run_agent_conversation_callbacks_demo() {
  try {
    auto model=std::make_shared<loop_demo::MockModel>();
    auto cfg=loop_demo::config();
    cfg.agent_id="callback-demo";
    myagent::Agent agent(cfg,model);
    std::vector<std::string> events;
    // 默认不记录工具参数/结果，避免在 UI 日志中意外泄露秘密。
    agent.set_conversation_callback([&](const myagent::Json& e) {
      const std::string type(e.at("event").as_string());
      events.push_back(type);
      if(type=="tool_call" || type=="tool_result")
        loop_demo::check(!e.contains("arguments") && !e.contains("output"),
                         "工具敏感数据不应默认暴露");
      // 故意抛出异常：观察者不能让 Agent 执行失败。
      if(type=="assistant_delta")throw std::runtime_error("UI observer failed");
    });
    model->handle=[](const myagent::Json&,unsigned)->myagent::Json {
      return loop_demo::answer("你好，回调！");
    };
    loop_demo::check(agent.ask("你好")=="你好，回调！","ask 返回错误");
    loop_demo::check(events.size()>=3 && events.front()=="user_message" &&
                     events.back()=="turn_completed","ask 生命周期事件不完整");
    events.clear();
    const auto result=agent.react("再回答一次");
    loop_demo::check(result.status==myagent::LoopStatus::Completed,"ReAct 未完成");
    loop_demo::check(events.front()=="user_message" &&
                     events.back()=="turn_completed","ReAct 生命周期事件不完整");

    // 多模态输入：回调应显示文字与媒体类型，但不暴露 data URL/base64。
    std::vector<myagent::Json> media_events;
    agent.set_conversation_callback([&](const myagent::Json& e) {
      media_events.push_back(e);
    });
    const std::string secret="base64-private-image-payload";
    myagent::Json parts=j::array{
      j::object{{"type","input_text"},{"text","描述这张图"}},
      j::object{{"type","input_image"},{"image_url","data:image/png;base64,"+secret}}
    };
    loop_demo::check(agent.ask_parts(parts)=="你好，回调！",
                     "ask_parts 的回复错误");
    bool saw_text=false;
    for(const auto& e:media_events) {
      loop_demo::check(j::serialize(e).find(secret)==std::string::npos,
                       "回调泄露了多模态输入的 data URL");
      if(std::string(e.at("event").as_string())=="user_message")
        saw_text=j::serialize(e.at("content")).find("描述这张图")!=std::string::npos;
      loop_demo::check(e.contains("turn_id") &&
          !std::string(e.at("turn_id").as_string()).empty(),
          "缺少稳定轮次 ID");
    }
    loop_demo::check(saw_text,"多模态回调丢失文本输入");

    // 工具事件：先报告调用意图，再报告执行结果；默认仅提供元数据。
    agent.tool("sum","计算两个整数之和",[](int a,int b){return a+b;},
               mymcp::arg<int>("a"),mymcp::arg<int>("b"));
    events.clear();
    unsigned tool_round=0;
    model->handle=[&](const myagent::Json&,unsigned)->myagent::Json {
      if(++tool_round==1)
        return j::object{{"status","completed"},{"output",j::array{
          j::object{{"type","function_call"},{"name","local__sum"},
                    {"call_id","demo-call"},{"arguments",R"({"a":20,"b":22})"}}}}};
      return loop_demo::answer("计算结果为 42");
    };
    loop_demo::check(agent.ask("20+22 等于多少？")=="计算结果为 42",
                     "工具轮次没有完成");
    // 刚才已经切换到了 media_events 监听器；检查工具事件的实际 JSON。
    bool saw_call=false,saw_result=false;
    for(const auto& e:media_events) {
      const std::string kind(e.at("event").as_string());
      if(kind=="tool_call" || kind=="tool_result") {
        loop_demo::check(!e.contains("arguments") && !e.contains("output"),
                         "默认监听器不应暴露工具敏感数据");
        saw_call |= kind=="tool_call";
        saw_result |= kind=="tool_result";
      }
    }
    loop_demo::check(saw_call && saw_result,"工具调用或结果事件缺失");

    // 仅在显式允许后，输出工具参数与结果；调用方必须保护这些数据。
    std::vector<myagent::Json> detailed;
    agent.set_conversation_callback([&](const myagent::Json& e) {
      detailed.push_back(e);
    },true);
    tool_round=0;
    loop_demo::check(agent.ask("再次计算 20+22")=="计算结果为 42",
                     "启用工具结果观察后执行失败");
    bool exposed_args=false,exposed_output=false;
    for(const auto& e:detailed) {
      const std::string type(e.at("event").as_string());
      if(type=="tool_call")exposed_args=e.contains("arguments");
      if(type=="tool_result")exposed_output=e.contains("output");
    }
    loop_demo::check(exposed_args && exposed_output,
                     "显式工具 payload 开关未生效");

    // 失败也必须通知宿主；失败的会话不会提交到历史。
    events.clear();
    model->handle=[](const myagent::Json&,unsigned)->myagent::Json {
      throw std::runtime_error("模拟模型失败");
    };
    const auto failed=agent.react("触发错误");
    loop_demo::check(failed.status==myagent::LoopStatus::Failed,
                     "失败状态不正确");
    loop_demo::check(!detailed.empty() &&
        std::string(detailed.back().at("event").as_string())=="turn_failed",
        "缺少失败回调");

    // 自定义流式模型：验证普通 ask() 的真实 delta，以及观察者异常隔离。
    struct DeltaModel final : myagent::Model {
      myagent::Json create(const myagent::Json&) override {
        return loop_demo::answer("Hello callbacks");
      }
      myagent::Json create_stream(const myagent::Json& request,
          std::chrono::milliseconds,
          const std::function<void(const std::string&)>& on_delta) override {
        on_delta("Hello ");
        on_delta("callbacks");
        return create(request);
      }
    };
    myagent::Agent stream_agent(cfg,std::make_shared<DeltaModel>());
    int delta_count=0,completed_count=0;
    stream_agent.set_conversation_callback([&](const myagent::Json& e) {
      const std::string type(e.at("event").as_string());
      if(type=="assistant_delta") {
        ++delta_count;
        throw std::runtime_error("模拟 UI 回调错误");
      }
      if(type=="turn_completed") ++completed_count;
    });
    loop_demo::check(stream_agent.ask("流式输出测试")=="Hello callbacks",
                     "ask 的流式输出或异常隔离失败");
    loop_demo::check(delta_count==2 && completed_count==1,
                     "ask 未正确转发真实文本增量");

    // 强类型回调：无需自行按 event 字符串过滤，同样支持完整输入、增量和返回。
    stream_agent.set_conversation_callback({});
    myagent::ConversationCallbacks named;
    int named_input=0,named_delta=0,named_message=0,named_done=0;
    named.on_user_message=[&](const myagent::Json& e) {
      if(std::string(e.at("content").as_string())=="类型化回调")++named_input;
    };
    named.on_assistant_delta=[&](const myagent::Json&) {++named_delta;};
    named.on_assistant_message=[&](const myagent::Json& e) {
      if(std::string(e.at("content").as_string())=="Hello callbacks")++named_message;
    };
    named.on_turn_completed=[&](const myagent::Json&) {++named_done;};
    stream_agent.set_conversation_callbacks(std::move(named));
    loop_demo::check(stream_agent.ask("类型化回调")=="Hello callbacks",
                     "具名回调模式执行失败");
    loop_demo::check(named_input==1 && named_delta==2 &&
                     named_message==1 && named_done==1,
                     "七种强类型监听器注册或分发异常");

    // 子 Agent 同样使用 set_conversation_callback；Team 的每个 worker
    // 可通过 team.agent("worker").set_conversation_callback(...) 单独订阅。
    std::cout<<"run_agent_conversation_callbacks_demo: ask / ReAct / failure OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"run_agent_conversation_callbacks_demo 失败："<<e.what()<<'\n';
    return 1;
  }
}
int main(){return run_agent_conversation_callbacks_demo();}
