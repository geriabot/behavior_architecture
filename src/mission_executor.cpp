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

#include <dlfcn.h>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "ament_index_cpp/get_package_share_directory.hpp"

#include "behavior_architecture/config_parser.hpp"
#include "behavior_architecture/behavior_runner.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"
#include "behavior_architecture/llm_plan_orchestrator.hpp"

// Include all orchestrator implementations to trigger registration
#include "behavior_architecture/examples/restaurant_orchestrator.hpp"
#include "behavior_architecture/examples/simple_orchestrator.hpp"

using behavior_architecture::ActionConfig;
using behavior_architecture::parse_config;
using behavior_architecture::setup_blackboard_from_config;

/**
 * @brief Resolve file path - handles both absolute and package-relative paths
 * @param file_path The path to resolve
 * @param package_name Package name for relative paths
 * @return Resolved absolute path
 */
std::string resolve_file_path(const std::string & file_path, const std::string & package_name)
{
  if (file_path.empty()) {
    throw std::runtime_error("Empty file path provided");
  }
  
  // If it's an absolute path, return as-is
  if (file_path[0] == '/') {
    return file_path;
  }
  
  // Otherwise, resolve relative to package share directory
  try {
    std::string package_share = ament_index_cpp::get_package_share_directory(package_name);
    return package_share + "/" + file_path;
  } catch (const std::exception & e) {
    throw std::runtime_error(
      "Failed to resolve file path '" + file_path + 
      "' in package '" + package_name + "': " + e.what());
  }
}

/**
 * @brief Load orchestrator libraries dynamically
 * @param libraries List of library filenames to load
 * @param package_name Package name for resolving library paths
 */
void load_orchestrator_libraries(
  const std::vector<std::string> & libraries, 
  const std::string & package_name)
{
  for (const auto & lib_name : libraries) {
    std::string lib_path;
    
    try {
      // Try to find library in package lib directory
      std::string package_share = ament_index_cpp::get_package_share_directory(package_name);
      std::string package_lib = package_share + "/../lib/" + lib_name;
      
      // Load the library
      void * handle = dlopen(package_lib.c_str(), RTLD_LAZY | RTLD_GLOBAL);
      if (!handle) {
        // Try without path prefix (rely on LD_LIBRARY_PATH)
        handle = dlopen(lib_name.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (!handle) {
          RCLCPP_ERROR(
            rclcpp::get_logger("mission_executor"),
            "Failed to load orchestrator library '%s': %s", 
            lib_name.c_str(), dlerror());
          throw std::runtime_error("Failed to load orchestrator library: " + lib_name);
        }
      }
      
      RCLCPP_INFO(
        rclcpp::get_logger("mission_executor"),
        "Loaded orchestrator library: %s", lib_name.c_str());
        
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        rclcpp::get_logger("mission_executor"),
        "Error loading orchestrator library '%s': %s", 
        lib_name.c_str(), e.what());
      throw;
    }
  }
}

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  
  // Check for config file argument
  if (argc < 2) {
    RCLCPP_ERROR(
      rclcpp::get_logger("mission_executor"),
      "Usage: mission_executor <config_file.yaml>");
    RCLCPP_INFO(
      rclcpp::get_logger("mission_executor"),
      "Available orchestrator types:");
    for (const auto & type : behavior_architecture::OrchestratorFactory::get_registered_types()) {
      RCLCPP_INFO(rclcpp::get_logger("mission_executor"), "  - %s", type.c_str());
    }
    return 1;
  }
  
  std::string config_file = argv[1];

  // ── Optional flags: --save-exec [--exec-dir PATH] ────────────────────────
  bool save_exec_flag = false;
  std::string exec_dir_flag;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--save-exec") {
      save_exec_flag = true;
    } else if (std::string(argv[i]) == "--exec-dir" && i + 1 < argc) {
      exec_dir_flag = argv[++i];
    }
  }

  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "Loading configuration from: %s", config_file.c_str());
  
  // Parse configuration
  ActionConfig config;
  try {
    config = parse_config(config_file);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(
      rclcpp::get_logger("mission_executor"),
      "Failed to parse configuration: %s", e.what());
    return 1;
  }
  
  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "Configuration loaded successfully");
  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "  Node name: %s", config.node_name.c_str());
  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "  Orchestrator type: %s", config.orchestrator_type.c_str());
  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "  Number of behaviors: %zu", config.behaviors.size());

  // Load orchestrator libraries if specified
  if (!config.orchestrator_libraries.empty()) {
    RCLCPP_INFO(
      rclcpp::get_logger("mission_executor"),
      "Loading %zu orchestrator libraries...", config.orchestrator_libraries.size());
    try {
      load_orchestrator_libraries(config.orchestrator_libraries, config.package_name);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        rclcpp::get_logger("mission_executor"),
        "Failed to load orchestrator libraries: %s", e.what());
      return 1;
    }
  }

  // Create shared blackboard for inter-node communication
  auto blackboard = BT::Blackboard::create();

  // Create a ROS node and add it to the blackboard
  auto node = std::make_shared<rclcpp::Node>(config.node_name);
  blackboard->set("node", node);

  // Apply CLI overrides
  if (save_exec_flag) {config.save_exec = true;}
  if (!exec_dir_flag.empty()) {config.exec_dir = exec_dir_flag;}

  // Store config in blackboard so orchestrator on_configure can create runners
  setup_blackboard_from_config(blackboard, config);

  // Create orchestrator using factory
  RCLCPP_INFO(
    rclcpp::get_logger("mission_executor"),
    "Creating orchestrator of type: %s", config.orchestrator_type.c_str());

  auto orchestrator = behavior_architecture::OrchestratorFactory::create_orchestrator(
    config.orchestrator_type,
    blackboard);
  
  if (!orchestrator) {
    RCLCPP_ERROR(
      rclcpp::get_logger("mission_executor"),
      "Failed to create orchestrator. Available types:");
    for (const auto & type : behavior_architecture::OrchestratorFactory::get_registered_types()) {
      RCLCPP_ERROR(rclcpp::get_logger("mission_executor"), "  - %s", type.c_str());
    }
    return 1;
  }

  // Configure nodes — on_configure creates runners from blackboard config
  RCLCPP_INFO(rclcpp::get_logger("mission_executor"), "Configuring nodes...");
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);

  // Activate orchestrator
  RCLCPP_INFO(rclcpp::get_logger("mission_executor"), "Activating orchestrator...");
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);

  // Create executor and add nodes
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);

  for (auto & runner : orchestrator->get_runners()) {
    executor.add_node(runner->get_node_base_interface());
  }

  executor.add_node(orchestrator->get_node_base_interface());

  RCLCPP_INFO(rclcpp::get_logger("mission_executor"), "Action executor ready. Starting execution...");

  executor.spin();

  rclcpp::shutdown();
  return 0;
}
