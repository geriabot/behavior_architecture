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
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "behaviortree_cpp/bt_factory.h"
#include "behaviortree_cpp/utils/demangle_util.h"
#include "nlohmann/json.hpp"

namespace behavior_architecture
{
namespace interpretation
{

namespace
{

std::string
trim(const std::string & text)
{
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

/// The value of a blackboard entry as JSON
nlohmann::json
entry_to_json(const std::string & key, const BT::Any & value)
{
  if (value.empty()) {
    throw std::runtime_error("the entry '" + key + "' has no value");
  }
  if (value.isString()) {
    // A string that holds a JSON object or array is added as JSON, any other as a string
    const auto text = value.cast<std::string>();
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
      return parsed;
    }
    return text;
  }
  if (value.isType<bool>()) {
    return value.cast<bool>();
  }
  if (value.isIntegral()) {
    return value.cast<int64_t>();
  }
  if (value.isNumber()) {
    return value.cast<double>();
  }
  auto & any = const_cast<BT::Any &>(value);
  if (auto * v = any.castPtr<std::vector<std::string>>()) {
    return *v;
  }
  if (auto * v = any.castPtr<std::vector<int>>()) {
    return *v;
  }
  if (auto * v = any.castPtr<std::vector<int64_t>>()) {
    return *v;
  }
  if (auto * v = any.castPtr<std::vector<double>>()) {
    return *v;
  }
  if (auto * v = any.castPtr<std::vector<bool>>()) {
    return nlohmann::json(*v);
  }
  if (auto * v = any.castPtr<nlohmann::json>()) {
    return *v;
  }
  throw std::runtime_error(
          "the entry '" + key + "' is of type " + BT::demangle(value.type()) +
          ", which cannot be written as JSON");
}

}  // namespace

bool
is_json_object_literal(const std::string & value)
{
  const auto text = trim(value);
  if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
    return false;
  }
  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  return !parsed.is_discarded() && parsed.is_object();
}

std::string
build_context(
  const std::string & context, const std::string & context_keys,
  const BT::Blackboard & blackboard)
{
  if (trim(context_keys).empty()) {
    return context;
  }
  nlohmann::json object = nlohmann::json::object();
  if (!trim(context).empty()) {
    object = nlohmann::json::parse(context, nullptr, false);
    if (object.is_discarded() || !object.is_object()) {
      throw std::runtime_error("the context is not a JSON object");
    }
  }
  std::stringstream list(context_keys);
  std::string item;
  while (std::getline(list, item, ',')) {
    item = trim(item);
    if (item.empty()) {
      continue;
    }
    // "name=key": the entry `key` under the name `name`; "key": under its own name
    std::string name = item;
    std::string key = item;
    const auto equals = item.find('=');
    if (equals != std::string::npos) {
      name = trim(item.substr(0, equals));
      key = trim(item.substr(equals + 1));
    }
    if (name.empty() || key.empty()) {
      throw std::runtime_error("context_keys: '" + item + "' is not 'key' or 'name=key'");
    }
    const auto entry = blackboard.getAnyLocked(key);
    if (!entry) {
      throw std::runtime_error("context_keys: the blackboard has no entry '" + key + "'");
    }
    object[name] = entry_to_json(key, *entry.get());
  }
  return object.dump();
}

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
  // A JSON object written in the attribute is taken as it is: BehaviorTree.CPP would read it,
  // enclosed in braces, as the name of a blackboard entry
  const auto context_port = config().input_ports.find("context");
  if (context_port != config().input_ports.end() && is_json_object_literal(context_port->second)) {
    request.context = context_port->second;
  } else {
    getInput("context", request.context);
  }
  std::string context_keys;
  getInput("context_keys", context_keys);
  try {
    request.context = build_context(request.context, context_keys, *config().blackboard);
  } catch (const std::exception & e) {
    setOutput("message", std::string("Interpret: ") + e.what());
    return BT::NodeStatus::FAILURE;
  }
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
