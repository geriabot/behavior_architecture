# Copyright 2026 Rodrigo Pérez-Rodríguez
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

"""
Launch an interpretation server (one per system, in the system's namespace).

ros2 launch behavior_architecture interpretation.launch.py namespace:=my_system \
    params_file:=/path/to/interpretation_params.yaml

params_file is a ROS parameters file for the node `interpretation_server` (its questions_file,
templates_file, question_modules, routes, variants, interpreter_overrides, route_overrides
and log_dir); the arguments questions_file, templates_file and log_dir, when given, take
precedence over it.
Without arguments, the example of config/interpretation/ (a fake model) is launched.
"""

import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.actions import OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def _server(context):
    value = {name: LaunchConfiguration(name).perform(context)
             for name in ('namespace', 'params_file', 'questions_file', 'templates_file',
                          'log_dir')}
    parameters = []
    if value['params_file']:
        parameters.append(value['params_file'])
    overrides = {name: value[name] for name in ('questions_file', 'templates_file', 'log_dir')
                 if value[name]}
    if overrides:
        parameters.append(overrides)
    return [Node(
        package='behavior_architecture',
        executable='interpretation_server',
        name='interpretation_server',
        namespace=value['namespace'],
        output='screen',
        emulate_tty=True,
        parameters=parameters,
    )]


def generate_launch_description():
    example = os.path.join(get_package_share_directory('behavior_architecture'), 'config',
                           'interpretation', 'example_params.yaml')
    return LaunchDescription([
        DeclareLaunchArgument(
            'namespace', default_value='',
            description='Namespace of the server (the system that uses it)'),
        DeclareLaunchArgument(
            'params_file', default_value=example,
            description='ROS parameters file of the interpretation_server node'),
        DeclareLaunchArgument(
            'questions_file', default_value='', description='Questions (YAML); overrides'),
        DeclareLaunchArgument(
            'templates_file', default_value='', description='Prompt templates (YAML); overrides'),
        DeclareLaunchArgument(
            'log_dir', default_value='', description='Directory of the call log; overrides'),
        OpaqueFunction(function=_server),
    ])
