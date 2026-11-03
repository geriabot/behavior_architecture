// Copyright 2026 Rodrigo Pérez-Rodríguez
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

#ifndef BEHAVIOR_ARCHITECTURE__EPISODIC_MEMORY_FILES_HPP_
#define BEHAVIOR_ARCHITECTURE__EPISODIC_MEMORY_FILES_HPP_

#include <filesystem>
#include <system_error>

namespace behavior_architecture
{

/// The files of the episodic memory of MCPLLMPlanOrchestrator, in its context directory
inline constexpr char PLAN_SUCCESS_MEMORY_FILE[] = "plan_success_episodic_memory.json";
inline constexpr char PLAN_FAILURE_MEMORY_FILE[] = "plan_failure_episodic_memory.json";
inline constexpr char BT_SUCCESS_MEMORY_FILE[] = "bt_success_episodic_memory.json";
inline constexpr char BT_FAILURE_MEMORY_FILE[] = "bt_failure_episodic_memory.json";
/// The former name of PLAN_SUCCESS_MEMORY_FILE, still read
inline constexpr char LEGACY_PLAN_SUCCESS_MEMORY_FILE[] = "plan__success_episodic_memory.json";

/**
 * @brief The file to read a memory from: `file` if it exists, else `legacy` if it exists
 * (a file written under a former name), else `file`
 */
inline std::filesystem::path
readable_memory_file(const std::filesystem::path & file, const std::filesystem::path & legacy)
{
  std::error_code ec;
  if (!std::filesystem::exists(file, ec) && std::filesystem::exists(legacy, ec)) {
    return legacy;
  }
  return file;
}

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__EPISODIC_MEMORY_FILES_HPP_
