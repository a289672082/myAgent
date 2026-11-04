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


# Build local, independent MCP SDK; no myMcpTools Git submodule.
if(NOT TARGET mymcp)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../mcp/sdk"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-sdk")
endif()
if(NOT TARGET myagent_mcp)
  add_library(myagent_mcp ALIAS mymcp)
endif()
if(NOT TARGET myagent::mcp)
  add_library(myagent::mcp ALIAS mymcp)
endif()
add_library(myagent STATIC src/agent.cpp src/loop_engine.cpp src/plan_manager.cpp src/token_usage.cpp src/context_manager.cpp src/multi_agent.cpp src/tool_policy.cpp
  src/chat_completions.cpp src/router.cpp src/memory_store.cpp src/agent_team_config.cpp
  src/team_checkpoint.cpp src/task_scheduler.cpp src/workspace_io.cpp src/file_io.cpp)
add_library(myagent::agent ALIAS myagent)
target_include_directories(myagent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(myagent PUBLIC mymcp PRIVATE yaml-cpp::yaml-cpp)
