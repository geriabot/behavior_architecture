# Generic Mission Executor

The `mission_executor` is a generic program that allows you to run behavior architectures configured through YAML files, eliminating the need for hardcoded main programs.

## Overview

Instead of creating a separate main program for each orchestrator, you can now:
1. Create your orchestrator class (inheriting from `BaseOrchestrator`)
2. Register it with the `OrchestratorFactory`
3. Create a YAML configuration file
4. Run it with the generic `mission_executor`

## Quick Start

### Running with Launch Files

```bash
# Run with restaurant configuration
ros2 launch behavior_architecture mission_executor_restaurant.launch.py

# Run with simple configuration
ros2 launch behavior_architecture mission_executor_simple.launch.py

# Run with custom config file
ros2 launch behavior_architecture mission_executor.launch.py config_file:=/path/to/your/config.yaml
```

### Running Directly

```bash
# Get the config file path from the installed package
ros2 run behavior_architecture mission_executor $(ros2 pkg prefix behavior_architecture)/share/behavior_architecture/config/restaurant_config.yaml
```

## Creating a New Action

### 1. Create Your Orchestrator

Create your orchestrator class (e.g., `my_orchestrator.hpp` and `my_orchestrator.cpp`):

```cpp
// my_orchestrator.hpp
#ifndef BEHAVIOR_ARCHITECTURE__EXAMPLES__MY_ORCHESTRATOR_HPP_
#define BEHAVIOR_ARCHITECTURE__EXAMPLES__MY_ORCHESTRATOR_HPP_

#include "behavior_architecture/base_orchestrator.hpp"

namespace behavior_architecture
{
namespace examples
{

class MyOrchestrator : public BaseOrchestrator
{
public:
  MyOrchestrator(BT::Blackboard::Ptr blackboard);

protected:
  void control_cycle() override;
  void go_to_state(int state) override;

private:
  // Your state enum and variables
};

}  // namespace examples
}  // namespace behavior_architecture

#endif
```

```cpp
// my_orchestrator.cpp
#include "behavior_architecture/examples/my_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace behavior_architecture
{
namespace examples
{

// IMPORTANT: Register your orchestrator with the factory
// Simply create a static instance of OrchestratorRegistrar with your class type
static OrchestratorRegistrar<MyOrchestrator> my_orchestrator_registrar("my_orchestrator");

MyOrchestrator::MyOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("my_orchestrator", blackboard)
{
  RCLCPP_INFO(get_logger(), "MyOrchestrator initialized");
}

void MyOrchestrator::control_cycle()
{
  // Your orchestration logic here
}

void MyOrchestrator::go_to_state(int state)
{
  // Your state transition logic here
}

}  // namespace examples
}  // namespace behavior_architecture
```

### 2. Create Your Configuration File

Create a YAML configuration file (e.g., `config/my_mission_config.yaml`):

```yaml
# Node name for the ROS blackboard node
node_name: "bt_node"

# Type of orchestrator to use (must match the registered name)
orchestrator_type: "my_orchestrator"

# Package name for resolving relative paths
package_name: "behavior_architecture"

# Plugin libraries to load (BehaviorTree.CPP plugins)
plugin_libraries:
  - "libsocial_bt_nodes_plugin.so"
  - "libmy_custom_plugin.so"

# List of behaviors to create and manage
behaviors:
  # First behavior
  - name: "behavior1_runner"
    behavior_file: "behaviors/behavior1.xml"
    control_period_ms: 50
  
  # Second behavior
  - name: "behavior2_runner"
    behavior_file: "behaviors/behavior2.xml"
    control_period_ms: 100
```

### 3. Update CMakeLists.txt

Add your orchestrator to the `mission_executor` target:

```cmake
# Generic mission executor (YAML-configured)
add_executable(mission_executor
  src/mission_executor.cpp
  src/examples/restaurant_orchestrator.cpp
  src/examples/simple_orchestrator.cpp
  src/examples/my_orchestrator.cpp  # Add your orchestrator here
)
```

### 4. Create a Launch File (Optional)

Create a launch file for convenience (e.g., `launch/mission_executor_my_action.launch.py`):

```python
#!/usr/bin/env python3

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    """Launch the mission executor with my custom configuration."""
    
    pkg_dir = get_package_share_directory('behavior_architecture')
    config_file = os.path.join(pkg_dir, 'config', 'my_mission_config.yaml')
    
    mission_executor_node = Node(
        package='behavior_architecture',
        executable='mission_executor',
        name='mission_executor_my_action',
        output='screen',
        emulate_tty=True,
        arguments=[config_file]
    )

    return LaunchDescription([
        mission_executor_node
    ])
```

### 5. Build and Run

```bash
# Build the package
cd ~/your_workspace
colcon build --packages-select behavior_architecture

# Source the workspace
source install/setup.bash

# Run your action
ros2 launch behavior_architecture mission_executor_my_action.launch.py
```

## Configuration File Format

### Required Fields

- `orchestrator_type`: String identifier for the orchestrator (must be registered)
- `behaviors`: List of behavior configurations

### Optional Fields

- `node_name`: Name for the ROS blackboard node (default: "bt_node")
- `package_name`: Package name for resolving relative paths (default: "behavior_architecture")
- `plugin_libraries`: List of BehaviorTree plugin libraries to load
- `control_period_ms`: Control cycle period in milliseconds for each behavior (default: 50)

### Behavior Configuration

Each behavior in the `behaviors` list requires:
- `name`: Unique identifier for the behavior runner
- `behavior_file`: Path to the BehaviorTree XML file (relative to package share directory or absolute)
- `control_period_ms`: (optional) Control cycle period in milliseconds

### File Path Resolution

- **Absolute paths**: Used as-is (e.g., `/home/user/behaviors/my_behavior.xml`)
- **Relative paths**: Resolved relative to the package share directory (e.g., `behaviors/my_behavior.xml` → `<package_share>/behaviors/my_behavior.xml`)

## Benefits

1. **No Hardcoded Mains**: Create new actions without writing new main programs
2. **Easy Configuration**: All action parameters in one YAML file
3. **Reusable**: Same executable for all your orchestrators
4. **Maintainable**: Separate concerns - orchestration logic in C++, configuration in YAML
5. **Extensible**: Easy to add new orchestrators by registering them with the factory

## Troubleshooting

### "Orchestrator type not found"

Make sure you:
1. Registered your orchestrator with `REGISTER_ORCHESTRATOR`
2. Linked your orchestrator implementation in CMakeLists.txt
3. Spelled the orchestrator type correctly in the YAML file

To see available orchestrator types:
```bash
ros2 run behavior_architecture mission_executor
```

### "Failed to resolve file path"

Check that:
1. Your `package_name` is correct
2. The behavior XML files exist in the specified paths
3. For relative paths, files are in `<package_share>/<behavior_file>`

### YAML Parsing Errors

Ensure your YAML file:
1. Has correct indentation (use spaces, not tabs)
2. Has all required fields
3. Uses correct YAML syntax (colons, hyphens, etc.)

## Examples

The package includes two example configurations:

1. **Restaurant Configuration** (`config/restaurant_config.yaml`): Demonstrates a restaurant waiter scenario with follow and order collection behaviors
2. **Simple Configuration** (`config/simple_config.yaml`): Shows a basic 2-state orchestrator

You can use these as templates for your own configurations.
