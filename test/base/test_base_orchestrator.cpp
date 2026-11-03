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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "behavior_architecture/base_orchestrator.hpp"
#include "behavior_architecture/behavior_config.hpp"
#include "behavior_architecture/behavior_runner.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "lifecycle_msgs/msg/transition.hpp"
#include "rclcpp/rclcpp.hpp"

using namespace std::chrono_literals;
using behavior_architecture::BehaviorConfig;
using behavior_architecture::BehaviorRunner;
using lifecycle_msgs::msg::State;
using lifecycle_msgs::msg::Transition;

namespace
{

const char * const kValid =
  "<root BTCPP_format=\"4\"><BehaviorTree ID=\"Greet\"><Sleep msec=\"20\"/></BehaviorTree>"
  "</root>";
const char * const kFailsAtOnce =
  "<root BTCPP_format=\"4\"><BehaviorTree ID=\"Fail\"><AlwaysFailure/></BehaviorTree></root>";
// The tree's ID is the ID of a built-in node
const char * const kTreeIdIsNodeId =
  "<root BTCPP_format=\"4\"><BehaviorTree ID=\"Sleep\"><Sleep msec=\"20\"/></BehaviorTree>"
  "</root>";

/// An orchestrator whose protected members the tests use; it has no FSM
class ProbeOrchestrator : public behavior_architecture::BaseOrchestrator
{
public:
  explicit ProbeOrchestrator(BT::Blackboard::Ptr blackboard)
  : BaseOrchestrator("probe_orchestrator", blackboard) {}

  using BaseOrchestrator::activate_runner;
  using BaseOrchestrator::check_behavior_finished;
  using BaseOrchestrator::deactivate_all_runners;
  using BaseOrchestrator::last_status_;
  using BaseOrchestrator::runners_;

protected:
  void control_cycle() override {}
  void go_to_state(int) override {}
};

class BaseOrchestratorTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    dir_ = std::filesystem::temp_directory_path() /
      ("behavior_architecture_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir_);
    blackboard_ = BT::Blackboard::create();
  }

  void TearDown() override
  {
    std::filesystem::remove_all(dir_);
  }

  std::string write(const std::string & name, const std::string & xml)
  {
    const auto path = dir_ / name;
    std::ofstream(path) << xml;
    return path.string();
  }

  std::shared_ptr<BehaviorRunner> runner(const std::string & name, const std::string & file)
  {
    auto r = std::make_shared<BehaviorRunner>(
      blackboard_, name, file, std::vector<std::string>{}, "behavior_architecture", 10);
    r->set_parameter(rclcpp::Parameter("enable_groot_monitoring", false));
    return r;
  }

  /// Spins the nodes, in this thread, until `done` or a timeout
  template<typename Done>
  bool spin_until(rclcpp::Executor & executor, Done done, std::chrono::milliseconds timeout = 5s)
  {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      executor.spin_some(10ms);
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  std::filesystem::path dir_;
  BT::Blackboard::Ptr blackboard_;
};

}  // namespace

TEST(TreeIdsUsedByNodes, NamesTheConflictingIds)
{
  BT::BehaviorTreeFactory factory;
  EXPECT_EQ(
    behavior_architecture::tree_ids_used_by_nodes(kTreeIdIsNodeId, factory),
    std::vector<std::string>{"Sleep"});
  EXPECT_TRUE(behavior_architecture::tree_ids_used_by_nodes(kValid, factory).empty());
  EXPECT_EQ(
    behavior_architecture::tree_ids_used_by_nodes(
      "<root><BehaviorTree ID='Main'><SubTree ID=\"Delay\"/></BehaviorTree>"
      "<BehaviorTree ID=\"Delay\"><AlwaysSuccess/></BehaviorTree></root>", factory),
    std::vector<std::string>{"Delay"});
}

TEST_F(BaseOrchestratorTest, ValidTreeActivatesAndRuns)
{
  auto r = runner("valid_runner", write("valid.xml", kValid));
  r->trigger_transition(Transition::TRANSITION_CONFIGURE);
  EXPECT_EQ(
    r->trigger_transition(Transition::TRANSITION_ACTIVATE).id(),
    State::PRIMARY_STATE_ACTIVE);
  EXPECT_EQ(r->activation_error(), "");
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(r->get_node_base_interface());
  EXPECT_TRUE(spin_until(executor, [&]() {return r->get_bt_status() == BT::NodeStatus::SUCCESS;}));
  r->trigger_transition(Transition::TRANSITION_DEACTIVATE);
}

TEST_F(BaseOrchestratorTest, TreeIdEqualToANodeIdFailsTheActivationWithItsName)
{
  const auto file = write("conflict.xml", kTreeIdIsNodeId);
  auto r = runner("conflict_runner", file);
  r->trigger_transition(Transition::TRANSITION_CONFIGURE);
  EXPECT_EQ(
    r->trigger_transition(Transition::TRANSITION_ACTIVATE).id(), State::PRIMARY_STATE_INACTIVE);
  EXPECT_NE(r->activation_error().find("'Sleep'"), std::string::npos) << r->activation_error();
  EXPECT_NE(r->activation_error().find(file), std::string::npos) << r->activation_error();

  // The runner can be activated again, for example with a corrected tree
  r->set_bt(kValid);
  EXPECT_EQ(
    r->trigger_transition(Transition::TRANSITION_ACTIVATE).id(),
    State::PRIMARY_STATE_ACTIVE);
  EXPECT_EQ(r->activation_error(), "");
  r->trigger_transition(Transition::TRANSITION_DEACTIVATE);
}

