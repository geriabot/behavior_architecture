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

"""The question keys `parse` (loose, strict) and `validate` (fields, object)."""

import json
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import FakeBackend
from behavior_interpretation.interpreters.llm.backends import Limits
from behavior_interpretation.questions import ConfigError
from behavior_interpretation.validation import InvalidAnswer
from behavior_interpretation.validation import parse_answer_strict
import pytest
from test_backends import SCHEMA
from test_backends import Stub


def request(question, context=None):
    return SimpleNamespace(question=question, request_id='r1',
                           context=json.dumps(context) if context is not None else '',
                           text='', images=[], budget_s=0.0, variant='')


@pytest.fixture
def stub():
    server = Stub()
    yield server
    server.close()


def ask(definition, answer, context=None):
    fake = FakeBackend({'answers': [answer]})
    server = InterpretationServer(questions={'q': dict({'prompt': 'q'}, **definition)},
                                  backends={'fake': fake})
    return server.interpret(request('q', context))


# --- parse: strict ---------------------------------------------------------------------------

@pytest.mark.parametrize('text, expected', [
    ('{"id": 3}', {'id': 3}),
    ('  {"id": 3}\n', {'id': 3}),                 # json.loads skips the white space
    ('[{"id": 3}, {"id": 4}]', {'id': 3}),         # a list: its first item
    ('{}', {}),
])
def test_strict_reads_what_llm_router_read(text, expected):
    assert parse_answer_strict(text) == expected


@pytest.mark.parametrize('text', [
    '```json\n{"id": 3}\n```',                     # a fence is removed by the backend, or not
    '<think>which one?</think>{"id": 3}',
    'The answer is {"id": 3}.',
    '{"id": 3} and more',
    '[]',
    '[3]',
    '3',
    '"text"',
    'null',
    '',
    None,
])
def test_strict_rejects_the_rest(text):
    with pytest.raises(InvalidAnswer):
        parse_answer_strict(text)


@pytest.mark.parametrize('text', [
    '```json\n{"id": 3}\n```', '<think>which one?</think>{"id": 3}', 'The answer is {"id": 3}.'])
def test_loose_and_strict_on_the_server(text):
    loose = ask({'fields': ['int:id']}, text)
    assert (loose.source, loose.answer) == ('llm', '{"id": 3}')
    strict = ask({'fields': ['int:id'], 'parse': 'strict'}, text)
    assert (strict.success, strict.source, strict.fallback_reason, strict.answer) == \
        (True, 'invalid', 'invalid', '')


def test_strict_with_retries_and_fallback():
    fake = FakeBackend({'answers': ['Sure: {"id": 1}', '{"id": 2}']})
    question = {'prompt': 'q', 'fields': ['int:id'], 'parse': 'strict', 'max_retries': 1}
    server = InterpretationServer(questions={'q': question}, backends={'fake': fake})
    assert server.interpret(request('q')).answer == '{"id": 2}'
    result = ask({'fields': ['int:id'], 'parse': 'strict', 'fallback': -1}, '[]')
    assert (result.source, result.fallback_reason, result.answer) == \
        ('fallback', 'invalid', '{"id": -1}')


def _ollama_reply(content):
    return (200, json.dumps({'model': 'm', 'message': {'role': 'assistant', 'content': content}}),
            0)


def _openai_reply(content):
    return (200, json.dumps({'model': 'm', 'choices': [{'message': {'content': content}}]}), 0)


def _gemini_reply(content):
    return (200, json.dumps({'candidates': [{'content': {'parts': [{'text': content}]}}]}), 0)


