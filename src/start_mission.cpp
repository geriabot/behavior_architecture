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
#include "llm_planner_interfaces/srv/start_mission.hpp"
#include "yaml-cpp/yaml.h"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = rclcpp::Node::make_shared("start_mission");

  // Path to the YAML file with the goal definition
  node->declare_parameter("mission_file", std::string(""));
  // Optional separate file listing robot skills; overrides any 'skills' key in mission_file
  node->declare_parameter("skills_file", std::string(""));

  const std::string mission_file   = node->get_parameter("mission_file").as_string();
  const std::string skills_file = node->get_parameter("skills_file").as_string();

  if (mission_file.empty()) {
    RCLCPP_ERROR(node->get_logger(), "Parameter 'mission_file' is required");
    RCLCPP_ERROR(node->get_logger(),
      "Usage: ros2 run behavior_architecture start_mission "
      "--ros-args -p mission_file:=/path/to/goal.yaml [-p skills_file:=/path/to/skills.yaml]");
    rclcpp::shutdown();
    return 1;
  }

  // Load goal YAML
  YAML::Node doc;
  try {
    doc = YAML::LoadFile(mission_file);
  } catch (const YAML::Exception & e) {
    RCLCPP_ERROR(node->get_logger(), "Failed to load '%s': %s", mission_file.c_str(), e.what());
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
  const std::string useful_info = doc["useful_info"] ? doc["useful_info"].as<std::string>() : "";

  std::vector<std::string> preconditions;
  if (doc["preconditions"]) {
    for (const auto & p : doc["preconditions"]) {
      preconditions.push_back(p.as<std::string>());
    }
  }

  std::vector<std::string> postconditions;
  if (doc["postconditions"]) {
    for (const auto & p : doc["postconditions"]) {
      postconditions.push_back(p.as<std::string>());
    }
  }

  // Skills: prefer dedicated skills_file; fall back to 'skills' key in mission_file
  std::vector<std::string> skills;
  if (!skills_file.empty()) {
    try {
      YAML::Node sdoc = YAML::LoadFile(skills_file);
      if (sdoc["skills"]) {
        for (const auto & s : sdoc["skills"]) {
          skills.push_back(s.as<std::string>());
        }
      }
      RCLCPP_INFO(node->get_logger(), "Loaded %zu skill(s) from '%s'",
        skills.size(), skills_file.c_str());
    } catch (const YAML::Exception & e) {
      RCLCPP_ERROR(node->get_logger(), "Failed to load skills file '%s': %s",
        skills_file.c_str(), e.what());
      rclcpp::shutdown();
      return 1;
    }
  } else if (doc["skills"]) {
    for (const auto & s : doc["skills"]) {
      skills.push_back(s.as<std::string>());
    }
    RCLCPP_INFO(node->get_logger(), "Loaded %zu skill(s) from mission_file", skills.size());
  }

  auto client = node->create_client<llm_planner_interfaces::srv::StartMission>("start_mission");

  RCLCPP_INFO(node->get_logger(), "Waiting for /start_mission service...");
  if (!client->wait_for_service(std::chrono::seconds(10))) {
    RCLCPP_ERROR(node->get_logger(), "Service not available after 10s");
    rclcpp::shutdown();
    return 1;
  }

  auto request = std::make_shared<llm_planner_interfaces::srv::StartMission::Request>();
  request->goal    = goal;
  request->context = context;
  request->skills  = skills;
  request->preconditions = preconditions;
  request->postconditions = postconditions;
  request->useful_info = useful_info;

  RCLCPP_INFO(node->get_logger(), "Sending goal: '%s'", goal.c_str());
  RCLCPP_INFO(node->get_logger(), "Context:      '%s'", context.c_str());
  RCLCPP_INFO(node->get_logger(), "Skills:       %zu listed", skills.size());

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
