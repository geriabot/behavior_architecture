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

"""The anthropic backend's requests, against a stub HTTP server on 127.0.0.1 (no model)."""

import json

from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import Limits
import pytest
from test_backends import _image
from test_backends import SCHEMA
from test_backends import STRICT
from test_backends import Stub


@pytest.fixture
def stub():
    server = Stub()
    yield server
    server.close()


def _reply(text, stop_reason='end_turn'):
    return json.dumps({'model': 'claude-x', 'stop_reason': stop_reason,
                       'content': [{'type': 'thinking', 'thinking': '...'},
                                   {'type': 'text', 'text': text}],
                       'usage': {'input_tokens': 10, 'output_tokens': 4,
                                 'cache_read_input_tokens': 5,
                                 'cache_creation_input_tokens': 1}})


def _headers(request):
    return {k.lower(): v for k, v in request['headers'].items()}


def test_request_of_the_gateways_claude_provider(stub, monkeypatch):
    monkeypatch.setenv('ANTHROPIC_API_KEY', 'test-key')
    backend = create_backend('anthropic', {'url': stub.url, 'system_prompt': 'You are a robot.'})
    stub.replies = [(200, _reply(' {"id": 2} '), 0)]
    result = backend.generate('claude-x', 'Which?', SCHEMA, [], Limits(5, max_tokens=80))
    assert (result.text, result.prompt_tokens, result.completion_tokens,
            result.model_version) == ('{"id": 2}', 16, 4, 'claude-x')
    request = stub.requests[0]
    assert request['path'] == '/v1/messages'
    headers = _headers(request)
    assert (headers['x-api-key'], headers['anthropic-version']) == ('test-key', '2023-06-01')
    assert 'anthropic-beta' not in headers
    # The parameters of ClaudeProvider._build_params for the same call
    assert json.loads(request['body']) == {
        'model': 'claude-x', 'max_tokens': 4096,
        'messages': [{'role': 'user', 'content': 'Which?'}],
        'output_config': {'effort': 'low',
                          'format': {'type': 'json_schema', 'schema': STRICT}},
        'system': 'You are a robot.'}
    assert b'\\u' not in request['body']  # serialized as httpx does (UTF-8 as it is)


def test_fallback_models_images_and_options(stub, monkeypatch):
    monkeypatch.setenv('MY_CLAUDE_KEY', 'other-key')
    image = _image()
    backend = create_backend('anthropic', {'url': stub.url, 'api_key_env': 'MY_CLAUDE_KEY',
                                           'effort': ''})
    stub.replies = [(200, _reply('Hello.'), 0)]
    backend.generate('claude-opus-5-5', 'Say hello', None, [image], Limits(5, max_tokens=5000))
    request = stub.requests[0]
    headers = _headers(request)
    assert headers['x-api-key'] == 'other-key'
    assert headers['anthropic-beta'] == 'server-side-fallback-2026-07-01'
    body = json.loads(request['body'])
    assert body['max_tokens'] == 5000 and body['output_config'] == {}
    assert body['fallbacks'] == 'default'
    assert body['messages'][0]['content'] == [
        {'type': 'text', 'text': 'Say hello'},
        {'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png',
                                     'data': image.base64}}]


def test_schema_rejected_is_sent_again_without_format(stub, monkeypatch):
    monkeypatch.setenv('ANTHROPIC_API_KEY', 'k')
    backend = create_backend('anthropic', {'url': stub.url})
    stub.replies = [(400, '{"type": "error", "error": {"type": "invalid_request_error"}}', 0),
                    (200, _reply('{"id": 1}'), 0)]
    assert backend.generate('c', 'p', SCHEMA, [], Limits(5)).text == '{"id": 1}'
    bodies = [json.loads(r['body']) for r in stub.requests]
    assert 'format' in bodies[0]['output_config']
    assert bodies[1]['output_config'] == {'effort': 'low'}
    # Without a schema, a 400 is an error
    stub.replies = [(400, '{}', 0)]
    with pytest.raises(BackendError):
        backend.generate('c', 'p', None, [], Limits(5))


def test_errors(stub, monkeypatch):
    monkeypatch.delenv('ANTHROPIC_API_KEY', raising=False)
    backend = create_backend('anthropic', {'url': stub.url})
    with pytest.raises(BackendError, match='no API key'):
        backend.generate('c', 'p', None, [], Limits(5))
    monkeypatch.setenv('ANTHROPIC_API_KEY', 'k')
    stub.replies = [(200, _reply('', stop_reason='refusal'), 0), (200, _reply('  '), 0),
                    (500, 'overloaded', 0)]
    for match in ('declined', 'empty reply', 'HTTP 500'):
        with pytest.raises(BackendError, match=match):
            backend.generate('c', 'p', None, [], Limits(5))
    with pytest.raises(BackendError, match='dialect'):
        create_backend('anthropic', {'dialect': 'llm_router'}).build_payload(
            'c', 'p', None, [], Limits(5))
