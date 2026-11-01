#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
namespace myagent::detail {
// Open the actual file handle first, verify a regular file, and enforce the
// byte limit while reading. Never follow a symlink as the final file.
std::string read_regular_file(const std::filesystem::path& file,std::size_t limit);
// Write via a unique temporary file and durable same-directory replacement.
// single_writer=true also refuses concurrent writes from other processes
// through a lock directory (stale locks must be removed manually after crash).
void atomic_replace(const std::filesystem::path& path,const std::string& data,
                    bool single_writer=false);
} // namespace myagent::detail
