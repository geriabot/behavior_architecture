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

#ifndef BEHAVIOR_ARCHITECTURE__ORCHESTRATOR_FACTORY_HPP_
#define BEHAVIOR_ARCHITECTURE__ORCHESTRATOR_FACTORY_HPP_

#include <memory>
#include <string>
#include <map>
#include <functional>
#include "behavior_architecture/base_orchestrator.hpp"
#include "behaviortree_cpp/blackboard.h"

namespace behavior_architecture
{

/**
 * @brief Factory for creating orchestrator instances from string identifiers
 * 
 * This factory allows for dynamic creation of orchestrators based on configuration,
 * supporting the generic action_executor pattern.
 */
class OrchestratorFactory
{
public:
  using OrchestratorCreator = std::function<std::shared_ptr<BaseOrchestrator>(BT::Blackboard::Ptr)>;

  /**
   * @brief Register a new orchestrator type
   * @param type_name Unique identifier for the orchestrator type
   * @param creator Function that creates an instance of the orchestrator
   */
  static void register_orchestrator(
    const std::string & type_name,
    OrchestratorCreator creator);

  /**
   * @brief Create an orchestrator instance
   * @param type_name The type of orchestrator to create
   * @param blackboard Shared blackboard for the orchestrator
   * @return Shared pointer to the created orchestrator, or nullptr if type not found
   */
  static std::shared_ptr<BaseOrchestrator> create_orchestrator(
    const std::string & type_name,
    BT::Blackboard::Ptr blackboard);

  /**
   * @brief Check if an orchestrator type is registered
   * @param type_name The type name to check
   * @return true if the type is registered, false otherwise
   */
  static bool is_registered(const std::string & type_name);

  /**
   * @brief Get list of all registered orchestrator types
   * @return Vector of registered type names
   */
  static std::vector<std::string> get_registered_types();

private:
  static std::map<std::string, OrchestratorCreator> & get_registry();
};

/**
 * @brief Template helper class for automatic orchestrator registration
 * 
 * This class uses static initialization to automatically register orchestrators
 * with the factory when the program starts.
 * 
 * Usage in your orchestrator's .cpp file:
 * static OrchestratorRegistrar<SimpleOrchestrator> simple_registrar("simple");
 */
template<typename T>
class OrchestratorRegistrar
{
public:
  explicit OrchestratorRegistrar(const std::string & type_name)
  {
    OrchestratorFactory::register_orchestrator(
      type_name,
      [](BT::Blackboard::Ptr blackboard) -> std::shared_ptr<BaseOrchestrator> {
        return std::make_shared<T>(blackboard);
      });
  }
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__ORCHESTRATOR_FACTORY_HPP_
