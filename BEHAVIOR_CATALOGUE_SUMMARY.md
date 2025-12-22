# Behavior Catalogue Restructuring - Summary

## Overview

The behavior_architecture package has been restructured to provide a clear catalogue of reusable complex behaviors that can be used in other packages.

## Changes Made

### 1. Directory Structure

**New Structure:**
```
behaviors/
├── reusable/              # Production-ready, reusable behaviors
│   └── follow_behavior.xml
├── examples/              # Example/demo behaviors
│   ├── collect_order.xml
│   ├── state1.xml
│   └── state2.xml
└── README.md              # Complete catalogue documentation
```

**Rationale:**
- `reusable/`: Contains well-tested, documented, parameterized behaviors ready for use in production
- `examples/`: Contains demo and test behaviors that may be specific to certain use cases

### 2. Files Reorganized

**Moved to reusable/**:
- `follow_behavior.xml` - Generic target following behavior with obstacle avoidance

**Moved to examples/**:
- `collect_order.xml` - Restaurant-specific order collection
- `state1.xml` - Simple test behavior
- `state2.xml` - Simple test behavior

### 3. Documentation Created

**[behaviors/README.md](behaviors/README.md)**:
- Complete catalogue documentation
- Description of each reusable behavior with:
  - Purpose and use case
  - Required BT node plugins
  - All parameters with defaults
  - Usage examples
  - Integration instructions
- Guidelines for creating new reusable behaviors
- File naming conventions
- Documentation templates

**[docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md)**:
- Quick reference guide
- Three usage methods (YAML, C++, SubTree inclusion)
- Common patterns
- Troubleshooting tips
- Code examples in C++, Python, YAML, and XML

### 4. Code Updates

**Config Files Updated:**
- `config/restaurant_config.yaml`: Updated paths for follow_behavior and collect_order
- `config/simple_config.yaml`: Updated paths for state1 and state2

**Source Files Updated:**
- `src/examples/simple_main.cpp`: Updated paths for state behaviors
- `src/examples/restaurant_main.cpp`: Updated paths for follow and collect_order behaviors

**Build System Updated:**
- `CMakeLists.txt`: Enhanced install directive to properly install XML and MD files from subdirectories

**Main Documentation Updated:**
- `README.md`: Added section highlighting the reusable behavior catalogue with links to detailed docs

## Benefits

1. **Clear Organization**: Developers can easily distinguish between production-ready reusable behaviors and examples

2. **Discoverability**: Comprehensive documentation makes it easy to find and understand available behaviors

3. **Reusability**: Well-documented, parameterized behaviors can be used across multiple packages without modification

4. **Maintainability**: Clear structure makes it easier to add new behaviors and maintain existing ones

5. **Best Practices**: Documentation provides templates and guidelines for creating high-quality behaviors

## Usage Example

### In Your Package's YAML Config:

```yaml
behaviors:
  # Use reusable behavior from behavior_architecture
  - name: "follow_runner"
    behavior_file: "behaviors/reusable/follow_behavior.xml"
    control_period_ms: 50
    
  # Your custom behavior
  - name: "custom_runner"
    behavior_file: "behaviors/my_custom.xml"
    control_period_ms: 100
```

### As a SubTree in Your Behavior XML:

```xml
<?xml version="1.0"?>
<root BTCPP_format="4">
  
  <include path="package://behavior_architecture/behaviors/reusable/follow_behavior.xml"/>
  
  <BehaviorTree ID="MyBehavior">
    <Sequence>
      <Speak text="Starting to follow"/>
      <SubTree ID="FollowBehavior" target_frame="person" min_distance="0.8"/>
      <Speak text="Following complete"/>
    </Sequence>
  </BehaviorTree>
  
</root>
```

## Next Steps

### For Users:
1. Review [behaviors/README.md](behaviors/README.md) for available reusable behaviors
2. Check [docs/USING_REUSABLE_BEHAVIORS.md](docs/USING_REUSABLE_BEHAVIORS.md) for integration examples
3. Start using reusable behaviors in your packages

### For Contributors:
1. Follow the guidelines in [behaviors/README.md](behaviors/README.md) when creating new behaviors
2. Start behaviors in `examples/` for testing
3. Move to `reusable/` when ready for production with full documentation
4. Update the catalogue documentation when adding new behaviors

## File Locations

All documentation is located in the behavior_architecture package:

- **Catalogue**: `behaviors/README.md`
- **Quick Reference**: `docs/USING_REUSABLE_BEHAVIORS.md`
- **Main README**: `README.md`
- **Reusable Behaviors**: `behaviors/reusable/`
- **Example Behaviors**: `behaviors/examples/`

## Building

After sourcing your workspace and ensuring all dependencies are available:

```bash
cd ~/social_nao_ws
colcon build --packages-select behavior_architecture
source install/setup.bash
```

The behaviors will be installed to:
```
install/behavior_architecture/share/behavior_architecture/behaviors/
├── reusable/
├── examples/
└── README.md
```
