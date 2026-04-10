# Generic Mission Executor Update

## What's New

The `behavior_architecture` package now includes a **generic `mission_executor`** program that eliminates the need for hardcoded main programs. Instead of creating a new executable for each orchestrator, you can now configure everything through YAML files.

## Key Components

### 1. OrchestratorFactory System

A factory pattern implementation that allows dynamic creation of orchestrator instances:

- **Header**: [orchestrator_factory.hpp](include/behavior_architecture/orchestrator_factory.hpp)
- **Implementation**: [orchestrator_factory.cpp](src/behavior_architecture/orchestrator_factory.cpp)

### 2. Generic Mission Executor

A single executable that reads YAML configuration and creates all necessary components:

- **Source**: [mission_executor.cpp](src/mission_executor.cpp)
- **Executable**: `mission_executor`

### 3. YAML Configuration Files

Example configurations in the `config/` directory:

- [restaurant_config.yaml](config/restaurant_config.yaml) - Restaurant waiter scenario
- [simple_config.yaml](config/simple_config.yaml) - Simple 2-state orchestrator

### 4. Launch Files

Convenient launch files for common configurations:

- [mission_executor.launch.py](launch/mission_executor.launch.py) - Generic launcher with configurable config file
- [mission_executor_restaurant.launch.py](launch/mission_executor_restaurant.launch.py) - Restaurant scenario
- [mission_executor_simple.launch.py](launch/mission_executor_simple.launch.py) - Simple example

## Usage

### Quick Start

```bash
# Using launch files (recommended)
ros2 launch behavior_architecture mission_executor_restaurant.launch.py
ros2 launch behavior_architecture mission_executor_simple.launch.py

# With custom config
ros2 launch behavior_architecture mission_executor.launch.py config_file:=/path/to/config.yaml

# Direct execution
ros2 run behavior_architecture mission_executor <config_file.yaml>
```

### List Available Orchestrators

```bash
ros2 run behavior_architecture mission_executor
# Output shows registered orchestrators:
#   - restaurant
#   - simple
```

## Benefits Over Hardcoded Mains

| Hardcoded Main | Generic Mission Executor |
|----------------|------------------------|
| New C++ file for each action | Single YAML config file |
| Recompile for every change | Edit config without recompiling |
| Hardcoded parameters | Flexible YAML configuration |
| Many similar executables | One executable for all actions |
| Difficult to maintain | Centralized and maintainable |

## Creating a New Action

See the comprehensive [MISSION_EXECUTOR_GUIDE.md](MISSION_EXECUTOR_GUIDE.md) for detailed instructions.

Quick summary:
1. Create your orchestrator class inheriting from `BaseOrchestrator`
2. Register it with the factory in your `.cpp` file
3. Create a YAML configuration file
4. Update CMakeLists.txt to include your orchestrator
5. Build and run with `mission_executor`

## Example: Adding a New Orchestrator

```cpp
// In my_orchestrator.cpp
#include "behavior_architecture/orchestrator_factory.hpp"

namespace behavior_architecture::examples {

// Register with factory - simple one-liner!
static OrchestratorRegistrar<MyOrchestrator> my_orch_registrar("my_action");

// ... rest of implementation
}
```

```yaml
# In config/my_mission_config.yaml
orchestrator_type: "my_action"
behaviors:
  - name: "behavior1"
    behavior_file: "behaviors/behavior1.xml"
```

```bash
# Run it
ros2 run behavior_architecture mission_executor config/my_mission_config.yaml
```

## Migration from Old Examples

The original hardcoded examples (`restaurant_example` and `simple_example`) are still available for backward compatibility, but new actions should use the `mission_executor` pattern.

To migrate existing code:
1. Keep your orchestrator implementation as-is
2. Add factory registration at the top of your `.cpp` file
3. Create a YAML config file
4. Use `mission_executor` instead of your custom main

## Configuration Schema

```yaml
# Required fields
orchestrator_type: string  # Must match registered name
behaviors:
  - name: string           # Unique behavior identifier
    behavior_file: string  # Path to BT XML (relative to package share dir)
    
    # Optional per-behavior fields
    package_name: string   # Override package for this behavior (default: uses global)
    control_period_ms: int # Override control period (default: 50)

# Optional global fields
node_name: string          # Default: "bt_node"
package_name: string       # Default: "behavior_architecture" - used when behavior doesn't specify
plugin_libraries:          # BehaviorTree plugin libraries
  - string
orchestrator_libraries:    # Orchestrator shared libraries to load
  - string
```

### Using Behaviors from Multiple Packages

You can mix behaviors from different packages in one configuration:

```yaml
package_name: "my_robot"  # Global default

behaviors:
  # Your robot's behavior (uses global package_name)
  - name: "custom_behavior"
    behavior_file: "behaviors/custom.xml"
    
  # Reusable behavior from behavior_architecture
  - name: "follow_behavior"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"  # Override for this behavior
    
  # Another behavior from a different package
  - name: "navigation"
    behavior_file: "behaviors/navigate.xml"
    package_name: "nav_behaviors"
```

## Dependencies

The mission_executor requires `yaml-cpp`, which has been added to the package dependencies.

## Documentation

- **[MISSION_EXECUTOR_GUIDE.md](MISSION_EXECUTOR_GUIDE.md)** - Complete guide for creating new actions
- **[README.md](README.md)** - Original package README

## Testing

Build and test the package:

```bash
cd ~/your_workspace
colcon build --packages-select behavior_architecture
source install/setup.bash

# Test help output
ros2 run behavior_architecture mission_executor

# Test with example configs
ros2 launch behavior_architecture mission_executor_restaurant.launch.py
ros2 launch behavior_architecture mission_executor_simple.launch.py
```

## Files Modified/Added

### New Files
- `include/behavior_architecture/orchestrator_factory.hpp`
- `src/behavior_architecture/orchestrator_factory.cpp`
- `src/mission_executor.cpp`
- `config/restaurant_config.yaml`
- `config/simple_config.yaml`
- `launch/mission_executor.launch.py`
- `launch/mission_executor_restaurant.launch.py`
- `launch/mission_executor_simple.launch.py`
- `MISSION_EXECUTOR_GUIDE.md`
- `GENERIC_MISSION_EXECUTOR_README.md` (this file)

### Modified Files
- `CMakeLists.txt` - Added yaml-cpp dependency, orchestrator_factory, mission_executor target
- `package.xml` - Added yaml-cpp dependency
- `src/examples/restaurant_orchestrator.cpp` - Added factory registration
- `src/examples/simple_orchestrator.cpp` - Added factory registration

### Unchanged
- Original example executables (`restaurant_example`, `simple_example`) still work
- All orchestrator implementations remain backward compatible
- All existing behavior XML files work as-is

## Future Enhancements

Possible future improvements:
- Dynamic plugin loading from config
- Validation of config files
- Config file schema validation
- Support for multiple orchestrators in one config
- Runtime orchestrator switching
- Configuration hot-reload
