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
#include "behavior_architecture/yaml_utils.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

  try {
    skills_ = blackboard_->get<std::vector<std::string>>("llm_skills");
  } catch (...) {}

  try {
    save_exec_ = blackboard_->get<bool>("llm_save_exec");
  } catch (...) {}
  try {
    exec_base_dir_ = blackboard_->get<std::string>("llm_exec_dir");
  } catch (...) {}
  try {
    mission_name_ = blackboard_->get<std::string>("llm_mission_name");
  } catch (...) {}

  try {
    restart_after_forced_ = blackboard_->get<bool>("llm_restart_after_forced");
  } catch (...) {
    restart_after_forced_ = true;  // default: keep original behavior
  }
  try {
    replan_active_ = blackboard_->get<bool>("llm_replan_active");
  } catch (...) {
    replan_active_ = true;  // default: keep original behavior
  }

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

  // Service clients and server (safe to create in constructor for lifecycle nodes).
  plan_client_ = create_client<llm_planner_interfaces::srv::PlanTask>("plan_task");
  replan_client_ = create_client<llm_planner_interfaces::srv::ReplanTask>("replan_task");
  generate_bt_client_ = create_client<llm_bt_builder::srv::GenerateBT>("generate_bt");
  fix_bt_client_ = create_client<llm_bt_builder::srv::FixBT>("fix_bt");

  start_mission_srv_ = create_service<llm_planner_interfaces::srv::StartMission>(
    "start_mission",
    [this](
      const llm_planner_interfaces::srv::StartMission::Request::SharedPtr req,
      llm_planner_interfaces::srv::StartMission::Response::SharedPtr resp)
    {
      handle_start_mission(req, resp);
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
  auto ret = BaseOrchestrator::on_activate(previous_state);
  RCLCPP_INFO(get_logger(), "⏳ Waiting for a new mission — call /start_mission.");
  return ret;
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

// ─────────────────────────────────────────────────────────────────────────────
// StartMission service handler
// ─────────────────────────────────────────────────────────────────────────────

void LLMPlanOrchestrator::handle_start_mission(
  const llm_planner_interfaces::srv::StartMission::Request::SharedPtr req,
  llm_planner_interfaces::srv::StartMission::Response::SharedPtr resp)
{
  if (state_ != State::IDLE && state_ != State::SUCCESS && state_ != State::FAILED) {
    resp->accepted = false;
    resp->message = "Orchestrator is busy (state=" + state_name(state_) + ")";
    RCLCPP_WARN(get_logger(), "%s", resp->message.c_str());
    return;
  }

  goal_ = req->goal;
  context_ = req->context;
  preconditions_ = req->preconditions;
  postconditions_ = req->postconditions;
  useful_info_ = req->useful_info;
  if (!req->mission_name.empty()) {
    mission_name_ = req->mission_name;
  }
  if (!req->skills.empty()) {
    skills_ = std::vector<std::string>(req->skills.begin(), req->skills.end());
  }
  steps_.clear();
  current_step_ = 0;
  replan_count_ = 0;
  bt_regeneration_count_ = 0;
  last_failure_reason_.clear();
  last_failure_code_.clear();
  step_failure_history_.clear();
  tree_loaded_ = false;

  if (save_exec_) {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    exec_run_dir_ = std::filesystem::path(exec_base_dir_) / mission_name_ / oss.str();
    std::filesystem::create_directories(exec_run_dir_);
    RCLCPP_INFO(get_logger(), "Exec save dir: %s", exec_run_dir_.c_str());
  }

  // Reset metrics for this run
  // Reset métricas solo si se van a guardar
  if (save_exec_) {
    run_metrics_ = RunMetrics{};
    current_replan_id_ = 0;
    current_fix_count_ = 0;
  }

  resp->accepted = true;
  resp->message = "Goal accepted, planning…";
  RCLCPP_INFO(get_logger(), "Goal accepted: '%s'", goal_.c_str());

  initial_blackboard_keys_.clear();
  auto keys = blackboard_->getKeys();
  for (const auto & k : keys) {
    initial_blackboard_keys_.emplace_back(k.data(), k.size());
  }

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

      // Initialize per-step metrics for the initial plan.
      if (save_exec_) {
        run_metrics_.steps.clear();
        for (std::size_t i = 0; i < steps_.size(); ++i) {
          StepMetrics sm;
          sm.replan_id = current_replan_id_;
          sm.step_id = static_cast<int>(i);
          sm.bt_status = "PENDING";
          run_metrics_.steps.push_back(sm);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - plan_start_time_).count();
        run_metrics_.steps.front().plan_time_ms = elapsed;
        run_metrics_.plan_total_time_ms = elapsed;
      }

      if (save_exec_) {
        current_plan_dir_ = exec_run_dir_ / "plan";
        std::filesystem::create_directories(current_plan_dir_);
        const auto plan_path = current_plan_dir_ / "plan.yaml";
        std::ofstream pf(plan_path);
        if (pf.is_open()) {
          if (!preconditions_.empty()) {
            pf << "preconditions:\n";
            for (const auto& cond : preconditions_) {
              pf << "  - " << cond << "\n";
            }
          }
          if (!postconditions_.empty()) {
            pf << "postconditions:\n";
            for (const auto& cond : postconditions_) {
              pf << "  - " << cond << "\n";
            }
          }
          pf << plan_yaml_;
          RCLCPP_INFO(get_logger(), "Saved plan YAML: %s", plan_path.c_str());
        } else {
          RCLCPP_WARN(get_logger(), "Could not save plan YAML to: %s", plan_path.c_str());
        }
      }

      advance_to_step(current_step_);
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

      if (save_exec_) {
        save_bt_xml(result->bt_xml, current_step_);
      }

      try {
        last_bt_xml_ = result->bt_xml;
        steps_[current_step_].cached_bt_xml = result->bt_xml;  // cache for FORCED_FAILURE restart
        auto it = runners_.find("llm_bt_runner");
        auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
        runner->set_bt(result->bt_xml);

        // Reset status tracking so check_behavior_finished() works for every step,
        // regardless of whether the previous step ended with the same status string.
        last_status_ = "";
        status_received_ = "";
        activate_runner("llm_bt_runner");
        tree_loaded_ = true;
        step_start_time_ = now();
        
        // Record BT generation time for this step just before executing the BT
        if (save_exec_) {
          auto bt_gen_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bt_gen_start_time_).count();
          for (auto& sm : run_metrics_.steps) {
            if (sm.replan_id == current_replan_id_ && sm.step_id == static_cast<int>(current_step_)) {
              sm.bt_gen_time_ms = bt_gen_elapsed;
              run_metrics_.bt_gen_total_time_ms += bt_gen_elapsed;
              break;
            }
          }
        }
        transition_to(State::EXECUTING_BT);
      } catch (const std::exception & e) {
        RCLCPP_ERROR(get_logger(), "Failed to create BT from XML: %s", e.what());
        last_failure_reason_ = std::string("createTreeFromText exception: ") + e.what();
        
        if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < MAX_BT_REGENERATIONS) {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          bt_regeneration_count_ = 0;
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
        if (save_exec_) {
          auto bt_exec_elapsed = (now() - step_start_time_).to_chrono<std::chrono::milliseconds>().count();
          for (auto & sm : run_metrics_.steps) {
            if (sm.replan_id == current_replan_id_ && sm.step_id == static_cast<int>(current_step_)) {
              sm.bt_exec_time_ms = bt_exec_elapsed;
              sm.bt_status = "FAILED_TIMEOUT";
              break;
            }
          }
        }
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          bt_regeneration_count_ = 0;
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
        break;
      }

      // Check runner status via the published topic (already handled by base status_callback)
      if (!check_behavior_finished()) {break;}

      // Always read and clear BB failure vars so they cannot bleed into future
      // steps, and so that FORCED_FAILURE is detected even when the BT returns
      // SUCCESS (e.g. ForcePlanFail is inside a Fallback branch that succeeds).
      last_failure_reason_ = collect_failure_reason();
      const bool forced_plan_restart = (last_failure_code_ == "FORCED_FAILURE");

      // A FORCED_FAILURE overrides a BT SUCCESS: treat the step as not-success.
      const bool success = (last_status_ == "SUCCESS") && !forced_plan_restart;
      bool accepted_failure = false;
      if (!forced_plan_restart && !success) {
        accepted_failure =
          (last_failure_reason_.find("NO_REAL_FAILURE") != std::string::npos);
      }

      if (save_exec_) {
        auto bt_exec_elapsed = (now() - step_start_time_).to_chrono<std::chrono::milliseconds>().count();
        for (auto & sm : run_metrics_.steps) {
          if (sm.replan_id == current_replan_id_ && sm.step_id == static_cast<int>(current_step_)) {
            sm.bt_exec_time_ms = bt_exec_elapsed;
            sm.bt_status = success ? "SUCCESS" : (forced_plan_restart ? "FORCED_RESTART" : (accepted_failure ? "FAILED_ACCEPTED" : "FAILED"));
            break;
          }
        }
      }

      deactivate_runner("llm_bt_runner");
      tree_loaded_ = false;

      if (forced_plan_restart) {
        // A BT node wrote FORCED_FAILURE — read fail_message from blackboard
        std::string fail_message;
        try {
          fail_message = blackboard_->get<std::string>("fail_message");
          blackboard_->set("fail_message", std::string{});  // clear after reading
        } catch (...) {}
        
        // Append fail_message to the failure reason
        if (!fail_message.empty()) {
          last_failure_reason_ += " [FORCED_FAILURE: " + fail_message + "]";
        }
        
        RCLCPP_WARN(
          get_logger(),
          "Step %zu emitted FORCED_FAILURE (BT returned %s) — %s: %s",
          current_step_, last_status_.c_str(),
          restart_after_forced_ ? "restarting plan from step 0" : "requesting replan",
          last_failure_reason_.c_str());
        
        steps_[current_step_].cached_bt_xml.clear();  // prevent loop: force regeneration
        publish_status("FORCED_PLAN_RESTART");
        bt_regeneration_count_ = 0;
        
        if (restart_after_forced_) {
          // Original behavior: restart from step 0
          current_step_ = 0;
          step_failure_history_.clear();
          advance_to_step(0);
        } else {
          // Alternative: request a full replan
          if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
            request_replan();
          } else {
            publish_status("FAILED_NO_MORE_REPLANS");
            transition_to(State::FAILED);
          }
        }
      } else if (success || accepted_failure) {
        bt_regeneration_count_ = 0;
        if (accepted_failure) {
          RCLCPP_WARN(
            get_logger(), "Step %zu finished with FAILURE and NO_REAL_FAILURE cause, advancing: %s",
            current_step_, last_failure_reason_.c_str());
          publish_status("STEP_" + std::to_string(current_step_) + "_FAILED_ACCEPTED");
        } else {
          RCLCPP_INFO(get_logger(), "Step %zu succeeded", current_step_);
          publish_status("STEP_" + std::to_string(current_step_) + "_SUCCESS");
        }
        current_step_++;
        step_failure_history_.clear();  // history is per-step

        if (current_step_ >= steps_.size()) {
          RCLCPP_INFO(get_logger(), "All steps completed — goal achieved");
          publish_status("GOAL_SUCCESS");
          transition_to(State::SUCCESS);
        } else {
          advance_to_step(current_step_);
        }
      } else {
        RCLCPP_WARN(get_logger(), "Step %zu FAILED: %s", current_step_, last_failure_reason_.c_str());
        publish_status("STEP_" + std::to_string(current_step_) + "_FAILED");

        if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < MAX_BT_REGENERATIONS && !last_bt_xml_.empty()) {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          bt_regeneration_count_ = 0;
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
      }
      break;
    }

    // ── WAITING_FIX_BT: poll async fix_bt future ───────────────────────────
    case State::WAITING_FIX_BT:
    {
      if (!fix_bt_future_.has_value()) {break;}
      if (fix_bt_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {break;}

      auto result = fix_bt_future_->get();
      fix_bt_future_.reset();

      if (!result->success || result->bt_xml.empty()) {
        RCLCPP_ERROR(get_logger(), "Fix BT failed for step %zu. Will trigger mission replan instead.", current_step_);
        if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          bt_regeneration_count_ = 0;
          request_replan();
        } else {
          publish_status("FAILED_NO_MORE_REPLANS");
          transition_to(State::FAILED);
        }
        break;
      }

      RCLCPP_INFO(get_logger(), "Fixed BT XML received for step %zu, loading tree", current_step_);

      if (save_exec_) {
        save_bt_xml(result->bt_xml, current_step_);
      }

      try {
        last_bt_xml_ = result->bt_xml;
        steps_[current_step_].cached_bt_xml = result->bt_xml;  // cache for FORCED_FAILURE restart
        auto it = runners_.find("llm_bt_runner");
        auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
        runner->set_bt(result->bt_xml);
        last_status_ = "";
        status_received_ = "";
        activate_runner("llm_bt_runner");
        tree_loaded_ = true;
        step_start_time_ = now();
        transition_to(State::EXECUTING_BT);
      } catch (const std::exception & e) {
        RCLCPP_ERROR(get_logger(), "Failed to create fixed BT from XML: %s", e.what());
        last_failure_reason_ = std::string("createTreeFromText exception: ") + e.what();
        if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < MAX_BT_REGENERATIONS) {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else if (replan_count_ < MAX_REPLAN_ATTEMPTS) {
          bt_regeneration_count_ = 0;
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
      if (save_exec_) {
        current_replan_id_ = replan_count_;
      }
      RCLCPP_INFO(get_logger(), "Replan received: %zu steps (attempt %d)",
        steps_.size(), replan_count_);

      if (steps_.empty()) {
        RCLCPP_ERROR(get_logger(), "Replan has no steps");
        publish_status("REPLAN_EMPTY");
        transition_to(State::FAILED);
        break;
      }

      if (save_exec_) {
        for (std::size_t i = 0; i < steps_.size(); ++i) {
          StepMetrics sm;
          sm.replan_id = current_replan_id_;
          sm.step_id = static_cast<int>(i);
          sm.bt_status = "PENDING";
          run_metrics_.steps.push_back(sm);
        }
      }

      if (save_exec_) {
        current_plan_dir_ = exec_run_dir_ / ("replan_" + std::to_string(replan_count_));
        std::filesystem::create_directories(current_plan_dir_);
        const auto plan_path = current_plan_dir_ / "plan.yaml";
        std::ofstream pf(plan_path);
        if (pf.is_open()) {
          if (!preconditions_.empty()) {
            pf << "preconditions:\n";
            for (const auto& cond : preconditions_) {
              pf << "  - " << cond << "\n";
            }
          }
          if (!postconditions_.empty()) {
            pf << "postconditions:\n";
            for (const auto& cond : postconditions_) {
              pf << "  - " << cond << "\n";
            }
          }
          pf << plan_yaml_;
          RCLCPP_INFO(get_logger(), "Saved replan YAML: %s", plan_path.c_str());
        } else {
          RCLCPP_WARN(get_logger(), "Could not save replan YAML to: %s", plan_path.c_str());
        }
      }

      // Record replan time metrics (only for the first step of each replan)
      if (save_exec_) {
        auto elapsed_replan = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - replan_start_time_).count();
        for (auto & sm : run_metrics_.steps) {
          if (sm.replan_id == current_replan_id_ && sm.step_id == 0) {
            sm.replan_time_ms = elapsed_replan;
            run_metrics_.replan_total_time_ms += elapsed_replan;
            break;
          }
        }
      }

      advance_to_step(current_step_);
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
  request->skills = skills_;
  request->preconditions = preconditions_;
  request->postconditions = postconditions_;
  request->useful_info = useful_info_;
  request->mission_name = mission_name_;

  RCLCPP_INFO(get_logger(), "Requesting plan for goal: '%s'", goal_.c_str());
  RCLCPP_INFO(get_logger(), "  → context: '%s'", context_.c_str());
  plan_start_time_ = std::chrono::steady_clock::now();
  plan_future_ = plan_client_->async_send_request(request);
  transition_to(State::WAITING_PLAN);
}

void LLMPlanOrchestrator::request_replan()
{
  if (!replan_active_) {
    RCLCPP_WARN(
      get_logger(),
      "Replanning disabled (llm_replan_active=false). Failing mission at step %zu. Cause: %s",
      current_step_, last_failure_reason_.c_str());
    publish_status("REPLAN_DISABLED");
    transition_to(State::FAILED);
    return;
  }

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
  request->skills = skills_;
  request->preconditions = preconditions_;
  request->postconditions = postconditions_;
  request->useful_info = useful_info_;
  request->mission_name = mission_name_;

  RCLCPP_INFO(get_logger(), "Requesting replan (attempt %d) for step %zu: %s",
    replan_count_, current_step_, last_failure_reason_.c_str());
  // Take replan_start_time_ right before sending the replan request (for metrics)
  replan_start_time_ = std::chrono::steady_clock::now();
  replan_future_ = replan_client_->async_send_request(request);
  transition_to(State::WAITING_REPLAN);
}

void LLMPlanOrchestrator::advance_to_step(std::size_t idx)
{
  const auto & step = steps_[idx];
  if (!step.cached_bt_xml.empty()) {
    RCLCPP_INFO(get_logger(), "Reusing cached BT for step %zu", idx);
    last_bt_xml_ = step.cached_bt_xml;
    auto it = runners_.find("llm_bt_runner");
    auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
    try {
      runner->set_bt(step.cached_bt_xml);
      last_status_ = "";
      status_received_ = "";
      activate_runner("llm_bt_runner");
      tree_loaded_ = true;
      step_start_time_ = now();
      transition_to(State::EXECUTING_BT);
      return;
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        get_logger(),
        "Failed to reload cached BT for step %zu: %s — regenerating", idx, e.what());
      steps_[idx].cached_bt_xml.clear();
    }
  }
  request_generate_bt(step.objective_yaml);
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

  // Pass all actual blackboard vars from the memory
  std::string enriched_objective = objective_yaml;
  if (current_step_ < steps_.size() && !steps_[current_step_].skills_used.empty()) {
    enriched_objective = append_yaml_string_list(
      enriched_objective, "skills_used", steps_[current_step_].skills_used);
    RCLCPP_INFO(get_logger(), "Injecting %zu step skills_used for step %zu",
      steps_[current_step_].skills_used.size(), current_step_);
  }
  enriched_objective = behavior_architecture::append_yaml_multiline_block(enriched_objective, "useful_info", useful_info_);

  auto keys = blackboard_->getKeys();
  if (!keys.empty()) {
    std::string vars_block = "\navailable_blackboard_vars:";
    std::string typed_vars_block = "\navailable_blackboard_vars_typed:";
    int injected_count = 0;
    int injected_typed_count = 0;
    for (const auto & k : keys) {
      std::string key_str{k.data(), k.size()};
      if (key_str.empty() || key_str[0] == '_' || key_str == "bt_last_failure") continue;
      if (std::find(initial_blackboard_keys_.begin(), initial_blackboard_keys_.end(), key_str) != initial_blackboard_keys_.end()) continue;

      vars_block += "\n  - " + key_str;
      injected_count++;

      std::string type_name = "unknown";

      auto entry = blackboard_->getEntry(key_str);
      if (entry && entry->info.isStronglyTyped()) {
        const auto & resolved_type = entry->info.typeName();
        if (!resolved_type.empty()) {
          type_name = resolved_type;
          typed_vars_block += "\n  - key: " + key_str + "\n    type: " + type_name;
          injected_typed_count++;
        }
      }

      RCLCPP_INFO(
        get_logger(),
        "  -> BB var injected for GenerateBT: key='%s', type='%s'",
        key_str.c_str(), type_name.c_str());
    }
    if (injected_count > 0) {
      enriched_objective += vars_block;
      RCLCPP_INFO(get_logger(), "Injecting %d available blackboard vars for step %zu",
        injected_count, current_step_);
    }
    if (injected_typed_count > 0) {
      enriched_objective += typed_vars_block;
      RCLCPP_INFO(get_logger(), "Injecting %d typed blackboard vars for step %zu",
        injected_typed_count, current_step_);
    }
  }

  request->objective = enriched_objective;
  request->bt_nodes_yaml = bt_nodes_yaml;

  // Log step name and port mappings
  RCLCPP_INFO(
    get_logger(), "Requesting BT for step %zu/%zu: '%s'",
    current_step_, steps_.size() - 1, steps_[current_step_].description.c_str());

  // Log expected outputs (ports this step will write to the blackboard)
  const auto & step_outputs = steps_[current_step_].outputs;
  if (!step_outputs.empty()) {
    std::string out_str;
    for (const auto & v : step_outputs) { out_str += " " + v; }
    RCLCPP_INFO(get_logger(), "  → outputs (will write):%s", out_str.c_str());
  }
  // Take BT generation start time right before sending the request (for metrics)
  bt_gen_start_time_ = std::chrono::steady_clock::now();
  gen_bt_future_ = generate_bt_client_->async_send_request(request);
  transition_to(State::GENERATING_BT);
}

