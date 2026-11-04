# 独立 C++20 SDK：不依赖控制台、FTXUI。支持 cmake -S sdk -B build-sdk。
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myAgentSDK VERSION 0.1.0 LANGUAGES CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# yaml-cpp 0.9.0 is pinned as a local Git submodule. No network at CMake time.
set(MYAGENT_YAML_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp"
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
set(MYAGENT_MCP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/myMcpTools" CACHE PATH "Local myMcpTools source directory")
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
  src/chat_completions.cpp src/router.cpp src/memory_store.cpp src/agent_team_config.cpp
  src/team_checkpoint.cpp src/task_scheduler.cpp src/workspace_io.cpp src/file_io.cpp)
add_library(myagent::agent ALIAS myagent)
target_include_directories(myagent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(myagent PUBLIC myagent_mcp PRIVATE yaml-cpp::yaml-cpp)
