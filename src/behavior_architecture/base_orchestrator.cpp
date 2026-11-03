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

#include "behavior_architecture/base_orchestrator.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "lifecycle_msgs/msg/transition.hpp"

namespace behavior_architecture
{

BaseOrchestrator::BaseOrchestrator(
  const std::string & node_name,
  BT::Blackboard::Ptr blackboard)
: LifecycleNode(node_name),
  blackboard_(blackboard),
  last_status_(""),
  status_received_(""),
  control_cycle_rate_ms_(100),
  started_(false)
{
  // Declare common parameters
  this->declare_parameter<int>("control_cycle_rate_ms", 100);
  control_cycle_rate_ms_ = this->get_parameter("control_cycle_rate_ms").as_int();

  // Create status subscriber (absolute topic to receive from all behavior runners)
  status_sub_ = create_subscription<std_msgs::msg::String>(
    "/behavior_status", 10, 
    std::bind(&BaseOrchestrator::status_callback, this, _1));

  RCLCPP_INFO(get_logger(), "BaseOrchestrator initialized: %s", node_name.c_str());
}

void
BaseOrchestrator::status_callback(std_msgs::msg::String::UniquePtr msg)
{
  last_status_ = msg->data;
  RCLCPP_DEBUG(get_logger(), "Status received: %s", last_status_.c_str());
  
  // Reset status if behavior was deactivated, and forget the result of the previous run: a
  // behavior activated next that ends on its first tick with the same result (no RUNNING in
  // between) must still be reported as finished
  if (last_status_ == "DEACTIVATED") {
    last_status_ = "";
    status_received_ = "";
  }
}

bool
BaseOrchestrator::check_behavior_finished()
{
  // A runner that could not be activated ends at once, with FAILURE
  if (activation_failed_) {
    activation_failed_ = false;
    last_status_ = "FAILURE";
    status_received_ = "FAILURE";
    // No DEACTIVATED message will clear the result: the next activation does
    clear_status_on_activation_ = true;
    return true;
  }
  if (status_received_ != last_status_ && last_status_ != "") {
    status_received_ = last_status_;
    RCLCPP_DEBUG(get_logger(), "Behavior status changed to: %s", status_received_.c_str());
    // Only return true if the behavior is actually finished (SUCCESS or FAILURE)
    // Don't return true for RUNNING status
    if (status_received_ == "SUCCESS" || status_received_ == "FAILURE") {
      return true;
    }
  }
  return false;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BaseOrchestrator::on_activate(const rclcpp_lifecycle::State & previous_state)
{
  RCLCPP_INFO(get_logger(), "BaseOrchestrator activating from %s", previous_state.label().c_str());
  
  // Create and start control cycle timer
  timer_ = create_wall_timer(
    std::chrono::milliseconds(control_cycle_rate_ms_),
    std::bind(&BaseOrchestrator::control_cycle, this));
  
  started_ = true;
  
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BaseOrchestrator::on_deactivate(const rclcpp_lifecycle::State & /* previous_state */)
{
  RCLCPP_INFO(get_logger(), "BaseOrchestrator deactivating");

  deactivate_all_runners();
  if (timer_) {
    timer_->cancel();
    timer_.reset();
  }
  
  started_ = false;
  
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BaseOrchestrator::on_configure(const rclcpp_lifecycle::State & /* previous_state */)
{
  if (runners_.empty()) {
    std::vector<BehaviorConfig> behaviors;
    std::vector<std::string> plugins;
    std::string pkg_name = "behavior_architecture";

    if (blackboard_->get("behaviors_config", behaviors) &&
      blackboard_->get("plugin_libraries", plugins))
    {
      std::ignore = blackboard_->get("package_name", pkg_name);
      for (auto & bc : behaviors) {
        auto runner = std::make_shared<BehaviorRunner>(
          blackboard_, bc.name, bc.behavior_file, plugins,
          bc.package_name.empty() ? pkg_name : bc.package_name,
          bc.control_period_ms);
        register_runner(bc.name, runner);
      }
    }
  }

  RCLCPP_INFO(get_logger(), "BaseOrchestrator configuring %zu runner(s)", runners_.size());
  for (auto & [name, runner] : runners_) {
    runner->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  }
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void
BaseOrchestrator::register_runner(const std::string & name, BehaviorRunner::SharedPtr runner)
{
  runners_[name] = runner;
}

std::vector<BehaviorRunner::SharedPtr>
BaseOrchestrator::get_runners() const
{
  std::vector<BehaviorRunner::SharedPtr> result;
  result.reserve(runners_.size());
  for (const auto & [name, runner] : runners_) {
    result.push_back(runner);
  }
  return result;
}

bool
BaseOrchestrator::activate_runner(const std::string & name)
{
  auto it = runners_.find(name);
  if (it == runners_.end()) {
    RCLCPP_WARN(get_logger(), "Runner '%s' not registered", name.c_str());
    return false;
  }
  if (clear_status_on_activation_) {
    clear_status_on_activation_ = false;
    last_status_ = "";
    status_received_ = "";
  }
  const auto state =
    it->second->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);
  if (state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE) {
    // The runner could not build its tree; check_behavior_finished() reports it as a FAILURE
    // The runners are stored as lifecycle nodes (BehaviorRunner::SharedPtr)
    const auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
    const std::string reason = runner && !runner->activation_error().empty() ?
      runner->activation_error() : "it is " + state.label();
    RCLCPP_ERROR(get_logger(), "Runner '%s' could not be activated: %s", name.c_str(),
      reason.c_str());
    activation_failed_ = true;
    return false;
  }
  activation_failed_ = false;
  active_runners_.insert(name);
  return true;
}

void
BaseOrchestrator::deactivate_runner(const std::string & name)
{
  auto it = runners_.find(name);
  if (it != runners_.end()) {
    it->second->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_DEACTIVATE);
    active_runners_.erase(name);
  }
}

void
BaseOrchestrator::deactivate_all_runners()
{
  for (const auto & name : active_runners_) {
    auto it = runners_.find(name);
    if (it != runners_.end()) {
      it->second->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_DEACTIVATE);
    }
  }
  active_runners_.clear();
}

}  // namespace behavior_architecture
