cmake_minimum_required(VERSION 3.20)
project(myAgentConsole VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 根项目以终端控制台为默认入口。SDK、示例、测试按模块独立构建。
option(MYAGENT_BUILD_CONSOLE "Build FTXUI-based interactive console" ON)
option(MYAGENT_BUILD_EXAMPLES "Build standalone SDK examples" OFF)
option(MYAGENT_BUILD_TESTS "Build offline SDK regression tests" OFF)
option(MYAGENT_CONSOLE_BUILD_TESTS "Build offline console regression tests" ON)

if(MYAGENT_BUILD_TESTS OR (MYAGENT_BUILD_CONSOLE AND MYAGENT_CONSOLE_BUILD_TESTS))
  enable_testing()
endif()

# SDK 不允许依赖 console / FTXUI。也可直接运行 cmake -S sdk。
add_subdirectory(sdk)
if(MYAGENT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MYAGENT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(MYAGENT_BUILD_CONSOLE)
  add_subdirectory(console)
endif()
