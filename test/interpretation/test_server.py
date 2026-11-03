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

"""One call of the server, with the fake backend and the rule interpreter."""

import json
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.interpreters.llm.backends import FakeBackend
from behavior_interpretation.questions import ConfigError
import pytest

MODULE = '''
from behavior_interpretation import Question, formatter, rule


@rule('most_centered')
def most_centered(call):
    """Only for a call that asks just the id of a detection."""
    if [f.name for f in call.fields] != ['id']:
        return None
    detections = call.context.get('detections', [])
    if not detections:
        return -1
    return min(detections, key=lambda d: abs(d['x'] - 320))['id']


@formatter('names')
def names(fields):
    return ', '.join(f.name for f in fields)


class Greeting(Question):
    name = 'greeting'

    def build_prompt(self, context, text):
        return 'Say hello to ' + text

    def validate(self, answer, context):
        if not answer['reply'].startswith('Hello'):
            from behavior_interpretation.validation import InvalidAnswer
            raise InvalidAnswer('not a greeting')
        return answer

    def answer_without_model(self, context):
        return {'reply': 'Hello (rule)'} if context.get('offline') else None
'''


def request(question, context=None, text='', images=(), budget_s=0.0, variant=''):
    return SimpleNamespace(question=question, request_id='r1',
                           context=json.dumps(context) if context is not None else '',
                           text=text, images=list(images), budget_s=budget_s, variant=variant)


def image(value=1):
    return SimpleNamespace(encoding='rgb8', width=1, height=1, step=3, data=bytes([value] * 3))


@pytest.fixture
def module(tmp_path):
    path = tmp_path / 'my_questions.py'
    path.write_text(MODULE)
    return str(path)


def server(questions, answers=None, script=None, delay_s=0.0, **kwargs):
    fake = FakeBackend({'answers': answers or ['{}'], 'delay_s': delay_s}, script=script)
    return InterpretationServer(questions=questions, backends={'fake': fake}, **kwargs), fake


IDENTIFY = {
    'template': 'identify',
    'fields': ['int:id'],
    'choices_from': 'context.detection_ids',
    'uses_image': 'optional',
    'variants': {'no_image': {'uses_image': False, 'template': 'identify_text'}},
}
TEMPLATES = {'identify': 'Task: {task}\nDetections: {detection_ids}\n{output_format}{text}',
             'identify_text': 'Text only: {task}'}


def test_prompt_schema_and_answer():
    s, fake = server({'identify': IDENTIFY}, ['{"id": 2}'], templates=TEMPLATES)
    result = s.interpret(request('identify', {'task': 'the cup', 'detection_ids': [1, 2]}))
    assert (result.success, result.source, result.answer) == (True, 'llm', '{"id": 2}')
    call = fake.calls[0]
    assert call['prompt'].startswith('Task: the cup\nDetections: [1, 2]\n{\n  "properties"')
    assert call['schema'] == {'type': 'object', 'properties': {'id': {'type': 'integer'}},
                              'required': ['id']}
    assert call['question'] == 'identify'


def test_choices_from():
    s, _ = server({'identify': IDENTIFY}, ['{"id": 5}'], templates=TEMPLATES)
    result = s.interpret(request('identify', {'task': 't', 'detection_ids': [1, 2]}))
    assert (result.source, result.fallback_reason, result.answer) == ('invalid', 'invalid', '')
    s, _ = server({'identify': IDENTIFY}, ['{"id": -1}'], templates=TEMPLATES)
    result = s.interpret(request('identify', {'task': 't', 'detection_ids': [1, 2]}))
    assert (result.source, result.answer) == ('llm', '{"id": -1}')
    detections = [{'id': 4, 'x': 0}, {'id': 6, 'x': 300}]
    spec = dict(IDENTIFY, choices_from='context.detections')
    s, _ = server({'identify': spec}, ['{"id": 6}'], templates=TEMPLATES)
    assert s.interpret(request('identify', {'detections': detections})).source == 'llm'
    result = s.interpret(request('identify', {}))
    assert not result.success and 'choices_from' in result.message


def test_retries_and_fallbacks():
    question = {'prompt': 'q', 'fields': ['int:id'], 'max_retries': 1}
    s, fake = server({'q': question}, ['not json', '{"id": 1}'])
    result = s.interpret(request('q'))
    assert (result.source, result.answer, len(fake.calls)) == ('llm', '{"id": 1}', 2)
    s, fake = server({'q': dict(question, max_retries=0)}, ['not json', '{"id": 1}'])
    result = s.interpret(request('q'))
    assert (result.source, result.fallback_reason, len(fake.calls)) == ('invalid', 'invalid', 1)
    s, _ = server({'q': dict(question, max_retries=0, fallback=-1)}, ['!down'])
    result = s.interpret(request('q'))
    assert (result.source, result.fallback_reason, result.answer) == \
        ('fallback', 'model_down', '{"id": -1}')
    s, _ = server({'q': dict(question, fallback={'id': 0})}, ['!timeout'])
    result = s.interpret(request('q'))
    assert (result.source, result.fallback_reason, result.answer) == \
        ('fallback', 'timeout', '{"id": 0}')
    s, _ = server({'q': dict(question, fallback='none')}, ['!timeout'])
    assert s.interpret(request('q')).source == 'invalid'


