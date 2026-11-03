# Adding a tree to the catalogue

A tree belongs in `behaviors/reusable/` when other packages can use it as it is: it depends only
on BT nodes of published packages, it does not assume a particular robot beyond what it
documents, and its failures are handled. Trees for one application belong in that application's
package.

## Conventions

- File name: what the behavior does, in snake case (`follow_behavior.xml`,
  `navigate_to_goal.xml`).
- Tree `ID`: the same in CamelCase (`FollowBehavior`), different from every node ID.
- Format: `BTCPP_format="4"`.
- Values that a user may need to change are input ports of the subtree, remapped from the
  including tree, rather than constants in the XML, when the tree is meant to be included as a
  subtree.
- Failure paths: `Fallback`, `ReactiveFallback`, `RetryUntilSuccessful` or a recovery branch, so
  that the tree ends with `FAILURE` only when the behavior cannot be achieved.

## Header comment

Each file starts with a comment that a user can read without the code of its nodes:

```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  <!--
    NavigateToGoal: drive to a named location.

    Plugin libraries: libmy_nav_bt_nodes.so (package my_nav_bt_nodes)
    Needs:            the navigation stack's action /navigate_to_pose
    Ports:            goal_name (string, required); timeout (double, default 60.0, seconds)
    Result:           SUCCESS on arrival; FAILURE after the timeout or if the goal is unknown
    Usage:            <SubTree ID="NavigateToGoal" goal_name="kitchen" timeout="30.0"/>
  -->
  <BehaviorTree ID="NavigateToGoal">
    ...
  </BehaviorTree>
</root>
```

## Testing it

The `simple` example orchestrator runs `state1_runner`, retrying it while it fails, and then
`state2_runner`. A test configuration gives the new tree the first name:

```yaml
# /tmp/test_tree.yaml
orchestrator_type: "simple"
package_name: "behavior_architecture"
plugin_libraries:
  - "libmy_nav_bt_nodes.so"
behaviors:
  - name: "state1_runner"
    behavior_file: "behaviors/reusable/navigate_to_goal.xml"
  - name: "state2_runner"
    behavior_file: "/tmp/done.xml"   # any tree, e.g. <Sleep msec="100"/>
```

```bash
colcon build --packages-select behavior_architecture
source install/setup.bash
ros2 run behavior_architecture mission_executor /tmp/test_tree.yaml
```

The tree is tested in the situations it documents, including the failures: a missing target, a
lost connection, a timeout.

## Registering it

1. Add the file to `behaviors/reusable/`; `CMakeLists.txt` installs every `.xml` of `behaviors/`.
2. Add its entry to [behaviors/README.md](../behaviors/README.md): what it does, its structure,
   its BT nodes and plugin libraries, what it needs, its ports and its result.
3. If it needs BT nodes of a new package, add that package to `examples.repos`.
