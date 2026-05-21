# LLM BT Generation Evaluation

This workflow isolates Behavior Tree generation and repair from task planning.
It is intended for controlled experiments where the input is a single objective YAML and the outputs are:

- Generated BT XML files for each attempt.
- Raw per-attempt metrics.
- A per-run summary.
- An aggregate CSV across runs.
- Optional episodic memory of success and failure cases for learning across runs.

The launch file intentionally does not start `llm_bt_builder`, so you can run that node independently and inspect its logs without multiplexing them with the evaluator.

## Launch

```bash
source install/setup.bash

ros2 launch behavior_architecture llm_bt_eval.launch.py \
  objective_file:=/absolute/path/to/objective.yaml \
  bt_nodes_package:=social_bt_nodes \
  plugin_library:=libsocial_bt_nodes_plugin.so \
  task_name:=take_order_eval
```

To enable episodic memory (stores BT success/failure cases across runs):

```bash
ros2 launch behavior_architecture llm_bt_eval.launch.py \
  objective_file:=/absolute/path/to/objective.yaml \
  task_name:=take_order_eval \
  use_episodic_memory:=true
```

If you prefer explicit capabilities:

```bash
ros2 launch behavior_architecture llm_bt_eval.launch.py \
  objective_file:=/absolute/path/to/objective.yaml \
  capabilities_yaml:=/absolute/path/to/capabilities.yaml
```

For isolated validation of posterior steps that depend on previous blackboard outputs:

```bash
ros2 launch behavior_architecture llm_bt_eval.launch.py \
  objective_file:=/absolute/path/to/approach_customer.yaml \
  blackboard_seed:=/absolute/path/to/blackboard_seed.yaml \
  strict_objective_inputs:=true
```

## Run

```bash
source install/setup.bash

ros2 run behavior_architecture llm_bt_executor -- \
  --objective /absolute/path/to/objective.yaml \
  --bt-nodes-package social_bt_nodes \
  --plugin-library libsocial_bt_nodes_plugin.so \
  --task-name take_order_eval \
  --exec-dir exec/bt_generation_eval \
  --timeout-sec 60 \
  --max-fixes 3
```

With episodic memory enabled:

```bash
ros2 run behavior_architecture llm_bt_executor -- \
  --objective /absolute/path/to/objective.yaml \
  --task-name take_order_eval \
  --use-episodic-memory \
  --max-fixes 3
```

If you already have a capabilities file, replace `--bt-nodes-package` with:

```bash
--capabilities /absolute/path/to/capabilities.yaml
```

For posterior steps requiring previous context keys, preload the blackboard and validate required inputs:

```bash
ros2 run behavior_architecture llm_bt_executor -- \
  --objective /absolute/path/to/approach_customer.yaml \
  --blackboard-seed /absolute/path/to/blackboard_seed.yaml \
  --strict-objective-inputs
```

Example seed file:

```yaml
blackboard:
  detected_customer: "table_3_customer"
  first_dish: "pasta"
  second_dish: "salad"
  drink: "water"
```

## Execution Model

1. Load one objective YAML from disk.
2. Call `generate_bt`.
3. Execute the BT on the robot through `BehaviorRunner`.
4. If execution fails with a local BT/configuration error, call `fix_bt` and retry.
5. Stop when the BT succeeds, the failure is non-local, or the fix budget is exhausted.

## Saved Artifacts

Each run creates a directory:

```text
<exec-dir>/<task-name>/<timestamp>/
```

That directory contains:

- `objective.yaml`: frozen experiment input.
- `attempt_XX_generate.xml` or `attempt_XX_fix.xml`: BT proposed in each attempt.
- `attempt_metrics.csv`: raw data per generation/fix attempt.
- `run_summary.csv`: one-row run summary.

The base directory also accumulates:

- `bt_generation_summary.csv`: one row per run for batch analysis.

When episodic memory is enabled, an additional directory is created:

```text
<exec-dir>/episodic_memory/
```

That directory contains:

- `bt_success_cases.json`: JSON array of successful BT generations with their structural metrics and goals.
- `bt_failure_cases.json`: JSON array of failed BT attempts with failure causes and context.

