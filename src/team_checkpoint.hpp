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
                          const std::set<std::string>& inflight,
                          const std::map<std::string, std::vector<DelegationResult>>& prior_attempts = {});
struct RestoredTeamCheckpoint {
  std::map<std::string, DelegationResult> finished;
  // 失败且未产生工具副作用的尝试仍然发生了模型调用、消耗了 Token。
  // 需要跨备用 Agent 保存，防止恢复后账本丢失。
  std::map<std::string, std::vector<DelegationResult>> prior_attempts;
};
RestoredTeamCheckpoint load_team_checkpoint(
    const std::filesystem::path& path, const std::vector<TeamTask>& tasks,
    const std::function<TaskValidation(const TeamTask&,const DelegationResult&)>& validator);
} // namespace myagent::detail
