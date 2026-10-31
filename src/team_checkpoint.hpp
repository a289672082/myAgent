#pragma once
#include "myagent/multi_agent.hpp"
#include <map>
#include <set>
namespace myagent::detail {
// Checkpoint 记录已验证任务和启动中的任务；不保存 API Key 或模型对象。
// 如果进程在工具调用中崩溃，启动中的任务副作用未知：默认禁止盲目重试。
void save_team_checkpoint(const std::filesystem::path& path,
                          const std::vector<TeamTask>& tasks,
                          const std::map<std::string, DelegationResult>& finished,
                          const std::set<std::string>& inflight);
std::map<std::string, DelegationResult> load_team_checkpoint(
    const std::filesystem::path& path, const std::vector<TeamTask>& tasks,
    const std::function<TaskValidation(const TeamTask&,const DelegationResult&)>& validator);
} // namespace myagent::detail