def test_budget():
    question = {'prompt': 'q', 'fields': ['int:id']}
    # budget 0: only the route's limit (120 s) cuts
    s, _ = server({'q': question}, ['{"id": 1}'], delay_s=0.2)
    assert s.interpret(request('q', budget_s=0.0)).source == 'llm'
    # a budget shorter than the answer
    result = s.interpret(request('q', budget_s=0.05))
    assert (result.source, result.fallback_reason) == ('invalid', 'budget')
    # the route's own limit
    routes = {'slow': {'backend': 'fake', 'answers': ['{"id": 1}'], 'delay_s': 0.5,
                       'timeout_s': 0.05}}
    s = InterpretationServer(questions={'q': dict(question, route='slow')}, routes=routes)
    result = s.interpret(request('q'))
    assert (result.source, result.fallback_reason) == ('invalid', 'timeout')
    result = s.interpret(request('q', budget_s=-1))
    assert not result.success


def test_fields_from():
    question = {'prompt': '{output_format}', 'fields_from': 'context.inputs'}
    s, fake = server({'q': question}, ['{"class": "cup", "count": 2}'])
    context = {'inputs': ['str:class', 'int:count']}
    result = s.interpret(request('q', context))
    assert json.loads(result.answer) == {'class': 'cup', 'count': 2}
    assert fake.calls[0]['schema']['required'] == ['class', 'count']
    assert not s.interpret(request('q', {})).success


def test_variants(module):
    s, fake = server({'identify': IDENTIFY}, ['{"id": 1}'], templates=TEMPLATES)
    context = {'task': 't', 'detection_ids': [1]}
    s.interpret(request('identify', context, images=[image()]))
    assert fake.calls[-1]['prompt'].startswith('Task:') and len(fake.calls[-1]['images']) == 1
    s.interpret(request('identify', context, images=[image()], variant='no_image'))
    assert fake.calls[-1]['prompt'] == 'Text only: t' and fake.calls[-1]['images'] == []
    assert not s.interpret(request('identify', context, variant='nope')).success
    # The variant chosen by the server's configuration
    s, fake = server({'identify': IDENTIFY}, ['{"id": 1}'], templates=TEMPLATES,
                     variants={'identify': 'no_image'})
    s.interpret(request('identify', context, images=[image()]))
    assert fake.calls[-1]['prompt'] == 'Text only: t'
    with pytest.raises(ConfigError):
        server({'identify': IDENTIFY}, templates=TEMPLATES, variants={'identify': 'nope'})


def test_images():
    spec = {'prompt': 'p', 'fields': ['int:id'], 'uses_image': True, 'max_images': 2}
    s, fake = server({'q': spec}, ['{"id": 1}'])
    assert not s.interpret(request('q')).success  # needs an image
    assert s.interpret(request('q', images=[image(1), image(2)])).success
    assert [i.png for i in fake.calls[-1]['images']] != [] and \
        fake.calls[-1]['images'][0].sha256 != fake.calls[-1]['images'][1].sha256
    assert not s.interpret(request('q', images=[image()] * 3)).success  # max_images
    bad = SimpleNamespace(encoding='32FC1', width=1, height=1, step=4, data=bytes(4))
    assert not s.interpret(request('q', images=[bad])).success


def test_malformed_requests():
    s, _ = server({'q': {'prompt': 'p', 'fields': ['int:id']}}, ['{"id": 1}'])
    assert not s.interpret(request('unknown')).success
    bad = request('q')
    bad.context = 'not json'
    assert not s.interpret(bad).success
    bad.context = '[1]'
    assert not s.interpret(bad).success
    assert s.interpret({'question': 'q'}).success  # a dict is also a request


