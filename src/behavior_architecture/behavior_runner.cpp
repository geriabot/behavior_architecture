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

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <stdexcept>

namespace behavior_architecture
{

std::vector<std::string>
tree_ids_used_by_nodes(const std::string & xml, const BT::BehaviorTreeFactory & factory)
{
  static const std::regex tree_id(
    R"(<\s*(?:BehaviorTree|SubTree)\b[^>]*?\bID\s*=\s*["']([^"']*)["'])");
  std::set<std::string> ids;
  for (auto it = std::sregex_iterator(xml.begin(), xml.end(), tree_id);
    it != std::sregex_iterator(); ++it)
  {
    const std::string id = (*it)[1].str();
    if (factory.builders().count(id) != 0) {
      ids.insert(id);
    }
  }
  return {ids.begin(), ids.end()};
}

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
  declare_parameter("enable_groot_monitoring", true);
  declare_parameter("groot_port", 5555);
  
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
  std::lock_guard<std::mutex> lock(tree_mutex_);

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

const std::string &
BehaviorRunner::activation_error() const
{
  return activation_error_;
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
  activation_error_.clear();

  // Building the tree may fail (a file or a package that is not found, a plugin that cannot be
  // loaded, an unknown node, invalid XML, a tree ID that is a node ID): the transition then
  // fails with the reason, and the runner stays inactive, so that it can be activated again
  std::string source;
  // Held from the creation of the tree to the end of the activation, as the tree is replaced
  std::unique_lock<std::mutex> lock(tree_mutex_, std::defer_lock);
  try {
    BT::BehaviorTreeFactory factory;
    std::string xml_file;
    if (bt_xml_.empty()) {
      // Support both absolute paths and package-relative paths
      if (!xml_path_.empty() && xml_path_[0] == '/') {
        xml_file = xml_path_;
      } else {
        source = "the file '" + xml_path_ + "' of the package '" + package_name_ + "'";
        xml_file = ament_index_cpp::get_package_share_directory(package_name_) + "/" + xml_path_;
      }
      source = "the file '" + xml_file + "'";
      RCLCPP_INFO(get_logger(), "XML file: %s", xml_file.c_str());
    } else {
      source = "the XML given with set_bt()";
    }

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
    lock.lock();
    // Loggers must stop observing the old tree before it is replaced.
    groot_publisher_.reset();
    cout_logger_.reset();
    try {
      if (!bt_xml_.empty()) {
        tree_ = factory.createTreeFromText(bt_xml_, blackboard_);
        RCLCPP_DEBUG(get_logger(), "BT created from XML string");
      } else {
        tree_ = factory.createTreeFromFile(xml_file, blackboard_);
        RCLCPP_DEBUG(get_logger(), "BT created from XML");
      }
    } catch (const std::exception & e) {
      // BehaviorTree.CPP does not name the tree whose ID is the ID of a registered node
      std::string xml = bt_xml_;
      if (xml.empty()) {
        std::ifstream in(xml_file);
        xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      const auto conflicts = tree_ids_used_by_nodes(xml, factory);
      if (conflicts.empty()) {
        throw;
      }
      std::string ids;
      for (const auto & id : conflicts) {
        ids += (ids.empty() ? "'" : ", '") + id + "'";
      }
      throw std::runtime_error(
              "the ID " + ids + " of a <BehaviorTree> (or of a <SubTree>) is also the ID of a "
              "registered BT node; give the tree another ID (" + e.what() + ")");
    }
  } catch (const std::exception & e) {
    activation_error_ = "could not build the tree from " + source + ": " + e.what();
    RCLCPP_ERROR(get_logger(), "BehaviorRunner (%s) %s", get_name(), activation_error_.c_str());
    return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::FAILURE;
  }

  cout_logger_ = std::make_unique<BT::StdCoutLogger>(tree_);
  if (get_parameter("enable_groot_monitoring").as_bool()) {
    const auto port = get_parameter("groot_port").as_int();
    try {
      if (port < 1 || port > 65534) {
        throw std::invalid_argument("groot_port must be between 1 and 65534");
      }
      groot_publisher_ = std::make_unique<BT::Groot2Publisher>(tree_, port);
      RCLCPP_INFO(get_logger(), "Groot2 monitoring ready: localhost:%ld", port);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(get_logger(), "Groot2 monitoring unavailable on port %ld: %s",
        port, e.what());
    }
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
  {
    // The loggers observe the tree: wait for a tick in progress
    std::lock_guard<std::mutex> lock(tree_mutex_);
    groot_publisher_.reset();
    cout_logger_.reset();
  }

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
  // Waits for a tick in progress in the timer thread
  std::lock_guard<std::mutex> lock(tree_mutex_);
  executed_ = false;
  status_ = BT::NodeStatus::IDLE;

  tree_.haltTree();

  RCLCPP_INFO(get_logger(), "BehaviorRunner refreshed (%s)", get_name());
}

}  // namespace behavior_architecture