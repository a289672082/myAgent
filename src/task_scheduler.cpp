#include "task_scheduler.hpp"
namespace myagent::detail {
TaskScheduler::TaskScheduler(std::size_t workers) {
  if(!workers||workers>64)throw std::invalid_argument("TaskScheduler worker count must be 1..64");
  try {
    for(std::size_t i=0;i<workers;++i)threads_.emplace_back([this]{worker();});
  } catch(...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_=true;
    }
    ready_.notify_all();
    for(auto& thread:threads_)if(thread.joinable())thread.join();
    throw;
  }
}
TaskScheduler::~TaskScheduler() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_=true;
  }
  ready_.notify_all();
  for(auto& thread:threads_)if(thread.joinable())thread.join();
}
void TaskScheduler::worker() {
  for(;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock,[this]{return stopping_||!queue_.empty();});
      if(stopping_&&queue_.empty())return;
      task=std::move(queue_.front());
      queue_.pop();
    }
    task(); // packaged_task 将异常通过 future 交回调用线程
  }
}
} // namespace myagent::detail
