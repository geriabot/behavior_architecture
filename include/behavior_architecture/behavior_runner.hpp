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

#ifndef BEHAVIOR_ARCHITECTURE__BEHAVIOR_RUNNER_HPP_
#define BEHAVIOR_ARCHITECTURE__BEHAVIOR_RUNNER_HPP_

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "std_msgs/msg/string.hpp"
#include "behaviortree_cpp/behavior_tree.h"
#include "behaviortree_cpp/bt_factory.h"
#include "behaviortree_cpp/loggers/bt_cout_logger.h"
#include "behaviortree_cpp/loggers/groot2_publisher.h"
#include "ament_index_cpp/get_package_share_directory.hpp"

namespace behavior_architecture
{

using namespace std::chrono_literals;

/**
 * @brief BehaviorRunner - Lifecycle node that loads and executes BehaviorTree XML files
 * 
 * This class manages the execution of a BehaviorTree defined in an XML file.
 * It handles plugin loading, tree creation, and publishes execution status.
 * 
 * Features:
 * - Loads BT plugins dynamically
 * - Creates tree from XML file
 * - Publishes execution status (SUCCESS, FAILURE, RUNNING)
 * - Supports lifecycle management
 * - Can be activated/deactivated by orchestrator
 */
class BehaviorRunner : public rclcpp_lifecycle::LifecycleNode
{
public:
  /**
   * @brief Constructor
   * @param blackboard Shared blackboard for BT communication
   * @param name Node name
   * @param xml_path Relative path to XML file (from package share directory)
   * @param plugins List of plugin library names to load
   * @param package_name Package name containing the XML file (default: "behavior_architecture")
   * @param control_cycle_period_ms Control cycle period in milliseconds (default: 10ms)
   * @param custom_node_registrar Optional callback to register custom BT nodes
   */
  BehaviorRunner(
    BT::Blackboard::Ptr blackboard,
    const std::string & name,
    const std::string & xml_path,
    const std::vector<std::string> & plugins,
    const std::string & package_name = "behavior_architecture",
    int control_cycle_period_ms = 10,
    std::function<void(BT::BehaviorTreeFactory&)> custom_node_registrar = nullptr);

  /**
   * @brief Get current BehaviorTree execution status
   * @return Current BT node status
   */
  BT::NodeStatus get_bt_status();

  /**
   * @brief Set BT XML string directly (skips file loading in on_activate)
   * @param xml BehaviorTree XML string
   */
  void set_bt(const std::string & xml);

  /**
   * @brief Reset the behavior runner to initial state
   * 
   * Halts the current tree and resets execution status
   */
  void refresh();

protected:
  /**
   * @brief Main control cycle - ticks the BehaviorTree
   * 
   * Called periodically when node is active. Executes one tick of the tree
   * and publishes the resulting status.
   */
  void control_cycle();

  // Lifecycle callbacks
  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_activate(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_deactivate(const rclcpp_lifecycle::State & previous_state) override;

private:
  BT::Blackboard::Ptr blackboard_;
  std::string bt_xml_;  // if non-empty, on_activate uses createTreeFromText instead of file
  BT::Tree tree_;
  std::unique_ptr<BT::StdCoutLogger> cout_logger_;
  std::unique_ptr<BT::Groot2Publisher> groot_publisher_;
  BT::NodeStatus status_;
  
  std::string xml_path_;
  std::string package_name_;
  std::vector<std::string> plugins_;
  int control_cycle_period_ms_;
  std::function<void(BT::BehaviorTreeFactory&)> custom_node_registrar_;
  
  rclcpp::Node::SharedPtr node_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::String>::SharedPtr status_pub_;
  
  bool executed_;
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__BEHAVIOR_RUNNER_HPP_