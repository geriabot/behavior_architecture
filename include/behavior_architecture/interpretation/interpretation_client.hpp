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

#ifndef BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRETATION_CLIENT_HPP_
#define BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRETATION_CLIENT_HPP_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "behavior_architecture/srv/cancel_interpretation.hpp"
#include "behavior_architecture/srv/interpret.hpp"

namespace behavior_architecture
{
namespace interpretation
{

/// What a question asks (the fields of the request of Interpret.srv)
struct InterpretationRequest
{
  std::string question;
  std::string context;   // a JSON object ("" = {})
  std::string text;
  std::vector<sensor_msgs::msg::Image> images;
  double budget_s {0.0};  // 0 = only the server's per-call limit
  std::string variant;    // "" = the server's configuration
  std::string request_id;
  int max_tokens {0};     // the token cap of this call; 0 = the question's max_tokens
};

/// The answer (the fields of the response), or why there is none
struct InterpretationResult
{
  bool success {false};      // a response came back and the server could take the request
  std::string answer;        // a JSON object ("" when the source is "invalid")
  std::string source;        // "llm", "rule"... (the interpreter), "fallback" or "invalid"
  std::string fallback_reason;
  std::string message;

  /// An answer the asker can use (from an interpreter or a fallback)
  bool valid() const {return success && source != "invalid" && !answer.empty();}
};

/**
 * @brief A question in progress, polled without blocking.
 *
 * Copies share the same question. poll() sends the request as soon as the service is
 * available (within the client's timeout) and checks for the response; it never waits.
 */
class InterpretationHandle
{
public:
  enum class Status {PENDING, DONE, FAILED};

  InterpretationHandle() = default;

  /// PENDING until the response arrives (DONE) or the timeout passes (FAILED)
  Status poll();
  /// poll() != PENDING
  bool ready() {return poll() != Status::PENDING;}
  /// The result (meaningful once ready); FAILED: success false and the reason in message
  InterpretationResult result() const;
  /// Forget the question (its response, if any, is ignored)
  void cancel();
  /// A handle of a question (not default-constructed)
  bool valid() const {return static_cast<bool>(state_);}

private:
  friend class InterpretationClient;
  struct State;
  explicit InterpretationHandle(std::shared_ptr<State> state)
  : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

/**
 * @brief The cancellation of the server's calls in progress of a request, polled without
 * blocking (InterpretationClient::cancel).
 *
 * poll() sends the request as soon as the cancel service is available (within the client's
 * timeout) and checks for the response; it never waits. A handle that is not polled is sent
 * only if the service was available when it was created: poll it until ready() to be sure.
 * The cancelled calls answer their askers with source "invalid" and fallback_reason
 * "cancelled".
 */
class CancellationHandle
{
public:
  using Status = InterpretationHandle::Status;

  CancellationHandle() = default;

  /// PENDING until the response arrives (DONE) or the timeout passes (FAILED)
  Status poll();
  /// poll() != PENDING
  bool ready() {return poll() != Status::PENDING;}
  /// How many calls in progress the server cancelled (once DONE; 0 otherwise)
  int cancelled() const;
  /// The server's message, or why the cancellation failed
  std::string message() const;
  /// A handle of a cancellation (not default-constructed)
  bool valid() const {return static_cast<bool>(state_);}

private:
  friend class InterpretationClient;
  struct State;
  explicit CancellationHandle(std::shared_ptr<State> state)
  : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

/// A behavior offered to choose_behavior
struct OfferedBehavior
{
  int id {-1};
  std::string name;
  std::string description;
};

/// The behavior chosen, or -1 with the reason
struct BehaviorChoice
{
  int id {-1};
  std::string reason;   // "" when a behavior was chosen
};

/**
 * @brief Asks the interpretation server of the system without blocking.
 *
 * Owned by an orchestrator (by composition) or by a BT node. The service name is relative by
 * default (`interpretation/interpret`), resolved in the namespace of the node, so each system
 * asks its own server. ask() returns at once; the control cycle polls the handle.
 */
class InterpretationClient
{
public:
  using Interpret = behavior_architecture::srv::Interpret;
  using CancelInterpretation = behavior_architecture::srv::CancelInterpretation;
  static constexpr const char * DEFAULT_SERVICE = "interpretation/interpret";
  /// Context keys written by choose_behavior
  static constexpr const char * OFFERED_KEY = "offered_behaviors";
  static constexpr const char * OFFERED_IDS_KEY = "offered_ids";

