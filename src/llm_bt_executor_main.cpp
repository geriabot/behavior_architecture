#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "behaviortree_cpp/blackboard.h"
#include "lifecycle_msgs/msg/transition.hpp"
#include "rclcpp/executors/multi_threaded_executor.hpp"
#include "rclcpp/rclcpp.hpp"

#include "behavior_architecture/llm_bt_orchestrator.hpp"

namespace
{

void print_usage()
{
  std::cerr
    << "Usage: ros2 run behavior_architecture llm_bt_executor -- --objective <file> [options]\n"
    << "Options:\n"
    << "  --capabilities <file>          Capabilities YAML file.\n"
    << "  --plugin-library <file>        Repeatable BT plugin library.\n"
    << "  --bt-nodes-package <pkg>       Repeatable package used to auto-load capabilities.\n"
    << "  --task-name <name>             Name used for metric output directories.\n"
    << "  --mission-name <name>          Backward-compatible alias for --task-name.\n"
    << "  --exec-dir <dir>               Base directory for metrics and generated XMLs.\n"
    << "  --timeout-sec <seconds>        BT execution timeout.\n"
    << "  --control-period-ms <ms>       Executor control period.\n"
    << "  --max-fixes <n>                Maximum FixBT attempts.\n"
    << "  --useful-info <text>           Extra YAML block appended before generation.\n"
    << "  --blackboard-seed <file>       YAML file with initial blackboard key-values.\n"
    << "  --strict-objective-inputs      Fail if objective inputs are missing in blackboard.\n"
    << "  --use-episodic-memory          Enable storage and retrieval of episodic memory.\n"
    << "  --no-save-exec                 Disable metric and artifact persistence.\n";
}

}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  const auto args = rclcpp::remove_ros_arguments(argc, argv);

  std::string objective_file;
  std::string capabilities_yaml;
  std::string task_name;
  std::string exec_dir = "exec/bt_generation_eval";
  std::string useful_info;
  std::string blackboard_seed_path;
  std::vector<std::string> plugin_libraries{"libsocial_bt_nodes_plugin.so"};
  std::vector<std::string> bt_nodes_packages{"social_bt_nodes"};
  int control_period_ms = 50;
  int max_fixes = 3;
  double timeout_sec = 30.0;
  bool save_exec = true;
  bool use_episodic_memory = false;
  bool strict_objective_inputs = false;

  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string arg = args[i];
    auto require_value = [&](const std::string & name) -> std::string {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for " << name << "\n";
        print_usage();
        std::exit(1);
      }
      return args[++i];
    };

    if (arg == "--objective") {
      objective_file = require_value(arg);
    } else if (arg == "--capabilities") {
      capabilities_yaml = require_value(arg);
    } else if (arg == "--plugin-library") {
      plugin_libraries.push_back(require_value(arg));
    } else if (arg == "--bt-nodes-package") {
      bt_nodes_packages.push_back(require_value(arg));
    } else if (arg == "--task-name" || arg == "--mission-name") {
      task_name = require_value(arg);
    } else if (arg == "--exec-dir") {
      exec_dir = require_value(arg);
    } else if (arg == "--timeout-sec") {
      timeout_sec = std::stod(require_value(arg));
    } else if (arg == "--control-period-ms") {
      control_period_ms = std::stoi(require_value(arg));
    } else if (arg == "--max-fixes") {
      max_fixes = std::stoi(require_value(arg));
    } else if (arg == "--useful-info") {
      useful_info = require_value(arg);
    } else if (arg == "--blackboard-seed") {
      blackboard_seed_path = require_value(arg);
    } else if (arg == "--strict-objective-inputs") {
      strict_objective_inputs = true;
    } else if (arg == "--use-episodic-memory") {
      use_episodic_memory = true;
    } else if (arg == "--no-save-exec") {
      save_exec = false;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage();
      return 1;
    }
  }

  if (objective_file.empty()) {
    print_usage();
    return 1;
  }

  if (plugin_libraries.size() > 1) {
    plugin_libraries.erase(plugin_libraries.begin());
  }
  if (bt_nodes_packages.size() > 1) {
    bt_nodes_packages.erase(bt_nodes_packages.begin());
  }

  auto blackboard = BT::Blackboard::create();
  auto node = std::make_shared<rclcpp::Node>("llm_bt_executor");
  blackboard->set("node", node);
  blackboard->set("llm_objective_path", objective_file);
  blackboard->set("llm_control_period_ms", control_period_ms);
  blackboard->set("llm_timeout_sec", timeout_sec);
  blackboard->set("llm_max_bt_regenerations", max_fixes);
  blackboard->set("llm_plugin_libraries", plugin_libraries);
  blackboard->set("llm_bt_nodes_packages", bt_nodes_packages);
  blackboard->set("llm_save_exec", save_exec);
  blackboard->set("llm_use_episodic_memory", use_episodic_memory);
  blackboard->set("llm_fail_on_missing_inputs", strict_objective_inputs);
  blackboard->set("llm_shutdown_on_completion", true);
  blackboard->set("llm_exec_dir", exec_dir);
  if (!capabilities_yaml.empty()) {
    blackboard->set("llm_capabilities_yaml", capabilities_yaml);
  }
  if (!task_name.empty()) {
    blackboard->set("llm_task_name", task_name);
    blackboard->set("llm_mission_name", task_name);
  }
  if (!useful_info.empty()) {
    blackboard->set("llm_useful_info", useful_info);
  }
  if (!blackboard_seed_path.empty()) {
    blackboard->set("llm_blackboard_seed_path", blackboard_seed_path);
  }

  auto orchestrator = std::make_shared<behavior_architecture::LLMBTOrchestrator>(blackboard);
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  orchestrator->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  for (auto & runner : orchestrator->get_runners()) {
    executor.add_node(runner->get_node_base_interface());
  }
  executor.add_node(orchestrator->get_node_base_interface());
  executor.spin();

  rclcpp::shutdown();
  return 0;
}