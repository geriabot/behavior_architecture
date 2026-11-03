# Behavior trees shipped with the package

The trees of this directory are installed to `share/behavior_architecture/behaviors/`. They are
BehaviorTree.CPP v4 files (`BTCPP_format="4"`).

```
behaviors/
├── reusable/              trees meant to be used by other packages
│   └── follow_behavior.xml
└── examples/              trees of the example configurations
    ├── collect_order.xml
    ├── state1.xml
    └── state2.xml
```

[docs/USING_REUSABLE_BEHAVIORS.md](../docs/USING_REUSABLE_BEHAVIORS.md) explains how to use them
from another package, and [docs/ADDING_REUSABLE_BEHAVIORS.md](../docs/ADDING_REUSABLE_BEHAVIORS.md)
how to add one to `reusable/`.

## reusable/follow_behavior.xml

Tree `FollowBehavior`: follows a target while avoiding obstacles, and spins to search for it when
it is lost.

```
ReactiveFallback
├── ReactiveSequence
│   ├── IsTargetDetected   target_frame=target, base_frame=base_link, timeout=3.0
│   └── Follow             target_frame=target, base_frame=base_link, min_distance=0.5,
│                          avoidance_distance=0.2, max_linear_speed=1.0, max_angular_speed=0.5,
│                          succeed_on_reach=true, cmd_vel_topic=/target,
│                          sonar_topic=/sensors/sonar, touch_topic=/sensors/touch
└── SpinSearch             angular_speed=1.0, cmd_vel_topic=/target
```

| | |
|---|---|
| BT nodes | `IsTargetDetected`, `Follow`, `SpinSearch` from social_bt_nodes (`examples.repos`); plugin library `libsocial_bt_nodes_plugin.so` |
| Needs | the TF frames `target` and `base_link`; the topics `/sensors/sonar` and `/sensors/touch` |
| Publishes | velocity commands on `/target` |
| Result | `SUCCESS` when the target is reached (`succeed_on_reach`) |

The values are written in the XML; they are not ports of the tree. To change them, copy the file
into your package and edit it.

## examples/collect_order.xml

Tree `CollectOrder`, used by the `restaurant` example: asks a customer, in Spanish, whether they
are ready to order, listens, checks the yes/no answer and, if yes, asks for the order, extracts
the drink and the meal, repeats the order and thanks the customer. Its nodes are `Speak`,
`Listen`, `AskYesNoQuestion`, `Extract` and `SpeakEnum` from social_bt_nodes, which call the
services `/tts_service`, `/stt_service`, `/yesno_service` and `/extract_service`, and the
built-in `Switch2`, `Script` and `AlwaysFailure`.

## examples/state1.xml, examples/state2.xml

Trees `State1` and `State2`, used by the `simple` example: each says one sentence (`Estado 1`,
`Estado 2`) with the node `Speak` of social_bt_nodes, through `/tts_service`.
