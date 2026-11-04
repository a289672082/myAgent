#!/usr/bin/env python3
"""Embed the pinned upstream MCP SDK, demos, tests and third-party sources."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
sha = "3a6c8bf434ae3c1fd708ff43aab6b24f3e8dbd30"
upstream = "https://github.com/a289672082/myMcpTools.git"
dollar = chr(36)

def run(*args):
    subprocess.run(args, check=True, cwd=root)

def write(path, contents):
    dest = root / path
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(contents.replace("@{", dollar + "{").strip() + "\n", encoding="utf-8")

def cmake_part(text, beginning, ending=None):
    i = text.index(beginning)
    return text[i:text.index(ending, i)] if ending else text[i:]

if not (root / "mcp/CMakeLists.txt").exists():
    run("git", "subtree", "add", "--prefix=mcp", "--squash", upstream, sha)
if (root / "mcp/src").exists():
    run("git", "mv", "mcp/src", "mcp/sdk/src")
    run("git", "mv", "mcp/include", "mcp/sdk/include")
assert (root / "mcp/sdk/src/mcp.cpp").exists()
original = (root / "mcp/CMakeLists.txt").read_text(encoding="utf-8")

write("mcp/CMakeLists.txt", """
cmake_minimum_required(VERSION 3.20)
project(myMcpTools VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MYMCP_BUILD_EXAMPLES "Build MCP demos" ON)
option(MYMCP_BUILD_TESTS "Build MCP tests" ON)
add_subdirectory(sdk)
if(MYMCP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MYMCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
""")

write("mcp/sdk/CMakeLists.txt", """
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpSDK VERSION 0.2.0 LANGUAGES CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
set(MYMCP_JSONCONS_DIR "@{CMAKE_CURRENT_SOURCE_DIR}/../third_party/jsoncons")
set(MYMCP_HTTPLIB_DIR "@{CMAKE_CURRENT_SOURCE_DIR}/../third_party/cpp-httplib")
foreach(path IN ITEMS
  "@{MYMCP_JSONCONS_DIR}/include/jsoncons/json.hpp"
  "@{MYMCP_JSONCONS_DIR}/include/jsoncons_ext/jsonschema/jsonschema.hpp"
  "@{MYMCP_JSONCONS_DIR}/LICENSE"
  "@{MYMCP_HTTPLIB_DIR}/httplib.h"
  "@{MYMCP_HTTPLIB_DIR}/LICENSE")
  if(NOT EXISTS "@{path}")
    message(FATAL_ERROR "Missing embedded MCP source: @{path}")
  endif()
endforeach()
add_library(mymcp STATIC src/mcp.cpp src/schema.cpp src/oauth.cpp src/oauth_client.cpp)
add_library(mymcp::mymcp ALIAS mymcp)
target_include_directories(mymcp PUBLIC
  $<BUILD_INTERFACE:@{CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:@{MYMCP_JSONCONS_DIR}/include>
  $<BUILD_INTERFACE:@{MYMCP_HTTPLIB_DIR}>)
target_compile_definitions(mymcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(mymcp PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(mymcp PUBLIC ws2_32 crypt32)
endif()
install(TARGETS mymcp EXPORT mymcpTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY "@{MYMCP_JSONCONS_DIR}/include/" DESTINATION include)
install(FILES "@{MYMCP_HTTPLIB_DIR}/httplib.h" DESTINATION include)
install(FILES "@{MYMCP_JSONCONS_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME jsoncons-LICENSE)
install(FILES "@{MYMCP_HTTPLIB_DIR}/LICENSE" DESTINATION share/mymcp/licenses RENAME cpp-httplib-LICENSE)
install(EXPORT mymcpTargets NAMESPACE mymcp:: DESTINATION lib/cmake/mymcp)
""")

demo = cmake_part(original, "# No-argument, single-purpose demos", "foreach(demo IN ITEMS tools resources prompts mrtr tasks extensions stdio_client")
demo = demo.replace("examples/run_", "run_")
demo = demo.replace(dollar + "{CMAKE_CURRENT_SOURCE_DIR}/examples", dollar + "{CMAKE_CURRENT_SOURCE_DIR}")
write("mcp/examples/CMakeLists.txt", """
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpExamples LANGUAGES CXX)
endif()
if(NOT TARGET mymcp)
  add_subdirectory("@{CMAKE_CURRENT_LIST_DIR}/../sdk" "@{CMAKE_CURRENT_BINARY_DIR}/mcp-sdk")
endif()
""" + demo + """
add_executable(run_agent_tools_demo run_agent_tools_demo.cpp)
target_link_libraries(run_agent_tools_demo PRIVATE mymcp)
""")

tests = cmake_part(original, "add_executable(mymcp_example_server", "# No-argument, single-purpose demos")
# Install rules live in mcp/sdk, not in tests.
tests = re.sub(r"(?m)^install\([^\n]*\)\n", "", tests)
tests += """
if(TARGET run_tools_demo)
  foreach(name IN ITEMS tools resources prompts mrtr tasks extensions stdio_client progress oauth oauth_login)
    add_test(NAME demo_@{name} COMMAND run_@{name}_demo)
    set_tests_properties(demo_@{name} PROPERTIES TIMEOUT 20)
  endforeach()
  if(Python3_Interpreter_FOUND)
    add_test(NAME demo_http_and_sse
      COMMAND "@{Python3_EXECUTABLE}" "@{CMAKE_CURRENT_SOURCE_DIR}/run_demo_smoke.py"
        "$<TARGET_FILE:run_http_server_demo>" "$<TARGET_FILE:run_http_client_demo>"
        "$<TARGET_FILE:run_subscriptions_demo>" "$<TARGET_FILE:run_http_progress_demo>"
        "$<TARGET_FILE:run_oauth_resource_demo>" "$<TARGET_FILE:run_oauth_resource_client_demo>")
    set_tests_properties(demo_http_and_sse PROPERTIES TIMEOUT 35)
    add_test(NAME agent_tools_demo
      COMMAND "@{Python3_EXECUTABLE}" "@{CMAKE_CURRENT_SOURCE_DIR}/agent_tools_demo_smoke.py"
        "$<TARGET_FILE:run_agent_tools_demo>")
    set_tests_properties(agent_tools_demo PROPERTIES TIMEOUT 90)
  endif()
endif()
"""
tests += cmake_part(original, "# Simulated Codex/Claude legacy")
# Move paths from original root-based tests directory to this directory.
tests = tests.replace(dollar + "{CMAKE_SOURCE_DIR}/tests/", dollar + "{CMAKE_CURRENT_SOURCE_DIR}/")
tests = tests.replace("tests/fixtures/", "fixtures/")
tests = re.sub(r"\btests/(test_|listen_client\.cpp|legacy_http_client\.cpp)", r"\1", tests)
write("mcp/tests/CMakeLists.txt", """
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(myMcpTests LANGUAGES CXX)
endif()
if(NOT TARGET mymcp)
  add_subdirectory("@{CMAKE_CURRENT_LIST_DIR}/../sdk" "@{CMAKE_CURRENT_BINARY_DIR}/mcp-sdk")
endif()
enable_testing()
""" + tests)

agent_path = root / "sdk/CMakeLists.txt"
a = agent_path.read_text(encoding="utf-8")
i = a.index("# Shared third-party sources")
j = a.index("add_library(myagent STATIC", i)
a = a[:i] + """
# Build local, independent MCP SDK; no myMcpTools Git submodule.
if(NOT TARGET mymcp)
  add_subdirectory("@{CMAKE_CURRENT_SOURCE_DIR}/../mcp/sdk"
    "@{CMAKE_CURRENT_BINARY_DIR}/mcp-sdk")
endif()
if(NOT TARGET myagent_mcp)
  add_library(myagent_mcp ALIAS mymcp)
endif()
if(NOT TARGET myagent::mcp)
  add_library(myagent::mcp ALIAS mymcp)
endif()
""".replace("@{", dollar + "{") + a[j:]
a = a.replace("target_link_libraries(myagent PUBLIC myagent_mcp PRIVATE yaml-cpp::yaml-cpp)",
              "target_link_libraries(myagent PUBLIC mymcp PRIVATE yaml-cpp::yaml-cpp)")
agent_path.write_text(a, encoding="utf-8")

write("CMakeLists.txt", """
cmake_minimum_required(VERSION 3.20)
project(myAgentConsole VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MYAGENT_BUILD_CONSOLE "Build FTXUI console" ON)
option(MYAGENT_BUILD_EXAMPLES "Build Agent demos" OFF)
option(MYAGENT_BUILD_TESTS "Build Agent tests" OFF)
option(MYAGENT_CONSOLE_BUILD_TESTS "Build console tests" ON)
option(MYAGENT_BUILD_MCP_EXAMPLES "Build MCP demos" OFF)
option(MYAGENT_BUILD_MCP_TESTS "Build MCP tests" OFF)
if(MYAGENT_BUILD_TESTS OR MYAGENT_BUILD_MCP_TESTS OR
   (MYAGENT_BUILD_CONSOLE AND MYAGENT_CONSOLE_BUILD_TESTS))
  enable_testing()
endif()
add_subdirectory(mcp/sdk)
add_subdirectory(sdk)
if(MYAGENT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MYAGENT_BUILD_MCP_EXAMPLES)
  add_subdirectory(mcp/examples)
endif()
if(MYAGENT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(MYAGENT_BUILD_MCP_TESTS)
  add_subdirectory(mcp/tests)
endif()
if(MYAGENT_BUILD_CONSOLE)
  add_subdirectory(console)
endif()
""")

run("git", "rm", "--cached", "-f", "third_party/myMcpTools")
modules = root / ".gitmodules"
s = modules.read_text(encoding="utf-8")
s = re.sub(r'\[submodule "third_party/myMcpTools"\]\n(?:[ \t]+[^\n]*\n)+\n?', "", s)
modules.write_text(s.lstrip(), encoding="utf-8")
for p in (root / "vs2022").glob("*.vcxproj"):
    text = p.read_text(encoding="utf-8")
    text = text.replace("third_party\\myMcpTools\\third_party\\jsoncons", "mcp\\third_party\\jsoncons")
    text = text.replace("third_party\\myMcpTools\\include", "mcp\\sdk\\include")
    p.write_text(text, encoding="utf-8")

write("mcp/UPSTREAM.md", f"""
# Vendored MCP SDK
Source: {upstream}
Pinned upstream main commit: {sha}
The entire upstream source, examples, tests and third_party directories are embedded.
Builds do not fetch anything from the myMcpTools repository.
""")
readme = root / "README.md"
r = readme.read_text(encoding="utf-8")
if "# 本地 MCP SDK" not in r:
    readme.write_text("""# 本地 MCP SDK（不依赖 myMcpTools 子模块）

Agent SDK: sdk/；Agent demo: examples/。
MCP SDK: mcp/sdk/；MCP demo: mcp/examples/；MCP tests: mcp/tests/。
MCP 的 jsoncons、cpp-httplib 及许可证位于 mcp/third_party/。
根项目构建两套 SDK；可用 cmake -S sdk 或 cmake -S mcp 分别构建。
根项目使用 MYAGENT_BUILD_MCP_EXAMPLES、MYAGENT_BUILD_MCP_TESTS 可开启额外的示例和测试。
yaml-cpp 和 FTXUI 仍保留自己的子模块，与 myMcpTools 无关。

""" + r, encoding="utf-8")
agent_md = root / "AGENT.md"
if agent_md.exists():
    s = agent_md.read_text(encoding="utf-8")
    s = s.replace("myMcpTools 作为独立 SDK 通过 third_party/myMcpTools Git submodule 使用。",
                  "myMcpTools SDK、测试和 Demo 已内置在 mcp/ 目录，不依赖外部 myMcpTools 子模块。")
    agent_md.write_text(s, encoding="utf-8")
print("Imported latest MCP SDK:", sha)
