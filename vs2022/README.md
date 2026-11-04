# Visual Studio 2022 项目（x64 / Debug / Release）

仓库根目录的 `myAgent.sln` 可直接由 VS2022 打开，无需先手动运行 CMake。其工程文件位于 `vs2022/`：

- **myAgentConsole**（默认启动/默认编译项目）：构建 `console/` 的 FTXUI 交互控制台，可直接 F5 调试，工作目录为仓库根。
- **myAgentSDK**：只构建 `sdk/` 公共 C++20 SDK，不读取 FTXUI。
- **myAgentExamples**：构建 SDK 全部示例，未绑定控制台。
- **myAgentTests**：构建/运行 SDK 全部 CTest（由 `build.cmd` 完成）。

这些 `.vcxproj` 是 VS2022 的 **Makefile/NMake 桥接工程**：解决方案可直接打开、浏览源码、执行 Build/Clean/Rebuild、F5 调试；底层编译仍由仓库唯一的 CMakeLists 管理，这样不会手工复制 3rd-party 依赖或 SDK 编译选项。它们不是手工维护的 `ClCompile` 构建规则（源码条目用于 VS 浏览）。

第一次使用：

```bat
git submodule update --init --recursive
start myAgent.sln
```

安装 Visual Studio 2022 的 **Desktop development with C++**、MSVC v143、Windows SDK 和 CMake，
并保证 `cmake` 在 PATH 中。编译时会使用固定本地源码 `third_party/FTXUI`，不从网络动态下载。
默认解决方案只编译 `myAgentConsole`，避免重复构建 SDK/Examples/Tests；
需要这些工程时在“解决方案资源管理器”中右键对应工程选择“生成”。
控制台在 `build-vs2022/console/console/Debug/myagent_console.exe`（或 Release）输出。

还提供 **原生 CMake 生成的 Visual Studio 工程**，供需要逐文件设置编译选项、完整 IntelliSense 或直接在 SDK 源码上断点调试时使用：

```bat
cmake --preset vs2022-console
cmake --build --preset vs2022-console-debug
rem 打开 build-vs2022\native-console\myAgentConsole.sln

cmake --preset vs2022-sdk-demos
cmake --build --preset vs2022-sdk-debug
rem 打开 build-vs2022\native-sdk-demos\myAgentConsole.sln
```

注意：`build-vs2022/` 是构建输出，不提交至 Git。实际源码、SDK API、CMake 选项和 SDK CI 不因这些 VS 文件而改变。
