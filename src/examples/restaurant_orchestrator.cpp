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

#include "behavior_architecture/examples/restaurant_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace behavior_architecture
{
namespace examples
{

// Register this orchestrator with the factory
static OrchestratorRegistrar<RestaurantOrchestrator> restaurant_registrar("restaurant");

RestaurantOrchestrator::RestaurantOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("restaurant_orchestrator", blackboard),
  state_(RestaurantState::INIT)
{
  RCLCPP_INFO(get_logger(), "RestaurantOrchestrator initialized");
}

void
RestaurantOrchestrator::control_cycle()
{
  switch (state_) {
    case RestaurantState::INIT:
      RCLCPP_INFO(get_logger(), "[INIT] Starting restaurant service");
      go_to_state(static_cast<int>(RestaurantState::APPROACH_CUSTOMER));
      break;

    case RestaurantState::APPROACH_CUSTOMER:
      if (last_status_ == "") {
        RCLCPP_INFO_ONCE(get_logger(), "[APPROACH_CUSTOMER] Waiting for behavior to start...");
        break;
      }
      
      if (check_behavior_finished()) {
        if (last_status_ == "SUCCESS") {
          RCLCPP_INFO(get_logger(), "[APPROACH_CUSTOMER] Reached customer");
          go_to_state(static_cast<int>(RestaurantState::COLLECT_ORDER));
        } else {
          RCLCPP_WARN(get_logger(), "[APPROACH_CUSTOMER] Failed to reach customer");
          go_to_state(static_cast<int>(RestaurantState::COMPLETE));
        }
      }
      break;

    case RestaurantState::COLLECT_ORDER:
      if (check_behavior_finished()) {
        if (last_status_ == "SUCCESS") {
          RCLCPP_INFO(get_logger(), "[COLLECT_ORDER] Order collected successfully");
          go_to_state(static_cast<int>(RestaurantState::COMPLETE));
        } else {
          RCLCPP_WARN(get_logger(), "[COLLECT_ORDER] Failed to collect order");
          go_to_state(static_cast<int>(RestaurantState::COMPLETE));
        }
      }
      break;

    case RestaurantState::COMPLETE:
      RCLCPP_INFO_ONCE(get_logger(), "[COMPLETE] Restaurant service completed");
      break;

    default:
      RCLCPP_ERROR(get_logger(), "Unknown state, going to COMPLETE");
      go_to_state(static_cast<int>(RestaurantState::COMPLETE));
      break;
  }
}

void
RestaurantOrchestrator::go_to_state(int state)
{
  state_ = static_cast<RestaurantState>(state);

  switch (state_) {
    case RestaurantState::INIT:
      RCLCPP_INFO(get_logger(), "State: INIT");
      clear_activation();
      break;

    case RestaurantState::APPROACH_CUSTOMER:
      RCLCPP_INFO(get_logger(), "State: APPROACH_CUSTOMER - Activating follow behavior");
      clear_activation();
      add_activation("follow_behavior");
      break;

    case RestaurantState::COLLECT_ORDER:
      RCLCPP_INFO(get_logger(), "State: COLLECT_ORDER - Activating order collection");
      remove_activation("follow_behavior");
      add_activation("collect_order");
      break;

    case RestaurantState::COMPLETE:
      RCLCPP_INFO(get_logger(), "State: COMPLETE - Cleaning up");
      clear_activation();
      break;

    default:
      RCLCPP_WARN(get_logger(), "Transitioning to unknown state");
      break;
  }
}

}  // namespace examples
}  // namespace behavior_architecture
