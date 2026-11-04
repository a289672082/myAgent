cmake_minimum_required(VERSION 3.20)

# 独立控制台工程：只在显式 -S console 时配置。
# 不修改根 CMakeLists，不向 myAgent SDK 添加编译选项、头文件或 FTXUI 依赖。
project(myagent_console LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 当前构建仅需 SDK 和终端程序，原来的 SDK 示例/测试单独由主 CI 管理。
set(MYAGENT_BUILD_EXAMPLES OFF)
set(MYAGENT_BUILD_TESTS OFF)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/sdk")

# 默认使用已安装的 FTXUI，保持离线构建。若用户明确授权，
# 可使用 -DMYAGENT_CONSOLE_FETCH_FTXUI=ON 下载固定版本（仅 console 工程）。
option(MYAGENT_CONSOLE_FETCH_FTXUI "Download pinned FTXUI for this console build only" OFF)
find_package(ftxui 7 CONFIG QUIET)
if(NOT TARGET ftxui::ftxui)
  if(NOT MYAGENT_CONSOLE_FETCH_FTXUI)
    message(FATAL_ERROR
      "FTXUI not found. Install FTXUI v7 (vcpkg / package manager) or "
      "explicitly pass -DMYAGENT_CONSOLE_FETCH_FTXUI=ON. "
      "SDK root CMake does NOT require FTXUI.")
  endif()
  include(FetchContent)
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v7.0.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(ftxui)
endif()

add_library(myagent_console_session STATIC session.cpp)
target_include_directories(myagent_console_session PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(myagent_console_session PUBLIC myagent::agent)

add_executable(myagent_console main.cpp)
target_link_libraries(myagent_console PRIVATE myagent_console_session ftxui::ftxui)

# 配置/安全规则不依赖真实模型或终端交互：可离线自动回归。
option(MYAGENT_CONSOLE_BUILD_TESTS "Build console-only offline tests" ON)
if(MYAGENT_CONSOLE_BUILD_TESTS)
  enable_testing()
  add_executable(myagent_console_commands_tests tests/test_commands.cpp)
  target_link_libraries(myagent_console_commands_tests PRIVATE myagent_console_session)
  add_test(NAME myagent_console_commands COMMAND myagent_console_commands_tests)
  set_tests_properties(myagent_console_commands PROPERTIES TIMEOUT 15)
  add_executable(myagent_console_chat_tests tests/test_chat.cpp)
  target_link_libraries(myagent_console_chat_tests PRIVATE myagent_console_session)
  add_test(NAME myagent_console_chat COMMAND myagent_console_chat_tests)
  set_tests_properties(myagent_console_chat PROPERTIES TIMEOUT 20)
endif()
