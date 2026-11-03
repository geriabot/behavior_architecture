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

#ifndef INTERPRETATION__FAKE_INTERPRETATION_SERVER_HPP_
#define INTERPRETATION__FAKE_INTERPRETATION_SERVER_HPP_

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"
#include "rclcpp/rclcpp.hpp"
#include "behavior_architecture/srv/interpret.hpp"

namespace interpretation_test
{

/**
 * A stand-in for the interpretation server, spun in its own thread. Questions:
 *   echo     answers {"context": <the context>, "text": <text>, "images": <how many>}
 *   choose   answers {"behavior_id": <the text as an int>}
 *   invalid  no valid answer (source "invalid", reason "timeout")
 *   slow     answers {} after 1.5 s
 *   other    success false (unknown question)
 */
class FakeInterpretationServer
{
public:
  using Interpret = behavior_architecture::srv::Interpret;

  explicit FakeInterpretationServer(const std::string & ns)
  {
    node_ = std::make_shared<rclcpp::Node>("fake_interpretation_server", ns);
    service_ = node_->create_service<Interpret>(
      "interpretation/interpret",
      [](const std::shared_ptr<Interpret::Request> request,
      std::shared_ptr<Interpret::Response> response) {
        response->success = true;
        response->source = "llm";
        if (request->question == "echo") {
          nlohmann::json answer = {
            {"context", request->context}, {"text", request->text},
            {"images", request->images.size()}, {"variant", request->variant},
            {"budget_s", request->budget_s}, {"max_tokens", request->max_tokens}};
          response->answer = answer.dump();
        } else if (request->question == "choose") {
          response->answer = nlohmann::json({{"behavior_id", std::stoi(request->text)}}).dump();
          response->message = request->context;
        } else if (request->question == "invalid") {
          response->source = "invalid";
          response->fallback_reason = "timeout";
        } else if (request->question == "slow") {
          std::this_thread::sleep_for(std::chrono::milliseconds(1500));
          response->answer = "{}";
        } else {
          response->success = false;
          response->source = "";
          response->message = "Unknown question";
        }
      });
    executor_.add_node(node_);
    thread_ = std::thread([this]() {executor_.spin();});
  }

  ~FakeInterpretationServer()
  {
    executor_.cancel();
    thread_.join();
  }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::Service<Interpret>::SharedPtr service_;
  rclcpp::executors::SingleThreadedExecutor executor_;
  std::thread thread_;
};

/// Spins a node in its own thread (as mission_executor spins the blackboard's node)
class Spinner
{
public:
  explicit Spinner(rclcpp::Node::SharedPtr node)
  {
    executor_.add_node(node);
    thread_ = std::thread([this]() {executor_.spin();});
  }

  ~Spinner()
  {
    executor_.cancel();
    thread_.join();
  }

private:
  rclcpp::executors::SingleThreadedExecutor executor_;
  std::thread thread_;
};

}  // namespace interpretation_test

#endif  // INTERPRETATION__FAKE_INTERPRETATION_SERVER_HPP_
