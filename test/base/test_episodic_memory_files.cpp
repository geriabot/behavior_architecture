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

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "behavior_architecture/episodic_memory_files.hpp"

namespace fs = std::filesystem;
using behavior_architecture::LEGACY_PLAN_SUCCESS_MEMORY_FILE;
using behavior_architecture::PLAN_SUCCESS_MEMORY_FILE;
using behavior_architecture::readable_memory_file;

TEST(EpisodicMemoryFiles, ReadsTheFormerNameOnlyWhenTheFileIsMissing)
{
  const auto dir = fs::temp_directory_path() /
    ("behavior_architecture_memory_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const auto file = dir / PLAN_SUCCESS_MEMORY_FILE;
  const auto legacy = dir / LEGACY_PLAN_SUCCESS_MEMORY_FILE;
  EXPECT_EQ(std::string(PLAN_SUCCESS_MEMORY_FILE), "plan_success_episodic_memory.json");
  EXPECT_EQ(std::string(LEGACY_PLAN_SUCCESS_MEMORY_FILE), "plan__success_episodic_memory.json");

  // Neither exists: the file, which is written under its name
  EXPECT_EQ(readable_memory_file(file, legacy), file);
  // Only the former name exists: it is read
  std::ofstream(legacy) << "[]";
  EXPECT_EQ(readable_memory_file(file, legacy), legacy);
  // Both exist: the file
  std::ofstream(file) << "[]";
  EXPECT_EQ(readable_memory_file(file, legacy), file);
  fs::remove_all(dir);
}
