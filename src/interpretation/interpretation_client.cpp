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

#include "behavior_architecture/interpretation/interpretation_client.hpp"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace behavior_architecture
{
namespace interpretation
{

using json = nlohmann::json;
using Interpret = InterpretationClient::Interpret;

struct InterpretationHandle::State
{
  std::mutex mutex;
  rclcpp::Client<Interpret>::SharedPtr client;
  rclcpp::Logger logger {rclcpp::get_logger("interpretation_client")};
  std::shared_ptr<Interpret::Request> request;
  std::shared_future<Interpret::Response::SharedPtr> future;
  int64_t request_id {0};
  bool sent {false};
  std::chrono::steady_clock::time_point deadline;
  std::chrono::milliseconds timeout {0};
  Status status {Status::PENDING};
  InterpretationResult result;

  void fail(const std::string & message)
  {
    status = Status::FAILED;
    result = InterpretationResult();
    result.message = message;
    RCLCPP_WARN(logger, "%s: %s", request->question.c_str(), message.c_str());
  }
};

InterpretationHandle::Status
InterpretationHandle::poll()
{
  if (!state_) {
    return Status::FAILED;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto & s = *state_;
  if (s.status != Status::PENDING) {
    return s.status;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!s.sent) {
    if (!s.client->service_is_ready()) {
      if (now > s.deadline) {
        s.fail(
          "the service " + std::string(s.client->get_service_name()) +
          " is not available");
      }
      return s.status;
    }
    auto sent = s.client->async_send_request(s.request);
    s.request_id = sent.request_id;
    s.future = sent.future.share();
    s.sent = true;
  }
  if (s.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto response = s.future.get();
    s.result.success = response->success;
    s.result.answer = response->answer;
    s.result.source = response->source;
    s.result.fallback_reason = response->fallback_reason;
    s.result.message = response->message;
    s.status = Status::DONE;
    s.future = {};
    return s.status;
  }
  if (now > s.deadline) {
    s.client->remove_pending_request(s.request_id);
    s.future = {};
    s.fail("no response within " + std::to_string(s.timeout.count()) + " ms");
  }
  return s.status;
}

InterpretationResult
InterpretationHandle::result() const
{
  if (!state_) {
    InterpretationResult result;
    result.message = "no question was asked";
    return result;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->result;
}

void
InterpretationHandle::cancel()
{
  if (!state_) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->status != Status::PENDING) {
    return;
  }
  if (state_->sent) {
    state_->client->remove_pending_request(state_->request_id);
  }
  state_->future = {};
  state_->status = Status::FAILED;
  state_->result = InterpretationResult();
  state_->result.message = "cancelled";
}

InterpretationClient::InterpretationClient(
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base,
  rclcpp::node_interfaces::NodeGraphInterface::SharedPtr node_graph,
  rclcpp::node_interfaces::NodeServicesInterface::SharedPtr node_services,
  rclcpp::node_interfaces::NodeLoggingInterface::SharedPtr node_logging,
  const std::string & service_name, std::chrono::milliseconds timeout)
: logger_(node_logging->get_logger().get_child("interpretation")),
  timeout_(timeout)
{
  client_ = rclcpp::create_client<Interpret>(
    node_base, node_graph, node_services, service_name, rclcpp::ServicesQoS(), nullptr);
}

InterpretationHandle
InterpretationClient::ask(const InterpretationRequest & request)
{
  auto state = std::make_shared<InterpretationHandle::State>();
  state->client = client_;
  state->logger = logger_;
  state->request = std::make_shared<Interpret::Request>();
  state->request->question = request.question;
  state->request->request_id = request.request_id;
  state->request->context = request.context;
  state->request->text = request.text;
  state->request->images = request.images;
  state->request->budget_s = request.budget_s;
  state->request->variant = request.variant;
  state->timeout = timeout_;
  state->deadline = std::chrono::steady_clock::now() + timeout_;
  InterpretationHandle handle(state);
  handle.poll();  // sent now if the service is available
  return handle;
}

InterpretationHandle
InterpretationClient::ask(
  const std::string & question, const std::string & context, const std::string & text,
  const std::vector<sensor_msgs::msg::Image> & images, double budget_s)
{
  InterpretationRequest request;
  request.question = question;
  request.context = context;
  request.text = text;
  request.images = images;
  request.budget_s = budget_s;
  return ask(request);
}

std::string
InterpretationClient::offered_context(
  const std::vector<OfferedBehavior> & offered, const std::string & context)
{
  json object = json::object();
  if (!context.empty()) {
    try {
      object = json::parse(context);
    } catch (const json::parse_error & e) {
      throw std::invalid_argument(
              std::string("choose_behavior: the context is not JSON: ") +
              e.what());
    }
    if (!object.is_object()) {
      throw std::invalid_argument("choose_behavior: the context must be a JSON object");
    }
  }
  json behaviors = json::array();
  json ids = json::array();
  for (const auto & behavior : offered) {
    behaviors.push_back(
      {{"id", behavior.id}, {"name", behavior.name}, {"description", behavior.description}});
    ids.push_back(behavior.id);
  }
  object[OFFERED_KEY] = behaviors;
  object[OFFERED_IDS_KEY] = ids;
  return object.dump();
}

InterpretationHandle
InterpretationClient::choose_behavior(
  const std::string & question, const std::vector<OfferedBehavior> & offered,
  const std::string & context, const std::string & text, double budget_s,
  const std::string & variant)
{
  InterpretationRequest request;
  request.question = question;
  request.context = offered_context(offered, context);
  request.text = text;
  request.budget_s = budget_s;
  request.variant = variant;
  return ask(request);
}

BehaviorChoice
InterpretationClient::behavior_choice(
  const InterpretationResult & result, const std::vector<OfferedBehavior> & offered,
  const std::string & answer_field)
{
  BehaviorChoice choice;
  if (!result.success) {
    choice.reason = "no answer: " + result.message;
    return choice;
  }
  if (!result.valid()) {
    choice.reason = "no valid answer (" + result.fallback_reason + ")";
    return choice;
  }
  json answer;
  try {
    answer = json::parse(result.answer);
  } catch (const json::parse_error &) {
    choice.reason = "the answer is not JSON";
    return choice;
  }
  if (!answer.is_object()) {
    choice.reason = "the answer is not a JSON object";
    return choice;
  }
  std::string field = answer_field;
  if (field.empty()) {
    if (answer.size() != 1) {
      choice.reason = "the answer has " + std::to_string(answer.size()) +
        " fields: give the field with the id";
      return choice;
    }
    field = answer.begin().key();
  }
  if (!answer.contains(field) || !answer[field].is_number_integer()) {
    choice.reason = "the answer has no integer field \"" + field + "\"";
    return choice;
  }
  const int id = answer[field].get<int>();
  if (id == -1) {
    choice.reason = "none of the offered behaviors fits";
    return choice;
  }
  const bool is_offered = std::any_of(
    offered.begin(), offered.end(), [id](const OfferedBehavior & b) {return b.id == id;});
  if (!is_offered) {
    choice.reason = "the behavior " + std::to_string(id) + " was not offered";
    return choice;
  }
  choice.id = id;
  return choice;
}

bool
InterpretationClient::service_is_ready() const
{
  return client_->service_is_ready();
}

std::string
InterpretationClient::service_name() const
{
  return client_->get_service_name();
}

}  // namespace interpretation
}  // namespace behavior_architecture