void LLMPlanOrchestrator::request_fix_bt(const std::string & broken_xml, const std::string & error_msg)
{
  if (!fix_bt_client_->wait_for_service(std::chrono::seconds(0))) {
    RCLCPP_WARN(get_logger(), "fix_bt service not available yet");
  }

  bt_regeneration_count_++;

  // Incrementar fix_count en el StepMetrics correspondiente solo si se guardan métricas
  if (save_exec_) {
    for (auto& sm : run_metrics_.steps) {
      if (sm.replan_id == current_replan_id_ && sm.step_id == static_cast<int>(current_step_)) {
        sm.fix_count++;
        break;
      }
    }
  }
  auto request = std::make_shared<llm_bt_builder::srv::FixBT::Request>();

  std::string bt_nodes_yaml;
  if (!capabilities_yaml_.empty()) {
    if (capabilities_yaml_.find('\n') == std::string::npos &&
        capabilities_yaml_.size() > 5 &&
        capabilities_yaml_.substr(capabilities_yaml_.size() - 5) == ".yaml")
    {
      bt_nodes_yaml = load_file(capabilities_yaml_);
    } else {
      bt_nodes_yaml = capabilities_yaml_;
    }
  }

  std::string enriched_objective = steps_[current_step_].objective_yaml;
  if (current_step_ < steps_.size() && !steps_[current_step_].skills_used.empty()) {
    enriched_objective = append_yaml_string_list(
      enriched_objective, "skills_used", steps_[current_step_].skills_used);
    RCLCPP_INFO(get_logger(), "Injecting %zu step skills_used for step %zu (FixBT)",
      steps_[current_step_].skills_used.size(), current_step_);
  }
  enriched_objective = behavior_architecture::append_yaml_multiline_block(enriched_objective, "useful_info", useful_info_);
  auto keys = blackboard_->getKeys();
  if (!keys.empty()) {
    std::string vars_block = "\navailable_blackboard_vars:";
    std::string typed_vars_block = "\navailable_blackboard_vars_typed:";
    int injected_count = 0;
    int injected_typed_count = 0;
    for (const auto & k : keys) {
      std::string key_str{k.data(), k.size()};
      if (key_str.empty() || key_str[0] == '_' || key_str == "bt_last_failure") continue;
      if (std::find(initial_blackboard_keys_.begin(), initial_blackboard_keys_.end(), key_str) != initial_blackboard_keys_.end()) continue;

      vars_block += "\n  - " + key_str;
      injected_count++;

      std::string type_name = "unknown";

      auto entry = blackboard_->getEntry(key_str);
      if (entry && entry->info.isStronglyTyped()) {
        const auto & resolved_type = entry->info.typeName();
        if (!resolved_type.empty()) {
          type_name = resolved_type;
          typed_vars_block += "\n  - key: " + key_str + "\n    type: " + type_name;
          injected_typed_count++;
        }
      }

      RCLCPP_INFO(
        get_logger(),
        "  -> BB var injected for FixBT: key='%s', type='%s'",
        key_str.c_str(), type_name.c_str());
    }
    if (injected_count > 0) {
      enriched_objective += vars_block;
      RCLCPP_INFO(get_logger(), "Injecting %d available blackboard vars for step %zu (FixBT)",
        injected_count, current_step_);
    }
    if (injected_typed_count > 0) {
      enriched_objective += typed_vars_block;
      RCLCPP_INFO(get_logger(), "Injecting %d typed blackboard vars for step %zu (FixBT)",
        injected_typed_count, current_step_);
    }
  }

  request->objective = enriched_objective;
  request->broken_bt_xml = broken_xml;
  request->error_message = error_msg;
  request->bt_nodes_yaml = bt_nodes_yaml;

  RCLCPP_WARN(get_logger(), "Requesting FixBT for step %zu (attempt %d). Error: %s",
              current_step_, bt_regeneration_count_, error_msg.c_str());

  fix_bt_future_ = fix_bt_client_->async_send_request(request);
  transition_to(State::WAITING_FIX_BT);
}

