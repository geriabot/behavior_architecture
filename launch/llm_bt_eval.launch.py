#!/usr/bin/env python3

import os

from ament_index_python.packages import get_package_share_directory
from ament_index_python.packages import PackageNotFoundError
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def launch_setup(context, *args, **kwargs):
    capabilities_yaml = LaunchConfiguration('capabilities_yaml').perform(context)
    blackboard_seed = LaunchConfiguration('blackboard_seed').perform(context)
    useful_info = LaunchConfiguration('useful_info').perform(context)
    task_name = LaunchConfiguration('task_name').perform(context)
    tf_file = LaunchConfiguration('tf_file').perform(context)
    publish_static_tf = LaunchConfiguration('publish_static_tf').perform(context).lower() == 'true'
    save_exec = LaunchConfiguration('save_exec').perform(context).lower() == 'true'
    strict_inputs = LaunchConfiguration('strict_objective_inputs').perform(context).lower() == 'true'
    use_episodic_mem = LaunchConfiguration('use_episodic_memory').perform(context).lower() == 'true'

    executor_arguments = [
        '--objective', LaunchConfiguration('objective_file'),
        '--bt-nodes-package', LaunchConfiguration('bt_nodes_package'),
        '--plugin-library', LaunchConfiguration('plugin_library'),
        '--exec-dir', LaunchConfiguration('exec_dir'),
        '--timeout-sec', LaunchConfiguration('timeout_sec'),
        '--control-period-ms', LaunchConfiguration('control_period_ms'),
        '--max-fixes', LaunchConfiguration('max_fixes'),
    ]

    if capabilities_yaml:
        executor_arguments.extend([
            '--capabilities', LaunchConfiguration('capabilities_yaml'),
        ])

    if blackboard_seed:
        executor_arguments.extend([
            '--blackboard-seed', LaunchConfiguration('blackboard_seed'),
        ])

    if useful_info:
        executor_arguments.extend([
            '--useful-info', LaunchConfiguration('useful_info'),
        ])

    if task_name:
        executor_arguments.extend([
            '--task-name', LaunchConfiguration('task_name'),
        ])

    if strict_inputs:
        executor_arguments.append('--strict-objective-inputs')

    if not save_exec:
        executor_arguments.append('--no-save-exec')

    if use_episodic_mem:
        executor_arguments.append('--use-episodic-memory')

    nodes = []

    if publish_static_tf and tf_file:
        nodes.append(Node(
            package='behavior_architecture',
            executable='static_tf_publisher',
            name='static_tf_publisher',
            output='screen',
            emulate_tty=True,
            parameters=[{
                'tf_file': tf_file,
            }],
        ))

    nodes.append(Node(
        package='behavior_architecture',
        executable='llm_bt_executor',
        name='llm_bt_executor',
        output='screen',
        emulate_tty=True,
        arguments=executor_arguments,
    ))

    return nodes


def generate_launch_description():
    objectives_pkg = get_package_share_directory('llm_bt_builder')
    capabilities_pkg = get_package_share_directory('dummy_bt_nodes')

    default_objective = os.path.join(objectives_pkg, 'objectives/paper', 'follow_patient.yaml')
    default_exec_dir = os.path.join(os.getcwd(), 'exec', 'bt_generation_eval')
    default_capabilities = os.path.join(capabilities_pkg, 'node_descriptions', 'dummy_bt_nodes_extra.yaml')
    default_blackboard_seed = os.path.join(objectives_pkg, 'objectives/paper', 'blackboard_seed.yaml')
    default_task_name = os.path.splitext(os.path.basename(default_objective))[0]
    default_tf_file = ''

    try:
        locations_pkg = get_package_share_directory('dummy_robot')
        default_tf_file = os.path.join(locations_pkg, 'config', 'static_transforms.yaml')
    except PackageNotFoundError:
        pass


    objective_arg = DeclareLaunchArgument(
        'objective_file',
        default_value=default_objective,
        description='Objective YAML used as the only input for BT generation evaluation.',
    )
    capabilities_arg = DeclareLaunchArgument(
        'capabilities_yaml',
        default_value=default_capabilities,
        description='Optional capabilities YAML with bt_nodes structure (see *_bt_nodes.yaml). If empty, bt_nodes_package will be used.',
    )
    bt_nodes_package_arg = DeclareLaunchArgument(
        'bt_nodes_package',
        default_value='dummy_bt_nodes',
        description='Package used to auto-load node_descriptions when capabilities_yaml is empty.',
    )
    plugin_library_arg = DeclareLaunchArgument(
        'plugin_library',
        default_value='libdummy_bt_nodes_plugin.so',
        description='BehaviorTree plugin library used during execution.',
    )
    exec_dir_arg = DeclareLaunchArgument(
        'exec_dir',
        default_value=default_exec_dir,
        description='Base directory where metrics and XML artifacts will be stored.',
    )
    timeout_arg = DeclareLaunchArgument(
        'timeout_sec',
        default_value='60',
        description='Execution timeout for one BT attempt.',
    )
    control_period_arg = DeclareLaunchArgument(
        'control_period_ms',
        default_value='50',
        description='Control loop period for the BT executor.',
    )
    max_fixes_arg = DeclareLaunchArgument(
        'max_fixes',
        default_value='3',
        description='Maximum number of FixBT attempts before failing the run.',
    )
    blackboard_seed_arg = DeclareLaunchArgument(
        'blackboard_seed',
        default_value=default_blackboard_seed,
        description='Optional YAML file with initial blackboard values for isolated step evaluation.',
    )
    useful_info_arg = DeclareLaunchArgument(
        'useful_info',
        default_value='',
        description='Optional useful_info text to append into objective prompt (same path used in runtime start_mission context).',
    )
    task_name_arg = DeclareLaunchArgument(
        'task_name',
        default_value=default_task_name,
        description='Name used for run folders/metrics, mirroring runtime mission naming.',
    )
    tf_file_arg = DeclareLaunchArgument(
        'tf_file',
        default_value=default_tf_file,
        description='YAML file with static transforms consumed by static_tf_publisher.',
    )
    publish_static_tf_arg = DeclareLaunchArgument(
        'publish_static_tf',
        default_value='true',
        description='Whether to launch static_tf_publisher before BT evaluation.',
    )
    strict_objective_inputs_arg = DeclareLaunchArgument(
        'strict_objective_inputs',
        default_value='true',
        description='Fail early when objective.inputs keys are missing from blackboard.',
    )
    save_exec_arg = DeclareLaunchArgument(
        'save_exec',
        default_value='true',
        description='Whether to persist XML artifacts and CSV metrics.',
    )
    use_episodic_memory_arg = DeclareLaunchArgument(
        'use_episodic_memory',
        default_value='false',
        description='Whether to store and use episodic memory for BT success/failure cases.',
    )

    return LaunchDescription([
        objective_arg,
        capabilities_arg,
        bt_nodes_package_arg,
        plugin_library_arg,
        exec_dir_arg,
        timeout_arg,
        control_period_arg,
        max_fixes_arg,
        blackboard_seed_arg,
        useful_info_arg,
        task_name_arg,
        tf_file_arg,
        publish_static_tf_arg,
        strict_objective_inputs_arg,
        save_exec_arg,
        use_episodic_memory_arg,
        OpaqueFunction(function=launch_setup),
    ])