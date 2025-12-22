# Quick Start: Adding a New Reusable Behavior

This guide walks you through creating a new reusable behavior for the catalogue.

## Step 1: Create Your Behavior XML

Start in the `examples/` directory for development:

```bash
cd src/behavior_architecture/behaviors/examples/
cp follow_behavior.xml ../reusable/follow_behavior.xml  # Just as reference
```

Create your new behavior file:

```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <!-- 
    Behavior Name: Navigate To Goal
    
    Purpose: 
    Navigate the robot to a specified goal location using the navigation stack
    
    Use Case:
    When you need autonomous navigation to named locations or coordinates
    
    Required Plugins:
    - social_bt_nodes_plugin (for navigation nodes)
    
    Parameters:
    - goal_name (string, required): Name of the goal location
    - timeout (double, default: 60.0): Maximum time to reach goal in seconds
    - recovery_enabled (bool, default: true): Enable recovery behaviors on failure
    
    Example Usage:
    <SubTree ID="NavigateToGoal" goal_name="kitchen" timeout="30.0"/>
  -->
  
  <BehaviorTree ID="NavigateToGoal">
    <Sequence>
      <!-- Your behavior logic here -->
      <NavigateToPose goal="{goal_name}" timeout="{timeout}"/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

## Step 2: Test Your Behavior

Create a test YAML configuration:

```yaml
# config/test_navigate_config.yaml
orchestrator:
  type: "simple"
  control_period_ms: 100

behaviors:
  - name: "navigate_runner"
    behavior_file: "behaviors/examples/navigate_to_goal.xml"
    control_period_ms: 100

plugin_libs:
  - "social_bt_nodes_plugin"

states:
  NAVIGATE:
    behavior_runner: "navigate_runner"
    transitions:
      SUCCESS: "IDLE"
      FAILURE: "ERROR"
```

Test it:

```bash
cd ~/social_nao_ws
colcon build --packages-select behavior_architecture
source install/setup.bash

ros2 launch behavior_architecture action_executor.launch.py \
  config_file:=config/test_navigate_config.yaml
```

## Step 3: Refine and Parameterize

Ensure your behavior is:

- ✅ **Generic**: Works in multiple contexts, not just your specific robot
- ✅ **Parameterized**: All values that might change are BT parameters
- ✅ **Robust**: Handles errors gracefully (use Fallback nodes)
- ✅ **Documented**: Clear comments explaining everything

Example of good parameterization:

```xml
<BehaviorTree ID="PickAndPlace">
  <Sequence>
    <!-- All parameters exposed, not hardcoded -->
    <MoveToObject object_id="{target_object}" approach_distance="{approach_dist}"/>
    <Grasp object_id="{target_object}" force="{grasp_force}"/>
    <MoveToLocation location="{drop_location}"/>
    <Release/>
  </Sequence>
</BehaviorTree>
```

## Step 4: Move to Reusable

Once tested and refined:

```bash
cd behaviors/
mv examples/navigate_to_goal.xml reusable/navigate_to_goal.xml
```

## Step 5: Update Documentation

### Add to [behaviors/README.md](behaviors/README.md)

Add an entry in the "Available Reusable Behaviors" section:

```markdown
#### `navigate_to_goal.xml`
**Purpose**: Navigate robot to a named goal location

**Description**: Uses the navigation stack to autonomously navigate to a 
specified goal location with built-in recovery behaviors.

**Key Features**:
- Autonomous navigation to named locations
- Configurable timeout and recovery
- Status reporting on navigation progress
- Graceful failure handling

**Required BT Nodes**:
- `NavigateToPose`: Navigate to a named pose

**Parameters** (configured in XML):
- `goal_name` (string, required): Name of goal location
- `timeout` (double, 60.0): Maximum navigation time in seconds
- `recovery_enabled` (bool, true): Enable recovery behaviors

**Usage Example**:
\```xml
<SubTree ID="NavigateToGoal" 
         goal_name="kitchen" 
         timeout="30.0"/>
\```

**Integration**:
1. Ensure navigation stack is running
2. Load behavior in your runner:
   \```yaml
   - name: "nav_runner"
     behavior_file: "behaviors/reusable/navigate_to_goal.xml"
   \```
```

## Step 6: Update Config Files (if used in examples)

Update any config files that reference your behavior:

```yaml
# config/restaurant_config.yaml
behaviors:
  - name: "navigate_runner"
    behavior_file: "behaviors/reusable/navigate_to_goal.xml"  # Updated path
    control_period_ms: 100
```

## Step 7: Commit Your Changes

```bash
git add behaviors/reusable/navigate_to_goal.xml
git add behaviors/README.md
git add config/  # If you updated configs
git commit -m "Add navigate_to_goal reusable behavior

- Generic navigation behavior with parameterization
- Includes error handling and recovery
- Fully documented with usage examples"
```

## Checklist for Reusable Behaviors

Before moving a behavior to `reusable/`, verify:

- [ ] **Tested**: Works reliably in multiple scenarios
- [ ] **Parameterized**: No hardcoded values, all configs are parameters
- [ ] **Documented**: Complete header comment with all sections
- [ ] **Generic**: Not specific to one robot or use case
- [ ] **Error Handling**: Uses Fallback or recovery behaviors
- [ ] **Dependencies Clear**: All required plugins listed
- [ ] **Examples Provided**: Usage examples in documentation
- [ ] **Named Well**: Descriptive, action-oriented filename
- [ ] **Catalogue Updated**: Entry added to behaviors/README.md

## Common Patterns

### Pattern 1: Reactive Behavior (Always Monitoring)

```xml
<ReactiveFallback>
  <ReactiveSequence>
    <IsConditionMet condition="{condition}"/>
    <DoAction action="{action}"/>
  </ReactiveSequence>
  <RecoveryAction/>
</ReactiveFallback>
```

### Pattern 2: Sequential Task

```xml
<Sequence>
  <Setup parameters="{params}"/>
  <Execute task="{task}"/>
  <Verify result="{result}"/>
  <Cleanup/>
</Sequence>
```

### Pattern 3: Retry with Limit

```xml
<RetryNode num_attempts="3">
  <Fallback>
    <ActionNode/>
    <RecoveryNode/>
  </Fallback>
</RetryNode>
```

### Pattern 4: Parallel Actions

```xml
<Parallel success_count="2" failure_count="1">
  <MonitorSafety/>
  <ExecuteTask/>
  <PublishStatus/>
</Parallel>
```

## Tips

1. **Start Simple**: Begin with a simple version, then add features
2. **Test Incrementally**: Test each addition before moving on
3. **Use SubTrees**: Break complex behaviors into smaller SubTrees
4. **Log Generously**: Add logging nodes for debugging
5. **Consider Edge Cases**: What happens if sensors fail? If the robot gets stuck?
6. **Get Feedback**: Have others review your behavior before adding to catalogue

## Need Help?

- Check existing reusable behaviors in `behaviors/reusable/` for examples
- Review [behaviors/README.md](behaviors/README.md) for detailed guidelines
- See [docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md) for usage patterns
- Look at BehaviorTree.CPP documentation: https://www.behaviortree.dev/
