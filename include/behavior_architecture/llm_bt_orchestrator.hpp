#ifndef BEHAVIOR_ARCHITECTURE__LLM_BT_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__LLM_BT_ORCHESTRATOR_HPP_

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "std_msgs/msg/string.hpp"
#include "yaml-cpp/yaml.h"

#include "llm_bt_builder/srv/fix_bt.hpp"
#include "llm_bt_builder/srv/generate_bt.hpp"

#include "behavior_architecture/base_orchestrator.hpp"

namespace behavior_architecture
{

/**
 * @class LLMBTOrchestrator
 * @brief FSM-based orchestrator for isolated BT generation and repair evaluation.
 *
 * This orchestrator implements a complete workflow for evaluating LLM-based Behavior Tree
 * generation without task planning. It:
 *
 * 1. Loads a single objective YAML from disk
 * 2. Calls the `generate_bt` service to produce an initial BT XML
 * 3. Executes the BT on the robot via BehaviorRunner
 * 4. If execution fails with a recoverable error, calls `fix_bt` for repair
 * 5. Records metrics and artifacts (XML files, CSVs, optional episodic memory)
 *
 * ## State Machine
 * - IDLE → GENERATING_BT → EXECUTING_BT → SUCCESS
 *           ↓
 *        (local error) → WAITING_FIX_BT → (repeat)
 *
 * ## Episodic Memory (Optional)
 * When enabled via `llm_use_episodic_memory` blackboard key:
 * - Stores successful BT cases with structural metrics
 * - Stores failure cases with causes and attempt info
 * - Persists in JSON format for cross-run learning
 * - Capped at 200 cases per category (FIFO)
 *
 * ## Output Artifacts
 * - CSV metrics (per-attempt and aggregate)
 * - Generated BT XML files
 * - Optional JSON episodic memory
 *
 * ## Usage
 * Configure via blackboard keys set by action_executor or CLI:
 * - llm_objective_path: path to objective YAML
 * - llm_task_name: name for output directories
 * - llm_use_episodic_memory: enable memory persistence (default: false)
 * - llm_timeout_sec: BT execution timeout
 * - llm_max_bt_regenerations: max fix attempts
 */
class LLMBTOrchestrator : public BaseOrchestrator
{
public:
  explicit LLMBTOrchestrator(BT::Blackboard::Ptr blackboard);
  ~LLMBTOrchestrator() = default;

