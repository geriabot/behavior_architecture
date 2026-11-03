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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "behavior_architecture/interpretation/interpretation_client.hpp"
#include "fake_interpretation_server.hpp"

using behavior_architecture::interpretation::BehaviorChoice;
using behavior_architecture::interpretation::CancellationHandle;
using behavior_architecture::interpretation::InterpretationClient;
using behavior_architecture::interpretation::InterpretationHandle;
using behavior_architecture::interpretation::InterpretationRequest;
using behavior_architecture::interpretation::InterpretationResult;
using behavior_architecture::interpretation::OfferedBehavior;
using Status = InterpretationHandle::Status;
using namespace std::chrono_literals;

namespace
{

/// Polls as a control cycle would (every 10 ms) until the handle is ready, or the limit
template<typename HandleT>
Status wait_ready(HandleT & handle, std::chrono::milliseconds limit = 5000ms)
{
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    const auto status = handle.poll();
    if (status != Status::PENDING) {
      return status;
    }
    std::this_thread::sleep_for(10ms);
  }
  return Status::PENDING;
}

class InterpretationClientTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    node_ = std::make_shared<rclcpp::Node>("orchestrator", "/client_test_a");
    spinner_ = std::make_unique<interpretation_test::Spinner>(node_);
  }

  void TearDown() override
  {
    spinner_.reset();
    node_.reset();
  }

  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<interpretation_test::Spinner> spinner_;
};

}  // namespace

TEST_F(InterpretationClientTest, AsksWithoutBlockingInTheNodesNamespace)
{
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  InterpretationClient client(node_);
  EXPECT_EQ(client.service_name(), "/client_test_a/interpretation/interpret");

  InterpretationRequest request;
  request.question = "echo";
  request.context = "{\"task\": \"cup\"}";
  request.text = "hello";
  request.images.resize(2);
  request.budget_s = 2.5;
  request.variant = "no_image";
  request.max_tokens = 64;
  const auto start = std::chrono::steady_clock::now();
  auto handle = client.ask(request);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);  // ask() does not wait
  ASSERT_EQ(wait_ready(handle), Status::DONE);

  const auto result = handle.result();
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.valid());
  EXPECT_EQ(result.source, "llm");
  const auto answer = nlohmann::json::parse(result.answer);
  EXPECT_EQ(answer["context"], "{\"task\": \"cup\"}");
  EXPECT_EQ(answer["text"], "hello");
  EXPECT_EQ(answer["images"], 2);
  EXPECT_EQ(answer["variant"], "no_image");
  EXPECT_DOUBLE_EQ(answer["budget_s"].get<double>(), 2.5);
  EXPECT_EQ(answer["max_tokens"], 64);
  // A copy of the handle shares the question
  InterpretationHandle copy = handle;
  EXPECT_TRUE(copy.ready());
}

TEST_F(InterpretationClientTest, OtherNamespaceIsAnotherServer)
{
  interpretation_test::FakeInterpretationServer server("/client_test_b");
  InterpretationClient client(node_, InterpretationClient::DEFAULT_SERVICE, 300ms);
  auto handle = client.ask("echo", "{}");
  EXPECT_EQ(wait_ready(handle), Status::FAILED);
  EXPECT_FALSE(handle.result().success);
  EXPECT_NE(handle.result().message.find("not available"), std::string::npos);

  InterpretationClient absolute(node_, "/client_test_b/interpretation/interpret");
  auto other = absolute.ask("echo", "{}");
  EXPECT_EQ(wait_ready(other), Status::DONE);
}

TEST_F(InterpretationClientTest, NoServerFailsAfterTheTimeoutWithoutBlocking)
{
  InterpretationClient client(node_, InterpretationClient::DEFAULT_SERVICE, 300ms);
  auto handle = client.ask("echo", "{}");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(handle.poll(), Status::PENDING);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 50ms);
  EXPECT_EQ(wait_ready(handle), Status::FAILED);
  EXPECT_FALSE(handle.result().valid());
}

TEST_F(InterpretationClientTest, ServerThatAppearsLater)
{
  InterpretationClient client(node_);
  auto handle = client.ask("echo", "{}");
  EXPECT_EQ(handle.poll(), Status::PENDING);
  std::this_thread::sleep_for(200ms);
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  EXPECT_EQ(wait_ready(handle), Status::DONE);
  EXPECT_TRUE(handle.result().success);
}

TEST_F(InterpretationClientTest, NoResponseInTimeAndCancel)
{
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  InterpretationClient client(node_, InterpretationClient::DEFAULT_SERVICE, 500ms);
  auto handle = client.ask("slow", "{}");
  EXPECT_EQ(wait_ready(handle), Status::FAILED);
  EXPECT_NE(handle.result().message.find("no response"), std::string::npos);

  InterpretationClient patient(node_);
  auto cancelled = patient.ask("slow", "{}");
  cancelled.cancel();
  EXPECT_EQ(cancelled.poll(), Status::FAILED);
  EXPECT_EQ(cancelled.result().message, "cancelled");
}

TEST_F(InterpretationClientTest, InvalidAndUnknown)
{
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  InterpretationClient client(node_);
  auto invalid = client.ask("invalid", "{}");
  ASSERT_EQ(wait_ready(invalid), Status::DONE);
  EXPECT_TRUE(invalid.result().success);
  EXPECT_FALSE(invalid.result().valid());
  EXPECT_EQ(invalid.result().source, "invalid");
  EXPECT_EQ(invalid.result().fallback_reason, "timeout");

  auto unknown = client.ask("unknown", "{}");
  ASSERT_EQ(wait_ready(unknown), Status::DONE);
  EXPECT_FALSE(unknown.result().success);
}

