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

add_library(myagent STATIC src/agent.cpp)
add_library(myagent::agent ALIAS myagent)
target_include_directories(myagent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(myagent PUBLIC myagent_mcp PRIVATE yaml-cpp::yaml-cpp)

option(MYAGENT_BUILD_EXAMPLES "Build the standalone Agent examples" ON)
option(MYAGENT_BUILD_TESTS "Build offline Agent tests" ON)
if(MYAGENT_BUILD_EXAMPLES)
  add_executable(run_agent_demo examples/run_agent_demo.cpp)
  target_link_libraries(run_agent_demo PRIVATE myagent)
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
  set_tests_properties(myagent_skills PROPERTIES TIMEOUT 30)
  add_test(NAME myagent COMMAND myagent_tests)
  set_tests_properties(myagent PROPERTIES TIMEOUT 30)
endif()
