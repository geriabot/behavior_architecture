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
from failing_server import FailingServer  # noqa: E402


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
    # The token cap of a call: 0 by default (the question's), or the request's
    assert srv.Interpret.Request().max_tokens == 0
    request.max_tokens = 30
    assert call(client, request).success
    limits = [c['limits'].max_tokens for c in node.server.routes['text'].backend.calls]
    assert limits == [0, 0, 30]
    assert (tmp_path / 'log' / 'calls.jsonl').exists()


def _server_node(parameters):
    rclpy.init()
    try:
        return InterpretationServerNode(namespace='/test_options', parameter_overrides=[
            Parameter(name, value=value) for name, value in parameters.items()])
    except Exception:
        rclpy.try_shutdown()
        raise


def test_route_overrides_parameter(tmp_path):
    questions = tmp_path / 'questions.yaml'
    questions.write_text('questions:\n  q: {prompt: p, fields: ["int:id"], route: a}\n')
    node = _server_node({
        'questions_file': str(questions),
        'routes.a.backend': 'fake', 'routes.a.answers': ['{"id": 1}'],
        'routes.b.backend': 'fake', 'routes.b.answers': ['{"id": 2}'],
        'route_overrides.q': 'b'})
    try:
        assert node.server.interpret({'question': 'q'}).answer == '{"id": 2}'
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


def test_warmup_does_not_block_the_service(tmp_path):
    ollama = FailingServer([], (200, {}, '{"done": true}'), delay_s=1.0)
    questions = tmp_path / 'questions.yaml'
    questions.write_text('questions:\n  q: {prompt: p, fields: ["int:id"], route: a}\n')
    start = time.monotonic()
    node = _server_node({
        'questions_file': str(questions),
        'routes.a.backend': 'fake', 'routes.a.answers': ['{"id": 1}'],
        'routes.local.backend': 'ollama', 'routes.local.model': 'm',
        'routes.local.url': ollama.url, 'routes.local.warmup': True})
    try:
        assert time.monotonic() - start < 0.9   # the model takes 1 s to load
        assert node.server.warmups == {'local': 'loading'}
        assert node.server.interpret({'question': 'q'}).answer == '{"id": 1}'
        deadline = time.monotonic() + 5.0
        while node.server.warmups['local'] == 'loading' and time.monotonic() < deadline:
            time.sleep(0.05)
        assert node.server.warmups == {'local': 'ready'}
        assert [r['path'] for r in ollama.requests] == ['/api/generate']
    finally:
        node.destroy_node()
        rclpy.try_shutdown()
        ollama.close()


def test_templates_files_parameter(tmp_path):
    (tmp_path / 'a.yaml').write_text('t: "from a"\nu: "u from a"\n')
    (tmp_path / 'b.yaml').write_text('t: "from b"\n')
    questions = tmp_path / 'questions.yaml'
    questions.write_text('questions:\n  q: {template: t, fields: ["int:id"]}\n'
                         '  r: {template: u, fields: ["int:id"]}\n')
    node = _server_node({
        'questions_file': str(questions),
        'templates_files': [str(tmp_path / 'a.yaml'), str(tmp_path / 'b.yaml')],
        'routes.a.backend': 'fake', 'routes.a.answers': ['{"id": 1}']})
    try:
        node.server.interpret({'question': 'q'})
        node.server.interpret({'question': 'r'})
        prompts = [c['prompt'] for c in node.server.routes['a'].backend.calls]
        assert prompts == ['from b', 'u from a']
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


def test_cancel_service(tmp_path):
    questions = tmp_path / 'questions.yaml'
    questions.write_text('questions:\n  q: {prompt: p, fields: ["int:id"], fallback: -1}\n')
    node = _server_node({
        'questions_file': str(questions),
        'routes.a.backend': 'fake', 'routes.a.answers': ['{"id": 1}'],
        'routes.a.delay_s': 10.0})
    client_node = rclpy.create_node('interpretation_cancel_client', namespace='/test_options')
    executor = MultiThreadedExecutor(num_threads=4)
    executor.add_node(node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()
    try:
        interpret = client_node.create_client(srv.Interpret, 'interpretation/interpret')
        cancel = client_node.create_client(srv.CancelInterpretation, 'interpretation/cancel')
        assert cancel.service_name == '/test_options/interpretation/cancel'
        assert interpret.wait_for_service(timeout_sec=10.0)
        assert cancel.wait_for_service(timeout_sec=10.0)
        pending = interpret.call_async(srv.Interpret.Request(question='q', request_id='d1'))
        deadline = time.monotonic() + 5.0
        while not node.server.routes['a'].backend.calls:
            assert time.monotonic() < deadline
            time.sleep(0.01)
        start = time.monotonic()
        response = call(cancel, srv.CancelInterpretation.Request(request_id='d1'))
        assert (response.success, response.cancelled) == (True, 1)
        deadline = time.monotonic() + 2.0
        while not pending.done():
            assert time.monotonic() < deadline, 'the cancelled call did not return'
            time.sleep(0.01)
        assert time.monotonic() - start < 2.0
        result = pending.result()
        assert (result.success, result.source, result.fallback_reason, result.answer) == (
            True, 'invalid', 'cancelled', '')
        response = call(cancel, srv.CancelInterpretation.Request(request_id=''))
        assert not response.success
    finally:
        executor.shutdown()
        node.destroy_node()
        client_node.destroy_node()
        rclpy.try_shutdown()
        thread.join(timeout=5)
