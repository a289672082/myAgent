#include "developer_tools/developer_tools.hpp"
#include "myagent/agent.hpp"
#include <utility>
namespace developer_tools {
std::size_t RegisterToAgent(myagent::Agent& agent,ToolContext context,ToolOptions options) {
  auto definitions=CreateToolDefinitions(std::move(context),options);
  const auto count=definitions.size();
  for(auto& t:definitions)agent.tool(std::move(t.name),std::move(t.description),
                                    std::move(t.schema),std::move(t.handler));
  return count;
}
} // namespace developer_tools