bool LLMPlanOrchestrator::is_local_error(const std::string & reason)
{
  if (last_failure_code_ == "bt_config_error") {
    return true;
  }

  if (reason.empty() || reason == "BT returned FAILURE (no details written to blackboard)") {
    return false;
  }

  // Fallback only for orchestrator-side XML / loading errors that do not come
  // from structured BT node diagnostics.
  const std::vector<std::string> local_keywords = {
    "createTreeFromText exception",
    "missing port",
    "syntax",
    "parse",
    "XML",
    "Node configuration"
  };

  for (const auto& kw : local_keywords) {
    if (reason.find(kw) != std::string::npos) {
      return true;
    }
  }
  return false;
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
      if (s["skills_used"]) {
        for (const auto & skill : s["skills_used"]) {
          step.skills_used.push_back(skill.as<std::string>());
        }
      }
      if (s["objective"]) {
        // Extract declared output variable names before serialising
        if (s["objective"]["outputs"]) {
          for (const auto & out : s["objective"]["outputs"]) {
            step.outputs.push_back(out.as<std::string>());
          }
        }
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
// BT failure falldiagnosis
// ─────────────────────────────────────────────────────────────────────────────

std::string LLMPlanOrchestrator::collect_failure_reason()
{
  // Read the "bt_last_failure" key written by the failing BT node via bt_failure()
  std::string bb_reason;
  std::string bb_code;
  try {
    bb_reason = blackboard_->get<std::string>("bt_last_failure");
  } catch (...) {}
  try {
    bb_code = blackboard_->get<std::string>("bt_last_failure_code");
  } catch (...) {}
  // Clear so it doesn't bleed into the next step
  blackboard_->set("bt_last_failure", std::string{});
  blackboard_->set("bt_last_failure_code", std::string{});
  last_failure_code_ = bb_code;

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

void LLMPlanOrchestrator::save_bt_xml(const std::string & bt_xml, std::size_t step)
{
  if (current_plan_dir_.empty()) {return;}
  // Formato step_00.xml, step_01.xml, ... hasta step_99.xml
  std::ostringstream filename;
  filename << "step_" << std::setw(2) << std::setfill('0') << step << ".xml";
  const auto path = current_plan_dir_ / filename.str();
  std::ofstream f(path);
  if (!f.is_open()) {
    RCLCPP_WARN(get_logger(), "Could not save BT XML to: %s", path.c_str());
    return;
  }
  f << bt_xml;
  RCLCPP_INFO(get_logger(), "Saved BT XML: %s", path.c_str());
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

  if (save_exec_ && (new_state == State::SUCCESS || new_state == State::FAILED)) {
    write_metrics_csv();
  }

  if (new_state == State::SUCCESS) {
    RCLCPP_INFO(get_logger(),
      "✅ Mission '%s' completed successfully.", mission_name_.c_str());
    RCLCPP_INFO(get_logger(), "⏳ Waiting for a new mission — call /start_mission.");
  } else if (new_state == State::FAILED) {
    RCLCPP_WARN(get_logger(),
      "❌ Mission '%s' failed.", mission_name_.c_str());
    RCLCPP_INFO(get_logger(), "⏳ Waiting for a new mission — call /start_mission.");
  }
}

std::string LLMPlanOrchestrator::state_name(State s) const
{
  switch (s) {
    case State::IDLE: return "IDLE";
    case State::WAITING_PLAN: return "WAITING_PLAN";
    case State::GENERATING_BT: return "GENERATING_BT";
    case State::EXECUTING_BT: return "EXECUTING_BT";
    case State::WAITING_REPLAN: return "WAITING_REPLAN";
    case State::WAITING_FIX_BT: return "WAITING_FIX_BT";
    case State::SUCCESS: return "SUCCESS";
    case State::FAILED: return "FAILED";
    default: return "UNKNOWN";
  }
}

void LLMPlanOrchestrator::write_metrics_csv()
{
  // Only write if there are steps and an execution directory
  if (exec_run_dir_.empty() || run_metrics_.steps.empty()) return;

  std::filesystem::path metrics_path = exec_run_dir_ / "metrics.csv";
  bool write_header = !std::filesystem::exists(metrics_path);
  std::ofstream csv(metrics_path, std::ios::app);
  if (!csv.is_open()) return;

  // Write CSV header if file does not exist
  if (write_header) {
    csv << "mission_name,run_timestamp,plan_total_time_ms,replan_total_time_ms,bt_gen_total_time_ms,bt_exec_total_time_ms,replan_count,fix_count_total,replan_id,step_id,bt_gen_time_ms,bt_exec_time_ms,fix_count,bt_status\n";
  }

  // Get execution timestamp for this run
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
  std::string run_timestamp = oss.str();

 
  if (save_exec_) {
    run_metrics_.bt_exec_total_time_ms = 0;
    run_metrics_.fix_count_total = 0;
    for (const auto& m : run_metrics_.steps) {
      run_metrics_.bt_exec_total_time_ms += m.bt_exec_time_ms;
      run_metrics_.fix_count_total += m.fix_count;
    }
    run_metrics_.replan_count = replan_count_;
    // Write one row per step with all relevant metrics
    for (const auto& m : run_metrics_.steps) {
      csv << mission_name_ << "," << run_timestamp << ","
          << run_metrics_.plan_total_time_ms << ","
          << run_metrics_.replan_total_time_ms << ","
          << run_metrics_.bt_gen_total_time_ms << ","
          << run_metrics_.bt_exec_total_time_ms << ","
          << run_metrics_.replan_count << ","
          << run_metrics_.fix_count_total << ","
          << m.replan_id << ","
          << m.step_id << ","
          << m.bt_gen_time_ms << ","
          << m.bt_exec_time_ms << ","
          << m.fix_count << ","
          << m.bt_status << "\n";
    }
    csv.close();
  }
}

}  // namespace behavior_architecture
