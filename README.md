# behavior_architecture

Generic behavior architecture framework for ROS 2 robots using FSM (Finite State Machine) and BehaviorTree coordination with YAML-based configuration.

## 📚 Documentation Index

- **[behaviors/README.md](behaviors/README.md)** - Complete behavior catalogue with all available reusable behaviors
- **[docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md)** - Quick reference for using behaviors in your packages
- **[docs/ADDING_REUSABLE_BEHAVIORS.md](docs/ADDING_REUSABLE_BEHAVIORS.md)** - Step-by-step guide for creating new reusable behaviors
- **[GENERIC_MISSION_EXECUTOR_README.md](GENERIC_MISSION_EXECUTOR_README.md)** - YAML configuration guide
- **[MISSION_EXECUTOR_GUIDE.md](MISSION_EXECUTOR_GUIDE.md)** - Creating custom orchestrators
- **[BEHAVIOR_CATALOGUE_SUMMARY.md](BEHAVIOR_CATALOGUE_SUMMARY.md)** - Summary of catalogue restructuring

## Overview

This package provides a reusable framework for implementing hierarchical robot behaviors where:
- A **Finite State Machine (FSM)** orchestrates high-level behavior states
- **BehaviorTree nodes** (loaded as plugins) implement the actual behaviors for each state
- **BehaviorRunner** loads and executes BehaviorTree XML files with lifecycle management
- **BaseOrchestrator** coordinates BehaviorRunner activation/deactivation
- A **shared blackboard** enables communication between all components
- **Mission Executor** provides a generic YAML-configured runtime for any orchestrator

## Architecture

```
┌──────────────────────────────────────────────────────┐
│          Custom Orchestrator (Your Package)          │
│   ┌──────────────────────────────────────────────┐   │
│   │  FSM Control Logic                           │   │
│   │  - State transitions                         │   │
│   │  - BehaviorRunner coordination               │   │
│   └──────────────────────────────────────────────┘   │
│              inherits from                           │
│   ┌──────────────────────────────────────────────┐   │
│   │  BaseOrchestrator                            │   │
   │  - LifecycleNode management                  │   │
│   │  - BehaviorRunner activation/deactivation    │   │
│   │  - Status monitoring                         │   │
│   └──────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────┘
                         ↓
              activates/deactivates via cascade
                         ↓
┌──────────────────────────────────────────────────────┐
│              BehaviorRunner Nodes                    │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
│  │ Runner 1     │ │ Runner 2     │ │ Runner 3     │ │
│  │ - XML: bt1   │ │ - XML: bt2   │ │ - XML: bt3   │ │
│  │ - Plugins    │ │ - Plugins    │ │ - Plugins    │ │
│  └──────────────┘ └──────────────┘ └──────────────┘ │
└──────────────────────────────────────────────────────┘
                         ↓
              loads plugins and executes trees
                         ↓
┌──────────────────────────────────────────────────────┐
│             BehaviorTree Plugin Nodes                │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐   │
│  │ Follow  │ │ Speak   │ │ Listen  │ │ Custom  │   │
│  └─────────┘ └─────────┘ └─────────┘ └─────────┘   │
└──────────────────────────────────────────────────────┘
```

## Key Components

### BehaviorRunner
Lifecycle node that loads and executes BehaviorTree XML files with plugin support:
- Loads BT plugins dynamically (e.g., `social_bt_nodes_plugin`)
- Creates trees from XML files located in any package's share directory
- Publishes execution status (SUCCESS, FAILURE, RUNNING)
- Supports standard ROS 2 lifecycle for coordinated activation

### BaseOrchestrator
Base class for implementing FSM-based behavior coordination:
- Manages high-level state transitions
- Activates/deactivates BehaviorRunner nodes via `activate_runner` / `deactivate_runner`
- Monitors behavior execution status
- Provides shared blackboard for inter-component communication

## Key Features

- **Plugin-Based BT Nodes**: Load custom BT node libraries at runtime
- **Reusable BehaviorRunner**: Use in any package by specifying XML path and plugins
- **Reusable Behavior Catalogue**: Library of tested, documented behaviors ready to use
- **Extensible Orchestrator**: Implement custom FSM logic in derived classes
- **Lifecycle Management**: Coordinated node activation/deactivation via `rclcpp_lifecycle`
- **Package-Agnostic XML**: Load behavior trees from any package
- **Shared Blackboard**: Communication between orchestrator and BT nodes
- **Status Monitoring**: Built-in behavior execution tracking

