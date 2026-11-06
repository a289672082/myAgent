# 独立 SDK 示例：只有 MYAGENT_BUILD_EXAMPLES=ON 才参与根项目构建。
  add_executable(run_agent_demo run_agent_demo.cpp)
  target_link_libraries(run_agent_demo PRIVATE myagent)

  # Token 用量统计：独立 C++ 源文件和 run_token_usage_demo()，离线 Mock 数据。
  add_executable(run_token_usage_demo run_token_usage_demo.cpp)
  target_link_libraries(run_token_usage_demo PRIVATE myagent)
  # 上下文压缩、任务委派、协作、真正并行四种独立 Demo。
  foreach(feature IN ITEMS context_compression agent_delegation agent_collaboration parallel_agents)
    add_executable(run_${feature}_demo run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # 子 Agent Skill/MCP 示范使用真实 SKILL.md 文件；无论从哪个目录运行均能找到资源。
  target_compile_definitions(run_agent_delegation_demo PRIVATE
    MYAGENT_TEAM_DEMO_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/agent_team_demo_skills")

  # 高级功能六组独立离线 Demo：阶段级模型 Provider 路由（不是 AgentRouter）/重试/MCP Schema 缓存/
  # 多层子 Agent/Trace/费用预算，无 API Key 或外网依赖。
  foreach(feature IN ITEMS model_routing model_retry mcp_catalog_cache
                           nested_agents agent_trace agent_cost)
    add_executable(run_${feature}_demo run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # 普通 Loop 持久化/人工审批/崩溃恢复与 HTTP SSE 流式输出的独立 Demo。
  # 所有请求都由离线 Mock Model 或 127.0.0.1 本机 HTTP 服务处理。
  foreach(feature IN ITEMS agent_checkpoint agent_streaming)
    add_executable(run_${feature}_demo run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()

  # 新增运行时权限/断点恢复示例，均使用离线 Mock Model。
  foreach(feature IN ITEMS runtime_hardening team_checkpoint)
    add_executable(run_${feature}_demo run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # PR #25：关键上下文固定保留、Skills 热更新、多 Agent 预算恢复。
  # 三个独立 Demo 均在本机 Mock 运行，不需要网络或真实 API Key。
  foreach(feature IN ITEMS agent_pinned_context agent_skills_refresh team_budget_recovery)
    add_executable(run_${feature}_demo run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # 智能 Agent 路由：独立的 Router API，完全离线的模型选人/执行/失败测试。
  add_executable(run_agent_router_demo run_agent_router_demo.cpp)
  target_link_libraries(run_agent_router_demo PRIVATE myagent)
  # 统一对话生命周期回调：离线 Mock 覆盖 ask、ReAct 和失败事件。
  add_executable(run_agent_conversation_callbacks_demo
    run_agent_conversation_callbacks_demo.cpp)
  target_link_libraries(run_agent_conversation_callbacks_demo PRIVATE myagent)
  add_executable(run_model_http_json_demo run_model_http_json_demo.cpp)
  target_link_libraries(run_model_http_json_demo PRIVATE myagent)
  # 团队统一回调：协调器 + 已注册 Worker + 后注册 Worker + 自动协作。
  add_executable(run_team_conversation_callbacks_demo
    run_team_conversation_callbacks_demo.cpp)
  target_link_libraries(run_team_conversation_callbacks_demo PRIVATE myagent)
  # AGENTS.md 专项 Demo：离线 Mock 模型验证文件加载、字符串替换与规则清空。
  add_executable(run_agents_md_demo run_agents_md_demo.cpp)
  target_link_libraries(run_agents_md_demo PRIVATE myagent)
  target_compile_definitions(run_agents_md_demo PRIVATE
    MYAGENT_AGENTS_MD_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/agents_md_demo")

  # 单独的 P0 Skill YAML Demo；运行无需 API Key、环境变量和网络请求。
  add_executable(run_skill_yaml_demo run_skill_yaml_demo.cpp)
  target_link_libraries(run_skill_yaml_demo PRIVATE myagent)
  # 固定示例资源目录，保证从 build 或任意工作目录启动时都能找到样例。
  target_compile_definitions(run_skill_yaml_demo PRIVATE
    MYAGENT_SKILL_YAML_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/skill_yaml_demo")
  # 六种 Loop 各自有中文流程说明与独立 run_xxx_demo()，保留原有四种入口。
  foreach(mode IN ITEMS react plan_execute reflection evaluator_optimizer rewoo execute_verify_repair)
    add_executable(run_${mode}_demo run_${mode}_demo.cpp)
    target_link_libraries(run_${mode}_demo PRIVATE myagent)
    target_compile_definitions(run_${mode}_demo PRIVATE
      MYAGENT_LOOP_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/loop_assets")
  endforeach()

# Release 下启用示例中的 assert（避免表达式随 NDEBUG 被移除）。
get_property(_example_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _example_targets)
  if(_target MATCHES "^run_.*demo$")
    if(MSVC)
      target_compile_options(${_target} PRIVATE "$<$<CONFIG:Release>:/UNDEBUG>")
    else()
      target_compile_options(${_target} PRIVATE "$<$<CONFIG:Release>:-UNDEBUG>")
    endif()
  endif()
endforeach()
