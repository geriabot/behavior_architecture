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

import yaml
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction, TimerAction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def launch_setup(context, *args, **kwargs):
    """Read the unified config YAML and build the node list from it.

    All LLM provider settings, BT parameters, goal, and context are taken
    directly from the config file so that every mode (llm / fixed) is
    configured through a single YAML with the same structure.

    The 'launch_llm_nodes' argument controls whether llm_planner and
    llm_bt_builder are started here (true) or are assumed to be already
    running externally (false).
    """
    config_file     = LaunchConfiguration('config_file').perform(context)
    skills_file     = LaunchConfiguration('skills_file').perform(context)
    launch_llm_nodes = LaunchConfiguration('launch_llm_nodes').perform(context).lower() == 'true'

    with open(config_file, 'r') as f:
        config = yaml.safe_load(f)

    # ── LLM provider ─────────────────────────────────────────────────────────
    planner_provider = config.get('planner_llm_provider', config.get('llm_provider', 'gemini'))
    planner_model    = config.get('planner_llm_model',    config.get('llm_model',    'gemini-2.5-flash'))
    planner_api_key  = config.get('planner_llm_api_key',  config.get('llm_api_key',  ''))

    bt_builder_provider = config.get('bt_builder_llm_provider', config.get('llm_provider', 'gemini'))
    bt_builder_model    = config.get('bt_builder_llm_model',    config.get('llm_model',    'gemini-2.5-flash'))
    bt_builder_api_key  = config.get('bt_builder_llm_api_key',  config.get('llm_api_key',  ''))

    # ── BT executor ──────────────────────────────────────────────────────────
    bt_nodes_package      = config.get('bt_nodes_package',    'dummy_bt_nodes')
    bt_control_period_ms  = config.get('bt_control_period_ms', 50)
    bt_timeout_sec        = config.get('bt_timeout_sec',        60.0)

    plugin_libraries = config.get('plugin_libraries', [])
    plugin_library   = plugin_libraries[0] if plugin_libraries else 'libdummy_bt_nodes_plugin.so'

    nodes = []

    # ── 1. LLM Planner node (optional) ───────────────────────────────────────
    if launch_llm_nodes:
        nodes.append(Node(
            package='llm_planner',
            executable='llm_planner_node',
            name='llm_planner_node',
            output='screen',
            emulate_tty=True,
            parameters=[{
                'llm_provider': planner_provider,
                'llm_model_id': planner_model,
                'llm_api_key':  planner_api_key,
            }],
        ))

    # ── 2. LLM BT builder node (optional) ────────────────────────────────────
    if launch_llm_nodes:
        nodes.append(Node(
            package='llm_bt_builder',
            executable='bt_rag_agent_node.py',
            name='llm_bt_agent',
            output='screen',
            emulate_tty=True,
            parameters=[{
                'llm_provider': bt_builder_provider,
                'llm_model_id': bt_builder_model,
                'llm_api_key':  bt_builder_api_key,
            }],
        ))

    # ── 3. Action Executor (handles both fixed and LLM orchestrators) ─────────
    nodes.append(Node(
        package='behavior_architecture',
        executable='action_executor',
        name='action_executor',
        output='screen',
        emulate_tty=True,
        arguments=[config_file],
    ))

    # ── 4. Goal sender (reads goal + context from the same config file) ───────
    nodes.append(TimerAction(
        period=3.0,
        actions=[Node(
            package='behavior_architecture',
            executable='test_start_mission',
            name='test_start_mission',
            output='screen',
            emulate_tty=True,
            parameters=[{
                'goal_file': config_file,
                'skills_file': skills_file,
            }],
        )],
    ))

    return nodes


def generate_launch_description():
    """Launch the full LLM planning + execution pipeline.

    Nodes started:
      1. llm_planner_node    — generates and replans task YAML via LLM        (optional)
      2. llm_bt_agent_node   — generates BT XML from a step description via LLM (optional)
      3. llm_plan_executor   — orchestrates planning, BT generation, and BT execution
      4. test_start_mission  — sends the initial mission (after a 3 s delay)

    All configuration (LLM provider/model/key, BT parameters, goal, context)
    is loaded from a single YAML file using the unified format shared with the
    fixed-orchestrator configs (simple_config.yaml, restaurant_config.yaml).

    Set launch_llm_nodes:=false when llm_planner and llm_bt_builder are
    already running in another terminal / launch file.
    """
    pkg_dir = get_package_share_directory('behavior_architecture')
    default_config = os.path.join(pkg_dir, 'config', 'llm_config.yaml')
    default_skills = os.path.join(pkg_dir, 'config', 'skills.yaml')

    config_file_arg = DeclareLaunchArgument(
        'config_file',
        default_value=default_config,
        description=(
            'Path to the unified YAML config file '
            '(orchestrator_type must be "llm")'
        ),
    )

    skills_file_arg = DeclareLaunchArgument(
        'skills_file',
        default_value=default_skills,
        description='Path to the YAML file listing the robot skills (skills: [...]).',
    )

    launch_llm_nodes_arg = DeclareLaunchArgument(
        'launch_llm_nodes',
        default_value='false',
        description=(
            'Whether to launch llm_planner and llm_bt_builder nodes. '
            'Set to "false" if they are already running externally.'
        ),
    )

    return LaunchDescription([
        config_file_arg,
        skills_file_arg,
        launch_llm_nodes_arg,
        OpaqueFunction(function=launch_setup),
    ])
