// Copyright 2026 Rodrigo Perez-Rodriguez
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

#include "behavior_architecture/test_plan_orchestrator.hpp"
#include <rclcpp/rclcpp.hpp>
#include <behaviortree_cpp/blackboard.h>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char ** argv)
{
  const auto args = rclcpp::init_and_remove_ros_arguments(argc, argv);
  if (args.size() < 2) {
    std::cerr << "Usage: test_plan_executor <plan_directory> [plugin_library ...]" << std::endl;
    std::cerr << "  plugin_library: BT plugin libraries to load "
      "(default: libsocial_bt_nodes_plugin.so)" << std::endl;
    rclcpp::shutdown();
    return 1;
  }
  auto blackboard = BT::Blackboard::create();
  // The ROS node of the BT nodes, as mission_executor stores it
  auto node = rclcpp::Node::make_shared("test_plan_node");
  blackboard->set("node", node);
  if (args.size() > 2) {
    blackboard->set(
      "llm_plugin_libraries", std::vector<std::string>(args.begin() + 2, args.end()));
  }
  auto orchestrator = std::make_shared<behavior_architecture::TestPlanOrchestrator>(blackboard);

  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);

  orchestrator->run_plan_from_dir(args[1]);

  // The runner ticks its tree with its own timer: it is spun with the orchestrator
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  for (auto & runner : orchestrator->get_runners()) {
    executor.add_node(runner->get_node_base_interface());
  }
  executor.add_node(orchestrator->get_node_base_interface());
  executor.spin();
  rclcpp::shutdown();
  return 0;
}
