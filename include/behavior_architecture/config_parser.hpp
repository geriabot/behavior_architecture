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

#ifndef BEHAVIOR_ARCHITECTURE__CONFIG_PARSER_HPP_
#define BEHAVIOR_ARCHITECTURE__CONFIG_PARSER_HPP_

#include <string>
#include <vector>
#include <stdexcept>

#include <yaml-cpp/yaml.h>
#include "behaviortree_cpp/behavior_tree.h"

#include "behavior_architecture/behavior_config.hpp"

namespace behavior_architecture
{

inline bool is_llm_orchestrator_type(const std::string & orchestrator_type)
{
  return orchestrator_type == "llm" ||
         orchestrator_type == "mcp" ||
         orchestrator_type == "mcp_llm" ||
         orchestrator_type == "mcp_llm_plan_orchestrator";
}

/**
 * @brief Full configuration parsed from a YAML config file.
 */
struct ActionConfig
{
  std::string node_name = "bt_node";
  std::string orchestrator_type;
  std::vector<std::string> orchestrator_libraries;
  std::vector<std::string> plugin_libraries;
  std::string package_name = "behavior_architecture";
  std::vector<BehaviorConfig> behaviors;
  // LLM-specific fields
  std::vector<std::string> bt_nodes_packages;
  int bt_control_period_ms = 50;
  double bt_timeout_sec = 30.0;
  std::vector<std::string> skills;  // capabilities available to the robot
  bool save_exec = false;           // persist generated BT XMLs to disk
  std::string exec_dir = "exec";    // base directory for execution logs
  std::string mission_name;         // human-readable mission identifier
  bool restart_after_forced = true; // restart from step 0 on FORCED_FAILURE (vs replan)
};

/**
 * @brief Parse a YAML config file into an ActionConfig struct.
 * @throws std::runtime_error on missing required fields or YAML errors.
 */
inline ActionConfig parse_config(const std::string & config_file)
{
  ActionConfig config;
  try {
    YAML::Node yaml = YAML::LoadFile(config_file);

    if (yaml["node_name"]) {
      config.node_name = yaml["node_name"].as<std::string>();
    }
    if (!yaml["orchestrator_type"]) {
      throw std::runtime_error("Missing required field: orchestrator_type");
    }
    config.orchestrator_type = yaml["orchestrator_type"].as<std::string>();
    // Keep legacy aliases working while using a short canonical MCP type.
    if (
      config.orchestrator_type == "mcp_llm" ||
      config.orchestrator_type == "mcp_llm_plan_orchestrator")
    {
      config.orchestrator_type = "mcp";
    }

    if (yaml["package_name"]) {
      config.package_name = yaml["package_name"].as<std::string>();
    }
    if (yaml["orchestrator_libraries"]) {
      for (const auto & lib : yaml["orchestrator_libraries"]) {
        config.orchestrator_libraries.push_back(lib.as<std::string>());
      }
    }
    if (yaml["plugin_libraries"]) {
      for (const auto & p : yaml["plugin_libraries"]) {
        config.plugin_libraries.push_back(p.as<std::string>());
      }
    }
    if (yaml["bt_nodes_packages"]) {
      for (const auto & pkg : yaml["bt_nodes_packages"]) {
        config.bt_nodes_packages.push_back(pkg.as<std::string>());
      }
    } else if (yaml["bt_nodes_package"]) {
      config.bt_nodes_packages.push_back(yaml["bt_nodes_package"].as<std::string>());
    }

    if (is_llm_orchestrator_type(config.orchestrator_type)) {
      if (yaml["bt_control_period_ms"]) {
        config.bt_control_period_ms = yaml["bt_control_period_ms"].as<int>();
      }
      if (yaml["bt_timeout_sec"]) {
        config.bt_timeout_sec = yaml["bt_timeout_sec"].as<double>();
      }
      if (yaml["skills"]) {
        for (const auto & s : yaml["skills"]) {
          config.skills.push_back(s.as<std::string>());
        }
      }
      if (yaml["save_exec"]) {
        config.save_exec = yaml["save_exec"].as<bool>();
      }
      if (yaml["exec_dir"]) {
        config.exec_dir = yaml["exec_dir"].as<std::string>();
      }
      if (yaml["mission_name"]) {
        config.mission_name = yaml["mission_name"].as<std::string>();
      }
      if (yaml["restart_after_forced"]) {
        config.restart_after_forced = yaml["restart_after_forced"].as<bool>();
      }
    } else {
      if (!yaml["behaviors"]) {
        throw std::runtime_error("Missing required field: behaviors");
      }
      for (const auto & node : yaml["behaviors"]) {
        BehaviorConfig bc;
        if (!node["name"]) {
          throw std::runtime_error("Behavior missing required field: name");
        }
        bc.name = node["name"].as<std::string>();
        if (!node["behavior_file"]) {
          throw std::runtime_error("Behavior missing required field: behavior_file");
        }
        bc.behavior_file = node["behavior_file"].as<std::string>();
        if (node["control_period_ms"]) {
          bc.control_period_ms = node["control_period_ms"].as<int>();
        }
        if (node["package_name"]) {
          bc.package_name = node["package_name"].as<std::string>();
        }
        config.behaviors.push_back(bc);
      }
    }
  } catch (const YAML::Exception & e) {
    throw std::runtime_error("YAML parsing error: " + std::string(e.what()));
  }
  return config;
}

/**
 * @brief Store a parsed ActionConfig into a BT blackboard.
 *
 * Fixed orchestrators read "behaviors_config", "plugin_libraries", and "package_name".
 * LLM orchestrator reads "llm_plugin_libraries", "llm_bt_nodes_packages", etc.
 */
inline void setup_blackboard_from_config(
  BT::Blackboard::Ptr blackboard,
  const ActionConfig & config)
{
  if (is_llm_orchestrator_type(config.orchestrator_type)) {
    blackboard->set<std::vector<std::string>>("llm_plugin_libraries", config.plugin_libraries);
    blackboard->set<std::vector<std::string>>("llm_bt_nodes_packages", config.bt_nodes_packages);
    blackboard->set<int>("llm_control_period_ms", config.bt_control_period_ms);
    blackboard->set<double>("llm_timeout_sec", config.bt_timeout_sec);
    blackboard->set<std::vector<std::string>>("llm_skills", config.skills);
    blackboard->set<bool>("llm_save_exec", config.save_exec);
    blackboard->set<std::string>("llm_exec_dir", config.exec_dir);
    blackboard->set<std::string>("llm_mission_name", config.mission_name);
    blackboard->set<bool>("llm_restart_after_forced", config.restart_after_forced);
  } else {
    blackboard->set<std::vector<BehaviorConfig>>("behaviors_config", config.behaviors);
    blackboard->set<std::vector<std::string>>("plugin_libraries", config.plugin_libraries);
    blackboard->set<std::string>("package_name", config.package_name);
  }
}

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__CONFIG_PARSER_HPP_
