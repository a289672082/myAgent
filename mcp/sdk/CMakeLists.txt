# Standalone MCP SDK, with a stable myagent_mcp target and mymcp::mymcp alias.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpSDK VERSION 0.2.0 LANGUAGES CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
# Latest myMcpTools source is now checked into mcp/sdk, not a Git submodule.
# Direct pinned dependencies are stored in repository-level third_party.
set(MYMCP_JSONCONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/jsoncons")
set(MYMCP_HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cpp-httplib")
foreach(required IN ITEMS
  "${MYMCP_JSONCONS_DIR}/include/jsoncons/json.hpp"
  "${MYMCP_JSONCONS_DIR}/include/jsoncons_ext/jsonschema/jsonschema.hpp"
  "${MYMCP_JSONCONS_DIR}/LICENSE"
  "${MYMCP_HTTPLIB_DIR}/httplib.h"
  "${MYMCP_HTTPLIB_DIR}/LICENSE")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing MCP vendored dependency: ${required}; run git submodule update --init --recursive")
  endif()
endforeach()
add_library(myagent_mcp STATIC src/mcp.cpp src/schema.cpp src/oauth.cpp src/oauth_client.cpp)
add_library(mymcp ALIAS myagent_mcp)
add_library(mymcp::mymcp ALIAS myagent_mcp)
add_library(myagent::mcp ALIAS myagent_mcp)
set_target_properties(myagent_mcp PROPERTIES EXPORT_NAME mymcp)
target_compile_features(myagent_mcp PUBLIC cxx_std_20)
target_compile_definitions(myagent_mcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(myagent_mcp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MYMCP_JSONCONS_DIR}/include>
  $<BUILD_INTERFACE:${MYMCP_HTTPLIB_DIR}>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(myagent_mcp PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(myagent_mcp PUBLIC ws2_32 crypt32)
endif()
# Install headers and dependency licenses; no myMcpTools repository required.
install(TARGETS myagent_mcp EXPORT mymcpTargets ARCHIVE DESTINATION lib)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION include)
install(DIRECTORY "${MYMCP_JSONCONS_DIR}/include/" DESTINATION include)
install(FILES "${MYMCP_HTTPLIB_DIR}/httplib.h" DESTINATION include)
install(FILES "${MYMCP_JSONCONS_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME jsoncons-LICENSE)
install(FILES "${MYMCP_HTTPLIB_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME cpp-httplib-LICENSE)
install(EXPORT mymcpTargets NAMESPACE mymcp:: DESTINATION lib/cmake/mymcp)