## Usage

### Quick Start with Example

This package includes complete examples that demonstrate the YAML-configured mission executor:

```bash
# Build the workspace
cd ~/ros2_ws
colcon build --packages-select behavior_architecture
source install/setup.bash

# Run the simple example (basic two-state FSM)
ros2 launch behavior_architecture mission_executor_simple.launch.py

# Run the restaurant service example (more complex)
ros2 launch behavior_architecture mission_executor_restaurant.launch.py
```

The examples demonstrate:
- YAML-based orchestrator and behavior configuration
- Multiple BehaviorRunner nodes with different XML files
- FSM-based orchestrator coordinating state transitions
- Dynamic loading of orchestrator and plugin libraries
- Integration with `social_bt_nodes` plugin library
- Use of reusable behaviors from the package catalogue

## Reusable Behavior Catalogue

This package includes a **catalogue of reusable behaviors** that can be used in any ROS 2 package:

```
behaviors/
├── reusable/          # Production-ready, reusable behaviors
│   └── follow_behavior.xml
└── examples/          # Example/demo behaviors
    ├── collect_order.xml
    ├── state1.xml
    └── state2.xml
```

### Using Reusable Behaviors

Reference behaviors directly in your YAML configuration:

```yaml
behaviors:
  - name: "follow_runner"
    behavior_file: "behaviors/reusable/follow_behavior.xml"  # From this package
    control_period_ms: 50
```

Or include them as SubTrees in your behavior XML files:

```xml
<include path="package://behavior_architecture/behaviors/reusable/follow_behavior.xml"/>
<SubTree ID="FollowBehavior" target_frame="person"/>
```

**Documentation**:
- [behaviors/README.md](behaviors/README.md) - Complete catalogue with all available behaviors
- [docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md) - Quick reference guide

## Standard Workflow (Recommended)

The recommended approach uses YAML configuration with the generic `mission_executor`:

#### ltiple BehaviorRunner nodes with different XML files
- FSM-based orchestrator coordinating state transitions
- Dynamic loading of orchestrator and plugin libraries
- Integration with `social_bt_nodes` plugin library

### 1. Create BehaviorTree XML Files

Create XML files in your package's `behaviors/` directory:

```xml
<!-- behaviors/my_behavior.xml -->
<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MyBehavior">
    <Sequence>
      <Speak text="Hello World" service_name="/tts_service"/>
      <IsTargetDetected target_frame="target" base_frame="base_link"/>
      <Follow target_frame="target" base_frame="base_link"/>
    </Sequence>
  <# 2_package_name"               // Package containing the XML
);
```

### 3. Create Your Orchestrator Class

Inherit from `BaseOrchestrator` and implement the required methods:

```cpp
#include "behavior_architecture/base_orchestrator.hpp"

namespace my_package
{

enum class MyState : int {
  INIT = 0,
  STATE_1 = 1,
  STATE_2 = 2,
  STOP = 3
};

class MyOrchestrator : public behavior_architecture::BaseOrchestrator
{
public:
  MyOrchestrator(BT::Blackboard::Ptr blackboard)
  : BaseOrchestrator("my_orchestrator_node", blackboard),
    state_(MyState::INIT)
  {
    // Initialize your orchestrator
  }

protected:
  void control_cycle() override
  {
    switch (state_) {
      case MyState::INIT:
        go_to_state(static_cast<int>(MyState::STATE_1));
        break;
      
      case MyState::STATE_1:
        if (check_behavior_finished()) {
          if (last_status_ == "SUCCESS") {
            go_to_state(static_cast<int>(MyState::STATE_2));
          }
        }
        break;
      
      case MyState::STATE_2:
        if (check_behavior_finished()) {
          go_to_state(static_cast<int>(MyState::STOP));
        }
        break;
      
      case MyState::STOP:
        break;
    }
  }

  void go_to_state(int state) override
  {
    state_ = static_cast<MyState>(state);
    
    switch (state_) {
      case MyState::STATE_1:
        RCLCPP_INFO(get_logger(), "Transitioning to STATE_1");
        deactivate_all_runners();
        activate_runner("behavior_tree_node_1");
        break;
      
      case MyState::STATE_2:
        RCLCPP_INFO(get_logger(), "Transitioning to STATE_2");
        deactivate_all_runners();
        activate_runner("behavior_tree_node_2");
        break;
      
      case MyState::STOP:
        RCLCPP_INFO(get_logger(), "Stopping");
        deactivate_all_runners();
        break;
    }
  }

private:
  MyState state_;
};

}  // namespace my_package
```

