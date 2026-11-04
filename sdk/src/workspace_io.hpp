#pragma once
#include <filesystem>
#include <string>

namespace myagent::detail {
// 只提供工作区内的文本文件工具；不是整个进程/MCP 的安全沙箱。
// POSIX 使用 openat(O_NOFOLLOW) 和目录 FD，防止符号链接替换穿越。
// Windows 逐级打开并持有非共享写入/删除的目录句柄，拒绝 reparse point；
// 非工作区工具、MCP 与恶意宿主进程仍需独立的 OS 权限隔离。
std::string workspace_read(const std::filesystem::path& root,const std::string& relative);
std::string workspace_list(const std::filesystem::path& root,const std::string& relative);
bool workspace_write(const std::filesystem::path& root,const std::string& relative,
                     const std::string& content);
}
