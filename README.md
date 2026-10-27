# behavior_architecture

A ROS 2 framework that runs a robot's behaviour as a **state machine over Behavior Trees**: an
*orchestrator* (a lifecycle node with the state machine) decides which behaviour runs, and each
behaviour is a BehaviorTree.CPP tree run by its own *behavior runner* (a lifecycle node that ticks
the tree and reports how it ended). A generic executable, `mission_executor`, builds the whole
stack from a YAML file, so an application provides an orchestrator, its trees and a config, and no
`main`.

Besides the fixed orchestrators that an application writes, the package has **optional LLM
orchestrators** that plan a mission and generate its trees with a language model. They need two
extra source packages and are built only when those are in the workspace
([Installation](#installation)).

- ROS 2 **Jazzy**: branch `main`. ROS 2 **Lyrical**: branch `lyrical` ([note](#ros-2-lyrical)).
- License: Apache-2.0.

## Contents

- [How it works](#how-it-works)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Mission configuration](#mission-configuration)
- [Writing an orchestrator](#writing-an-orchestrator)
- [LLM orchestrators (optional)](#llm-orchestrators-optional)
- [Executables and launch files](#executables-and-launch-files)
- [ROS 2 Lyrical](#ros-2-lyrical)
- [Used by](#used-by)
- [Further documentation](#further-documentation)

## How it works

```
                      config.yaml
                          │
                  ┌───────▼────────┐  dlopen(orchestrator_libraries)
                  │mission_executor│  OrchestratorFactory::create_orchestrator(orchestrator_type)
                  └───────┬────────┘
                          │ configure + activate
            ┌─────────────▼──────────────┐
            │   Orchestrator (FSM)       │  control_cycle() every control_cycle_rate_ms
            │   : BaseOrchestrator       │  go_to_state(): activate_runner / deactivate_runner
            └──┬──────────┬───────────┬──┘
     lifecycle │          │           │       ▲ /behavior_status (std_msgs/String)
   transitions ▼          ▼           ▼       │ RUNNING · SUCCESS · FAILURE · DEACTIVATED
          ┌────────┐ ┌────────┐  ┌────────┐   │
          │runner A│ │runner B│  │runner C│───┘
          │ BT xml │ │ BT xml │  │ BT xml │   one BehaviorRunner per behaviour in the YAML
          └────────┘ └────────┘  └────────┘
                   shared BT::Blackboard ("node": an rclcpp::Node for the BT nodes)
```

**BehaviorRunner** (`behavior_runner.hpp`) is a lifecycle node that owns one tree:

- `on_activate` loads the BT plugin libraries, builds the tree from its XML file on the shared
  blackboard (a fresh tree on every activation), and starts a timer that ticks it every
  `control_period_ms`.
- Each tick publishes `RUNNING` on `/behavior_status`. When the tree returns `SUCCESS` or
  `FAILURE` the runner publishes it once and stops ticking; it stays active until it is deactivated.
- `on_deactivate` halts the tree and publishes `DEACTIVATED`.
- Groot2 monitoring: parameters `enable_groot_monitoring` (default `true`) and `groot_port`
  (default `5555`; Groot2 also uses the next port). The publisher opens on activation, so two
  runners active at the same time need different ports (set them in a parameters file keyed by the
  runner names) or monitoring disabled.

**BaseOrchestrator** (`base_orchestrator.hpp`) is the lifecycle node a state machine derives from:

- A derived class implements `control_cycle()`, the state machine, called every
  `control_cycle_rate_ms` (parameter, default 100 ms; the timer starts on activation), and
  `go_to_state(int)`, which activates and deactivates runners for the new state.
- `activate_runner(name)`, `deactivate_runner(name)` and `deactivate_all_runners()` drive the
  runners' lifecycle transitions.
- `check_behavior_finished()` returns `true` once when the status received on
  `/behavior_status` changes to `SUCCESS` or `FAILURE`; the status is then in `last_status_`.
  Every runner publishes on the same topic, so an orchestrator keeps one runner active at a time.
- `on_configure` creates one runner per entry of the YAML `behaviors` list (blackboard key
  `behaviors_config`) and configures them. An orchestrator can also create runners itself and
  add them with `register_runner()` before it is configured.
- `on_deactivate` deactivates every runner and stops the control cycle.

**OrchestratorFactory** (`orchestrator_factory.hpp`) maps a type name to a constructor. An
orchestrator registers itself with a static `OrchestratorRegistrar<T>` in its `.cpp`, so loading
the library that contains it is enough to make the type available.

**mission_executor** (`src/mission_executor.cpp`) reads the YAML, `dlopen`s the
`orchestrator_libraries`, creates an `rclcpp::Node` named `node_name` and stores it on a new
blackboard as `"node"`, writes the config to the blackboard, creates the orchestrator of
`orchestrator_type`, configures and activates it, and spins the node, the orchestrator and its
runners in a multi-threaded executor.

## Installation

The package has three layers; import only the ones you need. The three `.repos` files are at the
root of this repository.

| Layer | Brings | Source dependencies |
|---|---|---|
| **Base** | `libbehavior_architecture` (BaseOrchestrator, BehaviorRunner, OrchestratorFactory); `mission_executor` with the `simple` and `restaurant` examples and any plugin orchestrator; `test_plan_executor`, `static_tf_publisher` | none (`thirdparty.repos` is empty) |
| **LLM orchestrators** | types `llm` and `mcp` in `mission_executor`; executables `llm_plan_executor`, `llm_bt_executor`, `start_mission` | `llm.repos`: [llm_bt_builder](https://github.com/rodperex/llm_bt_builder), [llm_planner](https://github.com/rodperex/llm_planner) |
| **Examples** | the BT nodes used by `config/simple_config.yaml` and `config/restaurant_config.yaml` | `examples.repos`: [social_bt_nodes](https://github.com/rodperex/social_bt_nodes) |

Everything else (rclcpp, rclcpp_lifecycle, behaviortree_cpp, yaml-cpp, tf2_ros...) comes from ROS 2
binary packages.

### Base

```bash
mkdir -p ~/ws/src && cd ~/ws/src
git clone https://github.com/geriabot/behavior_architecture.git        # add -b lyrical on Lyrical
cd ~/ws
# llm_planner_interfaces and llm_bt_builder are optional dependencies (see below)
rosdep install --from-paths src --ignore-src -y \
  --skip-keys "llm_planner_interfaces llm_bt_builder"
colcon build --symlink-install
source install/setup.bash
```

`package.xml` declares `llm_planner_interfaces` and `llm_bt_builder` so that colcon builds them
first when they are in the workspace. colcon does not require them, but rosdep cannot resolve them,
hence `--skip-keys`.

### With the LLM orchestrators

```bash
cd ~/ws
vcs import src < src/behavior_architecture/llm.repos
rosdep install --from-paths src --ignore-src -y
colcon build --symlink-install
```

CMake looks for `llm_planner_interfaces` and `llm_bt_builder` and prints which variant it builds:

```
-- behavior_architecture: building the LLM orchestrators (llm, mcp, llm_bt)
-- behavior_architecture: llm_planner_interfaces or llm_bt_builder not found: the LLM orchestrators ...
```

If the packages are added to a workspace that already built the base, build again with
`--cmake-clean-cache` (or remove `build/behavior_architecture`) so that CMake looks for them again.

Run without arguments, `mission_executor` lists the orchestrator types it was built with:
`restaurant` and `simple` for the base, plus `llm`, `mcp`, `mcp_llm` and
`mcp_llm_plan_orchestrator` with the LLM orchestrators. Types that a configuration loads from
`orchestrator_libraries` are registered only when that configuration runs.

### Examples

```bash
vcs import src < src/behavior_architecture/examples.repos
```

## Quick start

With the base and the examples built:

```bash
ros2 launch behavior_architecture mission_executor_simple.launch.py
```

The `simple` orchestrator runs `behaviors/examples/state1.xml`, then `state2.xml`, retrying a state
whose tree fails, and stops. `mission_executor_restaurant.launch.py` runs the `restaurant`
orchestrator (follow a person, then take an order), which needs the topics and TF frames of a robot.

Any configuration can be run with:

```bash
ros2 launch behavior_architecture mission_executor.launch.py config_file:=/path/to/config.yaml
# or
ros2 run behavior_architecture mission_executor /path/to/config.yaml
```

## Mission configuration

A configuration for a fixed (non-LLM) orchestrator:

```yaml
node_name: "my_bt_node"                 # name of the rclcpp::Node on the blackboard
orchestrator_type: "my_orchestrator"    # a registered type (required)
package_name: "my_robot"                # default package of the behaviour files
orchestrator_libraries:                 # shared libraries that register orchestrator types
  - "libmy_robot_orchestrator.so"
plugin_libraries:                       # BehaviorTree.CPP plugin libraries, for every runner
  - "libmy_robot_bt_nodes.so"
behaviors:                              # one BehaviorRunner each (required)
  - name: "greet"                       # runner (and node) name, used by activate_runner()
    behavior_file: "behaviors/greet.xml"
    control_period_ms: 50
  - name: "follow"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"   # this file comes from another package
```

| Key | Default | Meaning |
|---|---|---|
| `orchestrator_type` | (required) | Type registered with `OrchestratorRegistrar`. |
| `node_name` | `bt_node` | Name of the ROS node that the BT nodes get from the blackboard key `"node"`. |
| `package_name` | `behavior_architecture` | Package whose share directory resolves relative `behavior_file` paths. |
| `orchestrator_libraries` | `[]` | Libraries `dlopen`ed before the orchestrator is created. They are looked up in `lib/` of the configuration's `package_name` (its install prefix), then through `LD_LIBRARY_PATH`: install them to `lib/` of the package. |
| `plugin_libraries` | `[]` | BT plugin libraries (`BT_REGISTER_NODES`) loaded by every runner; found the same way. |
| `behaviors[].name` | (required) | Runner name. |
| `behaviors[].behavior_file` | (required) | Tree XML: absolute, or relative to the share directory of the package. |
| `behaviors[].package_name` | global `package_name` | Package of this behaviour's file. |
| `behaviors[].control_period_ms` | `50` | Tick period of the runner. |

`mission_executor` puts `behaviors_config`, `plugin_libraries` and `package_name` on the
blackboard, from which `BaseOrchestrator::on_configure` creates the runners. The LLM types use other
keys ([LLM orchestrators](#llm-orchestrators-optional)).

The trees that ship with the package (`behaviors/reusable/`) can be used from any package by
setting `package_name: "behavior_architecture"` on the entry; see
[behaviors/README.md](behaviors/README.md).

## Writing an orchestrator

An orchestrator lives in the application's package, as a shared library that `mission_executor`
loads. In short:

```cpp
#include "behavior_architecture/base_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace my_robot
{

class MyOrchestrator : public behavior_architecture::BaseOrchestrator
{
public:
  explicit MyOrchestrator(BT::Blackboard::Ptr blackboard)
  : BaseOrchestrator("my_orchestrator", blackboard) {}

protected:
  enum State {INIT, GREET, FOLLOW, DONE};

  void control_cycle() override
  {
    switch (state_) {
      case INIT:
        go_to_state(GREET);
        break;
      case GREET:
        if (check_behavior_finished()) {
          go_to_state(last_status_ == "SUCCESS" ? FOLLOW : DONE);
        }
        break;
      case FOLLOW:
        if (check_behavior_finished()) {
          go_to_state(DONE);
        }
        break;
      case DONE:
        break;
    }
  }

  void go_to_state(int state) override
  {
    state_ = static_cast<State>(state);
    deactivate_all_runners();
    if (state_ == GREET) {activate_runner("greet");}
    if (state_ == FOLLOW) {activate_runner("follow");}
  }

private:
  State state_ {INIT};
};

// Registers the type "my_orchestrator" when the library is loaded
static behavior_architecture::OrchestratorRegistrar<MyOrchestrator> registrar("my_orchestrator");

}  // namespace my_robot
```

The library is built and installed to `lib/`, and the YAML names it in `orchestrator_libraries`.
[docs/CUSTOM_ORCHESTRATORS.md](docs/CUSTOM_ORCHESTRATORS.md) has the CMake, the BT plugin library,
the launch file and troubleshooting; `src/examples/` has the `simple` and `restaurant`
orchestrators.

## LLM orchestrators (optional)

Built only with `llm.repos` ([Installation](#with-the-llm-orchestrators)).

| Type / executable | What it does | Services it uses |
|---|---|---|
| `llm` (`LLMPlanOrchestrator`) | Waits for a mission on its `start_mission` service, asks the planner for a plan, generates one tree per step, runs them, and repairs or replans on failure. | `plan_task`, `replan_task` (llm_planner); `generate_bt`, `fix_bt` (llm_bt_builder) |
| `mcp`, with aliases `mcp_llm` and `mcp_llm_plan_orchestrator` (`MCPLLMPlanOrchestrator`) | `llm` plus mission snapshots, failure history and episodic memory on disk. | the same |
| `llm_bt` (`LLMBTOrchestrator`), run by `llm_bt_executor` | Generates, runs and repairs one tree for an objective file, without planning; for evaluating BT generation. | `generate_bt`, `fix_bt` |
| `llm_plan_executor` | `LLMPlanOrchestrator` with an empty blackboard (built-in defaults), without a YAML. | |
| `start_mission` | Sends the goal of a mission file to `start_mission`: `ros2 run behavior_architecture start_mission --ros-args -p mission_file:=goal.yaml [-p skills_file:=skills.yaml]` | |

For the LLM types the YAML has no `behaviors`. It has `plugin_libraries`, `bt_nodes_packages`
(whose `node_descriptions/<pkg>.yaml` describe the available BT nodes to the model),
`bt_control_period_ms`, `bt_timeout_sec`, `skills`, `save_exec`, `exec_dir`, `mission_name`,
`replan_active` and `restart_after_forced` (see `config/llm_config.yaml`). After the config file,
`mission_executor` also accepts `--save-exec`, `--exec-dir PATH` and `--replan-active true|false`.

`launch/llm_dummy_demo.launch.py` (planner, BT builder, `mission_executor` and `start_mission`) and
`launch/llm_bt_eval.launch.py` use the BT nodes of
[dummy_bt_nodes](https://github.com/rodperex/dummy_bt_nodes), which has to be cloned into the
workspace. The design of these orchestrators is in [ARCHITECTURE.md](ARCHITECTURE.md), and the BT
generation evaluation in [BT_GENERATION_EVAL.md](BT_GENERATION_EVAL.md).

## Executables and launch files

| Executable | Layer | Purpose |
|---|---|---|
| `mission_executor` | base | Runs any configuration ([Mission configuration](#mission-configuration)). |
| `simple_example`, `restaurant_example` | base | The two example orchestrators with their own `main` (the older form, which `mission_executor` replaces). |
| `test_plan_executor` | base | Runs the BT XML files of a directory in order: `test_plan_executor <plan_directory>`. |
| `static_tf_publisher` | base | Publishes the static transforms of a YAML file (`tf_file` parameter). |
| `llm_plan_executor`, `llm_bt_executor`, `start_mission` | LLM | See above. |

| Launch file | Runs |
|---|---|
| `mission_executor.launch.py` | `mission_executor` with `config_file:=` (default `config/restaurant_config.yaml`) |
| `mission_executor_simple.launch.py`, `mission_executor_restaurant.launch.py` | the example configurations (need `examples.repos`) |
| `simple_example.launch.py`, `restaurant_example.launch.py` | the example executables |
| `test.launch.py` | `test_plan_executor` |
| `llm_dummy_demo.launch.py`, `llm_bt_eval.launch.py` | the LLM pipeline and the BT generation evaluation (need `llm.repos` and dummy_bt_nodes) |

## ROS 2 Lyrical

Branch `lyrical` builds on ROS 2 Lyrical (and still on Jazzy). It differs from `main` in two
places: `ament_target_dependencies`, removed in Lyrical, is replaced by a small CMake function
(`link_ament_dependencies`) that links the targets of each dependency; and the LLM orchestrators
take the future of `async_send_request` with `.future.share()`, since Lyrical returns a
`FutureAndRequestId`. The optional LLM layer works the same way on both branches.

## Used by

- [attention_system](https://github.com/geriabot/attention_system): attention for a social robot.
- [interaction_system](https://github.com/geriabot/interaction_system): task-bounded dialogue for
  social robots. Its orchestrator is a plugin loaded through `orchestrator_libraries`, and it needs
  only the base.

## Further documentation

| Document | Contents |
|---|---|
| [docs/CUSTOM_ORCHESTRATORS.md](docs/CUSTOM_ORCHESTRATORS.md) | An orchestrator and BT nodes in your own package, step by step. |
| [behaviors/README.md](behaviors/README.md) | The catalogue of trees shipped with the package. |
| [docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md) | Using those trees from another package. |
| [docs/ADDING_REUSABLE_BEHAVIORS.md](docs/ADDING_REUSABLE_BEHAVIORS.md) | Adding a tree to the catalogue. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | The LLM orchestrators (need `llm.repos`). |
| [BT_GENERATION_EVAL.md](BT_GENERATION_EVAL.md) | Evaluating LLM BT generation with `llm_bt_executor` (needs `llm.repos`). |