### 4. Create Main Application

```cpp
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "behavior_architecture/behavior_runner.hpp"
#include "my_package/my_orchestrator.hpp"

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  // Create shared blackboard
  auto blackboard = BT::Blackboard::create();
  
  // Define plugins to load
  std::vector<std::string> plugins = {"social_bt_nodes_plugin"};
  
  // Create BehaviorRunner nodes
  auto runner1 = std::make_shared<behavior_architecture::BehaviorRunner>(
    blackboard,
    "behavior_1",
    "behaviors/behavior1.xml",
    plugins,
    "my_package"
  );
  
  auto runner2 = std::make_shared<behavior_architecture::BehaviorRunner>(
    blackboard,
    "behavior_2",
    "behaviors/behavior2.xml",
    plugins,
    "my_package"
  );

  // Create orchestrator
  auto orchestrator = std::make_shared<my_package::MyOrchestrator>(blackboard);

  // Configure all nodes
  runner1->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  runner2->trigger_transition(
    lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
  orchestrator->trigger_transition(
find_package(social_bt_nodes REQUIRED)  # If using social_bt_nodes plugins

add_executable(my_behavior_node src/main.cpp src/my_orchestrator.cpp)
target_link_libraries(my_behavior_node behavior_architecture)
ament_target_dependencies(my_behavior_node
  behavior_architecture
  rclcpp
  rclcpp_lifecycle
  behaviortree_cpp
)

# Install XML behavior files
install(DIRECTORY behaviors/
  DESTINATION share/${PROJECT_NAME}/behaviors
)
```

## BehaviorRunner API

### Constructor

```cpp
BehaviorRunner(
  BT::Blackboard::Ptr blackboard,      // Shared blackboard
  const std::string & name,            // Node name
  const std::string & xml_path,        // Path to XML (from package share)
  const std::vector<std::string> & plugins,  // Plugin libraries to load
  const std::string & package_name = "behavior_architecture"  // Package with XML
);
```

### Methods

- `BT::NodeStatus get_bt_status()` - Get current tree execution status
- `void refresh()` - Reset behavior runner to initial state

### Published Topics

- `behavior_status` (std_msgs/String) - Execution status updatesxecutor.add_node(runner1->get_node_base_interface());
  executor.add_node(runner2->get_node_base_interface());
  executor.add_node(orchestrator->get_node_base_interface());

  executor.spin();
Orchestrator
  rclcpp::shutdown();
  return 0;
}
```

### 5. Package Integration

Add to your package's `package.xml`:

```xml
<depend>behavior_architecture</depend>
<depend>rclcpp</depend>
<depend>rclcpp_lifecycle</depend>
<depend>behaviortree_cpp</depend>
```

Add to your `CMakeLists.txt`:

```cmake
find_package(behavior_architecture REQUIRED)

add_executable(my_behavior_node src/main.cpp src/my_orchestrator.cpp)
ament_target_dependencies(my_behavior_node
  behavior_architecture
  rclcpp
  rclcpp_lifecycle
  behaviortree_cpp
)
```

## Base Class API

### Protected Methods

#### `void control_cycle()` (pure virtual)
Main control loop called periodically. Implement your FSM logic here.

#### `void go_to_state(int state)` (pure virtual)
Handle state transitions. Activate/deactivate BT nodes as needed.

#### `bool check_behavior_finished()`
Check if the currently active behavior tree has finished execution.
Returns `true` if status changed from last check.

#### `void status_callback(std_msgs::msg::String::UniquePtr msg)`
Callback for behavior status updates. Status is stored in `last_status_`.

### Protected Members

- `BT::Blackboard::Ptr blackboard_` - Shared blackboard for BT communication
- `std::string last_status_` - Last received behavior status ("SUCCESS", "FAILURE", "RUNNING")
- `int control_cycle_rate_ms_` - Control cycle period (default: 100ms)

### Runner Management Methods

- `activate_runner(const std::string& runner_name)` - Activate a BehaviorRunner
- `deactivate_runner(const std::string& runner_name)` - Deactivate a BehaviorRunner
- `deactivate_all_runners()` - Deactivate all BehaviorRunners

## Dependencies

This package requires the following ROS 2 packages:

- `rclcpp`
- `rclcpp_lifecycle`
- `behaviortree_cpp` (BehaviorTree.CPP 4.x)
- `std_msgs`
- `ament_index_cpp`
- `yaml-cpp`
- `social_bt_nodes` (optional, runtime plugin only)

### Installing Dependencies

```bash
# Clone third-party dependencies
cd ~/ros2_ws/src
vcs import < behavior_architecture/thirdparty.repos

