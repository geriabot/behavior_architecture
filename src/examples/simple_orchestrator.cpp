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

#include "behavior_architecture/examples/simple_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace behavior_architecture
{
namespace examples
{

// Register this orchestrator with the factory
static OrchestratorRegistrar<SimpleOrchestrator> simple_registrar("simple");

SimpleOrchestrator::SimpleOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("simple_orchestrator", blackboard),
  state_(State::INIT)
{
  RCLCPP_INFO(get_logger(), "SimpleOrchestrator created");
}

void SimpleOrchestrator::control_cycle()
{
  switch (state_) {
    case State::INIT:
      RCLCPP_INFO(get_logger(), "State: INIT");
      go_to_state(static_cast<int>(State::STATE_1));
      break;

    case State::STATE_1:
      if (check_behavior_finished()) {
        RCLCPP_INFO(get_logger(), "State 1 behavior finished with status: %s", 
          last_status_.c_str());
        if (last_status_ == "SUCCESS") {
          go_to_state(static_cast<int>(State::STATE_2));
        } else {
          RCLCPP_WARN(get_logger(), "State 1 failed, retrying...");
          go_to_state(static_cast<int>(State::STATE_1));
        }
      }
      break;

    case State::STATE_2:
      if (check_behavior_finished()) {
        RCLCPP_INFO(get_logger(), "State 2 behavior finished with status: %s", 
          last_status_.c_str());
        if (last_status_ == "SUCCESS") {
          go_to_state(static_cast<int>(State::STOP));
        } else {
          RCLCPP_WARN(get_logger(), "State 2 failed, retrying...");
          go_to_state(static_cast<int>(State::STATE_2));
        }
      }
      break;

    case State::STOP:
      RCLCPP_INFO_ONCE(get_logger(), "State: STOP - All behaviors completed");
      break;
  }
}

void SimpleOrchestrator::go_to_state(int state)
{
  state_ = static_cast<State>(state);

  switch (state_) {
    case State::INIT:
      RCLCPP_INFO(get_logger(), "Transitioning to INIT");
      clear_activation();
      break;

    case State::STATE_1:
      RCLCPP_INFO(get_logger(), "Transitioning to STATE_1");
      clear_activation();
      add_activation("state1_runner");
      break;

    case State::STATE_2:
      RCLCPP_INFO(get_logger(), "Transitioning to STATE_2");
      clear_activation();
      add_activation("state2_runner");
      break;

    case State::STOP:
      RCLCPP_INFO(get_logger(), "Transitioning to STOP");
      clear_activation();
      break;
  }
}

}  // namespace examples
}  // namespace behavior_architecture
