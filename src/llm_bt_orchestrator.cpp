#include "behavior_architecture/llm_bt_orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "ament_index_cpp/get_package_share_directory.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"
#include "behavior_architecture/yaml_utils.hpp"
#include "yaml-cpp/yaml.h"

namespace behavior_architecture
{

namespace
{

std::string to_lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
    [](unsigned char c) {return static_cast<char>(std::tolower(c));});
  return value;
}

/// Strip leading/trailing whitespace and BT.CPP port-remapping braces (`{key}` → `key`).
/// Returns an empty string if the result would be empty (e.g. input was `"{}"` or blank).
std::string normalize_blackboard_key(const std::string & key)
{
  auto is_ws = [](unsigned char c) {return std::isspace(c) != 0;};

  std::size_t begin = 0;
  std::size_t end = key.size();
  while (begin < end && is_ws(static_cast<unsigned char>(key[begin]))) {
    ++begin;
  }
  while (end > begin && is_ws(static_cast<unsigned char>(key[end - 1]))) {
    --end;
  }

  std::string normalized = key.substr(begin, end - begin);
  if (normalized.size() >= 2 && normalized.front() == '{' && normalized.back() == '}') {
    normalized = normalized.substr(1, normalized.size() - 2);
  }
  return normalized;
}

bool parse_bool_text(const std::string & raw, bool & out)
{
  const std::string lowered = to_lower(raw);
  if (lowered == "true" || lowered == "1" || lowered == "yes") {
    out = true;
    return true;
  }
  if (lowered == "false" || lowered == "0" || lowered == "no") {
    out = false;
    return true;
  }
  return false;
}

std::string sanitize_path_component(std::string value)
{
  // Keep directory names portable and deterministic across runs.
  for (char & c : value) {
    const bool is_allowed =
      std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
    if (!is_allowed) {
      c = '_';
    }
  }

  // Collapse repeated underscores for cleaner folder names.
  value = std::regex_replace(value, std::regex("_+"), "_");

  // Trim leading/trailing separators.
  while (!value.empty() && (value.front() == '_' || value.front() == '.')) {
    value.erase(value.begin());
  }
  while (!value.empty() && (value.back() == '_' || value.back() == '.')) {
    value.pop_back();
  }

  if (value.empty()) {
    return "objective";
  }
  return value;
}

}  // namespace

static OrchestratorRegistrar<LLMBTOrchestrator> llm_bt_registrar("llm_bt");

