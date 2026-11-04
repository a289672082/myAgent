# No-argument, single-purpose demos. Every source defines run_xxxx_demo() and a
# no-argument main() that calls it; the command line is never used for setup.
foreach(demo IN ITEMS
  tools resources prompts mrtr tasks extensions stdio_server stdio_client
  http_server http_client http_progress subscriptions oauth oauth_login oauth_resource oauth_resource_client progress)
  add_executable(run_${demo}_demo run_${demo}_demo.cpp)
  target_link_libraries(run_${demo}_demo PRIVATE mymcp)
  target_include_directories(run_${demo}_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
# CMake fills in the correct Debug/Release path to the demo stdio server on
# Linux, macOS and Windows. No argv, cwd assumptions or handwritten .exe paths.
add_dependencies(run_stdio_client_demo run_stdio_server_demo)
add_dependencies(run_progress_demo run_stdio_server_demo)
target_compile_definitions(run_stdio_client_demo PRIVATE
  MYMCP_DEMO_STDIO_SERVER="$<TARGET_FILE:run_stdio_server_demo>")
target_compile_definitions(run_progress_demo PRIVATE
  MYMCP_DEMO_STDIO_SERVER="$<TARGET_FILE:run_stdio_server_demo>")

# All-in-one MCP stdio toolbox demo (23 tools, upstream snapshot).
add_executable(run_agent_tools_demo run_agent_tools_demo.cpp)
target_link_libraries(run_agent_tools_demo PRIVATE mymcp)

if(MYAGENT_BUILD_MCP_TESTS)
foreach(demo IN ITEMS tools resources prompts mrtr tasks extensions stdio_client progress oauth oauth_login)
  add_test(NAME demo_${demo} COMMAND run_${demo}_demo)
  set_tests_properties(demo_${demo} PROPERTIES TIMEOUT 20)
endforeach()
if(Python3_Interpreter_FOUND)
  add_test(NAME demo_http_and_sse
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/run_demo_smoke.py"
      "$<TARGET_FILE:run_http_server_demo>" "$<TARGET_FILE:run_http_client_demo>"
      "$<TARGET_FILE:run_subscriptions_demo>" "$<TARGET_FILE:run_http_progress_demo>" "$<TARGET_FILE:run_oauth_resource_demo>"
      "$<TARGET_FILE:run_oauth_resource_client_demo>")
  set_tests_properties(demo_http_and_sse PROPERTIES TIMEOUT 35)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME agent_tools_demo
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/agent_tools_demo_smoke.py"
      "$<TARGET_FILE:run_agent_tools_demo>")
  set_tests_properties(agent_tools_demo PROPERTIES TIMEOUT 90)
endif()
endif()
