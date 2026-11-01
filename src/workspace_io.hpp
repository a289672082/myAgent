#pragma once
#include <filesystem>
#include <string>

namespace myagent::detail {
// 只提供工作区内的文本文件工具；不是整个进程/MCP 的安全沙箱。
// POSIX 使用 openat(O_NOFOLLOW) 和目录 FD，防止符号链接替换穿越。
// 其他平台通过显式检查和原子写入实现兼容，强对抗需要 OS 沙箱。
std::string workspace_read(const std::filesystem::path& root,const std::string& relative);
std::string workspace_list(const std::filesystem::path& root,const std::string& relative);
bool workspace_write(const std::filesystem::path& root,const std::string& relative,
                     const std::string& content);
}