# Build workspace
cd ~/ros2_ws
colcon build
```

## Included Examples

### Restaurant Service Example

Location: `src/examples/`

Demonstrates a two-state behavior system:
1. **Follow Behavior**: Robot follows a person
2. **Collect Order**: Robot takes a food/drink order via speech

Files:
- `restaurant_orchestrator.cpp/hpp` - FSM orchestrator
- `restaurant_main.cpp` - Main application
- `behaviors/follow_behavior.xml` - Following behavior tree
- `behaviors/collect_order.xml` - Order collection behavior tree
- `launch/restaurant_example.launch.py` - Launch file

## Building

```bash
cd ~/ros2_ws
colMission Executor

The `mission_executor` is a generic executable that loads orchestrators and behaviors dynamically based on YAML configuration. This eliminates the need to write boilerplate main() programs for each robot application.

### Features

- **Dynamic Orchestrator Loading**: Load any registered orchestrator by name
- **YAML Configuration**: Define all components declaratively
- **Plugin System**: Load both orchestrator and BT node plugins at runtime
- **No Code Duplication**: One executable works for all behavior systems
- **Easy Testing**: Swap configurations without recompilation

### Usage

```bash
# Run with a specific configuration
ros2 run behavior_architecture mission_executor /path/to/config.yaml

# List available orchestrator types
ros2 run behavior_architecture mission_executor
```

See included examples for complete working configurations.

## Orchestrator Modes

There are two orchestrator modes, and **how you start a mission depends on which one you use**.

### Fixed Orchestrators

`SimpleOrchestrator`, `RestaurantOrchestrator`, and any custom orchestrator built from `BaseOrchestrator` start **automatically** when the lifecycle node transitions to ACTIVE. No external trigger is needed — the FSM begins executing its first state immediately in `on_activate()`.

```
ros2 launch behavior_architecture mission_executor_simple.launch.py
# → mission starts by itself
```

### LLM Orchestrator (`LLMPlanOrchestrator`)

`LLMPlanOrchestrator` exposes a `/start_mission` ROS 2 service and **waits** after activation. The FSM does not start until someone calls that service with the goal, context, and robot skills. This is intentional: the mission is dynamic and must be provided at runtime.

To trigger a mission in LLM mode you have two options:

**Option 1 — `test_start_mission` (provided helper)**

An ephemeral node that reads a YAML file and calls `/start_mission` once, then exits:

```bash
ros2 run behavior_architecture test_start_mission \
  --ros-args \
  -p mission_file:=/path/to/llm_config.yaml \
  -p skills_file:=/path/to/skills.yaml
```

The launch files (`llm_dummy_demo.launch.py`, `dummy_robot_llm.launch.py`) start this node automatically after a 3-second delay so `mission_executor` is ready.

**Option 2 — call the service directly**

```bash
ros2 service call /start_mission llm_planner_interfaces/srv/StartMission \
  "{goal: 'Greet the visitor and guide them to room 3',
    context: 'Hospital reception area',
    skills: ['Navigate to a location', 'Speak using text-to-speech']}"
```

In a real deployment, another node (e.g. an HRI component) would call `/start_mission` when appropriate, replacing `test_start_mission` entirely.

#### Summary

| Orchestrator type | Mission starts… | Trigger needed? |
|---|---|---|
| Fixed (`Simple`, `Restaurant`, custom) | Automatically on `ACTIVATE` | No |
| LLM (`LLMPlanOrchestrator`) | On `/start_mission` service call | Yes |

## Included Examples

### Simple Example

Location: `src/examples/simple_*`, `config/simple_config.yaml`

A minimal two-state FSM demonstration:
- **STATE_1**: Prints "Hello from State 1"
- **STATE_2**: Prints "Hello from State 2"

Run: `ros2 launch behavior_architecture mission_executor_simple.launch.py`

### Restaurant Service Example

Location: `src/examples/restaurant_*`, `config/restaurant_config.yaml`

Demonstrates a practical two-state behavior system:
1. **Follow Behavior**: Robot follows a person using vision
2. **Collect Order**: Robot takes a food/drink order via speech

Run: `ros2 launch behavior_architecture mission_executor_restaurant.launch.py`

Both examples showcase the complete workflow from orchestrator implementation to YAML configuration and launch files.