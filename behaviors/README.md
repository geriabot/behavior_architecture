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

**Parameters** (configured in XML):
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

**Usage Example**:
```xml
<SubTree ID="FollowBehavior" 
         target_frame="person" 
         base_frame="base_footprint"
         min_distance="0.8"
         cmd_vel_topic="/cmd_vel"/>
```

**Integration**:
1. Ensure your package depends on `behavior_architecture` and `social_bt_nodes`
2. Include the XML path in your BehaviorRunner configuration:
   ```cpp
   runner_config.xml_file_path = "behaviors/reusable/follow_behavior.xml";
   runner_config.plugin_libs = {"social_bt_nodes_plugin"};
   ```
3. Or reference as a SubTree from another behavior tree

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

### Method 1: Load directly in BehaviorRunner

In your C++ code:
```cpp
#include "behavior_architecture/behavior_runner.hpp"

// Create runner configuration
BehaviorRunnerConfig config;
config.name = "my_behavior_runner";
config.xml_file_path = "behaviors/reusable/follow_behavior.xml";
config.plugin_libs = {"social_bt_nodes_plugin"};
config.tree_name = "FollowBehavior";

// Create and use runner
auto runner = std::make_shared<BehaviorRunner>(config);
```

### Method 2: Include as SubTree

In your behavior tree XML:
```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <!-- Include the reusable behavior file -->
  <include path="package://behavior_architecture/behaviors/reusable/follow_behavior.xml"/>
  
  <BehaviorTree ID="MyMainBehavior">
    <Sequence>
      <!-- Your custom logic -->
      <MyCustomNode/>
      
      <!-- Use the reusable behavior -->
      <SubTree ID="FollowBehavior" target_frame="person"/>
      
      <!-- More custom logic -->
      <AnotherCustomNode/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

### Method 3: YAML Configuration (action_executor)

In your YAML config file:
```yaml
behavior_runners:
  follow_runner:
    xml_path: "behaviors/reusable/follow_behavior.xml"
    plugin_libs:
      - "social_bt_nodes_plugin"
    tree_name: "FollowBehavior"
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
- See [GENERIC_ACTION_EXECUTOR_README.md](../GENERIC_ACTION_EXECUTOR_README.md) for YAML configuration
- Refer to [ACTION_EXECUTOR_GUIDE.md](../ACTION_EXECUTOR_GUIDE.md) for orchestrator development
