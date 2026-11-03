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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "behaviortree_cpp/bt_factory.h"
#include "nlohmann/json.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "fake_interpretation_server.hpp"

using namespace std::chrono_literals;

namespace
{

class InterpretBTNodesTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    node_ = std::make_shared<rclcpp::Node>("bt_node", "/bt_test_a");
    spinner_ = std::make_unique<interpretation_test::Spinner>(node_);
    server_ = std::make_unique<interpretation_test::FakeInterpretationServer>("/bt_test_a");
    // The plugin, loaded as the behavior runners load plugin_libraries
    factory_.registerFromPlugin(INTERPRETATION_BT_PLUGIN);
    blackboard_ = BT::Blackboard::create();
    blackboard_->set("node", node_);
  }

  void TearDown() override
  {
    server_.reset();
    spinner_.reset();
  }

  /// Ticks the tree (every 10 ms, as a runner does) until it ends; how many ticks it took
  BT::NodeStatus run(const std::string & xml, int * ticks = nullptr)
  {
    auto tree = factory_.createTreeFromText(xml, blackboard_);
    BT::NodeStatus status = BT::NodeStatus::RUNNING;
    int count = 0;
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (status == BT::NodeStatus::RUNNING && std::chrono::steady_clock::now() < end) {
      status = tree.tickOnce();
      ++count;
      if (status == BT::NodeStatus::RUNNING) {
        std::this_thread::sleep_for(10ms);
      }
    }
    if (ticks) {
      *ticks = count;
    }
    return status;
  }

  static std::string tree(const std::string & body)
  {
    return "<root BTCPP_format=\"4\"><BehaviorTree ID=\"Main\">" + body +
           "</BehaviorTree></root>";
  }

  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<interpretation_test::Spinner> spinner_;
  std::unique_ptr<interpretation_test::FakeInterpretationServer> server_;
  BT::BehaviorTreeFactory factory_;
  BT::Blackboard::Ptr blackboard_;
};

}  // namespace

TEST_F(InterpretBTNodesTest, WaitsWithoutBlockingTheTree)
{
  blackboard_->set("ctx", std::string("{\"task\": \"cup\"}"));
  blackboard_->set("frames", std::vector<sensor_msgs::msg::Image>(2));
  int ticks = 0;
  const auto status = run(
    tree(
      "<Interpret question=\"echo\" context=\"{ctx}\" text=\"hi\" images=\"{frames}\" "
      "budget_s=\"1.5\" variant=\"v\" answer=\"{answer}\" source=\"{source}\"/>"), &ticks);
  EXPECT_EQ(status, BT::NodeStatus::SUCCESS);
  EXPECT_GE(ticks, 1);
  EXPECT_EQ(blackboard_->get<std::string>("source"), "llm");
  const auto answer = nlohmann::json::parse(blackboard_->get<std::string>("answer"));
  EXPECT_EQ(answer["context"], "{\"task\": \"cup\"}");
  EXPECT_EQ(answer["text"], "hi");
  EXPECT_EQ(answer["images"], 2);
  EXPECT_EQ(answer["variant"], "v");
  EXPECT_DOUBLE_EQ(answer["budget_s"].get<double>(), 1.5);
}

TEST_F(InterpretBTNodesTest, DefaultOutputKeysArePrefixed)
{
  EXPECT_EQ(run(tree("<Interpret question=\"choose\" text=\"4\"/>")), BT::NodeStatus::SUCCESS);
  EXPECT_EQ(blackboard_->get<std::string>("interpretation_answer"), "{\"behavior_id\":4}");
  EXPECT_EQ(blackboard_->get<std::string>("interpretation_source"), "llm");
}

TEST_F(InterpretBTNodesTest, InvalidUnknownAndMissing)
{
  EXPECT_EQ(run(tree("<Interpret question=\"invalid\"/>")), BT::NodeStatus::FAILURE);
  EXPECT_EQ(blackboard_->get<std::string>("interpretation_source"), "invalid");
  EXPECT_EQ(blackboard_->get<std::string>("interpretation_fallback_reason"), "timeout");
  EXPECT_EQ(
    run(tree("<Interpret question=\"invalid\" fail_on_invalid=\"false\"/>")),
    BT::NodeStatus::SUCCESS);
  EXPECT_EQ(run(tree("<Interpret question=\"unknown\"/>")), BT::NodeStatus::FAILURE);
  EXPECT_EQ(run(tree("<Interpret question=\"\"/>")), BT::NodeStatus::FAILURE);
  EXPECT_EQ(
    run(
      tree(
        "<Interpret question=\"echo\" service=\"/nobody/interpretation/interpret\" "
        "timeout_s=\"0.2\"/>")),
    BT::NodeStatus::FAILURE);
}

TEST_F(InterpretBTNodesTest, AskWithoutWaitingAndPickUpLater)
{
  int ticks = 0;
  const auto status = run(
    tree(
      "<Sequence>"
      "  <Interpret question=\"choose\" text=\"6\" wait=\"false\"/>"
      "  <InterpretationResult question=\"choose\" answer=\"{feature}\"/>"
      "</Sequence>"), &ticks);
  EXPECT_EQ(status, BT::NodeStatus::SUCCESS);
  EXPECT_EQ(blackboard_->get<std::string>("feature"), "{\"behavior_id\":6}");

  // A key of its own; and InterpretationResult wait=false before the answer arrives
  EXPECT_EQ(
    run(
      tree(
        "<Sequence>"
        "  <Interpret question=\"slow\" wait=\"false\" result_key=\"interpretation_q5\"/>"
        "  <InterpretationResult result_key=\"interpretation_q5\" wait=\"false\"/>"
        "</Sequence>")),
    BT::NodeStatus::FAILURE);
  EXPECT_EQ(
    run(tree("<InterpretationResult result_key=\"interpretation_q5\"/>")),
    BT::NodeStatus::SUCCESS);
  EXPECT_EQ(blackboard_->get<std::string>("interpretation_answer"), "{}");

  // Nothing asked under that key
  EXPECT_EQ(run(tree("<InterpretationResult question=\"never\"/>")), BT::NodeStatus::FAILURE);
}

TEST_F(InterpretBTNodesTest, HaltCancels)
{
  // The timeout halts the Interpret that waits for the slow answer
  EXPECT_EQ(
    run(
      tree(
        "<Timeout msec=\"100\"><Interpret question=\"slow\"/></Timeout>")),
    BT::NodeStatus::FAILURE);
}

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
