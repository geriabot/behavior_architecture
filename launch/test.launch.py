import os
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def launch_setup(context, *args, **kwargs):
    plan_dir = LaunchConfiguration('plan_dir').perform(context)
    # If the path is relative, resolve it from the current working directory (launch directory)
    if not os.path.isabs(plan_dir):
        plan_dir = os.path.abspath(plan_dir)

    nodes = [
        Node(
            package='behavior_architecture',
            executable='test_plan_executor',
            name='test_plan_executor',
            output='screen',
            emulate_tty=True,
            arguments=[plan_dir],
        )
    ]
    return nodes

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            'plan_dir',
            default_value='exec/test_plan',
            description='Relative path (from workspace root) to the base directory containing BT XMLs to execute.',
        ),
        OpaqueFunction(function=launch_setup),
    ])
