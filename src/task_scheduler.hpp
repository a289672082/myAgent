#pragma once
// P2 固定大小的并发调度队列。任务只共享调度器，不共享 Agent 会话。
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
namespace myagent::detail {
class TaskScheduler {
public:
  explicit TaskScheduler(std::size_t workers);
  ~TaskScheduler();
  TaskScheduler(const TaskScheduler&)=delete;
  TaskScheduler& operator=(const TaskScheduler&)=delete;
  template<class F> auto submit(F&& func)->std::future<std::invoke_result_t<F&>> {
    using Return=std::invoke_result_t<F&>;
    auto task=std::make_shared<std::packaged_task<Return()>>(std::forward<F>(func));
    auto future=task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(stopping_)throw std::runtime_error("Scheduler stopped");
      queue_.emplace([task]{(*task)();});
    }
    ready_.notify_one();
    return future;
  }
private:
  void worker();
  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> threads_;
  bool stopping_=false;
};
} // namespace myagent::detail