TEST_F(InterpretationClientTest, ChooseBehavior)
{
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  InterpretationClient client(node_);
  const std::vector<OfferedBehavior> offered = {{3, "follow", "Follow a person"},
    {5, "search", "Look around"}};

  auto handle = client.choose_behavior("choose", offered, "{\"task\": \"t\"}", "5");
  ASSERT_EQ(wait_ready(handle), Status::DONE);
  // The context the question received (the fake server returns it in the message)
  const auto context = nlohmann::json::parse(handle.result().message);
  EXPECT_EQ(context["task"], "t");
  EXPECT_EQ(context[InterpretationClient::OFFERED_IDS_KEY], nlohmann::json({3, 5}));
  EXPECT_EQ(context[InterpretationClient::OFFERED_KEY][0]["name"], "follow");
  BehaviorChoice choice = InterpretationClient::behavior_choice(handle.result(), offered);
  EXPECT_EQ(choice.id, 5);
  EXPECT_TRUE(choice.reason.empty());

  auto not_offered = client.choose_behavior("choose", offered, "", "7");
  ASSERT_EQ(wait_ready(not_offered), Status::DONE);
  choice = InterpretationClient::behavior_choice(not_offered.result(), offered, "behavior_id");
  EXPECT_EQ(choice.id, -1);
  EXPECT_NE(choice.reason.find("not offered"), std::string::npos);

  auto none = client.choose_behavior("choose", offered, "", "-1");
  ASSERT_EQ(wait_ready(none), Status::DONE);
  choice = InterpretationClient::behavior_choice(none.result(), offered);
  EXPECT_EQ(choice.id, -1);
  EXPECT_NE(choice.reason.find("none"), std::string::npos);

  InterpretationResult invalid;
  invalid.success = true;
  invalid.source = "invalid";
  invalid.fallback_reason = "budget";
  choice = InterpretationClient::behavior_choice(invalid, offered);
  EXPECT_EQ(choice.id, -1);
  EXPECT_NE(choice.reason.find("budget"), std::string::npos);

  InterpretationResult two_fields;
  two_fields.success = true;
  two_fields.source = "llm";
  two_fields.answer = "{\"a\": 3, \"b\": 5}";
  EXPECT_EQ(InterpretationClient::behavior_choice(two_fields, offered).id, -1);
  EXPECT_EQ(InterpretationClient::behavior_choice(two_fields, offered, "b").id, 5);

  EXPECT_THROW(client.choose_behavior("choose", offered, "[1]"), std::invalid_argument);
  EXPECT_THROW(client.choose_behavior("choose", offered, "not json"), std::invalid_argument);
}

TEST(InterpretationClientNodes, FromALifecycleNode)
{
  auto node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("lifecycle_orchestrator");
  InterpretationClient from_shared(node);
  InterpretationClient from_pointer(node.get());
  EXPECT_EQ(from_shared.service_name(), "/interpretation/interpret");
  EXPECT_FALSE(from_pointer.service_is_ready());
  InterpretationHandle empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.poll(), Status::FAILED);
}

TEST_F(InterpretationClientTest, CancelsTheCallsOfARequest)
{
  interpretation_test::FakeInterpretationServer server("/client_test_a");
  InterpretationClient client(node_);
  auto handle = client.cancel("dialogue-1");
  EXPECT_TRUE(handle.valid());
  ASSERT_EQ(wait_ready(handle), Status::DONE);
  EXPECT_EQ(handle.cancelled(), 1);
  EXPECT_EQ(server.cancelled(), std::vector<std::string>({"dialogue-1"}));
  // The client of the cancel service is reused
  auto again = client.cancel("dialogue-2");
  ASSERT_EQ(wait_ready(again), Status::DONE);
  // An empty request_id is rejected by the server
  auto empty = client.cancel("");
  EXPECT_EQ(wait_ready(empty), Status::FAILED);
  EXPECT_EQ(empty.cancelled(), 0);
  EXPECT_EQ(empty.message(), "request_id is empty");
  EXPECT_EQ(server.cancelled(), std::vector<std::string>({"dialogue-1", "dialogue-2", ""}));
}

TEST_F(InterpretationClientTest, CancelWithoutServerFailsAfterTheTimeout)
{
  InterpretationClient client(node_, InterpretationClient::DEFAULT_SERVICE, 300ms);
  auto handle = client.cancel("dialogue-1");
  EXPECT_EQ(wait_ready(handle), Status::FAILED);
  EXPECT_NE(handle.message().find("/client_test_a/interpretation/cancel"), std::string::npos);
  CancellationHandle none;
  EXPECT_FALSE(none.valid());
  EXPECT_EQ(none.poll(), Status::FAILED);
}

TEST(InterpretationClientNames, CancelServiceNextToTheInterpretService)
{
  EXPECT_EQ(
    InterpretationClient::cancel_service_for(InterpretationClient::DEFAULT_SERVICE),
    "interpretation/cancel");
  EXPECT_EQ(
    InterpretationClient::cancel_service_for("/ns/interpretation/interpret"),
    "/ns/interpretation/cancel");
  EXPECT_EQ(InterpretationClient::cancel_service_for("interpret"), "cancel");
}

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
