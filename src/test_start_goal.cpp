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

#include <string>

#include "rclcpp/rclcpp.hpp"
#include "llm_planner_interfaces/srv/start_goal.hpp"
#include "yaml-cpp/yaml.h"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = rclcpp::Node::make_shared("test_start_goal");

  // Path to the YAML file with the goal definition
  node->declare_parameter("goal_file", std::string(""));
  const std::string goal_file = node->get_parameter("goal_file").as_string();

  if (goal_file.empty()) {
    RCLCPP_ERROR(node->get_logger(), "Parameter 'goal_file' is required");
    RCLCPP_ERROR(node->get_logger(),
      "Usage: ros2 run behavior_architecture test_start_goal "
      "--ros-args -p goal_file:=/path/to/goal.yaml");
    rclcpp::shutdown();
    return 1;
  }

  // Load YAML
  YAML::Node doc;
  try {
    doc = YAML::LoadFile(goal_file);
  } catch (const YAML::Exception & e) {
    RCLCPP_ERROR(node->get_logger(), "Failed to load '%s': %s", goal_file.c_str(), e.what());
    rclcpp::shutdown();
    return 1;
  }

  if (!doc["goal"]) {
    RCLCPP_ERROR(node->get_logger(), "YAML file must contain a 'goal' key");
    rclcpp::shutdown();
    return 1;
  }

  const std::string goal    = doc["goal"].as<std::string>();
  const std::string context = doc["context"] ? doc["context"].as<std::string>() : "";

  auto client = node->create_client<llm_planner_interfaces::srv::StartGoal>("start_goal");

  RCLCPP_INFO(node->get_logger(), "Waiting for /start_goal service...");
  if (!client->wait_for_service(std::chrono::seconds(10))) {
    RCLCPP_ERROR(node->get_logger(), "Service not available after 10s");
    rclcpp::shutdown();
    return 1;
  }

  auto request = std::make_shared<llm_planner_interfaces::srv::StartGoal::Request>();
  request->goal    = goal;
  request->context = context;

  RCLCPP_INFO(node->get_logger(), "Sending goal: '%s'", goal.c_str());
  RCLCPP_INFO(node->get_logger(), "Context:      '%s'", context.c_str());

  auto future = client->async_send_request(request);
  if (rclcpp::spin_until_future_complete(node, future) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(node->get_logger(), "Service call failed");
    rclcpp::shutdown();
    return 1;
  }

  auto response = future.get();
  if (response->accepted) {
    RCLCPP_INFO(node->get_logger(), "Goal accepted: %s", response->message.c_str());
  } else {
    RCLCPP_WARN(node->get_logger(), "Goal rejected: %s", response->message.c_str());
  }

  rclcpp::shutdown();
  return 0;
}
