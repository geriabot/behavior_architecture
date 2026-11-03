#!/usr/bin/env python3

# Copyright 2025 Rodrigo Pérez-Rodríguez
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.actions import GroupAction
from launch.actions import IncludeLaunchDescription
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    """Launch the generic mission executor with a YAML configuration file."""
    
    # Get package directory
    pkg_dir = get_package_share_directory('behavior_architecture')
    
    # Default config file
    default_config_file = os.path.join(pkg_dir, 'config', 'restaurant_config.yaml')
    
    # Declare launch arguments
    config_file_arg = DeclareLaunchArgument(
        'config_file',
        default_value=default_config_file,
        description='Path to the YAML configuration file for the mission executor'
    )
    
    # The namespace of every node of mission_executor (the orchestrator, the runners and the
    # node of the BT nodes); /behavior_status is absolute and is not affected
    namespace_arg = DeclareLaunchArgument(
        'namespace',
        default_value='',
        description='Namespace of the nodes of mission_executor'
    )

    # Optional: an interpretation server (launch/interpretation.launch.py), off by default
    interpretation_arg = DeclareLaunchArgument(
        'interpretation',
        default_value='false',
        description='Also launch an interpretation server (interpretation.launch.py)'
    )
    interpretation_params_arg = DeclareLaunchArgument(
        'interpretation_params_file',
        default_value=os.path.join(pkg_dir, 'config', 'interpretation', 'example_params.yaml'),
        description='ROS parameters file of the interpretation server'
    )
    # By default the server runs in the namespace of mission_executor, where the client and the
    # BT nodes resolve the relative service name interpretation/interpret
    interpretation_namespace_arg = DeclareLaunchArgument(
        'interpretation_namespace',
        default_value=LaunchConfiguration('namespace'),
        description='Namespace of the interpretation server (default: namespace)'
    )
    # In a scope of its own: the arguments of the included file (its namespace) must not
    # replace the configurations of this one
    interpretation_server = GroupAction(
        scoped=True,
        condition=IfCondition(LaunchConfiguration('interpretation')),
        actions=[IncludeLaunchDescription(
            PythonLaunchDescriptionSource(
                os.path.join(pkg_dir, 'launch', 'interpretation.launch.py')),
            launch_arguments={
                'params_file': LaunchConfiguration('interpretation_params_file'),
                'namespace': LaunchConfiguration('interpretation_namespace'),
            }.items()
        )]
    )

    # Mission executor node
    mission_executor_node = Node(
        package='behavior_architecture',
        executable='mission_executor',
        namespace=LaunchConfiguration('namespace'),
        output='screen',
        emulate_tty=True,
        arguments=[LaunchConfiguration('config_file')]
    )

    return LaunchDescription([
        config_file_arg,
        namespace_arg,
        interpretation_arg,
        interpretation_params_arg,
        interpretation_namespace_arg,
        interpretation_server,
        mission_executor_node
    ])