## Metrics

`attempt_metrics.csv` stores these columns:

- `attempt_id`: chronological BT proposal index.
- `attempt_type`: `generate` or `fix`.
- `injected_blackboard_vars`: number of dynamic blackboard variables exposed to the LLM in that attempt.
- `generation_time_ms`: latency of `generate_bt` or `fix_bt`.
- `execution_time_ms`: time spent executing that BT on the robot.
- `total_attempt_time_ms`: generation plus execution time for that attempt.
- `bt_xml_chars` and `bt_xml_lines`: size of the generated tree artifact.
- `bt_node_count`: approximate number of BT nodes in the generated XML.
- `bt_tree_depth`: approximate maximum depth of the generated tree.
- `status`: `SUCCESS`, `FAILURE`, `TIMEOUT`, `LOAD_FAILED`, `GENERATION_FAILED`, or `FIX_FAILED`.
- `failure_code`: structured BT failure code when available.
- `failure_reason`: human-readable failure trace.
- `bt_xml_file`: XML artifact associated with the attempt.

`run_summary.csv` and `bt_generation_summary.csv` store these aggregate metrics:

- `attempt_count`: total BT proposals needed for the run.
- `fix_count`: number of `FixBT` recoveries attempted.
- `objective_chars`, `objective_lines`, `objective_step_count`, `objective_constraint_count`: complexity descriptors of the input objective.
- `total_injected_blackboard_vars`: total dynamic context surfaced to the LLM across attempts.
- `first_success_attempt_id`: first attempt that achieved success, `-1` if the run failed.
- `success_after_fix`: whether the run only succeeded after at least one `FixBT`.
- `max_bt_node_count` and `max_bt_tree_depth`: maximum structural complexity observed among generated trees.
- `total_generation_time_ms`: sum of all `generate_bt` and `fix_bt` latencies.
- `total_fix_time_ms`: sum of only `FixBT` latencies.
- `total_execution_time_ms`: total robot execution time across attempts.
- `total_wall_time_ms`: end-to-end duration for the run.
- `last_failure_code` and `last_failure_reason`: terminal failure descriptor for error analysis.

## Recommended Analysis For The Paper

- Success rate without fixes versus success rate with fixes.
- Distribution of `generation_time_ms` and `execution_time_ms`.
- Mean and median `attempt_count` until success.
- Fraction of failures attributable to `bt_config_error` versus execution/perception failures.
- Marginal gain of `FixBT`: runs solved after at least one failed BT.
- Timeout rate and its relation to objective type.

## Episodic Memory (Optional)

When `--use-episodic-memory` is enabled, the evaluator stores success and failure cases in JSON format for learning across runs. This is useful for:

- Identifying recurring failure patterns.
- Analyzing the distribution of successful tree structures.
- Tracking complexity metrics of successful vs. failed generations.

### Episodic Memory Structure

**Success Cases** (`bt_success_cases.json`):

```json
[
  {
    "timestamp": "2026-05-04T15:30:45Z",
    "task_goal": "Take an order from the customer",
    "bt_node_count": 12,
    "bt_tree_depth": 5,
    "generation_attempts": 1,
    "required_fix": false,
    "bt_xml_length": 2847
  },
  ...
]
```

**Failure Cases** (`bt_failure_cases.json`):

```json
[
  {
    "timestamp": "2026-05-04T15:31:10Z",
    "task_goal": "Navigate to the kitchen and pick up an order",
    "failure_cause": "Subtree reference not found: [MoveToKitchen]",
    "generation_attempts": 2,
    "was_fixable": true,
    "last_bt_xml_length": 3200
  },
  ...
]
```

The episodic memory is capped at 200 cases per category to prevent unbounded growth. Cases are timestamped for temporal analysis and include enough information to categorize failures and successes.

### Using Episodic Memory for Analysis

- Analyze task complexity by correlating `task_goal` with `bt_node_count` and `generation_attempts`.
- Classify failures by `failure_cause` to identify systematic issues (missing node types, incorrect ports, etc.).
- Compare the success rate of tasks that required fixes versus those that succeeded on the first attempt.
- Use timestamps to track learning over time if running sequential evaluations.