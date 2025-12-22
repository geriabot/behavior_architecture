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

#include "behavior_architecture/orchestrator_factory.hpp"
#include "rclcpp/rclcpp.hpp"

namespace behavior_architecture
{

std::map<std::string, OrchestratorFactory::OrchestratorCreator> &
OrchestratorFactory::get_registry()
{
  static std::map<std::string, OrchestratorCreator> registry;
  return registry;
}

void
OrchestratorFactory::register_orchestrator(
  const std::string & type_name,
  OrchestratorCreator creator)
{
  get_registry()[type_name] = creator;
  RCLCPP_INFO(
    rclcpp::get_logger("orchestrator_factory"),
    "Registered orchestrator type: %s", type_name.c_str());
}

std::shared_ptr<BaseOrchestrator>
OrchestratorFactory::create_orchestrator(
  const std::string & type_name,
  BT::Blackboard::Ptr blackboard)
{
  auto & registry = get_registry();
  auto it = registry.find(type_name);
  
  if (it != registry.end()) {
    RCLCPP_INFO(
      rclcpp::get_logger("orchestrator_factory"),
      "Creating orchestrator of type: %s", type_name.c_str());
    return it->second(blackboard);
  }
  
  RCLCPP_ERROR(
    rclcpp::get_logger("orchestrator_factory"),
    "Orchestrator type '%s' not found in registry", type_name.c_str());
  return nullptr;
}

bool
OrchestratorFactory::is_registered(const std::string & type_name)
{
  return get_registry().find(type_name) != get_registry().end();
}

std::vector<std::string>
OrchestratorFactory::get_registered_types()
{
  std::vector<std::string> types;
  for (const auto & entry : get_registry()) {
    types.push_back(entry.first);
  }
  return types;
}

}  // namespace behavior_architecture
