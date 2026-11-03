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

#include "behavior_architecture/interpretation/interpret_bt_nodes.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "behaviortree_cpp/bt_factory.h"

namespace behavior_architecture
{
namespace interpretation
{

std::string
pending_key(const std::string & question, const std::string & key)
{
  return key.empty() ? "interpretation_pending_" + question : key;
}

BT::NodeStatus
write_result(
  BT::TreeNode & node, InterpretationHandle::Status status, const InterpretationResult & result,
  bool fail_on_invalid)
{
  node.setOutput("answer", result.answer);
  node.setOutput("source", result.source);
  node.setOutput("fallback_reason", result.fallback_reason);
  node.setOutput("message", result.message);
  if (status != InterpretationHandle::Status::DONE || !result.success) {
    return BT::NodeStatus::FAILURE;
  }
  if (!result.valid() && fail_on_invalid) {
    return BT::NodeStatus::FAILURE;
  }
  return BT::NodeStatus::SUCCESS;
}

InterpretNode::InterpretNode(const std::string & name, const BT::NodeConfig & config)
: BT::StatefulActionNode(name, config)
{
  node_ = this->config().blackboard->get<rclcpp::Node::SharedPtr>("node");
  if (!node_) {
    throw BT::RuntimeError("Interpret: 'node' not found in the blackboard");
  }
}

BT::NodeStatus
InterpretNode::onStart()
{
  InterpretationRequest request;
  if (!getInput("question", request.question) || request.question.empty()) {
    setOutput("message", std::string("Interpret: the question is missing"));
    return BT::NodeStatus::FAILURE;
  }
  getInput("variant", request.variant);
  getInput("context", request.context);
  getInput("text", request.text);
  getInput("request_id", request.request_id);
  getInput("budget_s", request.budget_s);
  std::vector<sensor_msgs::msg::Image> images;
  if (getInput("images", images)) {
    request.images = images;
  }

  std::string service = InterpretationClient::DEFAULT_SERVICE;
  getInput("service", service);
  double timeout_s = 120.0;
  getInput("timeout_s", timeout_s);
  if (!client_ || service != service_ || timeout_s != timeout_s_) {
    client_ = std::make_unique<InterpretationClient>(
      node_, service,
      std::chrono::milliseconds(static_cast<int64_t>(timeout_s * 1000.0)));
    service_ = service;
    timeout_s_ = timeout_s;
  }
  fail_on_invalid_ = true;
  getInput("fail_on_invalid", fail_on_invalid_);
  bool wait = true;
  getInput("wait", wait);

  handle_ = client_->ask(request);
  if (!wait) {
    std::string key;
    getInput("result_key", key);
    config().blackboard->set(pending_key(request.question, key), handle_);
    handle_ = InterpretationHandle();
    return BT::NodeStatus::SUCCESS;
  }
  return onRunning();
}

BT::NodeStatus
InterpretNode::onRunning()
{
  const auto status = handle_.poll();
  if (status == InterpretationHandle::Status::PENDING) {
    return BT::NodeStatus::RUNNING;
  }
  return write_result(*this, status, handle_.result(), fail_on_invalid_);
}

void
InterpretNode::onHalted()
{
  handle_.cancel();
}

InterpretationResultNode::InterpretationResultNode(
  const std::string & name, const BT::NodeConfig & config)
: BT::StatefulActionNode(name, config)
{}

BT::NodeStatus
InterpretationResultNode::onStart()
{
  std::string question;
  std::string key;
  getInput("question", question);
  getInput("result_key", key);
  wait_ = true;
  getInput("wait", wait_);
  fail_on_invalid_ = true;
  getInput("fail_on_invalid", fail_on_invalid_);
  if (question.empty() && key.empty()) {
    setOutput("message", std::string("InterpretationResult: give question or result_key"));
    return BT::NodeStatus::FAILURE;
  }
  key = pending_key(question, key);
  if (!config().blackboard->get(key, handle_) || !handle_.valid()) {
    setOutput("message", "InterpretationResult: nothing was asked under " + key);
    return BT::NodeStatus::FAILURE;
  }
  return check();
}

BT::NodeStatus
InterpretationResultNode::onRunning()
{
  return check();
}

BT::NodeStatus
InterpretationResultNode::check()
{
  const auto status = handle_.poll();
  if (status == InterpretationHandle::Status::PENDING) {
    if (wait_) {
      return BT::NodeStatus::RUNNING;
    }
    setOutput("message", std::string("InterpretationResult: the answer has not arrived yet"));
    return BT::NodeStatus::FAILURE;
  }
  return write_result(*this, status, handle_.result(), fail_on_invalid_);
}

}  // namespace interpretation
}  // namespace behavior_architecture

BT_REGISTER_NODES(factory)
{
  factory.registerNodeType<behavior_architecture::interpretation::InterpretNode>("Interpret");
  factory.registerNodeType<behavior_architecture::interpretation::InterpretationResultNode>(
    "InterpretationResult");
}
