# 仅依赖共用 DeveloperTools Core / MCP Runtime；不链接 myAgent。
add_executable(myagent_developer_tools_mcp main.cpp)
target_compile_features(myagent_developer_tools_mcp PRIVATE cxx_std_20)
target_link_libraries(myagent_developer_tools_mcp PRIVATE developer_tools_core)
