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


#include "behavior_architecture/behavior_runner.hpp"

namespace behavior_architecture
{

BehaviorRunner::BehaviorRunner(
  BT::Blackboard::Ptr blackboard,
  const std::string & name,
  const std::string & xml_path,
  const std::vector<std::string> & plugins,
  const std::string & package_name,
  int control_cycle_period_ms,
  std::function<void(BT::BehaviorTreeFactory&)> custom_node_registrar)
  : LifecycleNode(name),
  blackboard_(blackboard),
  status_(BT::NodeStatus::IDLE),
  xml_path_(xml_path),
  package_name_(package_name),
  plugins_(plugins),
  control_cycle_period_ms_(control_cycle_period_ms),
  custom_node_registrar_(custom_node_registrar),
  executed_(false)
{
  RCLCPP_INFO(get_logger(), "BehaviorRunner constructor (%s)", name.c_str());
  
  blackboard_->get("node", node_);

  RCLCPP_DEBUG(get_logger(), "XML path: %s", xml_path_.c_str());
  RCLCPP_DEBUG(get_logger(), "Package: %s", package_name_.c_str());
  RCLCPP_DEBUG(get_logger(), "# plugins: %ld", plugins_.size());

  // Create status publisher (absolute topic for orchestrator communication)
  status_pub_ = create_publisher<std_msgs::msg::String>("/behavior_status", 10);
}

void
BehaviorRunner::control_cycle()
{
  std_msgs::msg::String msg;

  if (!executed_) {
    RCLCPP_DEBUG(get_logger(), "BehaviorRunner ticking tree (%s)", get_name());
    status_ = tree_.rootNode()->executeTick();
  
    switch (status_) {
      case BT::NodeStatus::SUCCESS:
        msg.data = "SUCCESS";
        status_pub_->publish(msg);
        RCLCPP_INFO(get_logger(), "Behavior tree (%s): SUCCESS", get_name());
        executed_ = true;
        break;
      case BT::NodeStatus::RUNNING:
        msg.data = "RUNNING";
        status_pub_->publish(msg);
        RCLCPP_INFO_ONCE(get_logger(), "Behavior tree (%s): RUNNING", get_name());
        break;
      default:
        msg.data = "FAILURE";
        status_pub_->publish(msg);
        RCLCPP_INFO(get_logger(), "Behavior tree (%s): FAILURE", get_name());
        executed_ = true;
        break;
    }
  }
}

BT::NodeStatus
BehaviorRunner::get_bt_status() {
  return status_;
}

void
BehaviorRunner::set_bt(const std::string & xml)
{
  bt_xml_ = xml;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BehaviorRunner::on_activate(const rclcpp_lifecycle::State & /* previous_state */)
{
  RCLCPP_INFO(get_logger(), "BehaviorRunner (%s) on_activate", get_name());

  // Support both absolute paths and package-relative paths
  std::string xml_file;
  if (!xml_path_.empty() && xml_path_[0] == '/') {
    xml_file = xml_path_;
  } else {
    xml_file = ament_index_cpp::get_package_share_directory(package_name_) + "/" + xml_path_;
  }

  RCLCPP_INFO(get_logger(), "XML file: %s", xml_file.c_str());

  BT::BehaviorTreeFactory factory;

  for (const auto & plugin : plugins_) {
    factory.registerFromPlugin(plugin);
    RCLCPP_DEBUG(get_logger(), "Plugin loaded: %s", plugin.c_str());
  }

  // Register custom nodes if callback provided
  if (custom_node_registrar_) {
    custom_node_registrar_(factory);
    RCLCPP_DEBUG(get_logger(), "Custom nodes registered");
  }

  RCLCPP_DEBUG(get_logger(), "Getting node from blackboard");
  blackboard_->get("node", node_);
  RCLCPP_DEBUG(get_logger(), "Creating BT");
  if (!bt_xml_.empty()) {
    tree_ = factory.createTreeFromText(bt_xml_, blackboard_);
    RCLCPP_DEBUG(get_logger(), "BT created from XML string");
  } else {
    tree_ = factory.createTreeFromFile(xml_file, blackboard_);
    RCLCPP_DEBUG(get_logger(), "BT created from XML");
  }

  status_pub_->on_activate();

  timer_ = create_wall_timer(
    std::chrono::milliseconds(control_cycle_period_ms_),
    std::bind(&BehaviorRunner::control_cycle, this));

  RCLCPP_INFO(get_logger(), "Timer created with period %dms", control_cycle_period_ms_);

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BehaviorRunner::on_deactivate(const rclcpp_lifecycle::State & /* previous_state */)
{
  RCLCPP_INFO(get_logger(), "BehaviorRunner(%s) on_deactivate", get_name());
  
  timer_ = nullptr;
  
  // Publish DEACTIVATED status before deactivating the publisher
  std_msgs::msg::String msg;
  msg.data = "DEACTIVATED";
  status_pub_->publish(msg);
  
  status_pub_->on_deactivate();

  refresh();

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void
BehaviorRunner::refresh()
{
  executed_ = false;
  status_ = BT::NodeStatus::IDLE;

  tree_.haltTree();

  RCLCPP_INFO(get_logger(), "BehaviorRunner refreshed (%s)", get_name());
}

}  // namespace behavior_architecture