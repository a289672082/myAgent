cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpExamples LANGUAGES CXX)
endif()
if(NOT TARGET mymcp)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../sdk" "${CMAKE_CURRENT_BINARY_DIR}/mcp-sdk")
endif()
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


add_executable(run_agent_tools_demo run_agent_tools_demo.cpp)
target_link_libraries(run_agent_tools_demo PRIVATE mymcp)
