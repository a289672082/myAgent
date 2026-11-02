#pragma once
// 有界 SSE 流解码器：按换行增量消费 data 行，支持 CRLF 与多段 data。
// 拒绝超限传输和不完整的最后一帧，不把服务器任意事件当作最终成功。
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
namespace myagent::detail {
class SseFrames {
public:
  template<class Callback>
  bool feed(const char* data,std::size_t size,Callback&& on_data) {
    constexpr std::size_t max_bytes=8*1024*1024;
    if(size>max_bytes-total_)throw std::runtime_error("SSE transfer exceeds 8 MiB");
    total_+=size;
    if(size>max_bytes-pending_.size())throw std::runtime_error("SSE frame exceeds 8 MiB");
    pending_.append(data,size);
    for(;;) {
      const auto pos=pending_.find('\n');
      if(pos==std::string::npos)break;
      auto line=pending_.substr(0,pos);
      pending_.erase(0,pos+1);
      if(!line.empty()&&line.back()=='\r')line.pop_back();
      if(line.empty()) {
        if(!payload_.empty()) {
          on_data(payload_);
          payload_.clear();
        }
      } else if(line.rfind("data:",0)==0) {
        auto value=line.substr(5);
        if(!value.empty()&&value.front()==' ')value.erase(0,1);
        if(!payload_.empty())payload_+='\n';
        if(value.size()>max_bytes-payload_.size())
          throw std::runtime_error("SSE payload exceeds 8 MiB");
        payload_+=value;
      }
    }
    return true;
  }
  bool empty()const{return pending_.empty()&&payload_.empty();}
private:
  std::size_t total_=0;
  std::string pending_,payload_;
};
} // namespace myagent::detail
