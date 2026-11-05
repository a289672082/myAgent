# 内置第三方依赖

以下目录包含对应上游仓库的完整源代码快照，包括头文件、示例、测试、文档和原始许可证文件；有意不包含 Git 历史记录（`.git`）。

| 本地目录 | 上游代码来源 | 固定提交 |
| --- | --- | --- |
| `third_party/jsoncons` | https://github.com/danielaparker/jsoncons | `91c5e03f9169f2accecf3d5c082163d6a2f34b18` |
| `third_party/cpp-httplib` | https://github.com/yhirose/cpp-httplib | `a18895f42e4768062b858c6847b1ae598b2a3be3` |

原始许可证保留在 `third_party/jsoncons/LICENSE` 和 `third_party/cpp-httplib/LICENSE`。重新分发 SDK 时必须检查并保留相关许可声明。

## 离线构建

```sh
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake **不会在线下载**这些库，而是直接使用仓库内的源码；缺少依赖目录时会给出明确错误。OpenSSL、CMake、C++20 编译器和线程库仍需由系统或工具链提供。

安装版 SDK 的分发内容也包含所需的 jsoncons、cpp-httplib 头文件和上游许可证。

## 更新依赖的流程

先审查上游变更及许可证，再更新固定提交，重新生成完整的上游源码快照（只排除 `.git` 元数据）；合并前务必在 Linux 与 Windows 测试。
