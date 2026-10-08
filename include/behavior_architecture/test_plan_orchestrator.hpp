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

#ifndef BEHAVIOR_ARCHITECTURE__TEST_PLAN_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__TEST_PLAN_ORCHESTRATOR_HPP_

#include "behavior_architecture/base_orchestrator.hpp"
#include <string>

namespace behavior_architecture {



class TestPlanOrchestrator : public BaseOrchestrator {
public:
    enum class State {
        IDLE,
        LOADING_BT,
        EXECUTING_BT,
        SUCCESS,
        FAILURE
    };

    explicit TestPlanOrchestrator(BT::Blackboard::Ptr blackboard);
    void run_plan_from_dir(const std::string& plan_dir);
    void control_cycle() override;
    void go_to_state(int state) override;

private:
    std::vector<std::filesystem::path> bt_files_;
    size_t current_bt_idx_ = 0;
    State state_ = State::IDLE;
    std::vector<std::string> plugin_libraries_;
};

} // namespace behavior_architecture

#endif // BEHAVIOR_ARCHITECTURE__TEST_PLAN_ORCHESTRATOR_HPP_
