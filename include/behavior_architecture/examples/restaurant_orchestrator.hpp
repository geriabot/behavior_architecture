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

#ifndef BEHAVIOR_ARCHITECTURE__EXAMPLES__RESTAURANT_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__EXAMPLES__RESTAURANT_ORCHESTRATOR_HPP_

#include "behavior_architecture/base_orchestrator.hpp"
#include "std_msgs/msg/string.hpp"

namespace behavior_architecture
{
namespace examples
{

enum class RestaurantState : int {
  INIT = 0,
  APPROACH_CUSTOMER = 1,
  COLLECT_ORDER = 2,
  COMPLETE = 3
};

/**
 * @brief Example orchestrator for restaurant waiter scenario
 * 
 * This orchestrator demonstrates how to use the behavior_architecture framework
 * to coordinate multiple behaviors in a restaurant service scenario:
 * 1. Approach customer (follow behavior)
 * 2. Collect their order (interaction behaviors)
 */
class RestaurantOrchestrator : public BaseOrchestrator
{
public:
  RestaurantOrchestrator(BT::Blackboard::Ptr blackboard);

protected:
  void control_cycle() override;
  void go_to_state(int state) override;

private:
  RestaurantState state_;
};

}  // namespace examples
}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__EXAMPLES__RESTAURANT_ORCHESTRATOR_HPP_