LLMBTOrchestrator::LLMBTOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("llm_bt_orchestrator", blackboard)
{
  try {
    plugin_libraries_ = blackboard_->get<std::vector<std::string>>("llm_plugin_libraries");
  } catch (...) {
    plugin_libraries_ = {"libsocial_bt_nodes_plugin.so"};
  }

  try {
    bt_nodes_packages_ = blackboard_->get<std::vector<std::string>>("llm_bt_nodes_packages");
  } catch (...) {
    bt_nodes_packages_ = {"social_bt_nodes"};
  }

  try {
    control_cycle_rate_ms_ = blackboard_->get<int>("llm_control_period_ms");
  } catch (...) {
    control_cycle_rate_ms_ = 50;
  }

  try {
    bt_timeout_sec_ = blackboard_->get<double>("llm_timeout_sec");
  } catch (...) {
    bt_timeout_sec_ = 30.0;
  }

  try {
    capabilities_yaml_ = blackboard_->get<std::string>("llm_capabilities_yaml");
  } catch (...) {
  }

  try {
    objective_path_ = blackboard_->get<std::string>("llm_objective_path");
  } catch (...) {
  }

  try {
    useful_info_ = blackboard_->get<std::string>("llm_useful_info");
  } catch (...) {
  }

  try {
    blackboard_seed_path_ = blackboard_->get<std::string>("llm_blackboard_seed_path");
  } catch (...) {
  }

  try {
    fail_on_missing_inputs_ = blackboard_->get<bool>("llm_fail_on_missing_inputs");
  } catch (...) {
    fail_on_missing_inputs_ = false;
  }

  try {
    save_exec_ = blackboard_->get<bool>("llm_save_exec");
  } catch (...) {
  }

  try {
    shutdown_on_completion_ = blackboard_->get<bool>("llm_shutdown_on_completion");
  } catch (...) {
  }

  try {
    exec_base_dir_ = blackboard_->get<std::string>("llm_exec_dir");
  } catch (...) {
  }

  try {
    task_name_ = blackboard_->get<std::string>("llm_task_name");
  } catch (...) {
  }

  if (task_name_.empty()) {
    try {
      task_name_ = blackboard_->get<std::string>("llm_mission_name");
    } catch (...) {
    }
  }

  try {
    max_bt_regenerations_ = blackboard_->get<int>("llm_max_bt_regenerations");
  } catch (...) {
  }

  try {
    use_episodic_memory_ = blackboard_->get<bool>("llm_use_episodic_memory");
  } catch (...) {
    use_episodic_memory_ = false;
  }

  if (use_episodic_memory_) {
    episodic_memory_dir_ = std::filesystem::path(exec_base_dir_) / "episodic_memory";
    bt_success_memory_path_ = episodic_memory_dir_ / "bt_success_cases.json";
    bt_failure_memory_path_ = episodic_memory_dir_ / "bt_failure_cases.json";
    std::error_code ec;
    std::filesystem::create_directories(episodic_memory_dir_, ec);
    if (!ec) {
      load_episodic_memory();
      RCLCPP_INFO(get_logger(), "Episodic memory enabled (path: %s)",
        episodic_memory_dir_.c_str());
    } else {
      RCLCPP_WARN(get_logger(), "Could not create episodic memory dir: %s",
        ec.message().c_str());
      use_episodic_memory_ = false;
    }
  }

  if (capabilities_yaml_.empty()) {
    std::string merged;
    for (const auto & pkg : bt_nodes_packages_) {
      try {
        const std::string path =
          ament_index_cpp::get_package_share_directory(pkg) +
          "/node_descriptions/" + pkg + ".yaml";
        const std::string loaded = load_file(path);
        if (!loaded.empty()) {
          merged += loaded + "\n";
          RCLCPP_INFO(get_logger(), "Loaded capabilities from package '%s': %s",
            pkg.c_str(), path.c_str());
        }
      } catch (const std::exception & e) {
        RCLCPP_WARN(get_logger(), "Could not load capabilities for package '%s': %s",
          pkg.c_str(), e.what());
      }
    }
    capabilities_yaml_ = merged;
  }

  generate_bt_client_ = create_client<llm_bt_builder::srv::GenerateBT>("generate_bt");
  fix_bt_client_ = create_client<llm_bt_builder::srv::FixBT>("fix_bt");
  status_pub_ = create_publisher<std_msgs::msg::String>("llm_bt_orchestrator/status", 10);

  RCLCPP_INFO(get_logger(),
    "LLMBTOrchestrator ready (objective='%s', period=%dms, timeout=%.1fs)",
    objective_path_.c_str(), control_cycle_rate_ms_, bt_timeout_sec_);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMBTOrchestrator::on_configure(const rclcpp_lifecycle::State & previous_state)
{
  auto runner = std::make_shared<BehaviorRunner>(
    blackboard_, "llm_bt_runner", "", plugin_libraries_, "behavior_architecture",
    control_cycle_rate_ms_);
  register_runner("llm_bt_runner", runner);
  return BaseOrchestrator::on_configure(previous_state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMBTOrchestrator::on_activate(const rclcpp_lifecycle::State & previous_state)
{
  status_pub_->on_activate();
  auto ret = BaseOrchestrator::on_activate(previous_state);
  start_run();
  return ret;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
LLMBTOrchestrator::on_deactivate(const rclcpp_lifecycle::State & previous_state)
{
  if (tree_loaded_) {
    deactivate_runner("llm_bt_runner");
    tree_loaded_ = false;
  }
  status_pub_->on_deactivate();
  return BaseOrchestrator::on_deactivate(previous_state);
}

void LLMBTOrchestrator::start_run()
{
  run_metrics_ = RunMetrics{};
  run_start_time_ = std::chrono::steady_clock::now();
  bt_regeneration_count_ = 0;
  last_failure_reason_.clear();
  last_failure_code_.clear();
  last_bt_xml_.clear();
  tree_loaded_ = false;
  current_attempt_index_.reset();

  if (!load_objective_file()) {
    last_failure_reason_ = "Could not load objective YAML from: " + objective_path_;
    publish_status("OBJECTIVE_LOAD_FAILED");
    transition_to(State::FAILED);
    return;
  }

  // Capture baseline keys before loading seed values so seeded vars are considered
  // dynamic context and can be injected into GenerateBT/FixBT prompts.
  initial_blackboard_keys_.clear();
  for (const auto & key : blackboard_->getKeys()) {
    initial_blackboard_keys_.emplace_back(key.data(), key.size());
  }

  if (!preload_blackboard_seed()) {
    publish_status("BLACKBOARD_SEED_LOAD_FAILED");
    transition_to(State::FAILED);
    return;
  }

  if (!validate_required_inputs()) {
    publish_status("OBJECTIVE_INPUTS_MISSING");
    transition_to(State::FAILED);
    return;
  }

  if (task_name_.empty()) {
    task_name_ = objective_name_.empty() ? std::filesystem::path(objective_path_).stem().string() :
      objective_name_;
  }

  if (save_exec_) {
    initialize_execution_dir();
  }
  request_generate_bt();
}

bool LLMBTOrchestrator::load_objective_file()
{
  objective_yaml_ = load_file(objective_path_);
  if (objective_yaml_.empty()) {
    return false;
  }

  objective_inputs_.clear();

  try {
    const YAML::Node root = YAML::Load(objective_yaml_);
    const YAML::Node objective_node = root["objective"] ? root["objective"] : root;
    if (objective_node["name"]) {
      objective_name_ = objective_node["name"].as<std::string>();
    }
    if (objective_node["description"]) {
      objective_description_ = objective_node["description"].as<std::string>();
    }
    if (objective_node["steps"] && objective_node["steps"].IsSequence()) {
      run_metrics_.objective_step_count = static_cast<int>(objective_node["steps"].size());
    }
    if (objective_node["constraints"]) {
      if (objective_node["constraints"].IsMap() || objective_node["constraints"].IsSequence()) {
        run_metrics_.objective_constraint_count = static_cast<int>(objective_node["constraints"].size());
      } else {
        run_metrics_.objective_constraint_count = 1;
      }
    }
    if (objective_node["inputs"] && objective_node["inputs"].IsSequence()) {
      for (const auto & input_node : objective_node["inputs"]) {
        if (input_node.IsScalar()) {
          const std::string normalized = normalize_blackboard_key(input_node.as<std::string>());
          if (!normalized.empty()) {
            objective_inputs_.push_back(normalized);
          }
        }
      }
    }
  } catch (const YAML::Exception & e) {
    RCLCPP_WARN(get_logger(), "Objective YAML parsed with warnings: %s", e.what());
  }

  run_metrics_.objective_chars = static_cast<int>(objective_yaml_.size());
  run_metrics_.objective_lines = static_cast<int>(std::count(
    objective_yaml_.begin(), objective_yaml_.end(), '\n')) + 1;

  RCLCPP_INFO(get_logger(), "Loaded objective '%s' from %s",
    objective_name_.c_str(), objective_path_.c_str());
  return true;
}

bool LLMBTOrchestrator::preload_blackboard_seed()
{
  if (blackboard_seed_path_.empty()) {
    return true;
  }

  const std::string seed_yaml = load_file(blackboard_seed_path_);
  if (seed_yaml.empty()) {
    last_failure_reason_ = "Could not load blackboard seed YAML from: " + blackboard_seed_path_;
    return false;
  }

  YAML::Node root;
  try {
    root = YAML::Load(seed_yaml);
  } catch (const YAML::Exception & e) {
    last_failure_reason_ = std::string("Invalid blackboard seed YAML: ") + e.what();
    return false;
  }

  YAML::Node values = root;
  if (root["blackboard"] && root["blackboard"].IsMap()) {
    values = root["blackboard"];
  }
  if (!values.IsMap()) {
    last_failure_reason_ =
      "Blackboard seed must be a YAML map (or contain a 'blackboard' map). File: " +
      blackboard_seed_path_;
    return false;
  }

  std::size_t loaded_count = 0;
  for (const auto & entry : values) {
    if (!entry.first.IsScalar()) {
      continue;
    }
    const std::string raw_key = entry.first.as<std::string>();
    if (normalize_blackboard_key(raw_key).empty()) {
      continue;
    }
    set_blackboard_seed_value(raw_key, entry.second);
    ++loaded_count;
  }

  RCLCPP_INFO(get_logger(), "Loaded %zu blackboard seed values from %s",
    loaded_count, blackboard_seed_path_.c_str());
  return true;
}

bool LLMBTOrchestrator::validate_required_inputs()
{
  if (objective_inputs_.empty()) {
    return true;
  }

  std::vector<std::string> missing;
  for (const auto & key : objective_inputs_) {
    if (!has_blackboard_key(key)) {
      missing.push_back(key);
    }
  }

  if (missing.empty()) {
    return true;
  }

  std::ostringstream oss;
  for (std::size_t i = 0; i < missing.size(); ++i) {
    oss << missing[i];
    if (i + 1 < missing.size()) {
      oss << ", ";
    }
  }

  const std::string missing_text = oss.str();

  // Build a suggested seed YAML snippet for the user
  std::ostringstream seed_hint;
  seed_hint << "Create a seed YAML file with:\n  blackboard:\n";
  for (const auto & key : missing) {
    seed_hint << "    " << key << ": <value>\n";
  }
  seed_hint << "Then pass: --blackboard-seed <path/to/seed.yaml>";

  if (!fail_on_missing_inputs_) {
    RCLCPP_WARN(get_logger(),
      "Objective '%s' declares inputs not yet in the blackboard: [%s].\n"
      "The BT will be generated referencing these values via '{key}' port remapping.\n"
      "If they are populated by previous BT steps this is fine, otherwise provide a seed.\n"
      "%s",
      objective_name_.c_str(), missing_text.c_str(), seed_hint.str().c_str());
    return true;
  }

  last_failure_reason_ =
    "Missing required objective inputs on blackboard: [" + missing_text +
    "]. " + seed_hint.str();
  return false;
}

bool LLMBTOrchestrator::has_blackboard_key(const std::string & key) const
{
  const std::string normalized = normalize_blackboard_key(key);
  if (normalized.empty()) {
    return false;
  }
  const auto keys = blackboard_->getKeys();
  return std::any_of(keys.begin(), keys.end(), [&normalized](const auto & bb_key) {
    return normalized == normalize_blackboard_key(std::string{bb_key.data(), bb_key.size()});
  });
}

void LLMBTOrchestrator::set_blackboard_seed_value(const std::string & key, const YAML::Node & value)
{
  const std::string normalized_key = normalize_blackboard_key(key);
  if (normalized_key.empty()) {
    return;
  }

  auto set_auto_scalar = [&](const std::string & raw) {
    auto set_scalar = [&](const auto & v) {
        blackboard_->set(normalized_key, v);
        blackboard_->set("{" + normalized_key + "}", v);
      };

      bool bool_value = false;
    if (parse_bool_text(raw, bool_value)) {
      set_scalar(bool_value);
      return;
    }

    try {
      std::size_t idx = 0;
      const int int_value = std::stoi(raw, &idx);
      if (idx == raw.size()) {
        set_scalar(int_value);
        return;
      }
    } catch (...) {
    }

    try {
      std::size_t idx = 0;
      const double double_value = std::stod(raw, &idx);
      if (idx == raw.size()) {
        set_scalar(double_value);
        return;
      }
    } catch (...) {
    }

    set_scalar(raw);
  };

  auto set_typed_scalar = [&](const std::string & type_name, const YAML::Node & value_node) -> bool {
      const std::string type = to_lower(type_name);
      if (type == "string") {
        const auto v = value_node.as<std::string>();
        blackboard_->set(normalized_key, v);
        blackboard_->set("{" + normalized_key + "}", v);
        return true;
      }
      if (type == "int" || type == "int32" || type == "integer") {
        const auto v = value_node.as<int>();
        blackboard_->set(normalized_key, v);
        blackboard_->set("{" + normalized_key + "}", v);
        return true;
      }
      if (type == "double" || type == "float") {
        const auto v = value_node.as<double>();
        blackboard_->set(normalized_key, v);
        blackboard_->set("{" + normalized_key + "}", v);
        return true;
      }
      if (type == "bool" || type == "boolean") {
        if (value_node.IsScalar()) {
          bool parsed_bool = false;
          if (parse_bool_text(value_node.as<std::string>(), parsed_bool)) {
            blackboard_->set(normalized_key, parsed_bool);
            blackboard_->set("{" + normalized_key + "}", parsed_bool);
            return true;
          }
        }
        const auto v = value_node.as<bool>();
        blackboard_->set(normalized_key, v);
        blackboard_->set("{" + normalized_key + "}", v);
        return true;
      }
      return false;
    };

  auto set_typed_sequence = [&](const std::string & type_name, const YAML::Node & value_node) -> bool {
      if (!value_node.IsSequence()) {
        return false;
      }

      const std::string type = to_lower(type_name);
      if (type == "string_array" || type == "vector_string") {
        std::vector<std::string> out;
        out.reserve(value_node.size());
        for (const auto & item : value_node) {
          out.push_back(item.as<std::string>());
        }
        blackboard_->set(normalized_key, out);
        blackboard_->set("{" + normalized_key + "}", out);
        return true;
      }
      if (type == "int_array" || type == "vector_int") {
        std::vector<int> out;
        out.reserve(value_node.size());
        for (const auto & item : value_node) {
          out.push_back(item.as<int>());
        }
        blackboard_->set(normalized_key, out);
        blackboard_->set("{" + normalized_key + "}", out);
        return true;
      }
      if (type == "double_array" || type == "float_array" || type == "vector_double") {
        std::vector<double> out;
        out.reserve(value_node.size());
        for (const auto & item : value_node) {
          out.push_back(item.as<double>());
        }
        blackboard_->set(normalized_key, out);
        blackboard_->set("{" + normalized_key + "}", out);
        return true;
      }
      if (type == "bool_array" || type == "vector_bool") {
        std::vector<bool> out;
        out.reserve(value_node.size());
        for (const auto & item : value_node) {
          if (!item.IsScalar()) {
            return false;
          }
          bool parsed = false;
          if (!parse_bool_text(item.as<std::string>(), parsed)) {
            return false;
          }
          out.push_back(parsed);
        }
        blackboard_->set(normalized_key, out);
        blackboard_->set("{" + normalized_key + "}", out);
        return true;
      }
      return false;
    };

  if (!value || value.IsNull()) {
    blackboard_->set(normalized_key, std::string{});
    return;
  }

  if (value.IsMap() && value["value"]) {
    const YAML::Node value_node = value["value"];
    if (value["type"] && value["type"].IsScalar()) {
      const std::string type_name = value["type"].as<std::string>();
      try {
        if (set_typed_scalar(type_name, value_node) || set_typed_sequence(type_name, value_node)) {
          return;
        }
        if (to_lower(type_name) == "yaml") {
          const auto v = YAML::Dump(value_node);
          blackboard_->set(normalized_key, v);
          blackboard_->set("{" + normalized_key + "}", v);
          return;
        }
      } catch (const std::exception & e) {
        RCLCPP_WARN(get_logger(),
          "Invalid typed blackboard seed for key '%s' (type=%s): %s. Falling back to string serialization.",
          normalized_key.c_str(), type_name.c_str(), e.what());
      }
    }

    if (value_node.IsScalar()) {
      set_auto_scalar(value_node.as<std::string>());
    } else {
      const auto v = YAML::Dump(value_node);
      blackboard_->set(normalized_key, v);
      blackboard_->set("{" + normalized_key + "}", v);
    }
    return;
  }

  if (value.IsScalar()) {
    set_auto_scalar(value.as<std::string>());
    return;
  }

  if (value.IsSequence()) {
    bool all_int = true;
    bool all_double = true;
    bool all_bool = true;
    bool all_string = true;

    std::vector<int> ints;
    std::vector<double> doubles;
    std::vector<bool> bools;
    std::vector<std::string> strings;
    ints.reserve(value.size());
    doubles.reserve(value.size());
    bools.reserve(value.size());
    strings.reserve(value.size());

    for (const auto & item : value) {
      if (!item.IsScalar()) {
        all_int = false;
        all_double = false;
        all_bool = false;
        all_string = false;
        break;
      }

      const std::string raw = item.as<std::string>();
      strings.push_back(raw);

      try {
        ints.push_back(item.as<int>());
      } catch (...) {
        all_int = false;
      }

      try {
        doubles.push_back(item.as<double>());
      } catch (...) {
        all_double = false;
      }

      bool parsed_bool = false;
      if (parse_bool_text(raw, parsed_bool)) {
        bools.push_back(parsed_bool);
      } else {
        all_bool = false;
      }
    }

    if (all_int) {
      blackboard_->set(normalized_key, ints);
      blackboard_->set("{" + normalized_key + "}", ints);
      return;
    }
    if (all_double) {
      blackboard_->set(normalized_key, doubles);
      blackboard_->set("{" + normalized_key + "}", doubles);
      return;
    }
    if (all_bool) {
      blackboard_->set(normalized_key, bools);
      blackboard_->set("{" + normalized_key + "}", bools);
      return;
    }
    if (all_string) {
      blackboard_->set(normalized_key, strings);
      blackboard_->set("{" + normalized_key + "}", strings);
      return;
    }
  }

  const auto v = YAML::Dump(value);
  blackboard_->set(normalized_key, v);
  blackboard_->set("{" + normalized_key + "}", v);
}

void LLMBTOrchestrator::initialize_execution_dir()
{
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  std::ostringstream timestamp;
  timestamp << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");

  std::string run_group = objective_name_;
  if (run_group.empty()) {
    run_group = task_name_;
  }
  run_group = sanitize_path_component(run_group);

  exec_run_dir_ = std::filesystem::path(exec_base_dir_) / run_group / timestamp.str();
  std::filesystem::create_directories(exec_run_dir_);

  std::ofstream objective_out(exec_run_dir_ / "objective.yaml");
  if (objective_out.is_open()) {
    objective_out << objective_yaml_;
  }

  RCLCPP_INFO(get_logger(), "BT evaluation directory: %s", exec_run_dir_.c_str());
}

void LLMBTOrchestrator::initialize_attempt_tracking(const std::string & attempt_type)
{
  AttemptMetrics attempt;
  attempt.attempt_id = static_cast<int>(run_metrics_.attempts.size());
  attempt.attempt_type = attempt_type;
  run_metrics_.attempts.push_back(attempt);
  current_attempt_index_ = run_metrics_.attempts.size() - 1;
  bt_gen_start_time_ = std::chrono::steady_clock::now();
}

int LLMBTOrchestrator::append_dynamic_blackboard_vars(std::string & yaml_text) const
{
  const auto keys = blackboard_->getKeys();
  if (keys.empty()) {
    return 0;
  }

  std::string vars_block = "\navailable_blackboard_vars:";
  int injected_count = 0;
  for (const auto & key : keys) {
    std::string key_str{key.data(), key.size()};
    if (key_str.empty() || key_str[0] == '_' || key_str == "bt_last_failure" ||
      key_str == "bt_last_failure_code")
    {
      continue;
    }
    if (key_str.front() == '{' && key_str.back() == '}') {
      // Internal alias used only for compatibility with non-standard port lookup.
      continue;
    }
    if (std::find(initial_blackboard_keys_.begin(), initial_blackboard_keys_.end(), key_str) !=
      initial_blackboard_keys_.end())
    {
      continue;
    }
    vars_block += "\n  - " + key_str;
    injected_count++;
  }

  if (injected_count > 0) {
    yaml_text += vars_block;
  }
  return injected_count;
}

void LLMBTOrchestrator::request_generate_bt()
{
  while (rclcpp::ok() && !generate_bt_client_->wait_for_service(std::chrono::seconds(1))) {
    RCLCPP_WARN(get_logger(), "Waiting for generate_bt service...");
  }
  if (!rclcpp::ok()) {
    return;
  }

  initialize_attempt_tracking("generate");

  std::string bt_nodes_yaml = capabilities_yaml_;
  if (!capabilities_yaml_.empty() && capabilities_yaml_.find('\n') == std::string::npos &&
    capabilities_yaml_.size() > 5 &&
    capabilities_yaml_.substr(capabilities_yaml_.size() - 5) == ".yaml")
  {
    bt_nodes_yaml = load_file(capabilities_yaml_);
  }

  std::string enriched_objective = objective_yaml_;
  if (!useful_info_.empty()) {
    enriched_objective = append_yaml_multiline_block(enriched_objective, "useful_info", useful_info_);
  }

  const int injected_count = append_dynamic_blackboard_vars(enriched_objective);
  if (injected_count > 0) {
    RCLCPP_INFO(get_logger(), "Injecting %d dynamic blackboard vars", injected_count);
    if (current_attempt_index_.has_value()) {
      auto & attempt = run_metrics_.attempts[*current_attempt_index_];
      attempt.injected_blackboard_vars = injected_count;
      run_metrics_.total_injected_blackboard_vars += injected_count;
    }
  }

  auto request = std::make_shared<llm_bt_builder::srv::GenerateBT::Request>();
  request->objective = enriched_objective;
  request->bt_nodes_yaml = bt_nodes_yaml;

  RCLCPP_INFO(get_logger(), "Requesting BT generation for objective '%s'", objective_name_.c_str());
  gen_bt_future_ = generate_bt_client_->async_send_request(request);
  transition_to(State::GENERATING_BT);
}

void LLMBTOrchestrator::request_fix_bt(const std::string & broken_xml, const std::string & error_msg)
{
  while (rclcpp::ok() && !fix_bt_client_->wait_for_service(std::chrono::seconds(1))) {
    RCLCPP_WARN(get_logger(), "Waiting for fix_bt service...");
  }
  if (!rclcpp::ok()) {
    return;
  }

  bt_regeneration_count_++;
  run_metrics_.fix_count++;
  initialize_attempt_tracking("fix");

  std::string bt_nodes_yaml = capabilities_yaml_;
  if (!capabilities_yaml_.empty() && capabilities_yaml_.find('\n') == std::string::npos &&
    capabilities_yaml_.size() > 5 &&
    capabilities_yaml_.substr(capabilities_yaml_.size() - 5) == ".yaml")
  {
    bt_nodes_yaml = load_file(capabilities_yaml_);
  }

  std::string enriched_objective = objective_yaml_;
  if (!useful_info_.empty()) {
    enriched_objective = append_yaml_multiline_block(enriched_objective, "useful_info", useful_info_);
  }

  const int injected_count = append_dynamic_blackboard_vars(enriched_objective);
  if (injected_count > 0) {
    RCLCPP_INFO(get_logger(), "Injecting %d dynamic blackboard vars for FixBT", injected_count);
    if (current_attempt_index_.has_value()) {
      auto & attempt = run_metrics_.attempts[*current_attempt_index_];
      attempt.injected_blackboard_vars = injected_count;
      run_metrics_.total_injected_blackboard_vars += injected_count;
    }
  }

  auto request = std::make_shared<llm_bt_builder::srv::FixBT::Request>();
  request->objective = enriched_objective;
  request->broken_bt_xml = broken_xml;
  request->error_message = error_msg;
  request->bt_nodes_yaml = bt_nodes_yaml;

  RCLCPP_WARN(get_logger(), "Requesting BT fix attempt %d: %s",
    bt_regeneration_count_, error_msg.c_str());
  fix_bt_future_ = fix_bt_client_->async_send_request(request);
  transition_to(State::WAITING_FIX_BT);
}

void LLMBTOrchestrator::control_cycle()
{
  switch (state_) {
    case State::IDLE:
    case State::SUCCESS:
    case State::FAILED:
      break;

    case State::GENERATING_BT:
    {
      if (!gen_bt_future_.has_value()) {
        break;
      }
      if (gen_bt_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        break;
      }

      auto result = gen_bt_future_->get();
      gen_bt_future_.reset();
      auto & attempt = run_metrics_.attempts[*current_attempt_index_];
      attempt.generation_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - bt_gen_start_time_).count();
      run_metrics_.total_generation_time_ms += attempt.generation_time_ms;

      if (!result->success || result->bt_xml.empty()) {
        attempt.status = "GENERATION_FAILED";
        attempt.failure_reason = result->message;
        last_failure_reason_ = result->message.empty() ? "BT generation returned empty XML" : result->message;
        publish_status("BT_GENERATION_FAILED");
        transition_to(State::FAILED);
        break;
      }

      try {
        last_bt_xml_ = result->bt_xml;
        analyze_bt_xml(result->bt_xml, attempt);
        attempt.bt_xml_file = save_bt_xml(result->bt_xml, attempt.attempt_id, attempt.attempt_type);
        auto it = runners_.find("llm_bt_runner");
        auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
        runner->set_bt(result->bt_xml);
        last_status_.clear();
        status_received_.clear();
        activate_runner("llm_bt_runner");
        tree_loaded_ = true;
        step_start_time_ = now();
        transition_to(State::EXECUTING_BT);
      } catch (const std::exception & e) {
        attempt.status = "LOAD_FAILED";
        attempt.failure_reason = e.what();
        last_failure_reason_ = std::string("createTreeFromText exception: ") + e.what();
        if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < max_bt_regenerations_) {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else {
          publish_status("BT_LOAD_FAILED");
          transition_to(State::FAILED);
        }
      }
      break;
    }

    case State::WAITING_FIX_BT:
    {
      if (!fix_bt_future_.has_value()) {
        break;
      }
      if (fix_bt_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        break;
      }

      auto result = fix_bt_future_->get();
      fix_bt_future_.reset();
      auto & attempt = run_metrics_.attempts[*current_attempt_index_];
      attempt.generation_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - bt_gen_start_time_).count();
      run_metrics_.total_generation_time_ms += attempt.generation_time_ms;
      run_metrics_.total_fix_time_ms += attempt.generation_time_ms;

      if (!result->success || result->bt_xml.empty()) {
        attempt.status = "FIX_FAILED";
        attempt.failure_reason = result->message;
        last_failure_reason_ = result->message.empty() ? "FixBT returned empty XML" : result->message;
        publish_status("BT_FIX_FAILED");
        transition_to(State::FAILED);
        break;
      }

      try {
        last_bt_xml_ = result->bt_xml;
        attempt.bt_xml_file = save_bt_xml(result->bt_xml, attempt.attempt_id, attempt.attempt_type);
        auto it = runners_.find("llm_bt_runner");
        auto runner = std::dynamic_pointer_cast<BehaviorRunner>(it->second);
        runner->set_bt(result->bt_xml);
        last_status_.clear();
        status_received_.clear();
        activate_runner("llm_bt_runner");
        tree_loaded_ = true;
        step_start_time_ = now();
        transition_to(State::EXECUTING_BT);
      } catch (const std::exception & e) {
        attempt.status = "LOAD_FAILED";
        attempt.failure_reason = e.what();
        last_failure_reason_ = std::string("createTreeFromText exception: ") + e.what();
        if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < max_bt_regenerations_) {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else {
          publish_status("BT_LOAD_FAILED");
          transition_to(State::FAILED);
        }
      }
      break;
    }

    case State::EXECUTING_BT:
    {
      if (!tree_loaded_) {
        break;
      }

      auto & attempt = run_metrics_.attempts[*current_attempt_index_];
      const double elapsed = (now() - step_start_time_).seconds();
      if (elapsed > bt_timeout_sec_) {
        deactivate_runner("llm_bt_runner");
        tree_loaded_ = false;
        attempt.execution_time_ms = (now() - step_start_time_).to_chrono<std::chrono::milliseconds>().count();
        attempt.status = "TIMEOUT";
        attempt.failure_code = "timeout";
        attempt.failure_reason = "Behavior tree execution timed out";
        run_metrics_.total_execution_time_ms += attempt.execution_time_ms;
        last_failure_code_ = attempt.failure_code;
        last_failure_reason_ = attempt.failure_reason;
        publish_status("BT_TIMEOUT");
        transition_to(State::FAILED);
        break;
      }

      if (!check_behavior_finished()) {
        break;
      }

      const bool success = (last_status_ == "SUCCESS");
      if (!success) {
        last_failure_reason_ = collect_failure_reason();
      }

      attempt.execution_time_ms = (now() - step_start_time_).to_chrono<std::chrono::milliseconds>().count();
      attempt.total_attempt_time_ms = attempt.generation_time_ms + attempt.execution_time_ms;
      run_metrics_.total_execution_time_ms += attempt.execution_time_ms;
      deactivate_runner("llm_bt_runner");
      tree_loaded_ = false;

      if (success) {
        attempt.status = "SUCCESS";
        run_metrics_.first_success_attempt_id = attempt.attempt_id;
        run_metrics_.success_after_fix = (attempt.attempt_type == "fix");
        publish_status("BT_SUCCESS");
        transition_to(State::SUCCESS);
      } else {
        attempt.status = "FAILURE";
        attempt.failure_reason = last_failure_reason_;
        attempt.failure_code = last_failure_code_;
        publish_status("BT_FAILURE");
        if (is_missing_seed_error(last_failure_reason_)) {
          // The BT is correct but a blackboard key referenced via {key} port remapping
          // was not present. Asking the LLM to fix this would produce the same BT.
          // Fail with a clear message guiding the user to provide a seed file.
          std::string seed_suggestion = "The BT references a blackboard key via {key} port "
            "remapping that was not populated. ";
          if (!objective_inputs_.empty()) {
            seed_suggestion += "Objective '" + objective_name_ + "' declares inputs: [";
            for (std::size_t i = 0; i < objective_inputs_.size(); ++i) {
              seed_suggestion += objective_inputs_[i];
              if (i + 1 < objective_inputs_.size()) {seed_suggestion += ", ";}
            }
            seed_suggestion += "]. ";
          }
          seed_suggestion += "Provide --blackboard-seed <yaml> with these values or run "
            "the full plan so previous BT steps populate the blackboard first.";
          RCLCPP_ERROR(get_logger(), "%s", seed_suggestion.c_str());
          last_failure_reason_ = seed_suggestion;
          transition_to(State::FAILED);
        } else if (is_local_error(last_failure_reason_) && bt_regeneration_count_ < max_bt_regenerations_ &&
          !last_bt_xml_.empty())
        {
          request_fix_bt(last_bt_xml_, last_failure_reason_);
        } else {
          transition_to(State::FAILED);
        }
      }
      break;
    }
  }
}

std::string LLMBTOrchestrator::collect_failure_reason()
{
  std::string bb_reason;
  std::string bb_code;
  try {
    bb_reason = blackboard_->get<std::string>("bt_last_failure");
  } catch (...) {
  }
  try {
    bb_code = blackboard_->get<std::string>("bt_last_failure_code");
  } catch (...) {
  }

  blackboard_->set("bt_last_failure", std::string{});
  blackboard_->set("bt_last_failure_code", std::string{});
  last_failure_code_ = bb_code;

  if (bb_reason.empty()) {
    return "BT returned FAILURE (no details written to blackboard)";
  }
  return bb_reason;
}

bool LLMBTOrchestrator::is_missing_seed_error(const std::string & reason) const
{
  // "missing required input 'X', received: ''" means the port {key} existed in XML
  // but the blackboard had no value — this is a seed issue, not a BT structure bug.
  if (reason.find("missing required input") != std::string::npos &&
    reason.find("received: ''") != std::string::npos)
  {
    return true;
  }
  return false;
}

bool LLMBTOrchestrator::is_local_error(const std::string & reason) const
{
  // A "missing required input" with an empty received value means the blackboard
  // key referenced by {key} port remapping was not present. The BT XML is structurally
  // correct — asking the LLM to fix it would produce the same BT. Treat as non-local.
  if (is_missing_seed_error(reason)) {
    return false;
  }

  if (last_failure_code_ == "bt_config_error") {
    return true;
  }

  if (reason.empty() || reason == "BT returned FAILURE (no details written to blackboard)") {
    return false;
  }

  static const std::vector<std::string> local_keywords = {
    "createTreeFromText exception",
    "missing port",
    "syntax",
    "parse",
    "XML",
    "Node configuration"
  };

  for (const auto & keyword : local_keywords) {
    if (reason.find(keyword) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string LLMBTOrchestrator::load_file(const std::string & path) const
{
  if (path.empty()) {
    return {};
  }

  std::ifstream input(path);
  if (!input.is_open()) {
    RCLCPP_WARN(get_logger(), "Cannot open file: %s", path.c_str());
    return {};
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string LLMBTOrchestrator::state_name(State state) const
{
  switch (state) {
    case State::IDLE:
      return "IDLE";
    case State::GENERATING_BT:
      return "GENERATING_BT";
    case State::WAITING_FIX_BT:
      return "WAITING_FIX_BT";
    case State::EXECUTING_BT:
      return "EXECUTING_BT";
    case State::SUCCESS:
      return "SUCCESS";
    case State::FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void LLMBTOrchestrator::analyze_bt_xml(const std::string & bt_xml, AttemptMetrics & attempt)
{
  attempt.bt_xml_chars = static_cast<int>(bt_xml.size());
  attempt.bt_xml_lines = static_cast<int>(std::count(bt_xml.begin(), bt_xml.end(), '\n')) + 1;

  static const std::regex tag_regex(R"(<\s*(/?)\s*([A-Za-z_][A-Za-z0-9_:\-\.]*)[^>]*?(\/?)\s*>)");
  int depth = 0;
  int max_depth = 0;
  int node_count = 0;

  for (std::sregex_iterator it(bt_xml.begin(), bt_xml.end(), tag_regex), end; it != end; ++it) {
    const std::string closing = (*it)[1].str();
    const std::string tag_name = (*it)[2].str();
    const std::string self_closing = (*it)[3].str();

    if (tag_name == "root" || tag_name == "BehaviorTree" || tag_name == "TreeNodesModel") {
      continue;
    }

    if (closing == "/") {
      depth = std::max(0, depth - 1);
      continue;
    }

    node_count++;
    depth++;
    max_depth = std::max(max_depth, depth);

    if (self_closing == "/") {
      depth = std::max(0, depth - 1);
    }
  }

  attempt.bt_node_count = node_count;
  attempt.bt_tree_depth = max_depth;
  run_metrics_.max_bt_node_count = std::max(run_metrics_.max_bt_node_count, node_count);
  run_metrics_.max_bt_tree_depth = std::max(run_metrics_.max_bt_tree_depth, max_depth);
}

std::string LLMBTOrchestrator::save_bt_xml(
  const std::string & bt_xml,
  int attempt_id,
  const std::string & attempt_type)
{
  if (!save_exec_ || exec_run_dir_.empty()) {
    return {};
  }

  std::ostringstream filename;
  filename << "attempt_" << std::setw(2) << std::setfill('0') << attempt_id << "_" << attempt_type << ".xml";
  const auto path = exec_run_dir_ / filename.str();
  std::ofstream output(path);
  if (!output.is_open()) {
    RCLCPP_WARN(get_logger(), "Could not save BT XML to: %s", path.c_str());
    return {};
  }

  output << bt_xml;
  return filename.str();
}

void LLMBTOrchestrator::publish_status(const std::string & msg)
{
  std_msgs::msg::String out;
  out.data = msg;
  status_pub_->publish(out);
}

void LLMBTOrchestrator::transition_to(State new_state)
{
  const State prev_state = state_;
  RCLCPP_INFO(get_logger(), "State: %s -> %s",
    state_name(state_).c_str(), state_name(new_state).c_str());
  state_ = new_state;

  // Record successful BT case to episodic memory
  if (prev_state == State::EXECUTING_BT && new_state == State::SUCCESS && use_episodic_memory_ && !last_bt_xml_.empty()) {
    record_success_case(objective_name_, last_bt_xml_);
  }

  // Record failure case to episodic memory
  if (new_state == State::FAILED && use_episodic_memory_) {
    std::string failure_cause = last_failure_reason_;
    if (failure_cause.empty()) {
      failure_cause = "Unknown failure";
    }
    record_failure_case(objective_name_, failure_cause, last_bt_xml_);
  }

  if (new_state == State::SUCCESS || new_state == State::FAILED) {
    run_metrics_.succeeded = (new_state == State::SUCCESS);
    run_metrics_.total_wall_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - run_start_time_).count();
    if (save_exec_) {
      write_metrics_files();
    }
    if (use_episodic_memory_) {
      save_episodic_memory();
    }
    if (shutdown_on_completion_ && rclcpp::ok()) {
      rclcpp::shutdown();
    }
  }
}

void LLMBTOrchestrator::write_metrics_files()
{
  if (exec_run_dir_.empty()) {
    return;
  }

  const auto attempts_path = exec_run_dir_ / "attempt_metrics.csv";
  std::ofstream attempts_csv(attempts_path);
  if (attempts_csv.is_open()) {
    attempts_csv << "task_name,objective_name,objective_path,attempt_id,attempt_type,injected_blackboard_vars,generation_time_ms,execution_time_ms,total_attempt_time_ms,bt_xml_chars,bt_xml_lines,bt_node_count,bt_tree_depth,status,failure_code,failure_reason,bt_xml_file\n";
    for (const auto & attempt : run_metrics_.attempts) {
      attempts_csv
        << csv_escape(task_name_) << ","
        << csv_escape(objective_name_) << ","
        << csv_escape(objective_path_) << ","
        << attempt.attempt_id << ","
        << csv_escape(attempt.attempt_type) << ","
        << attempt.injected_blackboard_vars << ","
        << attempt.generation_time_ms << ","
        << attempt.execution_time_ms << ","
        << attempt.total_attempt_time_ms << ","
        << attempt.bt_xml_chars << ","
        << attempt.bt_xml_lines << ","
        << attempt.bt_node_count << ","
        << attempt.bt_tree_depth << ","
        << csv_escape(attempt.status) << ","
        << csv_escape(attempt.failure_code) << ","
        << csv_escape(attempt.failure_reason) << ","
        << csv_escape(attempt.bt_xml_file) << "\n";
    }
  }

  const auto summary_path = exec_run_dir_ / "run_summary.csv";
  std::ofstream summary_csv(summary_path);
  if (summary_csv.is_open()) {
    summary_csv << "task_name,objective_name,objective_path,objective_chars,objective_lines,objective_step_count,objective_constraint_count,status,attempt_count,fix_count,total_injected_blackboard_vars,first_success_attempt_id,success_after_fix,max_bt_node_count,max_bt_tree_depth,total_generation_time_ms,total_fix_time_ms,total_execution_time_ms,total_wall_time_ms,last_failure_code,last_failure_reason\n";
    summary_csv
      << csv_escape(task_name_) << ","
      << csv_escape(objective_name_) << ","
      << csv_escape(objective_path_) << ","
      << run_metrics_.objective_chars << ","
      << run_metrics_.objective_lines << ","
      << run_metrics_.objective_step_count << ","
      << run_metrics_.objective_constraint_count << ","
      << csv_escape(run_metrics_.succeeded ? "SUCCESS" : "FAILED") << ","
      << run_metrics_.attempts.size() << ","
      << run_metrics_.fix_count << ","
      << run_metrics_.total_injected_blackboard_vars << ","
      << run_metrics_.first_success_attempt_id << ","
      << csv_escape(run_metrics_.success_after_fix ? "true" : "false") << ","
      << run_metrics_.max_bt_node_count << ","
      << run_metrics_.max_bt_tree_depth << ","
      << run_metrics_.total_generation_time_ms << ","
      << run_metrics_.total_fix_time_ms << ","
      << run_metrics_.total_execution_time_ms << ","
      << run_metrics_.total_wall_time_ms << ","
      << csv_escape(last_failure_code_) << ","
      << csv_escape(last_failure_reason_) << "\n";
  }

  const auto aggregate_path = std::filesystem::path(exec_base_dir_) / "bt_generation_summary.csv";
  const bool write_header = !std::filesystem::exists(aggregate_path);
  std::ofstream aggregate_csv(aggregate_path, std::ios::app);
  if (aggregate_csv.is_open()) {
    if (write_header) {
      aggregate_csv << "task_name,objective_name,objective_path,objective_chars,objective_lines,objective_step_count,objective_constraint_count,status,attempt_count,fix_count,total_injected_blackboard_vars,first_success_attempt_id,success_after_fix,max_bt_node_count,max_bt_tree_depth,total_generation_time_ms,total_fix_time_ms,total_execution_time_ms,total_wall_time_ms,last_failure_code,last_failure_reason,run_dir\n";
    }
    aggregate_csv
      << csv_escape(task_name_) << ","
      << csv_escape(objective_name_) << ","
      << csv_escape(objective_path_) << ","
      << run_metrics_.objective_chars << ","
      << run_metrics_.objective_lines << ","
      << run_metrics_.objective_step_count << ","
      << run_metrics_.objective_constraint_count << ","
      << csv_escape(run_metrics_.succeeded ? "SUCCESS" : "FAILED") << ","
      << run_metrics_.attempts.size() << ","
      << run_metrics_.fix_count << ","
      << run_metrics_.total_injected_blackboard_vars << ","
      << run_metrics_.first_success_attempt_id << ","
      << csv_escape(run_metrics_.success_after_fix ? "true" : "false") << ","
      << run_metrics_.max_bt_node_count << ","
      << run_metrics_.max_bt_tree_depth << ","
      << run_metrics_.total_generation_time_ms << ","
      << run_metrics_.total_fix_time_ms << ","
      << run_metrics_.total_execution_time_ms << ","
      << run_metrics_.total_wall_time_ms << ","
      << csv_escape(last_failure_code_) << ","
      << csv_escape(last_failure_reason_) << ","
      << csv_escape(exec_run_dir_.string()) << "\n";
  }
}

std::string LLMBTOrchestrator::csv_escape(const std::string & value)
{
  std::string escaped = value;
  std::size_t pos = 0;
  while ((pos = escaped.find('"', pos)) != std::string::npos) {
    escaped.insert(pos, 1, '"');
    pos += 2;
  }
  return "\"" + escaped + "\"";
}

std::string LLMBTOrchestrator::now_iso_utc() const
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#ifdef _WIN32
  gmtime_s(&utc_tm, &tt);
#else
  gmtime_r(&tt, &utc_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string LLMBTOrchestrator::json_escape(const std::string & input) const
{
  std::ostringstream ss;
  for (const char c : input) {
    switch (c) {
      case '"': ss << "\\\""; break;
      case '\\': ss << "\\\\"; break;
      case '\b': ss << "\\b"; break;
      case '\f': ss << "\\f"; break;
      case '\n': ss << "\\n"; break;
      case '\r': ss << "\\r"; break;
      case '\t': ss << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          ss << "\\u"
             << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<int>(static_cast<unsigned char>(c));
        } else {
          ss << c;
        }
    }
  }
  return ss.str();
}

void LLMBTOrchestrator::load_episodic_memory()
{
  bt_success_cases_.clear();
  bt_failure_cases_.clear();

  // Load success cases
  if (std::filesystem::exists(bt_success_memory_path_)) {
    std::ifstream success_file(bt_success_memory_path_);
    if (success_file.is_open()) {
      std::string line;
      std::getline(success_file, line); // Skip '['
      while (std::getline(success_file, line)) {
        if (line.find('{') != std::string::npos) {
          // Simple JSON parsing - just store the cases
          // In production, use a proper JSON library
          RCLCPP_DEBUG(get_logger(), "Loaded success case from episodic memory");
        }
      }
      RCLCPP_INFO(get_logger(), "Loaded %zu success cases from episodic memory",
        bt_success_cases_.size());
    }
  }

  // Load failure cases
  if (std::filesystem::exists(bt_failure_memory_path_)) {
    std::ifstream failure_file(bt_failure_memory_path_);
    if (failure_file.is_open()) {
      std::string line;
      std::getline(failure_file, line); // Skip '['
      while (std::getline(failure_file, line)) {
        if (line.find('{') != std::string::npos) {
          // Simple JSON parsing
          RCLCPP_DEBUG(get_logger(), "Loaded failure case from episodic memory");
        }
      }
      RCLCPP_INFO(get_logger(), "Loaded %zu failure cases from episodic memory",
        bt_failure_cases_.size());
    }
  }
}

void LLMBTOrchestrator::save_episodic_memory()
{
  // Save success cases
  std::ofstream success_file(bt_success_memory_path_);
  if (success_file.is_open()) {
    success_file << "[\n";
    for (std::size_t i = 0; i < bt_success_cases_.size(); ++i) {
      const auto & case_entry = bt_success_cases_[i];
      success_file << "  {\n";
      success_file << "    \"timestamp\": \"" << json_escape(case_entry.timestamp) << "\",\n";
      success_file << "    \"task_goal\": \"" << json_escape(case_entry.task_goal) << "\",\n";
      success_file << "    \"bt_node_count\": " << case_entry.bt_node_count << ",\n";
      success_file << "    \"bt_tree_depth\": " << case_entry.bt_tree_depth << ",\n";
      success_file << "    \"generation_attempts\": " << case_entry.generation_attempts << ",\n";
      success_file << "    \"required_fix\": " << (case_entry.required_fix ? "true" : "false") << ",\n";
      success_file << "    \"bt_xml_length\": " << case_entry.bt_xml.length() << "\n";
      success_file << "  }";
      if (i + 1 < bt_success_cases_.size()) {
        success_file << ",";
      }
      success_file << "\n";
    }
    success_file << "]\n";
    RCLCPP_INFO(get_logger(), "Saved %zu success cases to episodic memory",
      bt_success_cases_.size());
  }

  // Save failure cases
  std::ofstream failure_file(bt_failure_memory_path_);
  if (failure_file.is_open()) {
    failure_file << "[\n";
    for (std::size_t i = 0; i < bt_failure_cases_.size(); ++i) {
      const auto & case_entry = bt_failure_cases_[i];
      failure_file << "  {\n";
      failure_file << "    \"timestamp\": \"" << json_escape(case_entry.timestamp) << "\",\n";
      failure_file << "    \"task_goal\": \"" << json_escape(case_entry.task_goal) << "\",\n";
      failure_file << "    \"failure_cause\": \"" << json_escape(case_entry.failure_cause) << "\",\n";
      failure_file << "    \"generation_attempts\": " << case_entry.generation_attempts << ",\n";
      failure_file << "    \"was_fixable\": " << (case_entry.was_fixable ? "true" : "false") << ",\n";
      failure_file << "    \"last_bt_xml_length\": " << case_entry.last_bt_xml.length() << "\n";
      failure_file << "  }";
      if (i + 1 < bt_failure_cases_.size()) {
        failure_file << ",";
      }
      failure_file << "\n";
    }
    failure_file << "]\n";
    RCLCPP_INFO(get_logger(), "Saved %zu failure cases to episodic memory",
      bt_failure_cases_.size());
  }
}

void LLMBTOrchestrator::record_success_case(
  const std::string & task_goal,
  const std::string & bt_xml)
{
  BTSuccessCase case_entry;
  case_entry.timestamp = now_iso_utc();
  case_entry.task_goal = task_goal;
  case_entry.bt_xml = bt_xml;
  case_entry.required_fix = (bt_regeneration_count_ > 0);

  if (current_attempt_index_.has_value()) {
    const auto & attempt = run_metrics_.attempts[*current_attempt_index_];
    case_entry.bt_node_count = attempt.bt_node_count;
    case_entry.bt_tree_depth = attempt.bt_tree_depth;
    case_entry.generation_attempts = run_metrics_.attempts.size();
  }

  bt_success_cases_.push_back(case_entry);
  // Limit episodic memory to prevent unbounded growth
  if (bt_success_cases_.size() > 200) {
    bt_success_cases_.erase(bt_success_cases_.begin());
  }

  RCLCPP_INFO(get_logger(), "[episodic_mem] Recorded success: %s (attempts=%d, nodes=%d)",
    task_goal.c_str(), case_entry.generation_attempts, case_entry.bt_node_count);
}

void LLMBTOrchestrator::record_failure_case(
  const std::string & task_goal,
  const std::string & failure_cause,
  const std::string & last_bt_xml)
{
  BTFailureCase case_entry;
  case_entry.timestamp = now_iso_utc();
  case_entry.task_goal = task_goal;
  case_entry.failure_cause = failure_cause;
  case_entry.last_bt_xml = last_bt_xml;
  case_entry.was_fixable = (bt_regeneration_count_ > 0 && run_metrics_.succeeded);

  if (!run_metrics_.attempts.empty()) {
    case_entry.generation_attempts = run_metrics_.attempts.size();
  }

  bt_failure_cases_.push_back(case_entry);
  // Limit episodic memory to prevent unbounded growth
  if (bt_failure_cases_.size() > 200) {
    bt_failure_cases_.erase(bt_failure_cases_.begin());
  }

  RCLCPP_INFO(get_logger(), "[episodic_mem] Recorded failure: %s (cause=%s, attempts=%d)",
    task_goal.c_str(), failure_cause.c_str(), case_entry.generation_attempts);
}

}  // namespace behavior_architecture