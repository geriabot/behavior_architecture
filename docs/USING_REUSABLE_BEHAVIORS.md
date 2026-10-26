# Quick Reference: Using Reusable Behaviors

This guide provides quick examples for using behaviors from the behavior_architecture catalogue.

## Accessing Behaviors

All behavior XML files are installed to the package's share directory. You can reference them using:

```cpp
// In C++ code
std::string package_share_dir = ament_index_cpp::get_package_share_directory("behavior_architecture");
std::string behavior_path = package_share_dir + "/behaviors/reusable/follow_behavior.xml";
```

```python
# In Python code
from ament_index_python.packages import get_package_share_directory
import os

package_share_dir = get_package_share_directory('behavior_architecture')
behavior_path = os.path.join(package_share_dir, 'behaviors', 'reusable', 'follow_behavior.xml')
```

## Directory Structure

```
behaviors/
├── reusable/              # Production-ready, reusable behaviors
│   └── follow_behavior.xml
├── examples/              # Example/demo behaviors
│   ├── collect_order.xml
│   ├── state1.xml
│   └── state2.xml
└── README.md              # Full catalogue documentation
```

## Usage Methods

### Method 1: YAML Configuration (Recommended)

Most flexible approach using the mission_executor:

```yaml
# your_config.yaml
node_name: "my_robot_node"
orchestrator_type: "my_orchestrator"  # Your custom orchestrator type
package_name: "my_robot_package"      # Global default

plugin_libraries:
  - "libsocial_bt_nodes_plugin.so"

behaviors:
  # Reusable behavior from behavior_architecture package
  - name: "follow_runner"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"  # Specify source package
    control_period_ms: 50
    
  # Your custom behavior from your own package
  - name: "custom_runner"
    behavior_file: "behaviors/your_custom.xml"
    # package_name not specified, uses global package_name
    control_period_ms: 100
```

**Key points:**
- Each behavior can optionally specify its own `package_name`
- If not specified, it uses the global `package_name` from the config
- This allows mixing behaviors from different packages in one config
- **Note**: State machine logic (states, transitions) is implemented in your C++ orchestrator, not in YAML

### Method 2: Direct BehaviorRunner Creation

In your C++ orchestrator:

```cpp
#include "behavior_architecture/behavior_runner.hpp"

// Before the orchestrator is configured (e.g. in its constructor)
auto follow_runner = std::make_shared<behavior_architecture::BehaviorRunner>(
  blackboard_, "follow_runner", "behaviors/reusable/follow_behavior.xml",
  std::vector<std::string>{"libsocial_bt_nodes_plugin.so"},
  "behavior_architecture",   // package of the XML file
  50);                       // tick period in ms
register_runner("follow_runner", follow_runner);
```

### Method 3: SubTree Inclusion

Reference reusable behaviors from your own behavior trees:

```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <!-- Include the reusable behavior -->
  <include ros_pkg="behavior_architecture" path="behaviors/reusable/follow_behavior.xml"/>
  
  <BehaviorTree ID="MyComplexBehavior">
    <Sequence>
      <Speak text="I will follow you now"/>
      
      <!-- Use the reusable behavior as a SubTree -->
      <!-- Its values (frames, distances, topics) are fixed in its XML -->
      <SubTree ID="FollowBehavior"/>
      
      <Speak text="Following completed"/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

## Available Reusable Behaviors

### follow_behavior.xml

**Purpose**: Follow a detected target while avoiding obstacles

**Quick Example**:
```yaml
- name: "my_follow_runner"
  behavior_file: "behaviors/reusable/follow_behavior.xml"
  package_name: "behavior_architecture"
  control_period_ms: 50
```

**Common Parameters** (edit in XML or create a custom version):
- `target_frame`: TF frame to follow (default: "target")
- `base_frame`: Robot's base frame (default: "base_link")
- `min_distance`: How close to get to target (default: 0.5m)
- `cmd_vel_topic`: Where to publish velocity (default: "/target")

See [behaviors/README.md](../behaviors/README.md) for full documentation.

## Creating Your Own Behaviors

### Option 1: Start from Examples

1. Copy an example behavior:
   ```bash
   cp behaviors/examples/state1.xml behaviors/my_behavior.xml
   ```

2. Edit and test it

3. When ready for production, move to `reusable/` and document it

### Option 2: Include Reusable Behaviors

Build complex behaviors by combining reusable ones:

```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <include ros_pkg="behavior_architecture" path="behaviors/reusable/follow_behavior.xml"/>
  
  <BehaviorTree ID="MyWorkflow">
    <Sequence>
      <MyCustomGreeting/>
      <SubTree ID="FollowBehavior"/>
      <MyCustomGoodbye/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

## Package Dependencies

When using these behaviors in your package:

### In package.xml:
```xml
<depend>behavior_architecture</depend>
<depend>social_bt_nodes</depend>  <!-- If using social interaction nodes -->
```

### In CMakeLists.txt:
```cmake
find_package(behavior_architecture REQUIRED)
find_package(social_bt_nodes REQUIRED)
```

## Common Patterns

### Pattern 1: State-Based with Reusable Behaviors

**In your YAML config:**
```yaml
behaviors:
  - name: "follow"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"
  - name: "greet"
    behavior_file: "behaviors/my_package/greet.xml"
```

**In your C++ orchestrator** (see [CUSTOM_ORCHESTRATORS.md](CUSTOM_ORCHESTRATORS.md)):
```cpp
void MyOrchestrator::control_cycle()
{
  switch (state_) {
    case State::IDLE:
      if (button_pressed_) {go_to_state(static_cast<int>(State::GREETING));}
      break;
    case State::GREETING:
      if (check_behavior_finished()) {
        go_to_state(static_cast<int>(
          last_status_ == "SUCCESS" ? State::FOLLOWING : State::IDLE));
      }
      break;
    case State::FOLLOWING:
      if (check_behavior_finished()) {
        go_to_state(static_cast<int>(State::IDLE));
      }
      break;
  }
}

void MyOrchestrator::go_to_state(int state)
{
  state_ = static_cast<State>(state);
  deactivate_all_runners();
  if (state_ == State::GREETING) {activate_runner("greet");}
  if (state_ == State::FOLLOWING) {activate_runner("follow");}
}
```

### Pattern 2: Concurrent behaviours

Several runners can be active at once (`activate_runner` on each), but they all report on
`/behavior_status`, so `check_behavior_finished()` cannot tell which one finished. For concurrent
work, prefer one tree with a `Parallel` node, or `<include>` the reusable trees as SubTrees of it.

## Troubleshooting

### Behavior file not found
- Ensure behavior_architecture is built and sourced
- Check file paths are relative to package share directory
- Verify XML files were installed (check install/behavior_architecture/share/)

### BT nodes not found
- Verify plugin libraries are specified correctly
- Ensure dependent packages (like social_bt_nodes) are built
- Check that `plugin_libraries` lists every required plugin (e.g. `libsocial_bt_nodes_plugin.so`)

### Behavior doesn't work as expected
- Review the behavior's documentation in [behaviors/README.md](../behaviors/README.md)
- Check if required TF frames exist (`ros2 run tf2_ros tf2_echo`)
- Verify topic names match your robot's configuration
- Enable BT logging for detailed execution trace

## Further Reading

- [behaviors/README.md](../behaviors/README.md) - Complete catalogue documentation
- [README.md](../README.md) - Framework overview and mission configuration
- [CUSTOM_ORCHESTRATORS.md](CUSTOM_ORCHESTRATORS.md) - Creating custom orchestrators
