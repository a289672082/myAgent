cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpSDK VERSION 0.2.0 LANGUAGES CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
set(MYMCP_JSONCONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/jsoncons")
set(MYMCP_HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cpp-httplib")
foreach(path IN ITEMS
  "${MYMCP_JSONCONS_DIR}/include/jsoncons/json.hpp"
  "${MYMCP_JSONCONS_DIR}/include/jsoncons_ext/jsonschema/jsonschema.hpp"
  "${MYMCP_JSONCONS_DIR}/LICENSE"
  "${MYMCP_HTTPLIB_DIR}/httplib.h"
  "${MYMCP_HTTPLIB_DIR}/LICENSE")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Missing embedded MCP source: ${path}")
  endif()
endforeach()
add_library(mymcp STATIC src/mcp.cpp src/schema.cpp src/oauth.cpp src/oauth_client.cpp src/uri_template.cpp src/jwt.cpp)
add_library(mymcp::mymcp ALIAS mymcp)
target_include_directories(mymcp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${MYMCP_JSONCONS_DIR}/include>
  $<BUILD_INTERFACE:${MYMCP_HTTPLIB_DIR}>)
target_compile_definitions(mymcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(mymcp PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(mymcp PUBLIC ws2_32 crypt32)
endif()
install(TARGETS mymcp EXPORT mymcpTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY "${MYMCP_JSONCONS_DIR}/include/" DESTINATION include)
install(FILES "${MYMCP_HTTPLIB_DIR}/httplib.h" DESTINATION include)
install(FILES "${MYMCP_JSONCONS_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME jsoncons-LICENSE)
install(FILES "${MYMCP_HTTPLIB_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME cpp-httplib-LICENSE)
install(EXPORT mymcpTargets NAMESPACE mymcp:: DESTINATION lib/cmake/mymcp)
