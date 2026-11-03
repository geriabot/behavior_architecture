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

"""The requests of the HTTP backends, against a stub HTTP server on 127.0.0.1 (no model)."""

import base64
import http.server
import json
import threading
import time
from types import SimpleNamespace

from behavior_interpretation.fields import build_schema
from behavior_interpretation.fields import parse_fields
from behavior_interpretation.images import EncodedImage
from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.llm.backends import BackendTimeout
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import Limits
from behavior_interpretation.interpreters.llm.backends.llama_ros import llama_prompt
from behavior_interpretation.interpreters.llm.backends.openai import manager_strict_schema
import pytest

SCHEMA = build_schema(parse_fields(['int:id']))
STRICT = {'type': 'object', 'properties': {'id': {'type': 'integer'}}, 'required': ['id'],
          'additionalProperties': False}


def _image():
    data = bytes(range(12))
    return EncodedImage(SimpleNamespace(encoding='rgb8', width=2, height=2, step=6, data=data))


class Stub:
    """An HTTP server that records each request and answers with the next scripted reply."""

    def __init__(self):
        self.requests = []
        self.replies = []
        stub = self

        class Handler(http.server.BaseHTTPRequestHandler):

            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                stub.requests.append({'path': self.path, 'headers': dict(self.headers),
                                      'body': body})
                status, content, delay = stub.replies.pop(0) if stub.replies else (200, '{}', 0)
                time.sleep(delay)
                data = content.encode() if isinstance(content, str) else content
                self.send_response(status)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def stub():
    server = Stub()
    yield server
    server.close()


def test_ollama_llm_router_request(stub):
    image = _image()
    backend = create_backend('ollama', {'url': stub.url, 'keep_alive': '30m', 'num_ctx': 8192})
    stub.replies = [(200, json.dumps({'model': 'm:7b', 'message': {'content': '{"id": 2}'},
                                      'prompt_eval_count': 5, 'eval_count': 3}), 0)]
    result = backend.generate('m:7b', 'Which?', SCHEMA, [image], Limits(5))
    assert (result.text, result.prompt_tokens, result.completion_tokens,
            result.model_version) == ('{"id": 2}', 5, 3, 'm:7b')
    request = stub.requests[0]
    assert request['path'] == '/api/chat'
    # The bytes llm_router's ask_ollama sends for the same call
    expected = {'model': 'm:7b',
                'messages': [{'role': 'user', 'content': 'Which?', 'images': [image.base64]}],
                'format': STRICT, 'stream': False,
                'options': {'temperature': 0.0, 'num_ctx': 8192}, 'keep_alive': '30m'}
    assert request['body'] == json.dumps(expected).encode()
    assert base64.b64decode(json.loads(request['body'])['messages'][0]['images'][0]) == image.png
    # Without images: no images key
    payload = backend.build_payload('m:7b', 'Which?', SCHEMA, [], Limits(5))
    assert 'images' not in payload['messages'][0]


def test_ollama_llm_manager_request_and_stream(stub):
    backend = create_backend('ollama', {'url': stub.url, 'dialect': 'llm_manager',
                                        'system_prompt': 'You are a robot.', 'think': 'false'})
    lines = [{'model': 'q', 'message': {'content': '{"id"'}, 'done': False},
             {'model': 'q', 'message': {'content': ': 4}'}, 'done': True,
              'prompt_eval_count': 9, 'eval_count': 2}]
    stub.replies = [(200, '\n'.join(json.dumps(line) for line in lines) + '\n', 0)]
    result = backend.generate('q', 'Which?', SCHEMA, [], Limits(5, max_tokens=40))
    assert (result.text, result.prompt_tokens, result.completion_tokens) == ('{"id": 4}', 9, 2)
    expected = {'model': 'q',
                'messages': [{'role': 'system', 'content': 'You are a robot.'},
                             {'role': 'user', 'content': 'Which?'}],
                'stream': True, 'keep_alive': -1, 'think': False, 'format': SCHEMA,
                'options': {'num_predict': 40, 'num_ctx': 8192, 'temperature': 0}}
    # httpx's serialization (the gateway's client)
    assert stub.requests[0]['body'] == json.dumps(
        expected, ensure_ascii=False, separators=(',', ':')).encode()
    thinking = create_backend('ollama', {'dialect': 'llm_manager', 'think': 'low'})
    payload = thinking.build_payload('q', 'p', None, [], Limits(5, max_tokens=40))
    assert payload['think'] == 'low' and payload['options'] == {'num_predict': 4136,
                                                                'num_ctx': 8192}


def test_openai_llm_router_request(stub):
    image = _image()
    backend = create_backend('openai', {'url': stub.url + '/v1'})
    stub.replies = [(200, json.dumps({'model': 'gpt', 'choices': [
        {'message': {'content': '```json\n{"id": 1}\n```'}}],
        'usage': {'prompt_tokens': 4, 'completion_tokens': 1}}), 0)]
    result = backend.generate('gpt', 'Which?', SCHEMA, [image], Limits(5))
    assert result.text == '{"id": 1}'
    request = stub.requests[0]
    assert request['path'] == '/v1/chat/completions'
    expected = {'model': 'gpt', 'messages': [{'role': 'user', 'content': [
        {'type': 'text', 'text': 'Which?'},
        {'type': 'image_url', 'image_url': {'url': f'data:image/png;base64,{image.base64}'}}]}],
        'temperature': 0.0,
        'response_format': {'type': 'json_schema',
                            'json_schema': {'name': 'answer', 'schema': STRICT, 'strict': True}}}
    assert request['body'] == json.dumps(expected).encode()
    payload = backend.build_payload('gpt', 'Which?', SCHEMA, [], Limits(5))
    assert payload['messages'][0]['content'] == [{'type': 'text', 'text': 'Which?'}]


