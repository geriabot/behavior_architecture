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

"""The ROS node: its parameters and the service, in a namespace, with the fake backend."""

import json
import threading
import time

import pytest

rclpy = pytest.importorskip('rclpy')
srv = pytest.importorskip('behavior_architecture.srv')

from rclpy.executors import MultiThreadedExecutor  # noqa: E402
from rclpy.parameter import Parameter  # noqa: E402
from sensor_msgs.msg import Image  # noqa: E402

from behavior_interpretation.node import InterpretationServerNode  # noqa: E402,I100


@pytest.fixture
def ros(tmp_path):
    questions = tmp_path / 'questions.yaml'
    questions.write_text(
        'questions:\n'
        '  identify:\n'
        '    prompt: "Which of {ids}?"\n'
        '    fields: ["int:id"]\n'
        '    choices_from: context.ids\n'
        '    uses_image: optional\n'
        '    route: text\n'
        '    variants: {other: {prompt: "Other {ids}"}}\n')
    rclpy.init()
    node = InterpretationServerNode(
        namespace='/test_system',
        parameter_overrides=[
            Parameter('questions_file', value=str(questions)),
            Parameter('routes.text.backend', value='fake'),
            Parameter('routes.text.answers', value=['{"id": 2}']),
            Parameter('log_dir', value=str(tmp_path / 'log')),
        ])
    client_node = rclpy.create_node('interpretation_test_client', namespace='/test_system')
    executor = MultiThreadedExecutor()
    executor.add_node(node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()
    yield node, client_node, tmp_path
    executor.shutdown()
    node.destroy_node()
    client_node.destroy_node()
    rclpy.try_shutdown()
    thread.join(timeout=5)


def call(client, request, timeout=10.0):
    future = client.call_async(request)
    deadline = time.monotonic() + timeout
    while not future.done() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert future.done(), 'no response'
    return future.result()


def test_service_in_the_namespace(ros):
    node, client_node, tmp_path = ros
    client = client_node.create_client(srv.Interpret, 'interpretation/interpret')
    assert client.service_name == '/test_system/interpretation/interpret'
    assert client.wait_for_service(timeout_sec=10.0)

    request = srv.Interpret.Request(question='identify', request_id='t',
                                    context=json.dumps({'ids': [1, 2]}))
    image = Image(encoding='rgb8', width=1, height=1, step=3, data=[1, 2, 3])
    request.images = [image]
    response = call(client, request)
    assert (response.success, response.source, response.answer) == (True, 'llm', '{"id": 2}')

    response = call(client, srv.Interpret.Request(question='unknown'))
    assert not response.success and 'Unknown question' in response.message

    request.variant = 'other'
    assert call(client, request).success
    prompts = [c['prompt'] for c in node.server.routes['text'].backend.calls]
    assert prompts == ['Which of [1, 2]?', 'Other [1, 2]']
    assert (tmp_path / 'log' / 'calls.jsonl').exists()
