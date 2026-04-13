// Copyright 2025 Rodrigo Pérez-Rodríguez
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

#ifndef BEHAVIOR_ARCHITECTURE__LLM_PLAN_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__LLM_PLAN_ORCHESTRATOR_HPP_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "behaviortree_cpp/bt_factory.h"
#include "std_msgs/msg/string.hpp"

#include "llm_planner_interfaces/srv/plan_task.hpp"
#include "llm_planner_interfaces/srv/replan_task.hpp"
#include "llm_planner_interfaces/srv/start_mission.hpp"
#include "llm_bt_builder/srv/generate_bt.hpp"
#include "llm_bt_builder/srv/fix_bt.hpp"

#include "behavior_architecture/base_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace behavior_architecture
{

class LLMPlanOrchestrator : public BaseOrchestrator
{
public:
  explicit LLMPlanOrchestrator(BT::Blackboard::Ptr blackboard);
  ~LLMPlanOrchestrator() = default;

  /// BaseOrchestrator requires this pure virtual; LLM uses its own internal FSM.
  void go_to_state(int /*state*/) override {}

private:
  // Lifecycle overrides
  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_configure(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_activate(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_deactivate(const rclcpp_lifecycle::State & previous_state) override;
  // ── FSM ──────────────────────────────────────────────────────────────────
  enum class State
  {
    IDLE,
    WAITING_PLAN,
    GENERATING_BT,
    WAITING_FIX_BT,
    EXECUTING_BT,
    WAITING_REPLAN,
    SUCCESS,
    FAILED
  };

  State state_{State::IDLE};

  // ── Goal context ──────────────────────────────────────────────────────────
  std::string goal_;
  std::string context_;
  std::vector<std::string> skills_;  // capabilities available to the robot
  std::vector<std::string> preconditions_;
  std::vector<std::string> postconditions_;
  std::string mission_name_;         // optional identifier for saved plan filenames

  // ── Plan data ─────────────────────────────────────────────────────────────
  struct Step
  {
    int id;
    std::string description;
    std::string objective_yaml;  // serialised objective: block passed to llm_bt_builder
    std::vector<std::string> outputs;  // declared output blackboard variable names
  };

  std::string plan_yaml_;
  std::vector<Step> steps_;
  std::size_t current_step_{0};
  int replan_count_{0};
  static constexpr int MAX_REPLAN_ATTEMPTS = 3;
  int bt_regeneration_count_{0};
  static constexpr int MAX_BT_REGENERATIONS = 3;
  std::string last_failure_reason_;
  std::vector<std::string> step_failure_history_;  // all failure reasons tried for current step
  std::vector<std::string> initial_blackboard_keys_; // variables present before execution

  // ── BehaviorTree internals ────────────────────────────────────────────────
  // Plugins are loaded into the runner; no separate factory needed here.
  bool tree_loaded_{false};

  // ── ROS 2 interfaces ──────────────────────────────────────────────────────
  rclcpp::Service<llm_planner_interfaces::srv::StartMission>::SharedPtr start_mission_srv_;

  rclcpp::Client<llm_planner_interfaces::srv::PlanTask>::SharedPtr plan_client_;
  rclcpp::Client<llm_planner_interfaces::srv::ReplanTask>::SharedPtr replan_client_;
  rclcpp::Client<llm_bt_builder::srv::GenerateBT>::SharedPtr generate_bt_client_;
  rclcpp::Client<llm_bt_builder::srv::FixBT>::SharedPtr fix_bt_client_;

  rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::String>::SharedPtr status_pub_;

  // ── Pending async futures ─────────────────────────────────────────────────
  using PlanFuture = rclcpp::Client<llm_planner_interfaces::srv::PlanTask>::SharedFuture;
  using ReplanFuture = rclcpp::Client<llm_planner_interfaces::srv::ReplanTask>::SharedFuture;
  using GenBTFuture = rclcpp::Client<llm_bt_builder::srv::GenerateBT>::SharedFuture;
  using FixBTFuture = rclcpp::Client<llm_bt_builder::srv::FixBT>::SharedFuture;

  std::optional<PlanFuture> plan_future_;
  std::optional<ReplanFuture> replan_future_;
  std::optional<GenBTFuture> gen_bt_future_;
  std::optional<FixBTFuture> fix_bt_future_;

  // ── Parameters ────────────────────────────────────────────────────────────
  std::vector<std::string> plugin_libraries_;
  std::string capabilities_yaml_;
  double bt_timeout_sec_;
  rclcpp::Time step_start_time_;
  std::string last_bt_xml_;  // The last XML correctly accepted to pass into FixBT

  // ── Execution saving ──────────────────────────────────────────────────────
  bool save_exec_{false};
  std::string exec_base_dir_{"exec"};
  std::filesystem::path exec_run_dir_;
  std::filesystem::path current_plan_dir_;

  // ── Callbacks & helpers ───────────────────────────────────────────────────
  void control_cycle();

  void handle_start_mission(
    const llm_planner_interfaces::srv::StartMission::Request::SharedPtr req,
    llm_planner_interfaces::srv::StartMission::Response::SharedPtr resp);

  void request_plan();
  void request_replan();
  void request_generate_bt(const std::string & objective_yaml);
  void request_fix_bt(const std::string & broken_xml, const std::string & error_msg);

  bool is_local_error(const std::string & reason);

  std::vector<Step> parse_plan(const std::string & yaml_str);
  std::string load_file(const std::string & path);
  void save_bt_xml(const std::string & bt_xml, std::size_t step);

  /// Collect a descriptive failure reason from the BT tree and blackboard.
  /// Must be called BEFORE haltTree() so node statuses are still valid.
  std::string collect_failure_reason();

  void publish_status(const std::string & msg);
  void transition_to(State new_state);
  std::string state_name(State s) const;
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__LLM_PLAN_ORCHESTRATOR_HPP_
