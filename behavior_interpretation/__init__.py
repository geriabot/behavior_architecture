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
Interpretation: narrow, validated answers to the questions that the layers ask.

The server (`interpretation_server`, in `node.py`) is a thin ROS wrapper around
`InterpretationServer` (`server.py`), which has no ROS dependency and is what the tests use.
A question is declared in YAML (`questions.py`) and answered by a chain of interpreters
(`interpreters/`): `llm` (a model, through the backends of `interpreters/llm/backends`) and
`rule` (deterministic code registered by the system). See docs/INTERPRETATION_DESIGN.md.
"""

from behavior_interpretation.questions import formatter
from behavior_interpretation.questions import Question
from behavior_interpretation.questions import rule
from behavior_interpretation.server import InterpretationServer

__all__ = ['InterpretationServer', 'Question', 'formatter', 'rule']
