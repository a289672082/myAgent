#pragma once
// 语义 Agent 路由：独立于 Agent/AgentTeam，保留现有 ask()/Loop API。
// Router 只负责选择一个已注册 Agent，不负责拆解任务或转交会话控制权。
#include "myagent/agent.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace myagent {

struct RouteDecision {
  std::string selected_agent;
  std::string method; // "model" 或 "single_agent"；不伪造置信度。
  TokenUsage routing_usage; // 仅路由模型用量，不包含执行 Agent 的用量。
};

struct RouterResult {
  std::string selected_agent;
  std::string answer;
  std::string route_method;
  TokenUsage routing_usage;
  // ask() 时为空；react() 时保留完整 Loop 状态/工具用量/错误。
  std::optional<LoopResult> loop_result;
};

// 单会话同步路由器；同一 Router 和被路由的 Agent 不允许并发执行。
// 通过 shared_ptr 持有 Agent，避免悬空引用；每个 Agent 保持独立的
// Config / API Key / MCP / Skills / 会话历史。
class AgentRouter {
public:
  // routing_model 为空时，按 routing_config 的协议构建 Responses/Chat 后端。
  // 支持注入 Mock Model 进行完全离线的测试。
  explicit AgentRouter(Config routing_config,
                       std::shared_ptr<Model> routing_model = {});

  // name 是路由模型必须返回的唯一 ID；description 仅用于路由选择。
  // 禁止重复注册和空指针；Router 不会修改 Agent 的模型配置。
  void add_agent(std::string name, std::string description,
                 std::shared_ptr<Agent> agent);
  // 兼容简单调用 router.add_agent("coder", coderAgent)；
  // 路由时仅知道 name，建议生产使用含明确 description 的重载。
  void add_agent(std::string name, std::shared_ptr<Agent> agent) {
    const std::string description=name;
    add_agent(std::move(name),description,std::move(agent));
  }

  // 路由事件 route_selected / route_failed 只通知通用回调；
  // 现有七种 ConversationCallbacks 转发给所有已注册及未来注册的 Agent。
  // 同步回调中的异常会被吞掉；工具 payload 默认隐藏。
  // 注意：统一订阅会覆盖这些 Agent 之前安装的同类型回调。
  void set_conversation_callback(Agent::ConversationCallback callback,
                                 bool include_tool_payloads = false);
  void set_conversation_callbacks(ConversationCallbacks callbacks);

  // 只选择，不执行。失败时抛异常，不会自动回退执行其他 Agent，
  // 以免把任务或敏感数据交给未预期的角色。
  RouteDecision route(const std::string& task);
  // 选择 Agent 后执行其原生 ask()/react()；不会重复执行失败的工具任务。
  RouterResult ask(const std::string& task);
  RouterResult react(const std::string& task,
                     const LoopOptions& options = {});

private:
  struct Member {
    std::string description;
    std::shared_ptr<Agent> agent;
  };
  void observe(const Json& event) noexcept;
  std::map<std::string, Member> agents_;
  Config routing_config_;
  std::shared_ptr<Model> routing_model_;
  Agent::ConversationCallback conversation_callback_;
  ConversationCallbacks named_callbacks_;
  bool common_observer_set_ = false;
  bool named_observer_set_ = false;
  bool include_tool_payloads_ = false;
};

} // namespace myagent
