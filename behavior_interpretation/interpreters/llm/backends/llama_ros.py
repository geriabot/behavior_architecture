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
A local model through llama_ros (its action, default /llama/generate_response).

The goal is the one attention's router sends: the prompt (preceded by "Look at the image:
<__media__> " when there is an image; one <__media__> per image), the grammar schema (the
schema as json.dumps(indent=2), without additionalProperties) and the images. llama_ros is
not a dependency of this package: if llama_msgs cannot be imported, or its action server is
not up, the call fails with a clear error (model_down) and nothing waits for it to appear.

Options of the route: action.
"""

import json
import threading

from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult


def llama_prompt(prompt, images):
    if not images:
        return prompt
    if len(images) == 1:
        return 'Look at the image: <__media__> \n' + prompt
    return 'Look at the images: ' + ' '.join(['<__media__>'] * len(images)) + ' \n' + prompt


class LlamaRosBackend(Backend):

    kind = 'llama_ros'

    def __init__(self, options=None, node=None):
        super().__init__(options, node)
        self._client = None
        self._goal_type = None
        self._lock = threading.Lock()

    def _ensure_client(self):
        with self._lock:
            if self._client is not None:
                return
            if self.node is None:
                raise BackendError('llama_ros: the backend needs the server node')
            try:
                from llama_msgs.action import GenerateResponse
                from rclpy.action import ActionClient
                from rclpy.callback_groups import ReentrantCallbackGroup
            except ImportError as exc:
                raise BackendError(f'llama_ros: llama_msgs is not installed ({exc})') from exc
            self._goal_type = GenerateResponse
            self._client = ActionClient(
                self.node, GenerateResponse, self.option('action', '/llama/generate_response'),
                callback_group=ReentrantCallbackGroup())

    def generate(self, model, prompt, schema, images, limits, question=''):
        self._ensure_client()
        if not self._client.server_is_ready():
            raise BackendError(
                f'llama_ros: the action {self.option("action", "/llama/generate_response")} '
                'is not available')
        goal = self._goal_type.Goal()
        goal.prompt = llama_prompt(prompt, images)
        if schema is not None:
            goal.sampling_config.grammar_schema = json.dumps(schema, indent=2)
        for image in images:
            if image.message is None:
                raise BackendError('llama_ros: an image without its message')
            goal.images.append(image.message)

        done = threading.Event()
        outcome = {}

        def on_result(future):
            try:
                outcome['result'] = future.result()
            except Exception as exc:  # noqa: B902 (reported to the caller)
                outcome['error'] = exc
            done.set()

        def on_goal(future):
            try:
                handle = future.result()
            except Exception as exc:  # noqa: B902
                outcome['error'] = exc
                done.set()
                return
            if not handle.accepted:
                outcome['error'] = BackendError('llama_ros: the goal was rejected')
                done.set()
                return
            outcome['handle'] = handle
            handle.get_result_async().add_done_callback(on_result)

        self._client.send_goal_async(goal).add_done_callback(on_goal)
        if not done.wait(limits.timeout_s):
            if 'handle' in outcome:
                outcome['handle'].cancel_goal_async()
            raise BackendTimeout(f'llama_ros: no answer in {limits.timeout_s:.1f} s')
        if 'error' in outcome:
            error = outcome['error']
            raise error if isinstance(error, BackendError) else BackendError(f'llama_ros: {error}')
        result = outcome['result']
        if result.status != 4:  # action_msgs/GoalStatus.STATUS_SUCCEEDED
            raise BackendError(f'llama_ros: the goal ended with status {result.status}')
        return GenerateResult(result.result.response.text, model_version=model or 'llama_ros')
