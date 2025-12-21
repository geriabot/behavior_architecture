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
#include "rclcpp_cascade_lifecycle/rclcpp_cascade_lifecycle.hpp"

#include "behavior_architecture/behavior_runner.hpp"
#include "behavior_architecture/examples/simple_orchestrator.hpp"

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  // Create shared blackboard for inter-node communication
  auto blackboard = BT::Blackboard::create();
  
  // Create a ROS node and add it to the blackboard
  auto node = std::make_shared<rclcpp::Node>("bt_node");
  blackboard->set("node", node);
  
  // Create BehaviorRunner nodes with all required parameters
  // Plugin library name (BehaviorTree.CPP will automatically add lib prefix and .so suffix)
  std::vector<std::string> plugins = {"libsocial_bt_nodes_plugin.so"};
  
  auto state1_runner = std::make_shared<behavior_architecture::BehaviorRunner>(
    blackboard,
    "state1_runner",
    "behaviors/state1.xml",
    plugins,
    "behavior_architecture",
    50  // Control cycle period: 50ms
  );
  
  auto state2_runner = std::make_shared<behavior_architecture::BehaviorRunner>(
    blackboard,
    "state2_runner",
    "behaviors/state2.xml",
    plugins,
    "behavior_architecture",
    50  // Control cycle period: 50ms
  );

  // Create orchestrator
  auto orchestrator = std::make_shared<behavior_architecture::examples::SimpleOrchestrator>(
    blackboard
  );

  // Configure all nodes
  state1_runner->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE
  );
  state2_runner->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE
  );
  orchestrator->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE
  );

  // Activate orchestrator (it will coordinate behavior runners via cascade)
  orchestrator->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE
  );

  // Create executor and add nodes
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.add_node(state1_runner->get_node_base_interface());
  executor.add_node(state2_runner->get_node_base_interface());
  executor.add_node(orchestrator->get_node_base_interface());

  RCLCPP_INFO(rclcpp::get_logger("main"), "Simple example starting...");

  executor.spin();

  rclcpp::shutdown();
  return 0;
}
