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
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "ament_index_cpp/get_package_share_directory.hpp"

#include "behavior_architecture/config_parser.hpp"
#include "behavior_architecture/examples/simple_orchestrator.hpp"

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  // Accept an optional config file argument; default to the installed simple_config.yaml
  std::string config_file;
  if (argc >= 2) {
    config_file = argv[1];
  } else {
    config_file = ament_index_cpp::get_package_share_directory("behavior_architecture") +
      "/config/simple_config.yaml";
  }

  behavior_architecture::ActionConfig config;
  try {
    config = behavior_architecture::parse_config(config_file);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(rclcpp::get_logger("main"), "Failed to parse config: %s", e.what());
    return 1;
  }

  auto blackboard = BT::Blackboard::create();
  auto node = std::make_shared<rclcpp::Node>(config.node_name);
  blackboard->set("node", node);
  behavior_architecture::setup_blackboard_from_config(blackboard, config);

  auto orchestrator = std::make_shared<behavior_architecture::examples::SimpleOrchestrator>(
    blackboard);

  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  for (auto & runner : orchestrator->get_runners()) {
    executor.add_node(runner->get_node_base_interface());
  }
  executor.add_node(orchestrator->get_node_base_interface());

  RCLCPP_INFO(rclcpp::get_logger("main"), "Simple example starting...");

  executor.spin();

  rclcpp::shutdown();
  return 0;
}
