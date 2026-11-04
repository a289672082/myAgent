# Imported myMcpTools protocol, OAuth, HTTP, SSE and transport regressions.
# Own testing folder; no MCP tests are compiled into the Agent SDK.
add_executable(mymcp_example_server fixtures/server.cpp)
target_link_libraries(mymcp_example_server PRIVATE mymcp)
add_executable(mymcp_example_client fixtures/client.cpp)
target_link_libraries(mymcp_example_client PRIVATE mymcp)
add_executable(mymcp_listen_client listen_client.cpp)
target_link_libraries(mymcp_listen_client PRIVATE mymcp)
enable_testing()
foreach(test IN ITEMS protocol extended tasks_schema oauth)
 if(test STREQUAL "tasks_schema")
   set(path test_tasks_schema.cpp)
 else()
   set(path test_${test}.cpp)
 endif()
 add_executable(mymcp_${test}_tests ${path})
 target_link_libraries(mymcp_${test}_tests PRIVATE mymcp)
 add_test(NAME ${test} COMMAND mymcp_${test}_tests)
endforeach()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
 foreach(test IN ITEMS http_smoke stdio_subscription auth_http progress_cancel sse_smoke)
  if(test STREQUAL "sse_smoke")
   add_test(NAME ${test} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${test}.py" "$<TARGET_FILE:mymcp_example_server>" "$<TARGET_FILE:mymcp_listen_client>")
  else()
   add_test(NAME ${test} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${test}.py" "$<TARGET_FILE:mymcp_example_server>")
  endif()
 endforeach()
endif()

# Local mock OAuth authorization server: discovery, registration, PKCE, token exchange and refresh.
add_executable(mymcp_oauth_session_tests test_oauth_session.cpp)
target_link_libraries(mymcp_oauth_session_tests PRIVATE mymcp)
add_test(NAME oauth_session COMMAND mymcp_oauth_session_tests)

# Comprehensive, opt-in executable examples. Never used as production credential stores.
foreach(example IN ITEMS showcase_server showcase_client showcase_subscribe oauth_login oauth_resource_server)
  add_executable(mymcp_${example} fixtures/${example}.cpp)
  target_link_libraries(mymcp_${example} PRIVATE mymcp)
endforeach()
if(Python3_Interpreter_FOUND)
  add_test(NAME showcase_examples COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/showcase_smoke.py"
    "$<TARGET_FILE:mymcp_showcase_server>" "$<TARGET_FILE:mymcp_showcase_client>"
    "$<TARGET_FILE:mymcp_showcase_subscribe>")
  add_test(NAME oauth_resource_example COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/oauth_resource_smoke.py"
    "$<TARGET_FILE:mymcp_oauth_resource_server>")
  set_tests_properties(showcase_examples PROPERTIES TIMEOUT 75)
  set_tests_properties(oauth_resource_example PROPERTIES TIMEOUT 25)
endif()


# Simulated Codex/Claude legacy Streamable HTTP handshake, plus request-scoped
# SSE progress and cancellation/error-frame protocol regression checks.
if(Python3_Interpreter_FOUND)
  add_test(NAME http_streamable_compat
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/http_streamable_compat.py"
      "$<TARGET_FILE:mymcp_example_server>")
  set_tests_properties(http_streamable_compat PROPERTIES TIMEOUT 25)
endif()

add_executable(mymcp_legacy_http_client_tests legacy_http_client.cpp)
target_link_libraries(mymcp_legacy_http_client_tests PRIVATE mymcp)
if(Python3_Interpreter_FOUND)
  add_test(NAME legacy_http_client
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/legacy_http_client.py"
      "$<TARGET_FILE:mymcp_legacy_http_client_tests>")
  set_tests_properties(legacy_http_client PROPERTIES TIMEOUT 20)
endif()

# 进程不合作/忽略 SIGTERM、写端阻塞、接收预算回归。
add_executable(mymcp_transport_limits_tests test_transport_limits.cpp)
target_link_libraries(mymcp_transport_limits_tests PRIVATE mymcp)
add_test(NAME transport_limits COMMAND mymcp_transport_limits_tests)
set_tests_properties(transport_limits PROPERTIES TIMEOUT 25)

add_executable(mymcp_http_limits_tests test_http_limits.cpp)
target_link_libraries(mymcp_http_limits_tests PRIVATE mymcp)
add_test(NAME http_limits COMMAND mymcp_http_limits_tests)
set_tests_properties(http_limits PROPERTIES TIMEOUT 15)

# P0：OAuth Token 严格绑定 MCP Resource，防止跨路径/跨主机/HTTPS 降级。
add_executable(mymcp_oauth_resource_binding_tests test_oauth_resource_binding.cpp)
target_link_libraries(mymcp_oauth_resource_binding_tests PRIVATE mymcp)
add_test(NAME oauth_resource_binding COMMAND mymcp_oauth_resource_binding_tests)
set_tests_properties(oauth_resource_binding PROPERTIES TIMEOUT 15)

# P1：真实 HTTP tools/list 分页构建 x-mcp-header 缓存、SSE 失效及错误头拒绝。
add_executable(mymcp_http_header_routing_tests test_http_header_routing.cpp)
target_link_libraries(mymcp_http_header_routing_tests PRIVATE mymcp)
add_test(NAME http_header_routing COMMAND mymcp_http_header_routing_tests)
set_tests_properties(http_header_routing PROPERTIES TIMEOUT 20)
