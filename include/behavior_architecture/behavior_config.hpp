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

#ifndef BEHAVIOR_ARCHITECTURE__BEHAVIOR_CONFIG_HPP_
#define BEHAVIOR_ARCHITECTURE__BEHAVIOR_CONFIG_HPP_

#include <string>
#include <vector>

namespace behavior_architecture
{

/**
 * @brief Configuration for a single behavior runner.
 *
 * Mirrors the per-entry structure in the YAML config file:
 *
 *   behaviors:
 *     - name: "follow_behavior"
 *       behavior_file: "behaviors/reusable/follow_behavior.xml"
 *       package_name: ""          # optional — falls back to global package_name
 *       control_period_ms: 50
 */
struct BehaviorConfig
{
  std::string name;
  std::string behavior_file;
  std::string package_name;       ///< Leave empty to inherit global package_name
  int control_period_ms = 50;
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__BEHAVIOR_CONFIG_HPP_
