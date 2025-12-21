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

#ifndef BEHAVIOR_ARCHITECTURE__EXAMPLES__SIMPLE_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__EXAMPLES__SIMPLE_ORCHESTRATOR_HPP_

#include "behavior_architecture/base_orchestrator.hpp"

namespace behavior_architecture
{
namespace examples
{

/**
 * @brief Simple example orchestrator with 2 states
 * 
 * State machine:
 *   INIT -> STATE_1 -> STATE_2 -> STOP
 * 
 * Each state executes a simple BehaviorTree that speaks a message.
 */
class SimpleOrchestrator : public BaseOrchestrator
{
public:
  enum class State : int {
    INIT = 0,
    STATE_1 = 1,
    STATE_2 = 2,
    STOP = 3
  };

  explicit SimpleOrchestrator(BT::Blackboard::Ptr blackboard);

protected:
  void control_cycle() override;
  void go_to_state(int state) override;

private:
  State state_;
};

}  // namespace examples
}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__EXAMPLES__SIMPLE_ORCHESTRATOR_HPP_