def test_rule_chain_and_overrides(module):
    spec = {'prompt': 'p', 'fields': ['int:id'], 'interpreter': ['rule:most_centered', 'llm']}
    s, fake = server({'q': spec}, ['{"id": 99}'], question_modules=[module])
    context = {'detections': [{'id': 4, 'x': 10}, {'id': 6, 'x': 330}]}
    result = s.interpret(request('q', context))
    assert (result.source, result.answer, len(fake.calls)) == ('rule', '{"id": 6}', 0)
    # The rule declines (not only the id is asked): the model answers
    spec2 = dict(spec, fields=['int:id', 'str:why'])
    s, fake = server({'q': spec2}, ['{"id": 6, "why": "w"}'], question_modules=[module])
    assert s.interpret(request('q', context)).source == 'llm'
    # Every interpreter declines: fallback, or invalid, with the reason "declined"
    declined = dict(spec2, interpreter='rule:most_centered', fallback={'id': -1, 'why': ''})
    s, _ = server({'q': declined}, question_modules=[module])
    result = s.interpret(request('q', context))
    assert (result.source, result.fallback_reason) == ('fallback', 'declined')
    # A launch-time override, without editing the definition
    s, fake = server({'q': dict(spec, interpreter='llm')}, ['{"id": 4}'],
                     question_modules=[module], interpreter_overrides={'q': 'rule:most_centered'})
    assert s.interpret(request('q', context)).source == 'rule'
    # A variant may set the interpreter too
    spec3 = dict(spec, interpreter='llm', variants={'rule': {'interpreter': 'rule:most_centered'}})
    s, fake = server({'q': spec3}, ['{"id": 4}'], question_modules=[module])
    assert s.interpret(request('q', context, variant='rule')).source == 'rule'
    assert s.interpret(request('q', context)).source == 'llm'
    # Unknown rules and interpreters are found when the server starts
    with pytest.raises(ConfigError):
        server({'q': dict(spec, interpreter='rule:nope')}, question_modules=[module])
    with pytest.raises(ConfigError):
        server({'q': dict(spec, interpreter='world_model')})
    with pytest.raises(ConfigError):
        server({'q': spec}, question_modules=[module], interpreter_overrides={'x': 'llm'})


def test_question_class_and_formatter(module):
    questions = {'greeting': {'fields': ['str:reply'], 'interpreter': 'rule,llm'},
                 'formatted': {'prompt': '[{output_format}]', 'fields': ['int:a', 'str:b'],
                               'output_formatter': 'names'}}
    s, fake = server(questions, ['{"reply": "Hello Ana"}', '{"reply": "Bye"}', '{}'],
                     question_modules=[module])
    result = s.interpret(request('greeting', {}, text='Ana'))
    assert (result.source, result.answer) == ('llm', '{"reply": "Hello Ana"}')
    assert fake.calls[0]['prompt'] == 'Say hello to Ana'
    assert s.interpret(request('greeting', {}, text='Ana')).source == 'invalid'  # validate
    result = s.interpret(request('greeting', {'offline': True}))
    assert (result.source, result.answer) == ('rule', '{"reply": "Hello (rule)"}')
    s.interpret(request('formatted'))
    assert fake.calls[-1]['prompt'] == '[a, b]'


def test_raw_schema():
    schema = {'type': 'object', 'properties': {'intent': {'type': 'string',
                                                          'enum': ['yes', 'no']}},
              'required': ['intent']}
    s, fake = server({'q': {'prompt': 'p', 'schema': schema}}, ['{"intent": "yes", "x": 1}'])
    result = s.interpret(request('q'))
    assert json.loads(result.answer) == {'intent': 'yes', 'x': 1}
    assert fake.calls[0]['schema'] == schema


def test_configuration_errors():
    with pytest.raises(ConfigError):
        InterpretationServer(questions={})
    with pytest.raises(ConfigError):
        server({'q': {'template': 'missing', 'fields': ['int:id']}})
    with pytest.raises(ConfigError):
        server({'q': {'prompt': 'p', 'fields': ['int:id'], 'route': 'nope'}})
    with pytest.raises(ConfigError):
        server({'q': {'prompt': 'p', 'fields': ['double:x']}})
    with pytest.raises(ConfigError):
        server({'q': {'prompt': 'p', 'unknown_key': 1}})
    with pytest.raises(ConfigError):
        server({'q': {'prompt': 'p', 'interpreter': 'rule'}})  # no rule, no class


def test_questions_file(tmp_path):
    (tmp_path / 'templates.yaml').write_text('templates:\n  ask: "Ask {text}"\n')
    (tmp_path / 'questions.yaml').write_text(
        'questions:\n'
        '  q: {template: ask, fields: ["bool:yes"], route: text}\n'
        'routes:\n'
        '  text: {backend: fake, answers: [{"yes": true}]}\n')
    s = InterpretationServer(questions_file=str(tmp_path / 'questions.yaml'),
                             templates_file=str(tmp_path / 'templates.yaml'))
    result = s.interpret(request('q', text='now'))
    assert (result.source, result.answer) == ('llm', '{"yes": true}')
    assert s.routes['text'].backend.calls[0]['prompt'] == 'Ask now'
