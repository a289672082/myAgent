# Visual Studio 2022 原生解决方案（无需命令行）

## 直接在 Visual Studio 编译

本仓库已提交根目录的 `myAgent.sln` 和 `vs2022/*.vcxproj`，是真正的 **Visual C++ MSBuild 项目**，不是旧版 `Makefile/NMake` 包装器。不调用 `build.cmd`、`cmake --build`、`cmake --preset` 或任何预编译脚本。

1. 安装 **Visual Studio 2022** 的“使用 C++ 的桌面开发”工作负载（MSVC v143 + Windows 10/11 SDK）。
2. 获取完整的 Git 仓库，**必须包含两个 Git 子模块**：`third_party/FTXUI` 和 `third_party/yaml-cpp`。**GitHub 网页右上角的 Download ZIP 不含子模块源码**，直接解压 ZIP 后打开 `.sln` 会出现一百多条找不到 `.cpp` / `.hpp` 的错误。如果已经通过 Git 克隆，使用支持子模块的图形客户端（如 TortoiseGit：项目目录右键 → TortoiseGit → **Submodule Update**，选择初始化及递归更新）获取固定版本源码；若尚未克隆，请用 Git 客户端选择“递归克隆/Clone with submodules”。
3. 安装 **x64 OpenSSL 开发环境**（必须同时包含头文件、`libssl.lib` 和 `libcrypto.lib`）。工程会自动识别 `C:\Program Files\OpenSSL`、`C:\Program Files\OpenSSL-Win64` 和 `C:\OpenSSL-Win64` 三种常见目录；Debug 库应位于 `lib\VC\x64\MDd`，Release 库应位于 `lib\VC\x64\MD`。如果装在其他目录，请在 Visual Studio **视图 → 其他窗口 → 属性管理器** 设置 `vs2022/myAgent.Native.props` 的 `OpenSslRoot`，也可以使用环境变量 `OPENSSL_ROOT_DIR`。仅有 DLL、缺少 SDK 头文件与 `.lib` 的 OpenSSL 运行环境不足以编译。
4. 在 Windows 资源管理器中双击 **`myAgent.sln`**。
5. 在 Visual Studio 上方选择 **Debug / x64** 或 **Release / x64**。右键 `myAgentConsole` → **设为启动项目**。
6. 使用菜单 **生成 → 生成解决方案** 或 **Ctrl+Shift+B**，构建完成后按 **F5** 运行。输出统一位于 `build-vs2022/native-msbuild/<Debug|Release>/bin/`。

**编译器是 VS 自带的 MSVC (cl.exe)，链接器是 link.exe，静态库工具是 lib.exe；所有项目都由 Visual Studio 自身直接管理。** GitHub Actions 中使用 `msbuild` 仅用于自动化验证相同的原生工程，你本机不需要打开命令提示符。

## 解决方案中的项目

- SDK：`mymcp`、`myagent`。
- 第三方依赖：`ftxui_screen`、`ftxui_dom`、`ftxui_component`、`yaml_cpp`。
- 共享工具：`developer_tools_core`、`developer_tools_agent`。
- 可运行应用：`myAgentConsole`、`myagent_developer_tools_mcp`。
- Console 模块：`myagent_console_session`。
- 各类 Agent / MCP `run_xxxx_demo`：每个示例各自有一个 **原生可执行项目**，可右键单独生成、设为启动项目并按 F5。
- 回归测试：`myagent_console_commands_tests`、`myagent_console_chat_tests`、`developer_tools_tests`，可从 VS 直接生成运行。

默认“生成解决方案”只构建库和控制台，避免一次性编译全部独立 Demo；若要构建示例或测试，可在解决方案资源管理器中右键项目单独生成，或者在 **生成 → 配置管理器** 勾选对应项目。每个项目的 `ProjectReference` 会自动触发其所需库的编译。

## 对比旧版工程

| 项目 | 旧版已删除工程 | 本版 |
| --- | --- | --- |
| 根解决方案 | 曾删除，需要运行 CMake 才能生成 | 仓库自带 `myAgent.sln` |
| VS 项目类型 | Makefile / NMake，委托 `build.cmd` | Application / StaticLibrary，原生 C++ |
| 配置 | 常需命令行选择 CMake Preset | VS GUI 直接选 Debug / Release x64 |
| SDK 编译 | NMake 命令间接调用 CMake | MSVC `ClCompile` / MSBuild `Lib` |
| 调试 | 命令行桥接输出路径 | VS F5 本地 Windows 调试器 |
| CMake | 构建的必需步骤 | **仅为可选的跨平台构建方式** |

## 常见问题

### 大量“无法打开源文件”错误（最常见）

先在资源管理器检查下面三个文件是否真实存在（不要只看是否有空文件夹）：

- `third_party/FTXUI/src/ftxui/screen/box.cpp`
- `third_party/yaml-cpp/src/binary.cpp`
- `third_party/FTXUI/include/ftxui/component/app.hpp`

如果其中任何一个缺失，请先检出 Git 子模块。用 TortoiseGit 等图形工具选择 **Submodule Update / 初始化**，无需执行 CMake。若项目是从网页下载 ZIP 得到的，请重新使用支持 Git 子模块的客户端克隆；ZIP 没有父仓库的 Git 信息，不能直接对子模块进行更新。<https://git-scm.com/docs/git-submodule>

新版 `myAgent.Native.props` 会在 MSBuild 进入 C++ 编译之前检查上述依赖。缺失时应给出中文提示，而不是继续打印数百条 `C1083`；这**不会自动安装**第三方库。

### OpenSSL 头文件 / 库缺失

检查 `$(OpenSslRoot)\include\openssl\ssl.h`、`$(OpenSslLibDir)\libssl.lib` 和 `$(OpenSslLibDir)\libcrypto.lib`。
如果你安装的是 `OpenSSL-Win64`，新版公共属性文件将自动检测常见安装路径；其他路径请设置 `OpenSslRoot`。
**Debug** 和 **Release** 使用不同的 MSVC 运行库目录（MDd/MD），两套链接库都应存在。

### 其他错误

- **找不到 `mymcp.lib`**：通常是前置的 MCP 项目因缺少 OpenSSL 未生成，先解决前面的头文件/库错误，再右键“重新生成解决方案”。
- **`getenv` 或 `NOMINMAX` 的警告**：警告不等于阻断性错误，不会造成几十个源码文件同时丢失。
- **调试时缺少 OpenSSL DLL**：项目已在调试环境 PATH 前面追加 `$(OpenSslRoot)\bin`；若安装布局不同，检查 OpenSSL 安装目录。
- **MCP stdio 客户端 Demo 找不到服务端**：请先右键构建 `run_stdio_server_demo`；`run_stdio_client_demo` 和 `run_progress_demo` 已设置项目依赖和 VS 调试工作目录为输出目录。

本机原生项目所需的 FTXUI/yaml-cpp 源代码依旧属于 Git 子模块。**OpenSSL 没有随仓库捆绑，必须由 Windows 环境安装。** 当前方案保证无需命令行构建，但不代表“无需安装编译依赖”。
