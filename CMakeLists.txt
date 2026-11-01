cmake_minimum_required(VERSION 3.20)
project(myAgent VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# yaml-cpp 0.9.0 is pinned as a local Git submodule. No network at CMake time.
set(MYAGENT_YAML_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yaml-cpp"
    CACHE PATH "Local yaml-cpp source directory")
if(NOT EXISTS "${MYAGENT_YAML_CPP_DIR}/CMakeLists.txt"
   OR NOT EXISTS "${MYAGENT_YAML_CPP_DIR}/LICENSE")
  message(FATAL_ERROR "Missing yaml-cpp sources/license. Use: git submodule update --init --recursive")
endif()
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Build yaml-cpp tools" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Build yaml-cpp tests" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "Install yaml-cpp" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Static yaml-cpp" FORCE)
add_subdirectory("${MYAGENT_YAML_CPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/yaml-cpp")

# Shared third-party sources come from the pinned myMcpTools Git submodule.
# No FetchContent or network downloads during configuration/build.
set(MYAGENT_MCP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/myMcpTools" CACHE PATH "Local myMcpTools source directory")
foreach(required IN ITEMS
  "include/mymcp/mcp.hpp"
  "src/mcp.cpp"
  "src/oauth.cpp"
  "src/oauth_client.cpp"
  "src/schema.cpp"
  "third_party/jsoncons/include/jsoncons/json.hpp"
  "third_party/jsoncons/LICENSE"
  "third_party/cpp-httplib/httplib.h"
  "third_party/cpp-httplib/LICENSE")
  if(NOT EXISTS "${MYAGENT_MCP_DIR}/${required}")
    message(FATAL_ERROR "Missing ${MYAGENT_MCP_DIR}/${required}. Use: git submodule update --init --recursive")
  endif()
endforeach()

# Compile only the MCP runtime, not myMcpTools' demos and tests.
add_library(myagent_mcp STATIC
  "${MYAGENT_MCP_DIR}/src/mcp.cpp"
  "${MYAGENT_MCP_DIR}/src/oauth.cpp"
  "${MYAGENT_MCP_DIR}/src/oauth_client.cpp"
  "${MYAGENT_MCP_DIR}/src/schema.cpp")
target_compile_definitions(myagent_mcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(myagent_mcp PUBLIC
  "${MYAGENT_MCP_DIR}/include"
  "${MYAGENT_MCP_DIR}/third_party/jsoncons/include"
  "${MYAGENT_MCP_DIR}/third_party/cpp-httplib")
target_link_libraries(myagent_mcp PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(myagent_mcp PUBLIC ws2_32 crypt32)
endif()
add_library(myagent::mcp ALIAS myagent_mcp)

add_library(myagent STATIC src/agent.cpp src/loop_engine.cpp src/plan_manager.cpp src/token_usage.cpp src/context_manager.cpp src/multi_agent.cpp src/tool_policy.cpp
  src/chat_completions.cpp src/memory_store.cpp src/agent_team_config.cpp
  src/team_checkpoint.cpp src/task_scheduler.cpp src/workspace_io.cpp)
add_library(myagent::agent ALIAS myagent)
target_include_directories(myagent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(myagent PUBLIC myagent_mcp PRIVATE yaml-cpp::yaml-cpp)

option(MYAGENT_BUILD_EXAMPLES "Build the standalone Agent examples" ON)
option(MYAGENT_BUILD_TESTS "Build offline Agent tests" ON)
if(MYAGENT_BUILD_EXAMPLES)
  add_executable(run_agent_demo examples/run_agent_demo.cpp)
  target_link_libraries(run_agent_demo PRIVATE myagent)

  # Token 用量统计：独立 C++ 源文件和 run_token_usage_demo()，离线 Mock 数据。
  add_executable(run_token_usage_demo examples/run_token_usage_demo.cpp)
  target_link_libraries(run_token_usage_demo PRIVATE myagent)
  # 上下文压缩、任务委派、协作、真正并行四种独立 Demo。
  foreach(feature IN ITEMS context_compression agent_delegation agent_collaboration parallel_agents)
    add_executable(run_${feature}_demo examples/run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # 子 Agent Skill/MCP 示范使用真实 SKILL.md 文件；无论从哪个目录运行均能找到资源。
  target_compile_definitions(run_agent_delegation_demo PRIVATE
    MYAGENT_TEAM_DEMO_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/agent_team_demo_skills")

  # 新增运行时权限/断点恢复示例，均使用离线 Mock Model。
  foreach(feature IN ITEMS runtime_hardening team_checkpoint)
    add_executable(run_${feature}_demo examples/run_${feature}_demo.cpp)
    target_link_libraries(run_${feature}_demo PRIVATE myagent)
  endforeach()
  # 单独的 P0 Skill YAML Demo；运行无需 API Key、环境变量和网络请求。
  add_executable(run_skill_yaml_demo examples/run_skill_yaml_demo.cpp)
  target_link_libraries(run_skill_yaml_demo PRIVATE myagent)
  # 固定示例资源目录，保证从 build 或任意工作目录启动时都能找到样例。
  target_compile_definitions(run_skill_yaml_demo PRIVATE
    MYAGENT_SKILL_YAML_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/skill_yaml_demo")
  # 四种循环各自使用独立源码和 run_xxx_demo() 函数。
  foreach(mode IN ITEMS react plan_execute reflection evaluator_optimizer)
    add_executable(run_${mode}_demo examples/run_${mode}_demo.cpp)
    target_link_libraries(run_${mode}_demo PRIVATE myagent)
    target_compile_definitions(run_${mode}_demo PRIVATE
      MYAGENT_LOOP_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/loop_assets")
  endforeach()
endif()
if(MYAGENT_BUILD_TESTS)
  enable_testing()
  add_executable(myagent_tests tests/test_agent.cpp)
  target_link_libraries(myagent_tests PRIVATE myagent)
  # Exercise the real myMcpTools stdio client/server end-to-end.
  add_executable(myagent_test_stdio_server
    "${MYAGENT_MCP_DIR}/examples/run_stdio_server_demo.cpp")
  target_link_libraries(myagent_test_stdio_server PRIVATE myagent_mcp)
  add_dependencies(myagent_tests myagent_test_stdio_server)
  target_compile_definitions(myagent_tests PRIVATE
    MYAGENT_TEST_STDIO_SERVER="$<TARGET_FILE:myagent_test_stdio_server>")
  # Actual localhost HTTP endpoint tests for append-vs-exact Responses URLs.
  add_executable(myagent_url_tests tests/test_urls.cpp)
  target_link_libraries(myagent_url_tests PRIVATE myagent)
  add_test(NAME myagent_urls COMMAND myagent_url_tests)
  set_tests_properties(myagent_urls PROPERTIES TIMEOUT 30)
  add_executable(myagent_skill_tests tests/test_skill.cpp)
  target_link_libraries(myagent_skill_tests PRIVATE myagent)
  add_test(NAME myagent_skills COMMAND myagent_skill_tests)
  add_executable(myagent_skill_yaml_tests tests/test_skill_yaml.cpp)
  target_link_libraries(myagent_skill_yaml_tests PRIVATE myagent)
  add_test(NAME myagent_skills_yaml COMMAND myagent_skill_yaml_tests)
  set_tests_properties(myagent_skills_yaml PROPERTIES TIMEOUT 30)
  if(MYAGENT_BUILD_EXAMPLES)
    # 用真实样例文件执行独立 Demo，验证成功退出及完整的离线流程。
    add_test(NAME myagent_skill_yaml_demo COMMAND run_skill_yaml_demo)
    set_tests_properties(myagent_skill_yaml_demo PROPERTIES TIMEOUT 30)
  endif()
  set_tests_properties(myagent_skills PROPERTIES TIMEOUT 30)
  if(MYAGENT_BUILD_EXAMPLES)
    foreach(feature IN ITEMS runtime_hardening team_checkpoint)
      add_test(NAME myagent_${feature}_demo COMMAND run_${feature}_demo)
      set_tests_properties(myagent_${feature}_demo PROPERTIES TIMEOUT 30)
    endforeach()
  endif()
  # 静态审计回归：Checkpoint、上下文压缩、Chat 多工具调用、
  # Coordinator 模型预算与工作区路径安全。
  add_executable(myagent_audit_tests tests/test_audit_fixes.cpp)
  target_link_libraries(myagent_audit_tests PRIVATE myagent)
  target_include_directories(myagent_audit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME myagent_audit COMMAND myagent_audit_tests)
  set_tests_properties(myagent_audit PROPERTIES TIMEOUT 60)
  add_executable(myagent_p1_p2_tests tests/test_p1_p2.cpp)
  target_link_libraries(myagent_p1_p2_tests PRIVATE myagent)
  add_test(NAME myagent_p1_p2 COMMAND myagent_p1_p2_tests)
  set_tests_properties(myagent_p1_p2 PROPERTIES TIMEOUT 40)
  add_executable(myagent_runtime_hardening_tests tests/test_runtime_hardening.cpp)
  target_link_libraries(myagent_runtime_hardening_tests PRIVATE myagent)
  add_test(NAME myagent_runtime_hardening COMMAND myagent_runtime_hardening_tests)
  set_tests_properties(myagent_runtime_hardening PROPERTIES TIMEOUT 30)
  add_executable(myagent_context_tests tests/test_context.cpp)
  target_link_libraries(myagent_context_tests PRIVATE myagent)
  add_test(NAME myagent_context COMMAND myagent_context_tests)
  set_tests_properties(myagent_context PROPERTIES TIMEOUT 45)
  add_executable(myagent_team_tests tests/test_multi_agent.cpp)
  target_link_libraries(myagent_team_tests PRIVATE myagent)
  add_test(NAME myagent_team COMMAND myagent_team_tests)
  # 真正本机 HTTP 连接验证多个 Provider 的 base_url / API Key 不混用。
  add_executable(myagent_team_routes_tests tests/test_multi_agent_routes.cpp)
  target_link_libraries(myagent_team_routes_tests PRIVATE myagent)
  add_test(NAME myagent_team_routes COMMAND myagent_team_routes_tests)
  set_tests_properties(myagent_team_routes PROPERTIES TIMEOUT 45)
  set_tests_properties(myagent_team PROPERTIES TIMEOUT 45)
  if(MYAGENT_BUILD_EXAMPLES)
    foreach(feature IN ITEMS context_compression agent_delegation agent_collaboration parallel_agents)
      add_test(NAME myagent_${feature}_demo COMMAND run_${feature}_demo)
      set_tests_properties(myagent_${feature}_demo PROPERTIES TIMEOUT 45)
    endforeach()
  endif()
  add_executable(myagent_token_usage_tests tests/test_token_usage.cpp)
  target_link_libraries(myagent_token_usage_tests PRIVATE myagent)
  add_test(NAME myagent_token_usage COMMAND myagent_token_usage_tests)
  set_tests_properties(myagent_token_usage PROPERTIES TIMEOUT 30)
  if(MYAGENT_BUILD_EXAMPLES)
    add_test(NAME myagent_token_usage_demo COMMAND run_token_usage_demo)
    set_tests_properties(myagent_token_usage_demo PROPERTIES TIMEOUT 30)
  endif()
  add_executable(myagent_loop_tests tests/test_loops.cpp)
  target_link_libraries(myagent_loop_tests PRIVATE myagent)
  target_compile_definitions(myagent_loop_tests PRIVATE
    MYAGENT_LOOP_TEST_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/examples/loop_assets/sample.png")
  add_test(NAME myagent_loops COMMAND myagent_loop_tests)
  set_tests_properties(myagent_loops PROPERTIES TIMEOUT 45)
  if(MYAGENT_BUILD_EXAMPLES)
    foreach(mode IN ITEMS react plan_execute reflection evaluator_optimizer)
      add_test(NAME myagent_${mode}_demo COMMAND run_${mode}_demo)
      set_tests_properties(myagent_${mode}_demo PROPERTIES TIMEOUT 30)
    endforeach()
  endif()
  add_test(NAME myagent COMMAND myagent_tests)
  set_tests_properties(myagent PROPERTIES TIMEOUT 30)
endif()

# Release 构建必须让测试中的 assert 继续生效：否则 -DNDEBUG 会跳过断言
# 表达式，连表达式中的请求操作也可能不会执行。
if(MYAGENT_BUILD_TESTS)
  get_property(_myagent_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _myagent_targets)
    if(_target MATCHES "^myagent_.*tests$" OR _target MATCHES "^run_.*demo$")
      if(MSVC)
        target_compile_options(${_target} PRIVATE "$<$<CONFIG:Release>:/UNDEBUG>")
      else()
        target_compile_options(${_target} PRIVATE "$<$<CONFIG:Release>:-UNDEBUG>")
      endif()
    endif()
  endforeach()
endif()
