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

#include "behavior_architecture/llm_plan_orchestrator.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "ament_index_cpp/get_package_share_directory.hpp"
#include "yaml-cpp/yaml.h"

namespace behavior_architecture
{

// Auto-register with the factory so action_executor handles "llm" uniformly.
static OrchestratorRegistrar<LLMPlanOrchestrator> llm_registrar("llm");

LLMPlanOrchestrator::LLMPlanOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("llm_plan_orchestrator", blackboard)
{
  // Read LLM-specific config from blackboard (written by action_executor before factory call).
  // Fall back to sensible defaults when keys are absent (e.g. standalone use via
  // llm_plan_executor binary that creates an empty blackboard).
  try {
    plugin_libraries_ = blackboard_->get<std::vector<std::string>>("llm_plugin_libraries");
  } catch (...) {
    plugin_libraries_ = {"libsocial_bt_nodes_plugin.so"};
  }
  try {
    control_cycle_rate_ms_ = blackboard_->get<int>("llm_control_period_ms");
  } catch (...) {
    control_cycle_rate_ms_ = 50;
  }
  try {
    bt_timeout_sec_ = blackboard_->get<double>("llm_timeout_sec");
  } catch (...) {
    bt_timeout_sec_ = 30.0;
  }
  try {
    capabilities_yaml_ = blackboard_->get<std::string>("llm_capabilities_yaml");
  } catch (...) {}

  // Build capabilities by concatenating node_descriptions of each bt_nodes_package.
  if (capabilities_yaml_.empty()) {
    std::vector<std::string> bt_nodes_pkgs;
    try {
      bt_nodes_pkgs = blackboard_->get<std::vector<std::string>>("llm_bt_nodes_packages");
    } catch (...) {}
    std::string merged;
    for (const auto & pkg : bt_nodes_pkgs) {
      try {
        const std::string path =
          ament_index_cpp::get_package_share_directory(pkg) +
          "/node_descriptions/" + pkg + ".yaml";
        merged += load_file(path) + "\n";
        RCLCPP_INFO(get_logger(), "Loaded capabilities from package '%s': %s",
          pkg.c_str(), path.c_str());
      } catch (const std::exception & e) {
        RCLCPP_WARN(get_logger(), "Could not load capabilities for package '%s': %s",
          pkg.c_str(), e.what());
      }
    }
    if (!merged.empty()) {
      capabilities_yaml_ = merged;
      RCLCPP_INFO(get_logger(), "Merged capabilities from %zu package(s)", bt_nodes_pkgs.size());
    }
  }

  // Load all BT plugins.
  // (Moved to on_configure so the runner is created first)

  // Service clients and server (safe to create in constructor for lifecycle nodes).
  plan_client_ = create_client<llm_planner_interfaces::srv::PlanTask>("plan_task");
  replan_client_ = create_client<llm_planner_interfaces::srv::ReplanTask>("replan_task");
  generate_bt_client_ = create_client<llm_bt_builder::srv::GenerateBT>("generate_bt");

  start_goal_srv_ = create_service<llm_planner_interfaces::srv::StartGoal>(
    "start_goal",
    [this](
      const llm_planner_interfaces::srv::StartGoal::Request::SharedPtr req,
      llm_planner_interfaces::srv::StartGoal::Response::SharedPtr resp)
    {
      handle_start_goal(req, resp);
    });

  // Status publisher (lifecycle-managed: activated/deactivated in lifecycle callbacks).
  status_pub_ = create_publisher<std_msgs::msg::String>("llm_orchestrator/status", 10);

  RCLCPP_INFO(get_logger(), "LLMPlanOrchestrator ready (period=%dms, timeout=%.1fs)",
    control_cycle_rate_ms_, bt_timeout_sec_);
}

// ─────────────────────────────────────────────────────────────────────────────// Lifecycle callbacks
// ───────────────────────────────────────────────────────────────────────────────

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMPlanOrchestrator::on_configure(const rclcpp_lifecycle::State & previous_state)
{
  RCLCPP_INFO(get_logger(), "LLMPlanOrchestrator configuring");

  // Create the single reusable runner (empty xml_path — always uses set_bt())
  auto runner = std::make_shared<BehaviorRunner>(
    blackboard_, "llm_bt_runner", "", plugin_libraries_, "behavior_architecture",
    control_cycle_rate_ms_);
  register_runner("llm_bt_runner", runner);

  // BaseOrchestrator::on_configure will call runner->trigger_transition(CONFIGURE)
  return BaseOrchestrator::on_configure(previous_state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMPlanOrchestrator::on_activate(const rclcpp_lifecycle::State & previous_state)
{
  RCLCPP_INFO(get_logger(), "LLMPlanOrchestrator activating");
  status_pub_->on_activate();
  // BaseOrchestrator::on_activate creates the timer_ bound to control_cycle().
  return BaseOrchestrator::on_activate(previous_state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMPlanOrchestrator::on_deactivate(const rclcpp_lifecycle::State & previous_state)
{
  RCLCPP_INFO(get_logger(), "LLMPlanOrchestrator deactivating");
  if (tree_loaded_) {
    deactivate_runner("llm_bt_runner");
    tree_loaded_ = false;
  }
  status_pub_->on_deactivate();
  // BaseOrchestrator::on_deactivate cancels the timer.
  return BaseOrchestrator::on_deactivate(previous_state);
}

// ───────────────────────────────────────────────────────────────────────────────// StartGoal service handler
// ─────────────────────────────────────────────────────────────────────────────

void LLMPlanOrchestrator::handle_start_goal(
  const llm_planner_interfaces::srv::StartGoal::Request::SharedPtr req,
  llm_planner_interfaces::srv::StartGoal::Response::SharedPtr resp)
{
  if (state_ != State::IDLE && state_ != State::SUCCESS && state_ != State::FAILED) {
    resp->accepted = false;
    resp->message = "Orchestrator is busy (state=" + state_name(state_) + ")";
    RCLCPP_WARN(get_logger(), "%s", resp->message.c_str());
    return;
  }

  goal_ = req->goal;
  context_ = req->context;
  steps_.clear();
  current_step_ = 0;
  replan_count_ = 0;
  last_failure_reason_.clear();
  step_failure_history_.clear();
  tree_loaded_ = false;

  resp->accepted = true;
  resp->message = "Goal accepted, planning…";
  RCLCPP_INFO(get_logger(), "Goal accepted: '%s'", goal_.c_str());

  request_plan();
}

// ─────────────────────────────────────────────────────────────────────────────
// Control cycle — FSM tick
// ─────────────────────────────────────────────────────────────────────────────

void LLMPlanOrchestrator::control_cycle()
{
  switch (state_) {
    // ── IDLE: nothing to do ────────────────────────────────────────────────
    case State::IDLE:
    case State::SUCCESS:
    case State::FAILED:
      break;

    // ── WAITING_PLAN: poll async plan future ───────────────────────────────
    case State::WAITING_PLAN:
    {
      if (!plan_future_.has_value()) {break;}
      if (plan_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {break;}

      auto result = plan_future_->get();
      plan_future_.reset();

      if (!result->success || result->plan_yaml.empty()) {
        RCLCPP_ERROR(get_logger(), "Planning failed: %s", result->message.c_str());
        publish_status("PLAN_FAILED");
        transition_to(State::FAILED);
        break;
      }

      plan_yaml_ = result->plan_yaml;
      steps_ = parse_plan(plan_yaml_);
      current_step_ = 0;
      RCLCPP_INFO(get_logger(), "Plan received: %zu steps", steps_.size());

      if (steps_.empty()) {
        RCLCPP_ERROR(get_logger(), "Plan has no steps");
        publish_status("PLAN_EMPTY");
        transition_to(State::FAILED);
        break;
      }

      request_generate_bt(steps_[current_step_].objective_yaml);
      break;
    }

    // ── GENERATING_BT: poll async generate_bt future ──────────────────────
    case State::GENERATING_BT:
    {
      if (!gen_bt_future_.has_value()) {break;}
      if (gen_bt_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {break;}

      auto result = gen_bt_future_->get();
      gen_bt_future_.reset();

      if (!result->success || result->bt_xml.empty()) {
        RCLCPP_ERROR(get_logger(), "BT generation failed for step %zu", current_step_);
        last_failure_reason_ = "BT generation failed";
        // Treat as a step failure → replan
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
        break;
      }

      RCLCPP_INFO(get_logger(), "BT XML received for step %zu, loading tree", current_step_);

      try {
        auto it = runners_.find("llm_bt_runner");
        auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
        runner->set_bt(result->bt_xml);
        activate_runner("llm_bt_runner");
        tree_loaded_ = true;
        step_start_time_ = now();
        transition_to(State::EXECUTING_BT);
      } catch (const std::exception & e) {
        RCLCPP_ERROR(get_logger(), "Failed to create BT from XML: %s", e.what());
        last_failure_reason_ = std::string("createTreeFromText exception: ") + e.what();
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
      }
      break;
    }

    // ── EXECUTING_BT: tick current tree ────────────────────────────────────
    case State::EXECUTING_BT:
    {
      if (!tree_loaded_) {break;}

      // Timeout check
      double elapsed = (now() - step_start_time_).seconds();
      if (elapsed > bt_timeout_sec_) {
        RCLCPP_WARN(get_logger(), "Step %zu timed out after %.1fs", current_step_, elapsed);
        deactivate_runner("llm_bt_runner");
        tree_loaded_ = false;
        last_failure_reason_ = "Step timeout";
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
        break;
      }

      // Check runner status via the published topic (already handled by base status_callback)
      if (!check_behavior_finished()) {break;}

      const bool success = (last_status_ == "SUCCESS");
      if (!success) {
        last_failure_reason_ = collect_failure_reason();
      }

      deactivate_runner("llm_bt_runner");
      tree_loaded_ = false;

      if (success) {
        RCLCPP_INFO(get_logger(), "Step %zu succeeded", current_step_);
        publish_status("STEP_" + std::to_string(current_step_) + "_SUCCESS");
        current_step_++;
        step_failure_history_.clear();  // history is per-step

        if (current_step_ >= steps_.size()) {
          RCLCPP_INFO(get_logger(), "All steps completed — goal achieved");
          publish_status("GOAL_SUCCESS");
          transition_to(State::SUCCESS);
        } else {
          request_generate_bt(steps_[current_step_].objective_yaml);
        }
      } else {
        RCLCPP_WARN(get_logger(), "Step %zu FAILED: %s", current_step_, last_failure_reason_.c_str());
        publish_status("STEP_" + std::to_string(current_step_) + "_FAILED");
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
      }
      break;
    }

    // ── WAITING_REPLAN: poll async replan future ───────────────────────────
    case State::WAITING_REPLAN:
    {
      if (!replan_future_.has_value()) {break;}
      if (replan_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {break;}

      auto result = replan_future_->get();
      replan_future_.reset();

      if (!result->success || result->new_plan_yaml.empty()) {
        RCLCPP_ERROR(get_logger(), "Replanning failed: %s", result->message.c_str());
        publish_status("REPLAN_FAILED");
        transition_to(State::FAILED);
        break;
      }

      plan_yaml_ = result->new_plan_yaml;
      steps_ = parse_plan(plan_yaml_);
      current_step_ = 0;
      RCLCPP_INFO(get_logger(), "Replan received: %zu steps (attempt %d)",
        steps_.size(), replan_count_);

      if (steps_.empty()) {
        RCLCPP_ERROR(get_logger(), "Replan has no steps");
        publish_status("REPLAN_EMPTY");
        transition_to(State::FAILED);
        break;
      }

      request_generate_bt(steps_[current_step_].objective_yaml);
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Service request helpers
// ─────────────────────────────────────────────────────────────────────────────

void LLMPlanOrchestrator::request_plan()
{
  if (!plan_client_->wait_for_service(std::chrono::seconds(0))) {
    RCLCPP_WARN(get_logger(), "plan_task service not available yet, will retry");
  }

  auto request = std::make_shared<llm_planner_interfaces::srv::PlanTask::Request>();
  request->goal = goal_;
  request->context = context_;

  RCLCPP_INFO(get_logger(), "Requesting plan for goal: '%s'", goal_.c_str());
  plan_future_ = plan_client_->async_send_request(request);
  transition_to(State::WAITING_PLAN);
}

void LLMPlanOrchestrator::request_replan()
{
  if (!replan_client_->wait_for_service(std::chrono::seconds(0))) {
    RCLCPP_WARN(get_logger(), "replan_task service not available yet, will retry");
  }

  replan_count_++;
  auto request = std::make_shared<llm_planner_interfaces::srv::ReplanTask::Request>();
  request->goal = goal_;
  request->plan_yaml = plan_yaml_;
  request->failed_step = static_cast<int32_t>(current_step_);
  request->failure_reason = last_failure_reason_;
  step_failure_history_.push_back(last_failure_reason_);  // record before sending
  request->previous_failures = step_failure_history_;

  RCLCPP_INFO(get_logger(), "Requesting replan (attempt %d) for step %zu: %s",
    replan_count_, current_step_, last_failure_reason_.c_str());
  replan_future_ = replan_client_->async_send_request(request);
  transition_to(State::WAITING_REPLAN);
}

void LLMPlanOrchestrator::request_generate_bt(const std::string & objective_yaml)
{
  if (!generate_bt_client_->wait_for_service(std::chrono::seconds(0))) {
    RCLCPP_WARN(get_logger(), "generate_bt service not available yet");
  }

  // capabilities_yaml_ now holds the merged YAML content directly (not a path)
  std::string bt_nodes_yaml;
  if (!capabilities_yaml_.empty()) {
    // If it looks like a file path (no newlines, ends with .yaml), load it;
    // otherwise treat it as inline content already loaded.
    if (capabilities_yaml_.find('\n') == std::string::npos &&
        capabilities_yaml_.size() > 5 &&
        capabilities_yaml_.substr(capabilities_yaml_.size() - 5) == ".yaml")
    {
      bt_nodes_yaml = load_file(capabilities_yaml_);
    } else {
      bt_nodes_yaml = capabilities_yaml_;
    }
  }

  auto request = std::make_shared<llm_bt_builder::srv::GenerateBT::Request>();
  request->objective = objective_yaml;
  request->bt_nodes_yaml = bt_nodes_yaml;

  // Log only the first line to keep the log readable
  const std::string first_line = objective_yaml.substr(0, objective_yaml.find('\n'));
  RCLCPP_INFO(get_logger(), "Requesting BT for step %zu: '%s'", current_step_, first_line.c_str());
  gen_bt_future_ = generate_bt_client_->async_send_request(request);
  transition_to(State::GENERATING_BT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Plan YAML parser
// ─────────────────────────────────────────────────────────────────────────────

std::vector<LLMPlanOrchestrator::Step> LLMPlanOrchestrator::parse_plan(
  const std::string & yaml_str)
{
  std::vector<Step> steps;
  try {
    YAML::Node doc = YAML::Load(yaml_str);
    if (!doc["steps"]) {
      RCLCPP_ERROR(get_logger(), "Plan YAML has no 'steps' key");
      return steps;
    }
    for (const auto & s : doc["steps"]) {
      Step step;
      step.id = s["step_id"] ? s["step_id"].as<int>() : static_cast<int>(steps.size());
      step.description = s["description"] ? s["description"].as<std::string>() : "";
      if (s["objective"]) {
        // Serialise the objective sub-node with its key so the consumer
        // receives the same format as the hand-authored .yaml objective files.
        YAML::Node wrapper;
        wrapper["objective"] = s["objective"];
        YAML::Emitter emitter;
        emitter << wrapper;
        step.objective_yaml = emitter.c_str();
      } else if (s["bt_prompt"]) {
        // Backward-compat: plain string prompt from older plan format
        step.objective_yaml = s["bt_prompt"].as<std::string>();
      }
      if (!step.objective_yaml.empty()) {
        steps.push_back(step);
      } else {
        RCLCPP_WARN(get_logger(), "Skipping step %d: no objective or bt_prompt", step.id);
      }
    }
  } catch (const YAML::Exception & e) {
    RCLCPP_ERROR(get_logger(), "YAML parse error: %s", e.what());
  }
  return steps;
}

// ─────────────────────────────────────────────────────────────────────────────
// BT failure diagnosis
// ─────────────────────────────────────────────────────────────────────────────

std::string LLMPlanOrchestrator::collect_failure_reason()
{
  // Read the "bt_last_failure" key written by the failing BT node via bt_failure()
  std::string bb_reason;
  try {
    bb_reason = blackboard_->get<std::string>("bt_last_failure");
  } catch (...) {}
  // Clear so it doesn't bleed into the next step
  blackboard_->set("bt_last_failure", std::string{});

  if (bb_reason.empty()) {
    bb_reason = "BT returned FAILURE (no details written to blackboard)";
  }

  // Prefix with the step description for full context
  if (current_step_ < steps_.size() && !steps_[current_step_].description.empty()) {
    return "[" + steps_[current_step_].description + "] " + bb_reason;
  }
  return bb_reason;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

std::string LLMPlanOrchestrator::load_file(const std::string & path)
{
  if (path.empty()) {return {};}
  std::ifstream f(path);
  if (!f.is_open()) {
    RCLCPP_WARN(get_logger(), "Cannot open file: %s", path.c_str());
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void LLMPlanOrchestrator::publish_status(const std::string & msg)
{
  std_msgs::msg::String out;
  out.data = msg;
  status_pub_->publish(out);
}

void LLMPlanOrchestrator::transition_to(State new_state)
{
  RCLCPP_INFO(get_logger(), "State: %s → %s",
    state_name(state_).c_str(), state_name(new_state).c_str());
  state_ = new_state;
}

std::string LLMPlanOrchestrator::state_name(State s) const
{
  switch (s) {
    case State::IDLE: return "IDLE";
    case State::WAITING_PLAN: return "WAITING_PLAN";
    case State::GENERATING_BT: return "GENERATING_BT";
    case State::EXECUTING_BT: return "EXECUTING_BT";
    case State::WAITING_REPLAN: return "WAITING_REPLAN";
    case State::SUCCESS: return "SUCCESS";
    case State::FAILED: return "FAILED";
    default: return "UNKNOWN";
  }
}

}  // namespace behavior_architecture