  void go_to_state(int /*state*/) override {}

protected:
  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_configure(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_activate(const rclcpp_lifecycle::State & previous_state) override;

  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
  on_deactivate(const rclcpp_lifecycle::State & previous_state) override;

  void control_cycle() override;

private:
  enum class State
  {
    IDLE,
    GENERATING_BT,
    WAITING_FIX_BT,
    EXECUTING_BT,
    SUCCESS,
    FAILED
  };

  struct AttemptMetrics
  {
    int attempt_id = 0;
    std::string attempt_type;
    int injected_blackboard_vars = 0;
    long generation_time_ms = 0;
    long execution_time_ms = 0;
    long total_attempt_time_ms = 0;
    int bt_xml_chars = 0;
    int bt_xml_lines = 0;
    int bt_node_count = 0;
    int bt_tree_depth = 0;
    std::string status;
    std::string failure_code;
    std::string failure_reason;
    std::string bt_xml_file;
  };

  struct RunMetrics
  {
    std::vector<AttemptMetrics> attempts;
    int objective_chars = 0;
    int objective_lines = 0;
    int objective_step_count = 0;
    int objective_constraint_count = 0;
    long total_generation_time_ms = 0;
    long total_fix_time_ms = 0;
    long total_execution_time_ms = 0;
    long total_wall_time_ms = 0;
    int total_injected_blackboard_vars = 0;
    int fix_count = 0;
    int first_success_attempt_id = -1;
    int max_bt_node_count = 0;
    int max_bt_tree_depth = 0;
    bool succeeded = false;
    bool success_after_fix = false;
  };

  struct BTSuccessCase
  {
    /// @brief Timestamp when BT succeeded (ISO 8601 UTC format)
    std::string timestamp;
    /// @brief Goal/objective description for the BT
    std::string task_goal;
    /// @brief Number of nodes in the generated tree
    int bt_node_count = 0;
    /// @brief Maximum depth of the generated tree
    int bt_tree_depth = 0;
    /// @brief Number of generation/fix attempts needed for success
    int generation_attempts = 0;
    /// @brief Whether this BT required fix_bt to succeed
    bool required_fix = false;
    /// @brief Full BT XML (for analysis and reuse)
    std::string bt_xml;
  };

  struct BTFailureCase
  {
    /// @brief Timestamp when BT failed (ISO 8601 UTC format)
    std::string timestamp;
    /// @brief Goal/objective description for the BT
    std::string task_goal;
    /// @brief Root cause of failure
    std::string failure_cause;
    /// @brief Number of generation/fix attempts before giving up
    int generation_attempts = 0;
    /// @brief Whether the failure was eventually fixable (if continued)
    bool was_fixable = false;
    /// @brief Last attempted BT XML (for failure analysis)
    std::string last_bt_xml;
  };

  void start_run();
  bool load_objective_file();

  /// Load key-value pairs from the blackboard seed YAML into the shared blackboard.
  /// Both the bare key and its `{key}` alias are set so BT.CPP port remapping resolves them.
  /// @return true on success or when no seed path is configured
  bool preload_blackboard_seed();

  /// Check that every key listed in objective.inputs is present in the blackboard.
  /// In non-strict mode emits a WARN with a copy-paste seed YAML snippet and returns true.
  /// In strict mode (llm_fail_on_missing_inputs=true) sets last_failure_reason_ and returns false.
  /// @return true if all inputs are satisfied or strict mode is off
  bool validate_required_inputs();

  /// Check whether the given key (with or without `{}`) exists in the blackboard.
  bool has_blackboard_key(const std::string & key) const;

  /// Set a single seed value on the blackboard, inferring or applying an explicit type.
  /// Sets both `key` and `{key}` so BT.CPP input-port remapping (`target="{key}"`) works.
  /// @param key  Raw key from the seed YAML (may include `{}`)
  /// @param value YAML node for the value (scalar, sequence, or typed map with type/value)
  void set_blackboard_seed_value(const std::string & key, const YAML::Node & value);

  void initialize_execution_dir();
  void initialize_attempt_tracking(const std::string & attempt_type);
  int append_dynamic_blackboard_vars(std::string & yaml_text) const;
  void request_generate_bt();
  void request_fix_bt(const std::string & broken_xml, const std::string & error_msg);
  std::string collect_failure_reason();

  /// Return true if the failure reason describes a BT structural/config error that the LLM can fix.
  bool is_local_error(const std::string & reason) const;

  /// Return true when the failure is "missing required input … received: ''" — meaning a
  /// `{key}` port remapping found no value in the blackboard.  The BT XML is correct; asking
  /// the LLM to fix it would produce the same tree.  A blackboard seed is needed instead.
  bool is_missing_seed_error(const std::string & reason) const;
  std::string load_file(const std::string & path) const;
  std::string state_name(State state) const;
  void analyze_bt_xml(const std::string & bt_xml, AttemptMetrics & attempt);
  std::string save_bt_xml(const std::string & bt_xml, int attempt_id, const std::string & attempt_type);
  void publish_status(const std::string & msg);
  void transition_to(State new_state);
  void write_metrics_files();
  static std::string csv_escape(const std::string & value);

  /// @name Episodic Memory Methods
  /// @brief Persist and retrieve BT success/failure cases for cross-run learning
  /// @{

  /// Load previously saved episodic memory cases from JSON files
  void load_episodic_memory();

  /// Persist episodic memory cases to JSON (called on run termination if enabled)
  void save_episodic_memory();

  /// Record a successful BT generation with structural metrics
  /// @param task_goal The objective description
  /// @param bt_xml The generated BT XML
  void record_success_case(const std::string & task_goal, const std::string & bt_xml);

  /// Record a failed BT attempt with cause and context
  /// @param task_goal The objective description
  /// @param failure_cause Root cause of the failure
  /// @param last_bt_xml The last attempted BT XML
  void record_failure_case(
    const std::string & task_goal,
    const std::string & failure_cause,
    const std::string & last_bt_xml);

  /// Get current ISO 8601 UTC timestamp
  std::string now_iso_utc() const;

  /// Escape special JSON characters in a string
  std::string json_escape(const std::string & input) const;

  /// @}

  State state_{State::IDLE};

  rclcpp::Client<llm_bt_builder::srv::GenerateBT>::SharedPtr generate_bt_client_;
  rclcpp::Client<llm_bt_builder::srv::FixBT>::SharedPtr fix_bt_client_;
  rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::String>::SharedPtr status_pub_;

  using GenBTFuture = rclcpp::Client<llm_bt_builder::srv::GenerateBT>::SharedFuture;
  using FixBTFuture = rclcpp::Client<llm_bt_builder::srv::FixBT>::SharedFuture;

  std::optional<GenBTFuture> gen_bt_future_;
  std::optional<FixBTFuture> fix_bt_future_;

  std::vector<std::string> plugin_libraries_;
  std::vector<std::string> bt_nodes_packages_;
  std::string capabilities_yaml_;
  std::string objective_path_;
  std::string objective_yaml_;
  std::string objective_name_;
  std::string objective_description_;
  std::vector<std::string> objective_inputs_;
  std::string task_name_;
  std::string useful_info_;
  std::string blackboard_seed_path_;
  double bt_timeout_sec_{30.0};
  bool tree_loaded_{false};
  bool save_exec_{true};
  bool shutdown_on_completion_{true};
  bool fail_on_missing_inputs_{false};
  int bt_regeneration_count_{0};
  int max_bt_regenerations_{3};
  std::string last_failure_reason_;
  std::string last_failure_code_;
  std::string last_bt_xml_;
  std::vector<std::string> initial_blackboard_keys_;

  RunMetrics run_metrics_;
  std::optional<std::size_t> current_attempt_index_;
  std::chrono::steady_clock::time_point run_start_time_;
  std::chrono::steady_clock::time_point bt_gen_start_time_;
  rclcpp::Time step_start_time_;

  std::string exec_base_dir_{"exec/bt_generation_eval"};
  std::filesystem::path exec_run_dir_;

  // Episodic memory
  bool use_episodic_memory_{false};
  std::filesystem::path episodic_memory_dir_;
  std::filesystem::path bt_success_memory_path_;
  std::filesystem::path bt_failure_memory_path_;
  std::vector<BTSuccessCase> bt_success_cases_;
  std::vector<BTFailureCase> bt_failure_cases_;
};

}  // namespace behavior_architecture

#endif  // BEHAVIOR_ARCHITECTURE__LLM_BT_ORCHESTRATOR_HPP_