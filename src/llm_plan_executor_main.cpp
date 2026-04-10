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

#include "rclcpp/rclcpp.hpp"
#include "lifecycle_msgs/msg/transition.hpp"
#include "behavior_architecture/llm_plan_orchestrator.hpp"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  // Create an empty blackboard; LLMPlanOrchestrator will use built-in defaults.
  // To configure plugin_libraries, bt_nodes_packages, etc., set the keys here
  // before constructing the orchestrator, e.g.:
  //   blackboard->set<std::vector<std::string>>("llm_plugin_libraries", {...});
  auto blackboard = BT::Blackboard::create();

  auto orchestrator =
    std::make_shared<behavior_architecture::LLMPlanOrchestrator>(blackboard);

  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);

  rclcpp::spin(orchestrator->get_node_base_interface());
  rclcpp::shutdown();
  return 0;
}
