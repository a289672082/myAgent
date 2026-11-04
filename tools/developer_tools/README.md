# DeveloperTools — 一份实现，Agent 与 MCP 双注册

来自 `myMcpTools/examples/run_agent_tools_demo.cpp` 的 10 组、23 个工具，
已提取至 `tools/developer_tools/src/developer_tools.cpp`，仅此一处维护 Handler 和 Schema。
最后一个工具的标准名称改为 `find_symbol`（原教学 Demo 使用 `find_symbols`）。

## C++ Agent 原生注册

~~~cpp
#include "myagent/agent.hpp"
#include "developer_tools/developer_tools.hpp"

myagent::Agent agent(config);
developer_tools::RegisterToAgent(agent,
    {std::filesystem::current_path()},
    developer_tools::ToolOptions::All());

// 模型会发现 local__read_file、local__git_status 等工具。
~~~

**重要**：此注册通过 `Agent::tool()` 复用 SDK 内部 MCP Server
和 Loopback Client，但不启动外部进程，也不会自动发布公共 MCP 端点。

## 独立 MCP Server 注册

~~~cpp
#include "developer_tools/developer_tools.hpp"

mymcp::Server server("developer-tools","1.0.0");
developer_tools::RegisterToMcp(server,
    {std::filesystem::current_path()},
    developer_tools::ToolOptions::All());
server.run_stdio();
~~~

仓库附带 `myagent_developer_tools_mcp` 可执行文件；仅使用 DeveloperTools Core 和
已有 `myagent::mcp`，**不链接 myAgent 核心 SDK**。

默认 MCP Server 创建并限制在启动目录的 `demo_workspace/`；
如需指定工作区，请通过 `MYAGENT_TOOLS_WORKSPACE` 环境变量传入**已存在目录**，
绝不直接根据不可信工具参数改变工作区。示例：

~~~text
/mcp add dev stdio "C:\path\to\myagent_developer_tools_mcp.exe"
~~~

Console 默认通过 `RegisterToAgent()` 注册原生工具，默认工具审批是 `ask`；
不需要重复配置 MCP。若外部再接入同类 MCP，名称会分别带上
`local__` 和对应 MCP 前缀，请避免重复暴露造成模型困惑。

## ToolOptions

`ToolOptions::All()` 暴露全部 23 个（显式调用才开启所有类别），
`ToolOptions::ReadOnly()` 只暴露 14 个只读工具；
其他类别可以逐项使用 ToolOptions 成员启用/禁用。

## 安全限制

- 工具操作必须在已有工作区下执行，拒绝绝对路径、路径穿越和符号链接。单文件最大 1 MiB，搜索遍历最多 10000 项。
- 文件写入和补丁保留 Demo 限制；命令工具仅能执行版本查询，编译工具仅编译自动构造且校验过的 CMake 固定样例。
- HTTP 只能读取 `https://example.com/`，禁用跳转并限制响应大小。因此这**不是通用命令执行或通用 HTTP 下载工具**。
- 进程内回调不提供强隔离；文件路径检查仍存在 TOCTOU 风险。不要在同一工作区授予不可信并发操作主体。
- 实际部署前应通过 ToolPolicy 和宿主审批机制限制写入/编译/HTTP。MCP 服务端**必须自己实现权限约束**，不能依赖外部 Agent 的审批。
- 同一工具定义的多个 Handler 共享生命周期状态和互斥锁，防止临时注册对象释放导致悬空引用。