  /// From any node type (rclcpp::Node, rclcpp_lifecycle::LifecycleNode, a pointer to either)
  template<typename NodeT, typename = std::enable_if_t<
      !std::is_same_v<std::decay_t<NodeT>, InterpretationClient>>>
  explicit InterpretationClient(
    NodeT && node, const std::string & service_name = DEFAULT_SERVICE,
    std::chrono::milliseconds timeout = std::chrono::seconds(120))
  : InterpretationClient(
      node->get_node_base_interface(), node->get_node_graph_interface(),
      node->get_node_services_interface(), node->get_node_logging_interface(),
      service_name, timeout)
  {}

  InterpretationClient(
    rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base,
    rclcpp::node_interfaces::NodeGraphInterface::SharedPtr node_graph,
    rclcpp::node_interfaces::NodeServicesInterface::SharedPtr node_services,
    rclcpp::node_interfaces::NodeLoggingInterface::SharedPtr node_logging,
    const std::string & service_name, std::chrono::milliseconds timeout);

  InterpretationClient(const InterpretationClient &) = delete;
  InterpretationClient & operator=(const InterpretationClient &) = delete;

  /// Ask a question; returns at once (the request is sent now if the service is available)
  InterpretationHandle ask(const InterpretationRequest & request);
  InterpretationHandle ask(
    const std::string & question, const std::string & context, const std::string & text = "",
    const std::vector<sensor_msgs::msg::Image> & images = {}, double budget_s = 0.0);

  /**
   * @brief Ask the system's question that chooses a behavior among the offered ones.
   *
   * The offered behaviors are added to the context (OFFERED_KEY: [{id, name, description}],
   * OFFERED_IDS_KEY: [id]) so that the question's template and its choices_from can use them.
   * Filtering the catalog before asking stays in the orchestrator. Read the answer with
   * behavior_choice(). Throws std::invalid_argument if the context is not a JSON object.
   */
  InterpretationHandle choose_behavior(
    const std::string & question, const std::vector<OfferedBehavior> & offered,
    const std::string & context = "", const std::string & text = "", double budget_s = 0.0,
    const std::string & variant = "");

  /// The id chosen, or -1 with the reason (no valid answer, none fits, an id not offered).
  /// answer_field: the answer's field with the id ("" = its only field)
  static BehaviorChoice behavior_choice(
    const InterpretationResult & result, const std::vector<OfferedBehavior> & offered,
    const std::string & answer_field = "");

  /// The context with the offered behaviors added (see choose_behavior)
  static std::string offered_context(
    const std::vector<OfferedBehavior> & offered, const std::string & context);

  /**
   * @brief Cancel the server's calls in progress that were asked with this request_id (for
   * example when the dialogue that asked them has ended).
   *
   * Each of them answers at once with source "invalid" and fallback_reason "cancelled"; later
   * questions are not affected. Returns at once; the request is sent when the cancel service is
   * available (now, or when the handle is polled). The cancel service is `cancel` next to the
   * interpret service (cancel_service_for()), and its client is created at the first call.
   */
  CancellationHandle cancel(const std::string & request_id);

  /// The cancel service next to an interpret service: "interpretation/cancel" by default
  static std::string cancel_service_for(const std::string & service_name);

  bool service_is_ready() const;
  std::string service_name() const;

private:
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base_;
  rclcpp::node_interfaces::NodeGraphInterface::SharedPtr node_graph_;
  rclcpp::node_interfaces::NodeServicesInterface::SharedPtr node_services_;
  std::string service_name_arg_;
  rclcpp::Client<Interpret>::SharedPtr client_;
  std::mutex cancel_mutex_;
  rclcpp::Client<CancelInterpretation>::SharedPtr cancel_client_;
  rclcpp::Logger logger_;
  std::chrono::milliseconds timeout_;
};

}  // namespace interpretation
}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__INTERPRETATION__INTERPRETATION_CLIENT_HPP_
