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

#include <fstream>
#include <dlfcn.h>
#include <yaml-cpp/yaml.h>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_cascade_lifecycle/rclcpp_cascade_lifecycle.hpp"
#include "ament_index_cpp/get_package_share_directory.hpp"

#include "behavior_architecture/behavior_runner.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

// Include all orchestrator implementations to trigger registration
#include "behavior_architecture/examples/restaurant_orchestrator.hpp"
#include "behavior_architecture/examples/simple_orchestrator.hpp"

/**
 * @brief Configuration structure for a behavior runner
 */
struct BehaviorConfig
{
  std::string name;
  std::string behavior_file;
  std::string package_name;  // Optional: override global package for this behavior
  int control_period_ms = 50;  // Default 50ms
};

/**
 * @brief Configuration structure for the action executor
 */
struct ActionConfig
{
  std::string node_name = "bt_node";
  std::string orchestrator_type;
  std::vector<std::string> orchestrator_libraries;
  std::vector<std::string> plugin_libraries;
  std::string package_name = "behavior_architecture";
  std::vector<BehaviorConfig> behaviors;
};

/**
 * @brief Parse YAML configuration file
 * @param config_file Path to the YAML configuration file
 * @return ActionConfig structure with parsed configuration
 */
ActionConfig parse_config(const std::string & config_file)
{
  ActionConfig config;
  
  try {
    YAML::Node yaml_config = YAML::LoadFile(config_file);
    
    // Parse node name
    if (yaml_config["node_name"]) {
      config.node_name = yaml_config["node_name"].as<std::string>();
    }
    
    // Parse orchestrator type (required)
    if (!yaml_config["orchestrator_type"]) {
      throw std::runtime_error("Missing required field: orchestrator_type");
    }
    config.orchestrator_type = yaml_config["orchestrator_type"].as<std::string>();
    
    // Parse package name
    if (yaml_config["package_name"]) {
      config.package_name = yaml_config["package_name"].as<std::string>();
    }
    
    // Parse orchestrator libraries
    if (yaml_config["orchestrator_libraries"]) {
      for (const auto & lib : yaml_config["orchestrator_libraries"]) {
        config.orchestrator_libraries.push_back(lib.as<std::string>());
      }
    }
    
    // Parse plugin libraries
    if (yaml_config["plugin_libraries"]) {
      for (const auto & plugin : yaml_config["plugin_libraries"]) {
        config.plugin_libraries.push_back(plugin.as<std::string>());
      }
    }
    
    // Parse behaviors (required)
    if (!yaml_config["behaviors"]) {
      throw std::runtime_error("Missing required field: behaviors");
    }
    
    for (const auto & behavior_node : yaml_config["behaviors"]) {
      BehaviorConfig behavior;
      
      if (!behavior_node["name"]) {
        throw std::runtime_error("Behavior missing required field: name");
      }
      behavior.name = behavior_node["name"].as<std::string>();
      
      if (!behavior_node["behavior_file"]) {
        throw std::runtime_error("Behavior missing required field: behavior_file");
      }
      behavior.behavior_file = behavior_node["behavior_file"].as<std::string>();
      
      if (behavior_node["control_period_ms"]) {
        behavior.control_period_ms = behavior_node["control_period_ms"].as<int>();
      }
      
      // Optional per-behavior package override
      if (behavior_node["package_name"]) {
        behavior.package_name = behavior_node["package_name"].as<std::string>();
      }
      
      config.behaviors.push_back(behavior);
    }
    
  } catch (const YAML::Exception & e) {
    throw std::runtime_error("YAML parsing error: " + std::string(e.what()));
  }
  
  return config;
}

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
            rclcpp::get_logger("action_executor"),
            "Failed to load orchestrator library '%s': %s", 
            lib_name.c_str(), dlerror());
          throw std::runtime_error("Failed to load orchestrator library: " + lib_name);
        }
      }
      
      RCLCPP_INFO(
        rclcpp::get_logger("action_executor"),
        "Loaded orchestrator library: %s", lib_name.c_str());
        
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        rclcpp::get_logger("action_executor"),
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
      rclcpp::get_logger("action_executor"),
      "Usage: action_executor <config_file.yaml>");
    RCLCPP_INFO(
      rclcpp::get_logger("action_executor"),
      "Available orchestrator types:");
    for (const auto & type : behavior_architecture::OrchestratorFactory::get_registered_types()) {
      RCLCPP_INFO(rclcpp::get_logger("action_executor"), "  - %s", type.c_str());
    }
    return 1;
  }
  
  std::string config_file = argv[1];
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "Loading configuration from: %s", config_file.c_str());
  
  // Parse configuration
  ActionConfig config;
  try {
    config = parse_config(config_file);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(
      rclcpp::get_logger("action_executor"),
      "Failed to parse configuration: %s", e.what());
    return 1;
  }
  
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "Configuration loaded successfully");
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "  Node name: %s", config.node_name.c_str());
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "  Orchestrator type: %s", config.orchestrator_type.c_str());
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "  Number of behaviors: %zu", config.behaviors.size());
  
  // Load orchestrator libraries if specified
  if (!config.orchestrator_libraries.empty()) {
    RCLCPP_INFO(
      rclcpp::get_logger("action_executor"),
      "Loading %zu orchestrator libraries...", config.orchestrator_libraries.size());
    try {
      load_orchestrator_libraries(config.orchestrator_libraries, config.package_name);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        rclcpp::get_logger("action_executor"),
        "Failed to load orchestrator libraries: %s", e.what());
      return 1;
    }
  }
  
  // Create shared blackboard for inter-node communication
  auto blackboard = BT::Blackboard::create();
  
  // Create a ROS node and add it to the blackboard
  auto node = std::make_shared<rclcpp::Node>(config.node_name);
  blackboard->set("node", node);
  
  // Create behavior runners
  std::vector<std::shared_ptr<behavior_architecture::BehaviorRunner>> behavior_runners;
  
  for (const auto & behavior_config : config.behaviors) {
    RCLCPP_INFO(
      rclcpp::get_logger("action_executor"),
      "Creating behavior runner: %s", behavior_config.name.c_str());
    
    try {
      // Use per-behavior package_name if specified, otherwise use global package_name
      std::string pkg_name = behavior_config.package_name.empty() ? 
                             config.package_name : behavior_config.package_name;
      
      // Don't resolve the path here - BehaviorRunner will do it in on_activate
      // Just pass the relative path from the config
      auto runner = std::make_shared<behavior_architecture::BehaviorRunner>(
        blackboard,
        behavior_config.name,
        behavior_config.behavior_file,
        config.plugin_libraries,
        pkg_name,
        behavior_config.control_period_ms
      );
      
      behavior_runners.push_back(runner);
      
      RCLCPP_INFO(
        rclcpp::get_logger("action_executor"),
        "  Behavior file: %s", behavior_config.behavior_file.c_str());
      RCLCPP_INFO(
        rclcpp::get_logger("action_executor"),
        "  Package: %s", pkg_name.c_str());
      RCLCPP_INFO(
        rclcpp::get_logger("action_executor"),
        "  Control period: %d ms", behavior_config.control_period_ms);
      
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        rclcpp::get_logger("action_executor"),
        "Failed to create behavior runner '%s': %s",
        behavior_config.name.c_str(), e.what());
      return 1;
    }
  }
  
  // Create orchestrator using factory
  RCLCPP_INFO(
    rclcpp::get_logger("action_executor"),
    "Creating orchestrator of type: %s", config.orchestrator_type.c_str());
  
  auto orchestrator = behavior_architecture::OrchestratorFactory::create_orchestrator(
    config.orchestrator_type,
    blackboard);
  
  if (!orchestrator) {
    RCLCPP_ERROR(
      rclcpp::get_logger("action_executor"),
      "Failed to create orchestrator. Available types:");
    for (const auto & type : behavior_architecture::OrchestratorFactory::get_registered_types()) {
      RCLCPP_ERROR(rclcpp::get_logger("action_executor"), "  - %s", type.c_str());
    }
    return 1;
  }
  
  // Configure all nodes
  RCLCPP_INFO(rclcpp::get_logger("action_executor"), "Configuring nodes...");
  
  for (auto & runner : behavior_runners) {
    runner->trigger_transition(
      lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE
    );
  }
  
  orchestrator->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE
  );
  
  // Activate orchestrator (it will coordinate behavior runners via cascade)
  RCLCPP_INFO(rclcpp::get_logger("action_executor"), "Activating orchestrator...");
  
  orchestrator->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE
  );
  
  // Create executor and add nodes
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  
  for (auto & runner : behavior_runners) {
    executor.add_node(runner->get_node_base_interface());
  }
  
  executor.add_node(orchestrator->get_node_base_interface());
  
  RCLCPP_INFO(rclcpp::get_logger("action_executor"), "Action executor ready. Starting execution...");
  
  executor.spin();
  
  rclcpp::shutdown();
  return 0;
}