def test_openai_llm_manager_stream(stub):
    backend = create_backend('openai', {'url': stub.url, 'dialect': 'llm_manager'})
    events = [{'model': 'g', 'choices': [{'delta': {'content': '{"id":'}}]},
              {'model': 'g', 'choices': [{'delta': {'content': ' 5}'}}]},
              {'model': 'g', 'choices': [], 'usage': {'prompt_tokens': 3, 'completion_tokens': 2}}]
    stub.replies = [(200, ''.join(f'data: {json.dumps(e)}\n\n' for e in events) +
                     'data: [DONE]\n\n', 0)]
    optional = {'type': 'object', 'properties': {'a': {'type': 'string'}}, 'required': []}
    result = backend.generate('g', 'Which?', optional, [], Limits(5, max_tokens=30))
    assert (result.text, result.prompt_tokens, result.completion_tokens) == ('{"id": 5}', 3, 2)
    body = json.loads(stub.requests[0]['body'])
    assert body['stream'] is True and body['stream_options'] == {'include_usage': True}
    assert body['reasoning_effort'] == 'none' and body['max_completion_tokens'] == 30
    assert body['response_format']['json_schema'] == {
        'name': 'response', 'schema': dict(optional, additionalProperties=False),
        'strict': False}
    assert manager_strict_schema(SCHEMA) == (STRICT, True)


def test_gemini_bridge_request(stub, monkeypatch):
    monkeypatch.setenv('GEMINI_API_KEY', 'test-key')
    image = _image()
    backend = create_backend('gemini', {'url': stub.url})
    stub.replies = [(200, json.dumps({'modelVersion': 'gemini-x', 'candidates': [
        {'content': {'parts': [{'text': 'thinking', 'thought': True}, {'text': '{"id": 7}'}]}}],
        'usageMetadata': {'promptTokenCount': 11, 'candidatesTokenCount': 4}}), 0)]
    result = backend.generate('gemini-x', 'Which? ñ', SCHEMA, [image], Limits(5))
    assert (result.text, result.model_version) == ('{"id": 7}', 'gemini-x')
    request = stub.requests[0]
    assert request['path'] == '/v1beta/models/gemini-x:generateContent'
    headers = {key.lower(): value for key, value in request['headers'].items()}
    assert headers['x-goog-api-key'] == 'test-key'
    # nlohmann's dump(): compact, keys sorted, UTF-8 not escaped
    assert request['body'] == (
        '{"contents":[{"parts":[{"text":"Which? ñ"},{"inline_data":{"data":"' + image.jpeg_base64 +
        '","mime_type":"image/jpeg"}}]}],"generationConfig":{"responseMimeType":'
        '"application/json","responseSchema":{"properties":{"id":{"type":"INTEGER"}},'
        '"required":["id"],"type":"OBJECT"}}}').encode()


def test_http_errors(stub):
    backend = create_backend('ollama', {'url': stub.url})
    stub.replies = [(500, 'boom', 0)]
    with pytest.raises(BackendError) as error:
        backend.generate('m', 'p', SCHEMA, [], Limits(5))
    assert error.value.reason == 'model_down'
    stub.replies = [(200, '{}', 1.0)]
    with pytest.raises(BackendTimeout):
        backend.generate('m', 'p', SCHEMA, [], Limits(0.2))
    closed = create_backend('ollama', {'url': 'http://127.0.0.1:9'})
    with pytest.raises(BackendError) as error:
        closed.generate('m', 'p', SCHEMA, [], Limits(2))
    assert error.value.reason == 'model_down'


def test_llama_ros_without_llama_ros():
    backend = create_backend('llama_ros', {})
    with pytest.raises(BackendError):
        backend.generate('m', 'p', SCHEMA, [], Limits(1))
    assert llama_prompt('p', [_image()]) == 'Look at the image: <__media__> \np'
    assert llama_prompt('p', []) == 'p'


def test_fake_backend():
    backend = create_backend('fake', {'answers': [{'id': 1}, '!timeout', '!down'],
                                      'by_question': {'q2': ['{"id": 9}']}})
    assert backend.generate('', 'p', SCHEMA, [], Limits(1)).text == '{"id": 1}'
    with pytest.raises(BackendTimeout):
        backend.generate('', 'p', SCHEMA, [], Limits(1))
    with pytest.raises(BackendError):
        backend.generate('', 'p', SCHEMA, [], Limits(1))
    assert backend.generate('', 'p', SCHEMA, [], Limits(1), question='q2').text == '{"id": 9}'
    slow = create_backend('fake', {'answers': ['{}'], 'delay_s': 0.5})
    with pytest.raises(BackendTimeout):
        slow.generate('', 'p', SCHEMA, [], Limits(0.05))


def test_gemini_bridge_image_is_bgr_jpeg_quality_95():
    # As google_gemini_bridge_cpp: cv_bridge to bgr8, no resizing, cv::imencode(".jpeg", q 95)
    import base64
    import cv2
    import numpy as np
    image = _image()
    rgb = np.frombuffer(bytes(range(12)), dtype=np.uint8).reshape(2, 2, 3)
    ok, expected = cv2.imencode('.jpeg', cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR),
                                [cv2.IMWRITE_JPEG_QUALITY, 95])
    assert ok and base64.b64decode(image.jpeg_base64) == expected.tobytes()
    decoded = cv2.imdecode(np.frombuffer(base64.b64decode(image.jpeg_base64), np.uint8),
                           cv2.IMREAD_COLOR)
    assert decoded.shape == (2, 2, 3)
