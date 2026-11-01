#include "myagent/memory.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <set>
namespace myagent {
namespace j=mymcp::json;
namespace {
std::string lower(std::string s) {
  std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){
    return static_cast<char>(std::tolower(c));
  });
  return s;
}
std::set<std::string> tokens(const std::string& source) {
  std::set<std::string> words;
  std::string current;
  for(unsigned char c:source) {
    if(std::isalnum(c) || c >= 128)current+=static_cast<char>(std::tolower(c));
    else if(!current.empty()) { words.insert(current);current.clear(); }
  }
  if(!current.empty())words.insert(current);
  return words;
}
} // namespace
void MemoryStore::remember(MemoryRecord r) {
  if(r.id.empty()||r.id.size()>128||r.content.empty()||r.content.size()>16384)
    throw std::invalid_argument("Memory ID/content invalid");
  const auto it=std::find_if(records_.begin(),records_.end(),
    [&](const MemoryRecord& x){return x.id==r.id;});
  if(it!=records_.end())*it=std::move(r);
  else {
    if(records_.size()>=10000)throw std::runtime_error("Memory records limit reached");
    records_.push_back(std::move(r));
  }
}
bool MemoryStore::forget(const std::string& id) {
  auto it=std::remove_if(records_.begin(),records_.end(),
    [&](const MemoryRecord& x){return x.id==id;});
  if(it==records_.end())return false;
  records_.erase(it,records_.end());return true;
}
std::vector<MemoryRecord> MemoryStore::search(const std::string& query,std::size_t top_k) const {
  struct Match {std::size_t index,score;};
  const auto keywords=tokens(query);
  const auto input=lower(query);
  std::vector<Match> matches;
  if(input.empty()||top_k==0)return {};
  for(std::size_t i=0;i<records_.size();++i) {
    const auto& content=records_[i].content;
    const auto words=tokens(content);
    std::size_t score=lower(content).find(input)!=std::string::npos?20:0;
    for(const auto& word:keywords)if(words.count(word))score+=5;
    if(score)matches.push_back({i,score});
  }
  std::stable_sort(matches.begin(),matches.end(),
    [](const Match& a,const Match& b){return a.score>b.score;});
  std::vector<MemoryRecord> out;
  for(const auto& m:matches) {
    if(out.size()>=top_k)break;
    out.push_back(records_[m.index]);
  }
  return out;
}
void MemoryStore::save(const std::filesystem::path& file) const {
  mymcp::Json data=mymcp::json::array{};
  for(const auto& r:records_)
    data.push_back(mymcp::json::object{{"id",r.id},{"content",r.content},
                                       {"metadata",r.metadata}});
  const auto bytes=mymcp::json::serialize(data);
  if(bytes.size()>32*1024*1024)
    throw std::runtime_error("Memory save exceeds the 32 MiB load limit");
  std::ofstream output(file,std::ios::binary|std::ios::trunc);
  if(!output)throw std::runtime_error("Cannot save memory");
  output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
  if(!output)throw std::runtime_error("Memory save error");
}
void MemoryStore::load(const std::filesystem::path& file) {
  if(std::filesystem::file_size(file)>32*1024*1024)
    throw std::runtime_error("Memory file too large");
  std::ifstream input(file,std::ios::binary);
  if(!input)throw std::runtime_error("Cannot load memory");
  const auto bytes=std::string(std::istreambuf_iterator<char>(input),{});
  const auto data=mymcp::json::parse(bytes);
  if(!data.is_array()||data.size()>10000)
    throw std::runtime_error("Invalid memory file");
  MemoryStore candidate;
  for(const auto& entry:data.array_range())
    candidate.remember(MemoryRecord{
      std::string(entry.at("id").as_string()),
      std::string(entry.at("content").as_string()),entry.at("metadata")});
  *this=std::move(candidate);
}
} // namespace myagent