TEST_F(BaseOrchestratorTest, OtherBuildErrorsFailTheActivation)
{
  auto missing = runner("missing_runner", (dir_ / "missing.xml").string());
  missing->trigger_transition(Transition::TRANSITION_CONFIGURE);
  EXPECT_NE(
    missing->trigger_transition(Transition::TRANSITION_ACTIVATE).id(), State::PRIMARY_STATE_ACTIVE);
  EXPECT_NE(missing->activation_error().find("missing.xml"), std::string::npos);

  auto unknown = runner(
    "unknown_runner",
    write(
      "unknown.xml", "<root BTCPP_format=\"4\"><BehaviorTree ID=\"T\"><NoSuchNode/>"
      "</BehaviorTree></root>"));
  unknown->trigger_transition(Transition::TRANSITION_CONFIGURE);
  EXPECT_NE(
    unknown->trigger_transition(Transition::TRANSITION_ACTIVATE).id(), State::PRIMARY_STATE_ACTIVE);
  EXPECT_NE(unknown->activation_error().find("NoSuchNode"), std::string::npos);
}

TEST_F(BaseOrchestratorTest, AFailedActivationIsReportedAsFailure)
{
  auto orchestrator = std::make_shared<ProbeOrchestrator>(blackboard_);
  orchestrator->register_runner("bad", runner("bad", write("bad.xml", kTreeIdIsNodeId)));
  orchestrator->register_runner("fails", runner("fails", write("fails.xml", kFailsAtOnce)));
  orchestrator->register_runner("good", runner("good", write("good.xml", kValid)));
  orchestrator->trigger_transition(Transition::TRANSITION_CONFIGURE);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(orchestrator->get_node_base_interface());
  for (auto & r : orchestrator->get_runners()) {
    executor.add_node(r->get_node_base_interface());
  }
  // Let the subscription to /behavior_status match the runners' publishers
  spin_until(executor, []() {return false;}, 500ms);

  EXPECT_FALSE(orchestrator->activate_runner("bad"));
  EXPECT_TRUE(orchestrator->check_behavior_finished());
  EXPECT_EQ(orchestrator->last_status_, "FAILURE");
  EXPECT_FALSE(orchestrator->check_behavior_finished());

  // A behavior activated next that fails on its first tick, without RUNNING, is still reported
  orchestrator->deactivate_all_runners();
  EXPECT_TRUE(orchestrator->activate_runner("fails"));
  EXPECT_TRUE(spin_until(executor, [&]() {return orchestrator->check_behavior_finished();}));
  EXPECT_EQ(orchestrator->last_status_, "FAILURE");

  orchestrator->deactivate_all_runners();
  EXPECT_TRUE(orchestrator->activate_runner("good"));
  EXPECT_TRUE(spin_until(executor, [&]() {return orchestrator->check_behavior_finished();}));
  EXPECT_EQ(orchestrator->last_status_, "SUCCESS");
  orchestrator->deactivate_all_runners();

  EXPECT_FALSE(orchestrator->activate_runner("nobody"));
  EXPECT_FALSE(orchestrator->check_behavior_finished());
}

TEST_F(BaseOrchestratorTest, ConfigurationBehaviorsWithoutARunnerRegisteredInCodeGetOne)
{
  blackboard_->set(
    "behaviors_config", std::vector<BehaviorConfig>{
    {"in_code", write("a.xml", kValid), "", 50},
    {"from_yaml", write("b.xml", kValid), "", 50}});
  blackboard_->set("plugin_libraries", std::vector<std::string>{});
  auto orchestrator = std::make_shared<ProbeOrchestrator>(blackboard_);
  auto registered = runner("in_code", write("c.xml", kValid));
  orchestrator->register_runner("in_code", registered);
  orchestrator->trigger_transition(Transition::TRANSITION_CONFIGURE);

  ASSERT_EQ(orchestrator->runners_.size(), 2u);
  EXPECT_EQ(orchestrator->runners_.at("in_code"), registered);
  ASSERT_EQ(orchestrator->runners_.count("from_yaml"), 1u);
  EXPECT_EQ(
    orchestrator->runners_.at("from_yaml")->get_current_state().id(),
    State::PRIMARY_STATE_INACTIVE);
}

TEST_F(BaseOrchestratorTest, EveryRunnerRegisteredInCodeCreatesNone)
{
  // As dummy_robot does: the runners of the configuration, registered in code
  blackboard_->set(
    "behaviors_config", std::vector<BehaviorConfig>{
    {"state1_runner", write("a.xml", kValid), "", 50},
    {"state2_runner", write("b.xml", kValid), "", 50}});
  blackboard_->set("plugin_libraries", std::vector<std::string>{});
  auto orchestrator = std::make_shared<ProbeOrchestrator>(blackboard_);
  auto first = runner("state1_runner", write("a.xml", kValid));
  auto second = runner("state2_runner", write("b.xml", kValid));
  orchestrator->register_runner("state1_runner", first);
  orchestrator->register_runner("state2_runner", second);
  orchestrator->trigger_transition(Transition::TRANSITION_CONFIGURE);

  ASSERT_EQ(orchestrator->runners_.size(), 2u);
  EXPECT_EQ(orchestrator->runners_.at("state1_runner"), first);
  EXPECT_EQ(orchestrator->runners_.at("state2_runner"), second);
}

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
