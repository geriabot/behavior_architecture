# Migration Guide: Updating Packages to Use Generic Action Executor

This guide explains how to update existing packages that use `behavior_architecture` to work with the new generic `action_executor` system.

## Overview

The generic action_executor allows packages to use a single executable configured via YAML files instead of maintaining separate hardcoded main programs. This reduces code duplication and makes behavior orchestration more maintainable.

## Migration Steps

### 1. Update Orchestrator for Factory Registration

Add factory registration to your orchestrator class using the template-based registrar:

```cpp
// In your_orchestrator.cpp
#include "behavior_architecture/orchestrator_factory.hpp"

// At the top level (not in any namespace or function)
static behavior_architecture::OrchestratorRegistrar<YourOrchestratorClass> 
  your_orchestrator_registrar("your_type_name");
```

**Example from dummy_robot:**
```cpp
// In dummy_robot_orchestrator.cpp
static behavior_architecture::OrchestratorRegistrar<DummyRobotOrchestrator> 
  dummy_robot_registrar("dummy_robot");
```

### 2. Build Orchestrator as a Shared Library

Update `CMakeLists.txt` to build your orchestrator as a library:

```cmake
# Create orchestrator library (for use with action_executor)
add_library(${PROJECT_NAME}_orchestrator SHARED
  src/your_orchestrator.cpp
)
ament_target_dependencies(${PROJECT_NAME}_orchestrator ${dependencies})

# Install library
install(TARGETS ${PROJECT_NAME}_orchestrator
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
```

### 3. Create BT Nodes Plugin Library (If You Have Custom Nodes)

If your package has custom BehaviorTree nodes, create a plugin library:

**A. Create plugin registration file** (`src/bt_nodes/bt_plugins.cpp`):
```cpp
#include "behaviortree_cpp/bt_factory.h"
#include "your_package/bt_nodes/your_node1.hpp"
#include "your_package/bt_nodes/your_node2.hpp"

BT_REGISTER_NODES(factory)
{
  factory.registerNodeType<your_package::YourNode1>("YourNode1");
  factory.registerNodeType<your_package::YourNode2>("YourNode2");
}
```

**B. Update CMakeLists.txt:**
```cmake
# BT nodes plugin library
add_library(${PROJECT_NAME}_bt_nodes SHARED
  src/bt_nodes/bt_plugins.cpp
  src/bt_nodes/your_node1.cpp
  src/bt_nodes/your_node2.cpp
)
ament_target_dependencies(${PROJECT_NAME}_bt_nodes ${dependencies})

# Required for BehaviorTree.CPP plugin export
target_compile_definitions(${PROJECT_NAME}_bt_nodes PRIVATE BT_PLUGIN_EXPORT)

# Install libraries
install(TARGETS ${PROJECT_NAME}_orchestrator ${PROJECT_NAME}_bt_nodes
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
```

### 4. Create YAML Configuration File

Create `config/your_config.yaml`:

```yaml
# Node name for the ROS blackboard node
node_name: "your_bt_node"

# Type of orchestrator to use (must match registration name)
orchestrator_type: "your_type_name"

# Package name for resolving relative paths
package_name: "your_package"

# Orchestrator libraries to load (contains the orchestrator registration)
orchestrator_libraries:
  - "libyour_package_orchestrator.so"

# Plugin libraries to load (BehaviorTree.CPP plugins)
plugin_libraries:
  - "libsocial_bt_nodes_plugin.so"
  - "libyour_package_bt_nodes.so"  # Only if you have custom nodes

# List of behaviors to create and manage
behaviors:
  - name: "state1_runner"
    behavior_file: "behaviors/state1.xml"
    control_period_ms: 50
  
  - name: "state2_runner"
    behavior_file: "behaviors/state2.xml"
    control_period_ms: 50
```

### 5. Install Config Directory

Update `CMakeLists.txt` to install the config directory:

```cmake
# Install config files
install(DIRECTORY
  config
  DESTINATION share/${PROJECT_NAME}
)
```

### 6. Create Launch File

Create `launch/your_action_executor.launch.py`:

```python
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

def generate_launch_description():
    # Get the config file path
    config_file = PathJoinSubstitution([
        FindPackageShare('your_package'),
        'config',
        'your_config.yaml'
    ])
    
    # Create the action executor node
    action_executor = Node(
        package='behavior_architecture',
        executable='action_executor',
        output='screen',
        emulate_tty=True,
        arguments=[config_file]
    )
    
    return LaunchDescription([
        action_executor
    ])
```

### 7. Install Launch File

Update `CMakeLists.txt`:

```cmake
# Install launch files
install(DIRECTORY
  launch
  DESTINATION share/${PROJECT_NAME}
)
```

## Complete Example: dummy_robot Package

The `dummy_robot` package has been fully migrated. Key files:

- **Orchestrator:** [src/dummy_robot_orchestrator.cpp](../../dummy_robot/src/dummy_robot_orchestrator.cpp)
  - Added factory registration with `OrchestratorRegistrar`
  
- **BT Plugin:** [src/bt_nodes/bt_plugins.cpp](../../dummy_robot/src/bt_nodes/bt_plugins.cpp)
  - Registers LogMessage custom node

- **Config:** [config/dummy_robot_config.yaml](../../dummy_robot/config/dummy_robot_config.yaml)
  - Specifies orchestrator type, libraries, and behaviors

- **Launch:** [launch/dummy_robot_action_executor.launch.py](../../dummy_robot/launch/dummy_robot_action_executor.launch.py)
  - Uses generic action_executor

- **CMakeLists.txt:** Updated to:
  - Build orchestrator as shared library
  - Build BT nodes as plugin library with `BT_PLUGIN_EXPORT`
  - Install config directory

## Testing Your Migration

1. **Build the package:**
   ```bash
   colcon build --packages-select your_package
   source install/setup.bash
   ```

2. **Verify orchestrator registration:**
   ```bash
   ros2 run behavior_architecture action_executor
   ```
   You should see your orchestrator type listed.

3. **Run your action executor:**
   ```bash
   ros2 launch your_package your_action_executor.launch.py
   ```

## Benefits of Migration

1. **Less Code:** No need for custom main() programs
2. **Easier Configuration:** Change behaviors via YAML without recompiling
3. **Reusability:** Same action_executor works for all orchestrators
4. **Maintainability:** Single point of implementation for executor logic
5. **Flexibility:** Easy to add new behaviors or modify existing ones

## Backward Compatibility

Your original executable (e.g., `dummy_robot_main`) can be kept for backward compatibility. The new action_executor approach is additive, not replacing existing functionality.

## Troubleshooting

### Orchestrator not found in registry
- Ensure orchestrator library is listed in `orchestrator_libraries` in YAML
- Verify the registration name matches `orchestrator_type` in YAML
- Check that `OrchestratorRegistrar` is at file scope (not in function)

### BT node not recognized
- Add custom BT library to `plugin_libraries` in YAML
- Verify `BT_PLUGIN_EXPORT` compile definition is set
- Check that plugin registration uses `BT_REGISTER_NODES` macro
- Ensure library name matches what's in YAML (with `lib` prefix)

### Library loading errors
- Verify libraries are installed to `lib/` directory
- Check `LD_LIBRARY_PATH` includes your install directory
- Use `nm -D libname.so | grep symbol` to verify exports

## Additional Resources

- [ACTION_EXECUTOR_GUIDE.md](ACTION_EXECUTOR_GUIDE.md) - Detailed action_executor usage
- [GENERIC_ACTION_EXECUTOR_README.md](../GENERIC_ACTION_EXECUTOR_README.md) - Overview and quick start
