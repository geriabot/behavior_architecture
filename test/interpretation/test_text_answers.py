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

"""Answers that are free text (`parse: text`) and the Question.parse() hook."""

import json
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.interpreters.llm.backends import FakeBackend
from behavior_interpretation.questions import ConfigError
import pytest

MODULE = '''
import json

from behavior_interpretation import Question, rule
from behavior_interpretation.validation import InvalidAnswer


class OwnReading(Question):
    """Reads a JSON answer as a system's former gateway did: the last object of the text."""

    name = 'own'

    def parse(self, text):
        start = text.rfind('{')
        if start < 0:
            raise InvalidAnswer('no object')
        try:
            return json.loads(text[start:])
        except json.JSONDecodeError as exc:
            raise InvalidAnswer(str(exc)) from None

    def validate(self, answer, context):
        if answer.get('score', 0) > 10:
            raise InvalidAnswer('score above 10')
        return answer


class Polite(Question):
    name = 'polite'

    def validate(self, answer, context):
        if 'please' not in answer['text']:
            raise InvalidAnswer('not polite')
        return answer


@rule('canned')
def canned(call):
    return 'A canned line.'
'''


def request(question, text=''):
    return SimpleNamespace(question=question, request_id='r1', context='', text=text,
                           images=[], budget_s=0.0, variant='')


@pytest.fixture
def module(tmp_path):
    path = tmp_path / 'text_questions.py'
    path.write_text(MODULE)
    return str(path)


def server(definition, answers, name='say', **kwargs):
    fake = FakeBackend({'answers': answers})
    questions = {name: dict({'prompt': 'Say something about {text}.{output_format}'},
                            **definition)}
    return InterpretationServer(questions=questions, backends={'fake': fake}, **kwargs), fake


@pytest.mark.parametrize('raw', [
    'Hello there!',
    '  Two lines,\nwith "quotes" and {braces}.  ',
    '{"not": "parsed"}',
    '<think>x</think> kept as it is',
])
def test_text_is_returned_as_it_is(raw):
    s, fake = server({'parse': 'text'}, [raw])
    result = s.interpret(request('say', 'cats'))
    assert (result.success, result.source, result.fallback_reason) == (True, 'llm', '')
    assert json.loads(result.answer) == {'text': raw}
    # No structured output, and an empty {output_format}
    assert fake.calls[0]['schema'] is None
    assert fake.calls[0]['prompt'] == 'Say something about cats.'


def test_empty_text_is_invalid_unless_allowed():
    s, _ = server({'parse': 'text'}, [' \n'])
    result = s.interpret(request('say'))
    assert (result.source, result.fallback_reason, result.answer) == ('invalid', 'invalid', '')
    assert 'empty text' in result.message
    s, _ = server({'parse': 'text', 'allow_empty': True}, [''])
    result = s.interpret(request('say'))
    assert (result.source, json.loads(result.answer)) == ('llm', {'text': ''})


def test_retries_and_fallback_as_for_other_questions():
    s, fake = server({'parse': 'text', 'max_retries': 2}, ['', '!down', 'At last.'])
    result = s.interpret(request('say'))
    assert (result.source, json.loads(result.answer)) == ('llm', {'text': 'At last.'})
    assert len(fake.calls) == 3
    s, _ = server({'parse': 'text', 'fallback': 'Sorry, say that again?'}, ['!timeout'])
    result = s.interpret(request('say'))
    assert (result.source, result.fallback_reason) == ('fallback', 'timeout')
    assert json.loads(result.answer) == {'text': 'Sorry, say that again?'}
    s, _ = server({'parse': 'text', 'fallback': {'text': 'Hm.'}}, [''])
    result = s.interpret(request('say'))
    assert (result.source, result.fallback_reason, json.loads(result.answer)) == (
        'fallback', 'invalid', {'text': 'Hm.'})


def test_class_validate_and_rule_with_text(module):
    s, _ = server({'parse': 'text'}, ['Sit down.', 'Sit down, please.'], name='polite',
                  question_modules=[module])
    assert s.interpret(request('polite')).source == 'invalid'
    assert s.interpret(request('polite')).source == 'llm'
    s, fake = server({'parse': 'text', 'interpreter': 'rule:canned'}, ['unused'],
                     question_modules=[module])
    result = s.interpret(request('say'))
    assert (result.source, json.loads(result.answer)) == ('rule', {'text': 'A canned line.'})
    assert not fake.calls


@pytest.mark.parametrize('definition', [
    {'parse': 'text', 'fields': ['str:line']},
    {'parse': 'text', 'fields_from': 'context.fields'},
    {'parse': 'text', 'schema': {'type': 'object'}},
    {'allow_empty': True},
    {'parse': 'strict', 'allow_empty': True},
])
def test_text_configuration_errors(definition):
    with pytest.raises(ConfigError):
        server(definition, ['x'])


def test_a_variant_can_ask_for_text():
    s, fake = server({'fields': ['str:line'], 'variants': {'spoken': {'parse': 'text',
                                                                      'fields': None}}},
                     ['{"line": "Hi"}', 'Hi'])
    assert json.loads(s.interpret(request('say')).answer) == {'line': 'Hi'}
    result = s.interpret(SimpleNamespace(question='say', request_id='', context='', text='',
                                         images=[], budget_s=0.0, variant='spoken'))
    assert json.loads(result.answer) == {'text': 'Hi'}
    assert fake.calls[0]['schema'] is not None and fake.calls[1]['schema'] is None


def test_parse_hook_reads_and_validates_as_the_system(module):
    s, fake = server({'fields': ['int:score']}, [
        'Thinking... {"draft": 1} then {"score": 7}',
        'Thinking... {"score": 12}',
        'no object at all'],
        name='own', question_modules=[module])
    result = s.interpret(request('own'))
    assert (result.source, json.loads(result.answer)) == ('llm', {'score': 7})
    result = s.interpret(request('own'))
    assert (result.source, result.fallback_reason) == ('invalid', 'invalid')
    assert 'score above 10' in result.message
    result = s.interpret(request('own'))
    assert 'no object' in result.message
    # The fields still make the schema and the output format
    assert fake.calls[0]['schema']['required'] == ['score']


def test_default_parse_hook_follows_the_parse_key():
    s, _ = server({'fields': ['int:id']}, ['x'])
    question = s.question('say')
    assert question.parse('<think>a</think>{"id": 1}') == {'id': 1}
    s, _ = server({'fields': ['int:id'], 'parse': 'strict'}, ['x'])
    with pytest.raises(ValueError):
        s.question('say').parse('<think>a</think>{"id": 1}')
    s, _ = server({'parse': 'text'}, ['x'])
    assert s.question('say').parse('{"id": 1}') == {'text': '{"id": 1}'}
