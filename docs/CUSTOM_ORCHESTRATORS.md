# An orchestrator in your own package

How to run your robot's state machine with `mission_executor` without touching
behavior_architecture: the orchestrator and the BT nodes are shared libraries of your package, and a
YAML file ties them to the trees. The base layer of behavior_architecture is enough
([README](../README.md#installation)).

Complete packages built this way:
[dummy_robot](https://github.com/geriabot/dummy_robot) (a small one) and
[interaction_system](https://github.com/geriabot/interaction_system)
(`interaction_system_core/interaction_system`).

```
my_robot/
├── CMakeLists.txt
├── package.xml                     <depend>behavior_architecture</depend>, behaviortree_cpp...
├── include/my_robot/my_orchestrator.hpp
├── src/my_orchestrator.cpp         the state machine + OrchestratorRegistrar
├── src/bt_nodes/...                your BT nodes + BT_REGISTER_NODES (optional)
├── behaviors/*.xml                 the trees
├── config/my_mission.yaml
└── launch/my_mission.launch.py
```

## 1. The orchestrator

Derive from `behavior_architecture::BaseOrchestrator`, implement `control_cycle()` and
`go_to_state(int)`, and register the class at file scope with a type name:

```cpp
// src/my_orchestrator.cpp
#include "my_robot/my_orchestrator.hpp"
#include "behavior_architecture/orchestrator_factory.hpp"

namespace my_robot
{

MyOrchestrator::MyOrchestrator(BT::Blackboard::Ptr blackboard)
: BaseOrchestrator("my_orchestrator", blackboard)
{
}

void MyOrchestrator::control_cycle()
{
  switch (state_) {
    case State::INIT:
      go_to_state(static_cast<int>(State::GREET));
      break;
    case State::GREET:
      if (check_behavior_finished()) {
        go_to_state(static_cast<int>(
          last_status_ == "SUCCESS" ? State::FOLLOW : State::DONE));
      }
      break;
    case State::FOLLOW:
      if (check_behavior_finished()) {
        go_to_state(static_cast<int>(State::DONE));
      }
      break;
    case State::DONE:
      break;
  }
}

void MyOrchestrator::go_to_state(int state)
{
  state_ = static_cast<State>(state);
  deactivate_all_runners();
  if (state_ == State::GREET) {activate_runner("greet");}
  if (state_ == State::FOLLOW) {activate_runner("follow");}
}

// The type "my_orchestrator" exists once mission_executor loads this library
static behavior_architecture::OrchestratorRegistrar<MyOrchestrator> registrar("my_orchestrator");

}  // namespace my_robot
```

What the base class provides:

- The runner names are the `name` fields of the YAML `behaviors`; `on_configure` creates and
  configures one runner per entry.
- `check_behavior_finished()` is `true` once when the active runner reports `SUCCESS` or `FAILURE`
  on `/behavior_status`; `last_status_` holds which. Keep one runner active at a time, since every
  runner publishes on that topic.
- `control_cycle()` runs every `control_cycle_rate_ms` (ROS parameter of the orchestrator node,
  default 100).
- The blackboard (`blackboard_`) is shared with every tree; `blackboard_->get("node", node)` gives
  the `rclcpp::Node` that `mission_executor` created, for subscriptions or clients of your own.
- An orchestrator may override `on_configure`, `on_activate` or `on_deactivate`; it then calls the
  base implementation, which creates the runners and starts and stops the control cycle.

## 2. The BT nodes (if you have your own)

```cpp
// src/bt_nodes/bt_plugins.cpp
#include "behaviortree_cpp/bt_factory.h"
#include "my_robot/bt_nodes/greet.hpp"

BT_REGISTER_NODES(factory)
{
  factory.registerNodeType<my_robot::Greet>("Greet");
}
```

A node that needs ROS takes the node from the blackboard:
`config().blackboard->get<rclcpp::Node::SharedPtr>("node")`.

## 3. CMakeLists.txt

```cmake
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(behavior_architecture REQUIRED)

# The orchestrator, loaded by mission_executor from orchestrator_libraries
add_library(${PROJECT_NAME}_orchestrator SHARED src/my_orchestrator.cpp)
target_include_directories(${PROJECT_NAME}_orchestrator PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${behavior_architecture_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}_orchestrator
  ${behavior_architecture_LIBRARIES}
  rclcpp::rclcpp
  rclcpp_lifecycle::rclcpp_lifecycle
  behaviortree_cpp::behaviortree_cpp)

# The BT nodes, loaded by every runner from plugin_libraries
add_library(${PROJECT_NAME}_bt_nodes SHARED src/bt_nodes/bt_plugins.cpp src/bt_nodes/greet.cpp)
target_include_directories(${PROJECT_NAME}_bt_nodes PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(${PROJECT_NAME}_bt_nodes
  rclcpp::rclcpp
  behaviortree_cpp::behaviortree_cpp)
target_compile_definitions(${PROJECT_NAME}_bt_nodes PRIVATE BT_PLUGIN_EXPORT)

install(TARGETS ${PROJECT_NAME}_orchestrator ${PROJECT_NAME}_bt_nodes
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(DIRECTORY behaviors config launch DESTINATION share/${PROJECT_NAME})

ament_package()
```

Both libraries go to `lib/` of the install space: `mission_executor` and the runners load them by
file name, which a sourced workspace puts on `LD_LIBRARY_PATH`. The form above (linking targets,
not `ament_target_dependencies`) builds on both Jazzy and Lyrical.

## 4. The configuration

```yaml
# config/my_mission.yaml
node_name: "my_robot_bt_node"
orchestrator_type: "my_orchestrator"
package_name: "my_robot"
orchestrator_libraries:
  - "libmy_robot_orchestrator.so"
plugin_libraries:
  - "libmy_robot_bt_nodes.so"
behaviors:
  - name: "greet"
    behavior_file: "behaviors/greet.xml"
    control_period_ms: 50
  - name: "follow"                                   # a tree of behavior_architecture
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"
```

The keys are described in the [README](../README.md#mission-configuration). `follow_behavior.xml`
needs the nodes of social_bt_nodes (`libsocial_bt_nodes_plugin.so` in `plugin_libraries`).

## 5. The launch file

```python
# launch/my_mission.launch.py
from launch import LaunchDescription
from launch.substitutions import PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    config = PathJoinSubstitution([FindPackageShare('my_robot'), 'config', 'my_mission.yaml'])
    return LaunchDescription([
        Node(package='behavior_architecture', executable='mission_executor',
             arguments=[config], output='screen', emulate_tty=True),
    ])
```

## 6. Build and run

```bash
colcon build --packages-up-to my_robot
source install/setup.bash
ros2 launch my_robot my_mission.launch.py
```

The log shows `Loaded orchestrator library: libmy_robot_orchestrator.so`,
`Registered orchestrator type: my_orchestrator` and one `BehaviorRunner constructor` per behaviour.
`ros2 run behavior_architecture mission_executor` without arguments does not list your type: it
lists only the types compiled into the executable, and yours is registered when the configuration
loads its library.

## Troubleshooting

**`Failed to create orchestrator. Available types: ...`**: `orchestrator_type` does not match the
name given to `OrchestratorRegistrar`, or the library that contains it is missing from
`orchestrator_libraries`. Keep the registrar at file scope in a `.cpp` that is part of the library.

**`Failed to load orchestrator library`**: the library is not installed to `lib/`, the workspace is
not sourced, or the library has undefined symbols (the `dlerror()` message says which). Check with
`ldd -r install/my_robot/lib/libmy_robot_orchestrator.so`.

**A BT node is not recognised when the tree is built**: the plugin library is missing from
`plugin_libraries`, it was built without `BT_PLUGIN_EXPORT`, or it does not register the node with
`BT_REGISTER_NODES`.

**A behaviour file is not found**: relative paths are resolved against the share directory of the
behaviour's `package_name` (or the global one): check that `install(DIRECTORY behaviors ...)` puts
the file there.

**The orchestrator never sees a behaviour finish**: the runner name in `activate_runner()` must be a
`name` of the YAML (a warning `Runner '...' not registered` says otherwise), and only one runner
should be active at a time.
