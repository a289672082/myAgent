// 异步任务示例：注册 Task 工具、能力声明、状态轮询和协作取消。
// No command line parameters: asynchronous Tasks, client capability and polling.
#include "demo_common.hpp"
#include <atomic>
#include <chrono>
#include <thread>

int run_tasks_demo() {
  return demo::execute("run_tasks_demo", [] {
    mymcp::Server server("tasks-demo");
    // 显式开启服务端 Tasks 扩展能力。
    server.enable_tasks();
    // 注册耗时工具，实际工作在任务生命周期中完成。
    server.add_task_tool("create_report", "Build an in-memory sample report",
      mymcp::json::object{{"type", "object"}},
      [](const mymcp::Json&, std::atomic_bool& cancelled) -> mymcp::Json {
        for (int i=0; i<8; ++i) {
          // 通过原子标志协作检查取消请求，不能强行中断任意 C++ 回调。
          if (cancelled.load()) return mymcp::detail::make_tool_result("cancelled");
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return mymcp::detail::make_tool_result("demo report ready");
      });
    auto client = demo::in_process(server);
    // 客户端在发现服务端之前声明支持 Tasks。
    client.enable_tasks(); // must advertise before discovering
    // 工具调用先返回任务句柄，而不是等待报告处理完成。
    const auto result = client.call("create_report");
    demo::print("task created", result);
    // 提取 taskId，用于后续 tasks/get 等操作。
    const std::string id = result.at("taskId").as_string();
    bool completed = false;
    // 轮询状态并设置尝试次数上限，避免测试无限等待。
    for (int attempt=0; attempt<100; ++attempt) {
      auto state = client.get_task(id);
      auto status = state.at("status").as_string();
      if (status == "completed") {
        demo::print("task completed", state);
        completed = true;
        break;
      }
      demo::check(status != "failed" && status != "cancelled", "Task unexpectedly failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    demo::check(completed, "Task timed out");
    // See client.cancel_task(id) and client.update_task(id, responses) for other lifecycle operations.
  });
}
int main() { return run_tasks_demo(); }
