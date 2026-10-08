// Copyright 2026 Rodrigo Perez-Rodriguez
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

#include "behavior_architecture/test_plan_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <behaviortree_cpp/bt_factory.h>

namespace behavior_architecture {

// Register the test orchestrator
static OrchestratorRegistrar<TestPlanOrchestrator> test_registrar("test_plan");

TestPlanOrchestrator::TestPlanOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("test_plan_orchestrator", blackboard) {}


void TestPlanOrchestrator::run_plan_from_dir(const std::string& plan_dir) {
    bt_files_.clear();
    RCLCPP_INFO(get_logger(), "Using plan_dir: %s", plan_dir.c_str());
    if (!std::filesystem::exists(plan_dir)) {
        RCLCPP_ERROR(get_logger(), "Directory does not exist: %s", plan_dir.c_str());
    }
    for (const auto& entry : std::filesystem::directory_iterator(plan_dir)) {
        if (entry.path().extension() == ".xml") {
            RCLCPP_INFO(get_logger(), "Found BT XML: %s", entry.path().c_str());
            bt_files_.push_back(entry.path());
        }
    }
    RCLCPP_INFO(get_logger(), "Total BT XML files found: %zu", bt_files_.size());
    // Sort BT files alphabetically by filename
    std::sort(bt_files_.begin(), bt_files_.end(), [](const auto& a, const auto& b) {
        return a.filename() < b.filename();
    });
    current_bt_idx_ = 0;

    // Load plugin libraries (default: social_bt_nodes, can be extended)
    try {
        plugin_libraries_ = blackboard_->get<std::vector<std::string>>("llm_plugin_libraries");
    } catch (...) {
        plugin_libraries_ = {"libsocial_bt_nodes_plugin.so"};
    }

    // Register a single reusable runner
    auto runner = std::make_shared<BehaviorRunner>(
        blackboard_,
        "test_bt_runner",
        "", // empty xml_path, we will use set_bt
        plugin_libraries_,
        "behavior_architecture",
        10
    );
    register_runner("test_bt_runner", runner);
    state_ = State::LOADING_BT;
}

void TestPlanOrchestrator::control_cycle() {
    if (state_ == State::IDLE || bt_files_.empty()) {
        RCLCPP_INFO(get_logger(), "State is IDLE or no BT files to execute. Exiting control_cycle.");
        return;
    }
    auto runner_base = get_runners().front();
    auto runner = std::dynamic_pointer_cast<BehaviorRunner>(runner_base);
    RCLCPP_INFO(get_logger(), "Entering control_cycle. State: %d, current_bt_idx_: %zu", static_cast<int>(state_), current_bt_idx_);
    switch (state_) {
        case State::LOADING_BT: {
            if (current_bt_idx_ >= bt_files_.size()) {
                state_ = State::SUCCESS;
                RCLCPP_INFO(get_logger(), "All behavior trees executed successfully.");
                return;
            }
            RCLCPP_INFO(get_logger(), "Loading BT file: %s", bt_files_[current_bt_idx_].c_str());
            // Load the XML and set it in the runner
            std::ifstream bt_file(bt_files_[current_bt_idx_]);
            if (!bt_file.is_open()) {
                RCLCPP_ERROR(get_logger(), "Could not open behavior tree file: %s", bt_files_[current_bt_idx_].c_str());
                state_ = State::FAILURE;
                return;
            }
            std::string bt_xml((std::istreambuf_iterator<char>(bt_file)), std::istreambuf_iterator<char>());
            runner->set_bt(bt_xml);
            runner->refresh();
            RCLCPP_INFO(get_logger(), "Triggering CONFIGURE transition.");
            runner->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
            RCLCPP_INFO(get_logger(), "Triggering ACTIVATE transition.");
            runner->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);
            RCLCPP_INFO(get_logger(), "Executing behavior tree: %s", bt_files_[current_bt_idx_].filename().c_str());
            state_ = State::EXECUTING_BT;
            break;
        }
        case State::EXECUTING_BT: {
            BT::NodeStatus status = runner->get_bt_status();
            RCLCPP_INFO(get_logger(), "BT status: %s", BT::toStr(status).c_str());
            if (status == BT::NodeStatus::RUNNING) {
                RCLCPP_INFO(get_logger(), "BT is still running. Waiting for next cycle.");
                return;
            }
            RCLCPP_INFO(get_logger(), "Behavior tree finished with status: %s", BT::toStr(status).c_str());
            RCLCPP_INFO(get_logger(), "Triggering DEACTIVATE transition.");
            runner->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_DEACTIVATE);
            runner->refresh();
            if (status == BT::NodeStatus::SUCCESS) {
                RCLCPP_INFO(get_logger(), "BT succeeded. Moving to next BT.");
                current_bt_idx_++;
                state_ = State::LOADING_BT;
            } else {
                RCLCPP_ERROR(get_logger(), "Behavior tree failed: %s", bt_files_[current_bt_idx_].c_str());
                state_ = State::FAILURE;
            }
            break;
        }
        case State::SUCCESS:
            RCLCPP_INFO(get_logger(), "State: SUCCESS. All BTs completed.");
            break;
        case State::FAILURE:
            RCLCPP_ERROR(get_logger(), "State: FAILURE. Execution stopped.");
            break;
        case State::IDLE:
        default:
            RCLCPP_INFO(get_logger(), "State: IDLE or unknown. No action taken.");
            break;
    }
}

void TestPlanOrchestrator::go_to_state(int state) {
    // No-op for this simple orchestrator
}

} // namespace behavior_architecture
