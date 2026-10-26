# Behavior Trees Catalogue

This directory contains BehaviorTree XML files for the behavior_architecture package. The behaviors are organized into two categories:

## Directory Structure

```
behaviors/
├── reusable/          # Production-ready, reusable complex behaviors
│   └── follow_behavior.xml
├── examples/          # Example behaviors for testing and demonstration
│   ├── collect_order.xml
│   ├── state1.xml
│   └── state2.xml
└── README.md          # This file
```

## Reusable Behaviors

The `reusable/` directory contains **production-ready**, **well-tested**, and **documented** behavior trees that can be used directly in other packages. These behaviors are designed to be:

- **Generic**: Parameterized and configurable via BT XML parameters
- **Robust**: Include error handling and edge cases
- **Well-documented**: Clear comments explaining purpose, inputs, outputs, and behavior
- **Self-contained**: Minimal external dependencies (or clearly documented)

### Available Reusable Behaviors

#### `follow_behavior.xml`
**Purpose**: Follow a detected target while avoiding obstacles

**Description**: A reactive behavior that continuously checks for a target (e.g., person), follows them maintaining a safe distance while avoiding obstacles, and searches by spinning if the target is lost.

**Key Features**:
- Reactive target detection checking
- Obstacle avoidance during following
- Automatic search behavior when target is lost
- Configurable distances, speeds, and frame IDs

**Required BT Nodes**:
- `IsTargetDetected`: Check if target exists in TF tree
- `Follow`: Navigate towards target with obstacle avoidance
- `SpinSearch`: Rotate in place to search for target

**Parameters** (fixed in the XML):
- `target_frame`: TF frame of the target to follow (default: "target")
- `base_frame`: Robot's base frame (default: "base_link")
- `min_distance`: Minimum distance to maintain from target (default: 0.5m)
- `avoidance_distance`: Obstacle avoidance distance (default: 0.2m)
- `max_linear_speed`: Maximum forward speed (default: 1.0 m/s)
- `max_angular_speed`: Maximum rotation speed (default: 0.5 rad/s)
- `angular_speed`: Search rotation speed (default: 1.0 rad/s)
- `cmd_vel_topic`: Velocity command topic (default: "/target")
- `sonar_topic`: Sonar sensor topic (default: "/sensors/sonar")
- `touch_topic`: Touch sensor topic (default: "/sensors/touch")

The values are written in the XML, not exposed as ports of the subtree: to change them, copy the
file into your package and edit it.

**Integration**:
1. Build social_bt_nodes (`examples.repos`), which provides the nodes of this tree
2. Add it as a behaviour of your mission configuration (see [Using Behaviors in Other Packages](#using-behaviors-in-other-packages))
3. Or include it as a SubTree from another behavior tree

---

## Example Behaviors

The `examples/` directory contains **demonstration** and **test** behaviors. These are typically:

- Specific to particular use cases or applications
- Used for testing and demonstrations
- May be incomplete or simplified
- Serve as learning resources or starting points

### Available Examples

#### `collect_order.xml`
Restaurant service scenario - collects food and drink orders from customers using speech interaction.

**Features**:
- Speech synthesis (TTS) and recognition (STT)
- Yes/No confirmation checking
- Information extraction from speech
- Multi-turn conversation flow

#### `state1.xml` / `state2.xml`
Simple single-action behaviors used for basic FSM testing.

---

## Creating New Reusable Behaviors

When adding a new reusable behavior to the catalogue, follow these guidelines:

### 1. Design Principles
- **Parameterization**: Use BT ports for all configurable values
- **Modularity**: Break complex behaviors into reusable SubTrees
- **Error Handling**: Include failure paths and recovery behaviors
- **Reactivity**: Use Reactive nodes where appropriate for dynamic environments

### 2. Documentation Requirements
- Add a comprehensive header comment in the XML file explaining:
  - Purpose and use case
  - Required BT node plugins
  - All parameters with types and defaults
  - Expected behavior and edge cases
  - Example usage

### 3. File Naming Convention
Use descriptive, action-oriented names:
- `follow_behavior.xml` ✓
- `navigate_to_goal.xml` ✓
- `pick_and_place.xml` ✓
- `my_tree.xml` ✗ (too generic)

### 4. Testing
Before moving a behavior to `reusable/`:
- Test in multiple scenarios
- Verify parameter configuration works correctly
- Ensure graceful failure handling
- Test integration with different robots/platforms if possible

### 5. File Template
```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <!-- 
    Behavior Name: [Descriptive Name]
    
    Purpose: 
    [Clear description of what this behavior does]
    
    Use Case:
    [When and why to use this behavior]
    
    Required Plugins:
    - plugin_name_1
    - plugin_name_2
    
    Parameters:
    - param_1 (type, default): description
    - param_2 (type, default): description
    
    Example Usage:
    <SubTree ID="BehaviorName" param_1="value1" param_2="value2"/>
  -->
  
  <BehaviorTree ID="BehaviorName">
    <!-- Behavior tree definition -->
  </BehaviorTree>
  
</root>
```

---

## Using Behaviors in Other Packages

### Method 1: As a behaviour of a mission configuration

In the YAML of `mission_executor`, the entry names the package the file comes from:
```yaml
package_name: "my_robot"
plugin_libraries:
  - "libsocial_bt_nodes_plugin.so"
behaviors:
  - name: "follow_runner"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"
    control_period_ms: 50
```
The orchestrator then runs it with `activate_runner("follow_runner")`.

### Method 2: Include as SubTree

In your behavior tree XML:
```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <!-- Include the reusable behavior file -->
  <include ros_pkg="behavior_architecture" path="behaviors/reusable/follow_behavior.xml"/>
  
  <BehaviorTree ID="MyMainBehavior">
    <Sequence>
      <!-- Your custom logic -->
      <MyCustomNode/>
      
      <!-- Use the reusable behavior -->
      <SubTree ID="FollowBehavior"/>
      
      <!-- More custom logic -->
      <AnotherCustomNode/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

### Method 3: A runner created in code

An orchestrator can create a runner itself and register it before it is configured:
```cpp
#include "behavior_architecture/behavior_runner.hpp"

auto runner = std::make_shared<behavior_architecture::BehaviorRunner>(
  blackboard_, "follow_runner", "behaviors/reusable/follow_behavior.xml",
  std::vector<std::string>{"libsocial_bt_nodes_plugin.so"},
  "behavior_architecture",   // package of the XML file
  50);                       // tick period in ms
register_runner("follow_runner", runner);
```

---

## Dependencies

### Core Requirements
- `behavior_architecture`: Base framework
- `behaviortree_cpp`: BehaviorTree.CPP library

### Plugin Requirements (behavior-specific)
- `social_bt_nodes`: Social interaction nodes (Speak, Listen, Follow, etc.)
- Add other plugin packages as needed

---

## Contributing

To contribute a new reusable behavior:

1. Develop and test your behavior in the `examples/` directory first
2. Ensure it meets all reusability requirements (see "Creating New Reusable Behaviors")
3. Add comprehensive documentation in the XML file
4. Update this README with the new behavior's entry
5. Submit a pull request with:
   - The behavior XML file
   - Documentation updates
   - Test results or example usage

---

## Questions or Issues?

- Check the main [behavior_architecture README](../README.md) for framework details
- See [docs/CUSTOM_ORCHESTRATORS.md](../docs/CUSTOM_ORCHESTRATORS.md) for orchestrator development
