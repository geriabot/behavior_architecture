# An orchestrator in your own package

This guide builds a package, `my_robot`, whose FSM and BT nodes run with `mission_executor`
without changes to behavior_architecture. The orchestrator and the BT nodes are shared libraries
of the package, and a YAML file ties them to the trees. Only the base of behavior_architecture is
needed ([README](../README.md#installation)). Every file below is complete; together they build
and run on Jazzy and on Lyrical.

Packages built this way: [dummy_robot](https://github.com/geriabot/dummy_robot) (a small one) and
[interaction_system](https://github.com/geriabot/interaction_system)
(`interaction_system_core/interaction_system`).

```
my_robot/
├── CMakeLists.txt
├── package.xml
├── include/my_robot/my_orchestrator.hpp
├── include/my_robot/bt_nodes/greet.hpp
├── src/my_orchestrator.cpp         the FSM and its OrchestratorRegistrar
├── src/bt_nodes/bt_plugins.cpp     BT_REGISTER_NODES
├── behaviors/greet.xml
├── behaviors/follow.xml
├── config/my_mission.yaml
└── launch/my_mission.launch.py
```

## 1. package.xml

```xml
<?xml version="1.0"?>
<package format="3">
  <name>my_robot</name>
  <version>0.1.0</version>
  <description>An orchestrator and BT nodes for behavior_architecture</description>
  <maintainer email="you@example.com">you</maintainer>
  <license>Apache-2.0</license>

  <buildtool_depend>ament_cmake</buildtool_depend>

  <depend>behavior_architecture</depend>
  <depend>behaviortree_cpp</depend>
  <depend>rclcpp</depend>
  <depend>rclcpp_lifecycle</depend>
  <depend>std_msgs</depend>
  <depend>ament_index_cpp</depend>

  <export>
    <build_type>ament_cmake</build_type>
  </export>
</package>
```

## 2. The orchestrator

```cpp
// include/my_robot/my_orchestrator.hpp
#ifndef MY_ROBOT__MY_ORCHESTRATOR_HPP_
#define MY_ROBOT__MY_ORCHESTRATOR_HPP_

#include "behavior_architecture/base_orchestrator.hpp"

namespace my_robot
{

class MyOrchestrator : public behavior_architecture::BaseOrchestrator
{
public:
  explicit MyOrchestrator(BT::Blackboard::Ptr blackboard);

protected:
  enum class State : int {INIT, GREET, FOLLOW, DONE};

  void control_cycle() override;
  void go_to_state(int state) override;

private:
  State state_ {State::INIT};
};

}  // namespace my_robot

#endif  // MY_ROBOT__MY_ORCHESTRATOR_HPP_
```

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
  RCLCPP_INFO(get_logger(), "State %d", state);
  deactivate_all_runners();
  if (state_ == State::GREET) {activate_runner("greet");}
  if (state_ == State::FOLLOW) {activate_runner("follow");}
}

// Registers the type "my_orchestrator" when mission_executor loads this library
static behavior_architecture::OrchestratorRegistrar<MyOrchestrator> registrar("my_orchestrator");

}  // namespace my_robot
```

What the base class provides:

- One runner per entry of the YAML `behaviors`, named after its `name`, created and configured in
  `on_configure()`.
- `check_behavior_finished()`, `true` once when the active runner reports `SUCCESS` or `FAILURE`
  on `/behavior_status`, with the result in `last_status_`. Every runner publishes on that topic,
  so one runner is active at a time.
- The call of `control_cycle()` every `control_cycle_rate_ms` (a parameter of the orchestrator
  node, default 100), from activation to deactivation.
- The blackboard (`blackboard_`), shared with every tree. `blackboard_->get("node", node)` gives
  the `rclcpp::Node` that `mission_executor` created, for subscriptions or clients of the
  orchestrator's own; the orchestrator is a lifecycle node too, and can create them itself.
- `on_configure()`, `on_activate()` and `on_deactivate()`, which an orchestrator may override; an
  override calls the base implementation, which creates the runners and starts and stops the
  control cycle.

The full API is in the [README](../README.md#baseorchestrator-api).

## 3. A BT node

```cpp
// include/my_robot/bt_nodes/greet.hpp
#ifndef MY_ROBOT__BT_NODES__GREET_HPP_
#define MY_ROBOT__BT_NODES__GREET_HPP_

#include <string>

#include "behaviortree_cpp/action_node.h"
#include "rclcpp/rclcpp.hpp"

namespace my_robot
{

class Greet : public BT::SyncActionNode
{
public:
  Greet(const std::string & name, const BT::NodeConfig & config)
  : BT::SyncActionNode(name, config)
  {
    node_ = this->config().blackboard->get<rclcpp::Node::SharedPtr>("node");
  }

  static BT::PortsList providedPorts()
  {
    return {BT::InputPort<std::string>("text", "Hello", "What to say")};
  }

  BT::NodeStatus tick() override
  {
    const auto text = getInput<std::string>("text");
    if (!text) {
      return BT::NodeStatus::FAILURE;
    }
    RCLCPP_INFO(node_->get_logger(), "Greet: %s", text.value().c_str());
    return BT::NodeStatus::SUCCESS;
  }

private:
  rclcpp::Node::SharedPtr node_;
};

}  // namespace my_robot

#endif  // MY_ROBOT__BT_NODES__GREET_HPP_
```

```cpp
// src/bt_nodes/bt_plugins.cpp
#include "behaviortree_cpp/bt_factory.h"
#include "my_robot/bt_nodes/greet.hpp"

BT_REGISTER_NODES(factory)
{
  factory.registerNodeType<my_robot::Greet>("Greet");
}
```

A node that talks to a capability creates its publishers, subscriptions or clients on the
blackboard's `node`; `mission_executor` spins that node. A node that waits for something returns
`RUNNING` (a `StatefulActionNode`) instead of blocking the tick.

## 4. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(my_robot)

set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(std_msgs REQUIRED)
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
  behaviortree_cpp::behaviortree_cpp
  ament_index_cpp::ament_index_cpp
  ${std_msgs_TARGETS})

# The BT nodes, loaded by every behavior runner from plugin_libraries
add_library(${PROJECT_NAME}_bt_nodes SHARED src/bt_nodes/bt_plugins.cpp)
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

Both libraries go to `lib/` of the install space. `mission_executor` looks for the orchestrator
library in `lib/` of the configuration's `package_name`, then by file name; the runners load the
BT plugin by file name through the dynamic loader, and a sourced workspace puts `lib/` on
`LD_LIBRARY_PATH`. The form above links targets instead of calling
`ament_target_dependencies()`, so it builds on both Jazzy and Lyrical. A package that also asks
interpretation questions adds `behavior_architecture::interpretation` to the orchestrator's
libraries ([INTERPRETATION.md](INTERPRETATION.md#linking)).

## 5. The trees

```xml
<!-- behaviors/greet.xml -->
<root BTCPP_format="4">
  <BehaviorTree ID="GreetBehavior">
    <Sequence>
      <Greet text="Hello, I am my_robot"/>
      <Sleep msec="1000"/>
    </Sequence>
  </BehaviorTree>
</root>
```

```xml
<!-- behaviors/follow.xml -->
<root BTCPP_format="4">
  <BehaviorTree ID="FollowBehavior">
    <Sleep msec="1000"/>
  </BehaviorTree>
</root>
```

A tree's `ID` must differ from the IDs of the nodes it uses: a tree `ID="Greet"` containing the
node `Greet` fails to build. `follow.xml` stands for a real behavior; the package's own
`behaviors/reusable/follow_behavior.xml` follows a person with the nodes of social_bt_nodes
([behaviors/README.md](../behaviors/README.md)).

## 6. The configuration

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
  - name: "follow"
    behavior_file: "behaviors/follow.xml"
```

The keys are described in the [README](../README.md#mission-configuration). To use a tree of
behavior_architecture instead, the entry sets `package_name: "behavior_architecture"`, and
`plugin_libraries` lists the libraries its nodes need.

## 7. The launch file

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

## 8. Build and run

```bash
cd ~/ws
colcon build --packages-up-to my_robot
source install/setup.bash
ros2 launch my_robot my_mission.launch.py
```

The log shows, in this order:

```
[orchestrator_factory]: Registered orchestrator type: my_orchestrator
[mission_executor]: Loaded orchestrator library: libmy_robot_orchestrator.so
[greet]: BehaviorRunner constructor (greet)
[follow]: BehaviorRunner constructor (follow)
[my_orchestrator]: State 1
[my_robot_bt_node]: Greet: Hello, I am my_robot
[greet]: Behavior tree (greet): SUCCESS
[my_orchestrator]: State 2
[follow]: Behavior tree (follow): SUCCESS
[my_orchestrator]: State 3
```

`ros2 run behavior_architecture mission_executor` without arguments does not list
`my_orchestrator`: it lists only the types compiled into the executable, and this one is
registered when the configuration loads its library.

## Troubleshooting

**`Failed to create orchestrator. Available types: ...`.** `orchestrator_type` does not match the
name given to `OrchestratorRegistrar`, or the library that contains it is missing from
`orchestrator_libraries`. The registrar must be at namespace scope in a `.cpp` file that is part
of the library.

**`Failed to load orchestrator library`.** The library is not installed to `lib/`, the workspace
is not sourced, or the library has undefined symbols (the `dlerror()` message names them):
`ldd -r install/my_robot/lib/libmy_robot_orchestrator.so`.

**`could not build the tree from ...` when a runner is activated.** The tree could not be built,
for the reason that the message gives: a node is not registered (the plugin library is missing
from `plugin_libraries`, was built without `BT_PLUGIN_EXPORT`, or does not register the node with
`BT_REGISTER_NODES`), the tree's `ID` equals a node ID (the message names it), or the file was not
found. The runner stays inactive, `activate_runner()` returns `false`, and
`check_behavior_finished()` reports the behavior as finished with `FAILURE`.

**A behavior file is not found.** Relative paths are resolved in the share directory of the
behavior's `package_name` (or the global one): `install(DIRECTORY behaviors ...)` must put the
file there.

**The orchestrator never sees a behavior finish.** The runner name in `activate_runner()` must be
a `name` of the YAML (otherwise the log shows `Runner '...' not registered`), and only one runner
should be active at a time.
