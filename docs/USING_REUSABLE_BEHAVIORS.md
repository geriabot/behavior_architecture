# Using the shipped trees from another package

The trees of `behaviors/reusable/` ([catalogue](../behaviors/README.md)) can be used by any
package in three ways. In all of them, the BT nodes of the tree must be available: the plugin
libraries that provide them are listed in the mission's `plugin_libraries`, and the packages that
build them are dependencies of the package.

## As a behavior of a mission

An entry of `behaviors` names the package the file comes from:

```yaml
package_name: "my_robot"                  # default package of the behavior files
plugin_libraries:
  - "libsocial_bt_nodes_plugin.so"        # the nodes of follow_behavior.xml
  - "libmy_robot_bt_nodes.so"
behaviors:
  - name: "greet"
    behavior_file: "behaviors/greet.xml"  # from my_robot
  - name: "follow"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    package_name: "behavior_architecture"
```

The orchestrator runs it with `activate_runner("follow")`, as any other runner.
[example_dummy_robot_with_follow.yaml](example_dummy_robot_with_follow.yaml) is a complete
configuration of this kind for [dummy_robot](https://github.com/geriabot/dummy_robot).

## As a subtree

A tree of the package includes the file and uses its tree as a subtree:

```xml
<root BTCPP_format="4">
  <include ros_pkg="behavior_architecture" path="behaviors/reusable/follow_behavior.xml"/>

  <BehaviorTree ID="GreetAndFollow">
    <Sequence>
      <Greet text="I will follow you"/>
      <SubTree ID="FollowBehavior"/>
    </Sequence>
  </BehaviorTree>
</root>
```

The ROS 2 build of BehaviorTree.CPP resolves `ros_pkg` in the package's share directory. The
values of `FollowBehavior` are fixed in its file; the subtree takes no ports.

## As a runner created in code

An orchestrator can create a runner itself and register it before it is configured:

```cpp
#include "behavior_architecture/behavior_runner.hpp"

// In the orchestrator's constructor
auto runner = std::make_shared<behavior_architecture::BehaviorRunner>(
  blackboard_, "follow", "behaviors/reusable/follow_behavior.xml",
  std::vector<std::string>{"libsocial_bt_nodes_plugin.so"},
  "behavior_architecture",   // package of the XML file
  50);                       // tick period in ms
register_runner("follow", runner);
```

When any runner is registered this way, `on_configure()` creates none from the YAML
`behaviors`: the orchestrator then creates all of its runners in code. The runners must also be
spun; `mission_executor` adds every runner returned by `get_runners()` to its executor.

## Dependencies

```xml
<!-- package.xml -->
<exec_depend>behavior_architecture</exec_depend>
<exec_depend>social_bt_nodes</exec_depend>   <!-- the nodes of follow_behavior.xml -->
```

The trees are data: using them needs no `find_package()` and no linking. Only an orchestrator or
a BT node that includes headers of behavior_architecture needs `<depend>` and `find_package()`.

## Troubleshooting

**The file is not found.** behavior_architecture is not built or not sourced, or the entry lacks
`package_name: "behavior_architecture"` (the path is then resolved in the global package).
`ls $(ros2 pkg prefix behavior_architecture)/share/behavior_architecture/behaviors/reusable`
lists the installed trees.

**A node of the tree is not recognised.** The plugin library that provides it is missing from
`plugin_libraries`, or its package is not built. The runner logs
`Caught exception in callback for transition 13` and does not activate.

**The behavior does not act as expected.** The TF frames and topics written in the file must
match the robot's (`ros2 run tf2_ros tf2_echo base_link target`, `ros2 topic list`); copy the file
and edit them otherwise.
