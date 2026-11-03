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

#ifndef BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRET_BT_NODES_HPP_
#define BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRET_BT_NODES_HPP_

#include <memory>
#include <string>
#include <vector>

#include "behaviortree_cpp/action_node.h"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "behavior_architecture/interpretation/interpretation_client.hpp"

namespace behavior_architecture
{
namespace interpretation
{

/// Whether a port's value, as written in the XML, is a JSON object (taken literally)
bool is_json_object_literal(const std::string & value);

/**
 * @brief The context of a request: `context` (a JSON object, or "") with the blackboard entries
 * named in `context_keys` added to it
 *
 * `context_keys` is a comma-separated list of `key` (the entry `key`, under its name) or
 * `name=key` (the entry `key`, under `name`). A string entry that holds a JSON object or array
 * is added as JSON, any other string as a string; numbers, booleans, vectors of them and
 * vectors of strings as JSON. With no keys, `context` is returned unchanged.
 * @throw std::runtime_error if `context` is not a JSON object, or an entry is missing or of
 * a type that cannot be written as JSON
 */
std::string build_context(
  const std::string & context, const std::string & context_keys,
  const BT::Blackboard & blackboard);

/// The blackboard key where Interpret (wait=false) leaves its question for InterpretationResult
std::string pending_key(const std::string & question, const std::string & key);

/// Writes a result to the node's output ports and returns the node's status
BT::NodeStatus write_result(
  BT::TreeNode & node, InterpretationHandle::Status status, const InterpretationResult & result,
  bool fail_on_invalid);

/**
 * @brief Asks the interpretation server a question (BT node ID "Interpret").
 *
 * The context is the port `context`, a JSON object written in the attribute or read from the
 * blackboard, with the entries named in `context_keys` added to it (build_context()).
 *
 * The ROS node is the blackboard's "node" (as for every BT node of the framework). With
 * wait=true the node is RUNNING until the answer arrives, without blocking the tree: SUCCESS
 * with a valid answer (of an interpreter or a fallback), FAILURE if there is none (source
 * "invalid", unless fail_on_invalid is false) or the server did not answer. With wait=false
 * it returns SUCCESS at once and leaves the question in the blackboard (result_key, or
 * interpretation_pending_<question>), where the answer arrives and InterpretationResult
 * picks it up.
 */
class InterpretNode : public BT::StatefulActionNode
{
public:
  InterpretNode(const std::string & name, const BT::NodeConfig & config);

  static BT::PortsList providedPorts()
  {
    return {
      BT::InputPort<std::string>("question", "The question (as registered in the server)"),
      BT::InputPort<std::string>("variant", "", "A variant of the question (\"\": the server's)"),
      BT::InputPort<std::string>(
        "context", "", "A JSON object with what the question needs (literal or {entry})"),
      BT::InputPort<std::string>(
        "context_keys", "", "Blackboard entries added to the context: key or name=key, ..."),
      BT::InputPort<std::string>("text", "", "Free text (a user's line, a task)"),
      BT::InputPort<std::vector<sensor_msgs::msg::Image>>(
        "images", "Zero or more images (a blackboard entry)"),
      BT::InputPort<double>("budget_s", 0.0, "Time left for the answer (0: no budget)"),
      BT::InputPort<bool>("wait", true, "Wait for the answer (false: InterpretationResult)"),
      BT::InputPort<std::string>(
        "result_key", "", "wait=false: the blackboard key (\"\": interpretation_pending_<q>)"),
      BT::InputPort<std::string>("request_id", "", "Who asks (for the server's log)"),
      BT::InputPort<std::string>(
        "service", InterpretationClient::DEFAULT_SERVICE, "The server's service"),
      BT::InputPort<double>("timeout_s", 120.0, "Longest wait for the server's response"),
      BT::InputPort<bool>(
        "fail_on_invalid", true, "FAILURE when no valid answer came back (source invalid)"),
      BT::OutputPort<std::string>("answer", "{interpretation_answer}", "The answer (JSON)"),
      BT::OutputPort<std::string>("source", "{interpretation_source}", "Who answered"),
      BT::OutputPort<std::string>(
        "fallback_reason", "{interpretation_fallback_reason}", "Why there is no answer"),
      BT::OutputPort<std::string>(
        "message", "{interpretation_message}", "The server's or the client's message"),
    };
  }

  BT::NodeStatus onStart() override;
  BT::NodeStatus onRunning() override;
  void onHalted() override;

private:
  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<InterpretationClient> client_;
  std::string service_;
  double timeout_s_ {0.0};
  InterpretationHandle handle_;
  bool fail_on_invalid_ {true};
};

/**
 * @brief Picks up the answer of an Interpret with wait=false (BT node ID "InterpretationResult").
 *
 * With wait=true it is RUNNING until the answer arrives; with wait=false it fails at once if it
 * has not arrived yet. FAILURE also when nothing was asked under that key.
 */
class InterpretationResultNode : public BT::StatefulActionNode
{
public:
  InterpretationResultNode(const std::string & name, const BT::NodeConfig & config);

  static BT::PortsList providedPorts()
  {
    return {
      BT::InputPort<std::string>("question", "", "The question asked (for the default key)"),
      BT::InputPort<std::string>("result_key", "", "The key given to Interpret, if any"),
      BT::InputPort<bool>("wait", true, "Wait for the answer (false: FAILURE if not there)"),
      BT::InputPort<bool>(
        "fail_on_invalid", true, "FAILURE when no valid answer came back (source invalid)"),
      BT::OutputPort<std::string>("answer", "{interpretation_answer}", "The answer (JSON)"),
      BT::OutputPort<std::string>("source", "{interpretation_source}", "Who answered"),
      BT::OutputPort<std::string>(
        "fallback_reason", "{interpretation_fallback_reason}", "Why there is no answer"),
      BT::OutputPort<std::string>(
        "message", "{interpretation_message}", "The server's or the client's message"),
    };
  }

  BT::NodeStatus onStart() override;
  BT::NodeStatus onRunning() override;
  void onHalted() override {}

private:
  BT::NodeStatus check();

  InterpretationHandle handle_;
  bool wait_ {true};
  bool fail_on_invalid_ {true};
};

}  // namespace interpretation
}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRET_BT_NODES_HPP_
