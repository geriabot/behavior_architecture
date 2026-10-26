# LLM Orchestrators

behavior_architecture includes a family of orchestrators that plan a mission and generate, run and
repair its Behavior Trees with a language model. They are optional: they are built only when
`llm_planner_interfaces` and `llm_bt_builder` are in the workspace (import `llm.repos`, see the
[README](README.md#with-the-llm-orchestrators)). The base framework (BaseOrchestrator,
BehaviorRunner, `mission_executor`) is described in the [README](README.md#how-it-works).

## Overview

### Core Classes

#### 1. **BaseOrchestrator** (Base class)
- Provides lifecycle management (configure, activate, deactivate)
- Manages BehaviorRunner instances
- Defines the `control_cycle()` interface

#### 2. **LLMPlanOrchestrator** (Extends BaseOrchestrator)
- **Purpose**: Full task planning + BT generation workflow
- **Workflow**: Goal → PlanTask → Generate BT per step → Execute each BT
- **Planning Contract (important)**:
  - Planner output should favor finer-grained sequential steps (typically 5-10 for non-trivial missions).
  - Each step should carry one primary intention (e.g., detect, navigate, capture, validate, deliver).
  - Data acquisition and later validation/decision should be split across steps using explicit
    `objective.outputs` → `objective.inputs` wiring.
  - This keeps each generated BT smaller, easier to validate, and easier to repair.
- **State Machine**: 
  ```
  IDLE → WAITING_PLAN → [GENERATING_BT → WAITING_FIX_BT → EXECUTING_BT]* → SUCCESS/FAILED
  ```
- **Services Used**:
  - `plan_task`, `replan_task` (llm_planner_interfaces)
  - `generate_bt`, `fix_bt` (llm_bt_builder)
- **Output**: Per-step metrics, BT XMLs for each step
- **Use Case**: Real robot execution with full planning

#### 3. **MCPLLMPlanOrchestrator** (Extends LLMPlanOrchestrator)
- **Purpose**: Planning + BT generation WITH episodic memory & mission context
- **Extension Pattern**: Overrides key methods to add MCP-specific logging
- **Added Features**:
  - Mission snapshots (JSON)
  - Failure history tracking
  - Episodic memory for plan/BT cases (success & failure)
  - State timeline persistence
- **Episodic Memory Files**:
  ```
  exec/mcp_context/
  ├── mission_snapshot.json
  ├── failure_history.json
  ├── plan_success_episodic_memory.json
  ├── plan_failure_episodic_memory.json
  ├── bt_success_episodic_memory.json
  └── bt_failure_episodic_memory.json
  ```
- **Use Case**: A/B testing, research, episodic learning

#### 4. **LLMBTOrchestrator** (Extends BaseOrchestrator)
- **Purpose**: Isolated BT generation/repair evaluation (NO planning layer)
- **Workflow**: Objective YAML → Generate BT → Execute → Fix on error
- **State Machine**:
  ```
  IDLE → GENERATING_BT → EXECUTING_BT → SUCCESS/FAILED
                    ↑                      ↓
                    └── WAITING_FIX_BT ←──┘
  ```
- **Services Used**:
  - `generate_bt`, `fix_bt` (llm_bt_builder)
- **Output**: Per-attempt metrics, BT XMLs, optional episodic memory
- **Optional Episodic Memory**:
  ```
  exec/episodic_memory/
  ├── bt_success_cases.json      (200 max, FIFO)
  └── bt_failure_cases.json      (200 max, FIFO)
  ```
- **Use Case**: Isolated BT generation experiments, academic research

---

## Design Pattern: Virtual Method Extension

Both `MCPLLMPlanOrchestrator` and `LLMBTOrchestrator` use a **Template Method + Extension Hooks** pattern:

```cpp
// Base class (LLMPlanOrchestrator)
virtual void request_plan() { /* core logic */ }

// Derived class (MCPLLMPlanOrchestrator)
void request_plan() override {
    last_event_ = "request_plan";
    LLMPlanOrchestrator::request_plan();  // Call parent
    write_mission_snapshot(...);           // Add MCP-specific logic
}
```

This allows:
- Minimal code duplication
- Clear separation of concerns
- Easy to add new orchestrators (e.g., security tracking, resource profiling)

---

## Episodic Memory Pattern

### For MCPLLMPlanOrchestrator

Tracks **plan-level** and **step-level** successes/failures:

```json
{
  "timestamp": "2026-05-04T15:30:45Z",
  "mission_goal": "Take customer order",
  "step_id": 2,
  "step_goal": "Navigate to kitchen",
  "bt_xml": "<BehaviorTree>...</BehaviorTree>",
  "duration_sec": 42,
  "status": "success"
}
```

**Use Cases**:
- Learn which BT structures work best for specific goals
- Identify systematic failure patterns
- Guide LLM-based BT generation with historical precedents

### For LLMBTOrchestrator

Tracks **BT generation-level** successes/failures:

```json
{
  "timestamp": "2026-05-04T15:30:45Z",
  "task_goal": "Pick up package from shelf",
  "bt_node_count": 12,
  "bt_tree_depth": 5,
  "generation_attempts": 2,
  "required_fix": true,
  "status": "success"
}
```

**Use Cases**:
- Analyze complexity of successful BTs
- Identify objective types that require more fix attempts
- Build a corpus of reference BTs for analysis

---

## Configuration

### Blackboard Keys

All orchestrators read configuration from the BT::Blackboard:

| Key | Type | Default | Used By |
|-----|------|---------|---------|
| `llm_plugin_libraries` | vector<string> | `{"libsocial_bt_nodes_plugin.so"}` | All |
| `llm_bt_nodes_packages` | vector<string> | none | All (capabilities from `<pkg>/node_descriptions/<pkg>.yaml`) |
| `llm_capabilities_yaml` | string | - | All (overrides `llm_bt_nodes_packages`) |
| `llm_control_period_ms` | int | 50 | All |
| `llm_timeout_sec` | double | 30.0 | All |
| `llm_save_exec` | bool | false (plan), true (BT) | All |
| `llm_exec_dir` | string | `exec` (plan), `exec/bt_generation_eval` (BT) | All |
| `llm_mission_name` | string | - | LLMPlanOrchestrator; alias of `llm_task_name` in LLMBTOrchestrator |
| `llm_skills` | vector<string> | - | LLMPlanOrchestrator |
| `llm_replan_active` | bool | true | LLMPlanOrchestrator |
| `llm_restart_after_forced` | bool | true | LLMPlanOrchestrator |
| `llm_mcp_context_dir` | string | `<llm_exec_dir>/mcp_context` | MCPLLMPlanOrchestrator |
| `llm_task_name` | string | - | LLMBTOrchestrator |
| `llm_objective_path` | string | - | LLMBTOrchestrator |
| `llm_useful_info` | string | - | LLMBTOrchestrator |
| `llm_blackboard_seed_path` | string | - | LLMBTOrchestrator |
| `llm_fail_on_missing_inputs` | bool | false | LLMBTOrchestrator |
| `llm_shutdown_on_completion` | bool | true | LLMBTOrchestrator |
| `llm_use_episodic_memory` | bool | false | LLMBTOrchestrator |
| `llm_max_bt_regenerations` | int | 3 | LLMBTOrchestrator |

`mission_executor` fills the keys of the plan orchestrators from the YAML (see
`config/llm_config.yaml`); `llm_bt_executor` fills those of LLMBTOrchestrator from its command line.

### CLI Usage (LLMBTOrchestrator)

```bash
ros2 run behavior_architecture llm_bt_executor -- \
  --objective /path/to/objective.yaml \
  --task-name my_eval \
  --use-episodic-memory \
  --timeout-sec 60 \
  --max-fixes 3
```

### Launch Usage (LLMBTOrchestrator)

```bash
ros2 launch behavior_architecture llm_bt_eval.launch.py \
  objective_file:=/path/to/objective.yaml \
  task_name:=my_eval \
  use_episodic_memory:=true
```

---

## Metrics Output

### For LLMBTOrchestrator

**Per-Attempt CSV** (`attempt_metrics.csv`):
- Timing: `generation_time_ms`, `execution_time_ms`
- Structure: `bt_node_count`, `bt_tree_depth`
- Context: `injected_blackboard_vars`
- Result: `status`, `failure_reason`

**Aggregate CSV** (`bt_generation_summary.csv`):
- Attempt statistics: `attempt_count`, `fix_count`
- Complexity: `max_bt_node_count`, `max_bt_tree_depth`
- Success metrics: `first_success_attempt_id`, `success_after_fix`

### For MCPLLMPlanOrchestrator

Same as above PLUS episodic memory JSON files tracking historical cases.

---

## File Organization

```
behavior_architecture/
├── include/behavior_architecture/
│   ├── base_orchestrator.hpp           (Lifecycle base)
│   ├── llm_plan_orchestrator.hpp       (Full workflow)
│   ├── mcp_llm_plan_orchestrator.hpp   (MCP specialization)
│   ├── llm_bt_orchestrator.hpp         (Isolated BT eval)
│   ├── orchestrator_factory.hpp        (Registration & creation)
│   └── ...
├── src/
│   ├── behavior_architecture/          (base: BaseOrchestrator, BehaviorRunner, factory)
│   ├── llm_plan_orchestrator.cpp       (~1032 lines, core logic)
│   ├── mcp_llm_plan_orchestrator.cpp   (~995 lines, MCP extensions)
│   ├── llm_bt_orchestrator.cpp         (~900 lines, isolated BT)
│   ├── llm_bt_executor_main.cpp        (CLI entry point)
│   └── ...
├── launch/
│   ├── llm_bt_eval.launch.py           (Standalone BT eval launcher)
│   └── ...
├── BT_GENERATION_EVAL.md               (BT evaluation workflow)
├── ARCHITECTURE.md                     (This file)
└── CMakeLists.txt
```

---

## Extension Guide

To create a new orchestrator (e.g., `SecurityAwareLLMPlanOrchestrator`):

1. **Inherit from existing class**:
   ```cpp
   class SecurityAwareLLMPlanOrchestrator : public LLMPlanOrchestrator { ... }
   ```

2. **Override key methods**:
   ```cpp
   void transition_to(State new_state) override;
   std::string collect_failure_reason() override;
   ```

3. **Add new data members for your domain** (e.g., security logs, audit trail)

4. **Register in factory**:
   ```cpp
   static OrchestratorRegistrar<SecurityAwareLLMPlanOrchestrator> registrar("security_aware");
   ```

5. **Build it** as a shared library of your package, linked to `llm_bt_builder` and
   `llm_planner_interfaces`, and name it in `orchestrator_libraries` of the YAML (see
   [docs/CUSTOM_ORCHESTRATORS.md](docs/CUSTOM_ORCHESTRATORS.md)). The LLM orchestrators of this
   package are compiled into `mission_executor` only, so the derived class needs their sources or
   its own copy; inside this package, add the file to the `if(WITH_LLM_ORCHESTRATORS)` block of
   `CMakeLists.txt`.

---

## References

- [BT_GENERATION_EVAL.md](BT_GENERATION_EVAL.md) — Isolated BT evaluation workflow
- ROS 2 Lifecycle documentation
- BehaviorTree.CPP documentation