@pytest.mark.parametrize('backend, reply, stripped', [
    ('ollama', _ollama_reply, True),   # llm_router: http_backends.strip_code_fence, json.loads
    ('openai', _openai_reply, True),
    ('gemini', _gemini_reply, False),  # llm_router: json.loads of the bridge's text
])
def test_fence_per_backend_in_the_llm_router_dialect(stub, monkeypatch, backend, reply,
                                                     stripped):
    monkeypatch.setenv('GEMINI_API_KEY', 'test-key')
    options = {'url': stub.url}
    if backend == 'openai':
        options['api_key_env'] = ''
    fenced = '```json\n{"id": 3}\n```'
    stub.replies = [reply(fenced)]
    text = create_backend(backend, options).generate('m', 'q', SCHEMA, [], Limits(5)).text
    assert text == ('{"id": 3}' if stripped else fenced)
    if stripped:
        assert parse_answer_strict(text) == {'id': 3}
    else:
        with pytest.raises(InvalidAnswer):
            parse_answer_strict(text)


def test_gemini_llm_manager_still_removes_the_fence(stub, monkeypatch):
    monkeypatch.setenv('GEMINI_API_KEY', 'test-key')
    stub.replies = [_gemini_reply('```json\n{"id": 3}\n```')]
    backend = create_backend('gemini', {'url': stub.url, 'dialect': 'llm_manager'})
    assert backend.generate('m', 'q', SCHEMA, [], Limits(5)).text == '{"id": 3}'


# --- validate: object ------------------------------------------------------------------------

@pytest.mark.parametrize('text, answer', [
    ('{"id": 3}', {'id': 3}),
    ('{"id": 3.0}', {'id': 3.0}),                       # not converted to 3
    ('{"id": "3"}', {'id': '3'}),
    ('{"id": 3, "why": "closest"}', {'id': 3, 'why': 'closest'}),   # extra keys kept
    ('{"why": "closest"}', {'why': 'closest'}),         # the field missing
    ('{}', {}),
])
def test_object_returns_the_parsed_object(text, answer):
    result = ask({'fields': ['int:id'], 'validate': 'object'}, text)
    assert (result.success, result.source, json.loads(result.answer)) == (True, 'llm', answer)
    fields = ask({'fields': ['int:id']}, text)
    assert fields.source == ('llm' if 'id' in answer else 'invalid')


def test_object_still_sends_the_schema_and_output_format():
    fake = FakeBackend({'answers': ['{}']})
    question = {'prompt': 'Answer: {output_format}', 'fields_from': 'context.inputs',
                'validate': 'object'}
    server = InterpretationServer(questions={'q': question}, backends={'fake': fake})
    server.interpret(request('q', {'inputs': ['str:det_class', 'bool:found']}))
    call = fake.calls[0]
    assert call['schema'] == {'type': 'object', 'required': ['det_class', 'found'],
                              'properties': {'det_class': {'type': 'string'},
                                             'found': {'type': 'boolean'}}}
    assert call['prompt'].startswith('Answer: {\n  "properties"')


def test_object_with_choices_and_strict():
    question = {'fields': ['int:id'], 'validate': 'object', 'parse': 'strict',
                'choices_from': 'context.ids'}
    assert ask(question, '{"id": 2}', {'ids': [1, 2]}).source == 'llm'
    assert ask(question, '{"id": 5}', {'ids': [1, 2]}).source == 'invalid'
    assert ask(question, 'id: 2', {'ids': [1, 2]}).source == 'invalid'
    assert ask(question, '[1]', {'ids': [1, 2]}).source == 'invalid'


# --- configuration ---------------------------------------------------------------------------

@pytest.mark.parametrize('key, value', [('parse', 'tolerant'), ('validate', 'none')])
def test_unknown_modes_are_configuration_errors(key, value):
    with pytest.raises(ConfigError, match=key):
        InterpretationServer(questions={'q': {'prompt': 'q', 'fields': ['int:id'], key: value}},
                             backends={'fake': FakeBackend({'answers': ['{}']})})


def test_a_variant_can_change_the_modes():
    fake = FakeBackend({'answers': ['Sure: {"id": 1}']})
    question = {'prompt': 'q', 'fields': ['int:id'],
                'variants': {'as_router': {'parse': 'strict', 'validate': 'object'}}}
    server = InterpretationServer(questions={'q': question}, backends={'fake': fake})
    assert server.interpret(request('q')).source == 'llm'
    as_router = request('q')
    as_router.variant = 'as_router'
    assert server.interpret(as_router).source == 'invalid'
