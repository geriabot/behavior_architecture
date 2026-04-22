// Copyright 2026 Rodrigo Perez-Rodriguez
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BEHAVIOR_ARCHITECTURE__MCP_LLM_PLAN_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__MCP_LLM_PLAN_ORCHESTRATOR_HPP_

#include <filesystem>
#include <unordered_map>
#include <string>
#include <vector>

#include "llm_planner_interfaces/srv/start_mission.hpp"

#include "behavior_architecture/llm_plan_orchestrator.hpp"

namespace behavior_architecture
{

class MCPLLMPlanOrchestrator : public LLMPlanOrchestrator
{
public:
  explicit MCPLLMPlanOrchestrator(BT::Blackboard::Ptr blackboard);
  ~MCPLLMPlanOrchestrator() = default;

protected:
  void handle_start_mission(
    const llm_planner_interfaces::srv::StartMission::Request::SharedPtr req,
    llm_planner_interfaces::srv::StartMission::Response::SharedPtr resp) override;

  void request_plan() override;
  void request_replan() override;
  void request_generate_bt(const std::string & objective_yaml) override;
  void request_fix_bt(const std::string & broken_xml, const std::string & error_msg) override;
  std::string collect_failure_reason() override;
  void transition_to(State new_state) override;

private:
  struct FailureEntry
  {
    std::string timestamp;
    std::string reason;
    std::string state;
  };

  // Plan case: one per successful task execution (only if no replanning occurred)
  struct PlanCase
  {
    std::string timestamp;
    std::string goal;
    std::string context;
    std::string plan_yaml;
    int duration_sec;
    std::string status;  // "success" only
  };

  // BT case: one per step (only if BT was not fixed)
  struct BTCase
  {
    std::string timestamp;
    std::string mission_goal;
    std::string step_goal;
    int step_id;
    std::string bt_xml;
    int duration_sec;
    std::string status;  // "success" only
  };

  // Plan failure episodic case.
  struct PlanFailureCase
  {
    std::string timestamp;
    std::string mission_goal;
    std::string failure_cause;
    std::string state;
    int duration_sec;
    std::string status;  // "failure" only
  };

  // BT failure episodic case.
  struct BTFailureCase
  {
    std::string timestamp;
    std::string mission_goal;
    std::string step_goal;
    int step_id;
    std::string failure_cause;
    std::string state;
    int duration_sec;
    std::string status;  // "failure" only
  };

  std::filesystem::path mcp_context_dir_;
  std::filesystem::path mission_snapshot_path_;
  std::filesystem::path failure_history_path_;
  std::filesystem::path plan_failure_case_base_path_;
  std::filesystem::path bt_failure_case_base_path_;
  std::filesystem::path plan_case_base_path_;
  std::filesystem::path bt_case_base_path_;

  std::string mission_name_snapshot_;
  std::string goal_snapshot_;
  std::string context_snapshot_;
  std::string state_snapshot_;
  std::string last_event_;

  std::chrono::steady_clock::time_point execution_start_time_;
  std::vector<PlanFailureCase> plan_failure_case_base_cache_;
  std::vector<BTFailureCase> bt_failure_case_base_cache_;
  std::vector<PlanCase> plan_case_base_cache_;
  std::vector<BTCase> bt_case_base_cache_;
  std::vector<FailureEntry> failure_history_cache_;
  
  bool plan_was_replanned_;  // Track if plan was modified during execution
  std::unordered_map<int, bool> step_had_fix_;  // step_id -> true if FixBT was needed

  std::string now_iso_utc() const;
  std::string json_escape(const std::string & input) const;
  void write_mission_snapshot(bool accepted = true, const std::string & message = "");
  void write_failure_history();
  void write_plan_failure_case(const PlanFailureCase & case_entry);
  void write_bt_failure_case(const BTFailureCase & case_entry);
  void write_plan_case(const PlanCase & case_entry);
  void write_bt_case(const BTCase & case_entry);
  void record_plan_failure_case(const std::string & failure_cause);
  void record_bt_failure_case(const std::string & failure_cause);
  void record_successful_plan(const std::string & plan_yaml);
  void record_successful_bt(int step_id, const std::string & step_goal, const std::string & bt_xml);
  void load_plan_failure_case_base();
  void load_bt_failure_case_base();
  void load_plan_case_base();
  void load_bt_case_base();
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__MCP_LLM_PLAN_ORCHESTRATOR_HPP_
