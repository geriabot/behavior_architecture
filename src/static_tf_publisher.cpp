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

#include <string>
#include <vector>
#include <memory>

#include "rclcpp/rclcpp.hpp"
#include "yaml-cpp/yaml.h"
#include "geometry_msgs/msg/transform_stamped.hpp"
#include "tf2_ros/static_transform_broadcaster.h"

class StaticTFPublisher : public rclcpp::Node {
public:
  StaticTFPublisher()
  : Node("static_tf_publisher")
  {
    this->declare_parameter<std::string>("tf_file", "");
    std::string yaml_file = this->get_parameter("tf_file").as_string();
    
    if (yaml_file.empty()) {
      RCLCPP_ERROR(this->get_logger(), "Parameter 'tf_file' is required");
      rclcpp::shutdown();
      return;
    }

    static_broadcaster_ = std::make_shared<tf2_ros::StaticTransformBroadcaster>(this);

    YAML::Node doc;
    try {
      doc = YAML::LoadFile(yaml_file);
    } catch (const YAML::Exception & e) {
      RCLCPP_ERROR(this->get_logger(), "Failed to load '%s': %s", yaml_file.c_str(), e.what());
      rclcpp::shutdown();
      return;
    }

    if (!doc["static_transforms"]) {
      RCLCPP_ERROR(this->get_logger(), "YAML file must contain a 'static_transforms' key");
      rclcpp::shutdown();
      return;
    }

    std::vector<geometry_msgs::msg::TransformStamped> static_transforms;
    for (const auto & t : doc["static_transforms"]) {
      geometry_msgs::msg::TransformStamped tf;
      
      // Use current time for the timestamp
      tf.header.stamp = this->get_clock()->now(); 
      tf.header.frame_id = t["base_frame"] ? t["base_frame"].as<std::string>() : "map";
      tf.child_frame_id = t["child_frame"] ? t["child_frame"].as<std::string>() : "";
      
      tf.transform.translation.x = t["x"] ? t["x"].as<double>() : 0.0;
      tf.transform.translation.y = t["y"] ? t["y"].as<double>() : 0.0;
      tf.transform.translation.z = t["z"] ? t["z"].as<double>() : 0.0;
      
      // Default to identity rotation
      tf.transform.rotation.x = 0.0;
      tf.transform.rotation.y = 0.0;
      tf.transform.rotation.z = 0.0;
      tf.transform.rotation.w = 1.0;

      RCLCPP_INFO(this->get_logger(), "Loaded static TF: %s -> %s (x=%.2f, y=%.2f, z=%.2f)",
        tf.header.frame_id.c_str(), tf.child_frame_id.c_str(),
        tf.transform.translation.x, tf.transform.translation.y, tf.transform.translation.z);

      static_transforms.push_back(tf);
    }
    
    RCLCPP_INFO(this->get_logger(), "Loaded %zu static_transforms from YAML", static_transforms.size());

    // Publish all static transformations at once
    if (!static_transforms.empty()) {
      static_broadcaster_->sendTransform(static_transforms);
      RCLCPP_INFO(this->get_logger(), "Published static TFs successfully.");
    }
  }

private:
  // Es importante mantener el broadcaster como miembro de la clase
  std::shared_ptr<tf2_ros::StaticTransformBroadcaster> static_broadcaster_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<StaticTFPublisher>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}