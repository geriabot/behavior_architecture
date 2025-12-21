// Copyright 2024 Rodrigo Pérez-Rodríguez
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

#ifndef BEHAVIOR_ARCHITECTURE__BASE_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__BASE_ORCHESTRATOR_HPP_

#include <string>
#include <memory>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_cascade_lifecycle/rclcpp_cascade_lifecycle.hpp"
#include "std_msgs/msg/string.hpp"
#include "behaviortree_cpp/behavior_tree.h"

namespace behavior_architecture
{

using namespace std::chrono_literals;
using std::placeholders::_1;

/**
 * @brief Base class for behavior orchestrators using FSM + BehaviorTree architecture
 * 
 * This class provides a generic framework for implementing behavior orchestrators that:
 * - Use a Finite State Machine (FSM) for high-level behavior control
 * - Activate/deactivate BehaviorTree nodes based on FSM state
 * - Use cascade lifecycle management for coordinating multiple BT nodes
 * 
 * Derived classes should:
 * 1. Define their own State enum
 * 2. Override control_cycle() to implement FSM logic
 * 3. Override go_to_state() to handle state transitions
 * 4. Use add_activation()/remove_activation()/clear_activation() to control BT nodes
 */
class BaseOrchestrator : public rclcpp_cascade_lifecycle::CascadeLifecycleNode
{
public:
  /**
   * @brief Constructor
   * @param node_name Name of the orchestrator node
   * @param blackboard Shared blackboard for BehaviorTree communication
   */
  explicit BaseOrchestrator(
    const std::string & node_name,
    BT::Blackboard::Ptr blackboard);

  virtual ~BaseOrchestrator() = default;

protected:
  /**
   * @brief Main control loop - must be implemented by derived classes
   * 
   * This method implements the FSM logic, checking current state and
   * transitioning between states based on behavior tree status and other conditions.
   */
  virtual void control_cycle() = 0;

  /**
   * @brief Handle state transitions - must be implemented by derived classes
   * 
   * This method is called when transitioning to a new state. It should:
   * - Update internal state variable
   * - Activate/deactivate appropriate BT nodes
   * - Log state transition
   * 
   * @param state The new state to transition to (as int for generic handling)
   */
  virtual void go_to_state(int state) = 0;

  /**
   * @brief Check if the currently active behavior tree has finished
   * @return true if status changed from last check, false otherwise
   */
  bool check_behavior_finished();

  /**
   * @brief Callback for behavior status updates from BT nodes
   * @param msg Status message from BT node
   */
  void status_callback(std_msgs::msg::String::UniquePtr msg);

  // Lifecycle callbacks
  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_activate(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_deactivate(const rclcpp_lifecycle::State & previous_state) override;

  // Shared resources
  BT::Blackboard::Ptr blackboard_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr status_sub_;
  
  std::string last_status_;
  std::string status_received_;
  
  int control_cycle_rate_ms_;  // Control cycle period in milliseconds

private:
  bool started_;
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__BASE_ORCHESTRATOR_HPP_
