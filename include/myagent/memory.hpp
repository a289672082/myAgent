#pragma once
// P2：轻量长期记忆与关键词检索。不是向量数据库，也不执行检索文本里的指令。
#include "mymcp/mcp.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
namespace myagent {
struct MemoryRecord {
  std::string id;
  std::string content;
  mymcp::Json metadata = mymcp::json::object{};
};
class MemoryStore {
public:
  void remember(MemoryRecord record);
  bool forget(const std::string& id);
  std::vector<MemoryRecord> search(const std::string& query,std::size_t top_k=5) const;
  const std::vector<MemoryRecord>& all() const { return records_; }
  void save(const std::filesystem::path& file) const;
  void load(const std::filesystem::path& file);
private:
  std::vector<MemoryRecord> records_;
  std::size_t serialized_bytes_ = 2; // Exact serialized JSON array size.
};
} // namespace myagent
