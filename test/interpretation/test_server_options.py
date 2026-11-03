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

"""Opt-in options of a request and of the server's configuration."""

import json
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.call_log import read_calls
from behavior_interpretation.interpreters.llm.backends import FakeBackend
from behavior_interpretation.questions import ConfigError
import pytest


def request(question, **fields):
    values = {'question': question, 'request_id': 'r1', 'context': '', 'text': '', 'images': [],
              'budget_s': 0.0, 'variant': ''}
    values.update(fields)
    return SimpleNamespace(**values)


# --- The request's max_tokens ----------------------------------------------------------------

def test_request_max_tokens_replaces_the_question_cap(tmp_path):
    fake = FakeBackend({'answers': ['{"id": 1}']})
    s = InterpretationServer(
        questions={'q': {'prompt': 'p', 'fields': ['int:id'], 'max_tokens': 50}},
        backends={'fake': fake}, log_dir=str(tmp_path))
    assert s.interpret(request('q', max_tokens=120)).source == 'llm'
    assert s.interpret(request('q', max_tokens=0)).source == 'llm'
    assert s.interpret(request('q')).source == 'llm'   # a request without the field
    assert [c['limits'].max_tokens for c in fake.calls] == [120, 50, 50]
    # The log has max_tokens only when the request gave it
    assert ['max_tokens' in c for c in read_calls(str(tmp_path))] == [True, False, False]
    assert read_calls(str(tmp_path))[0]['max_tokens'] == 120


@pytest.mark.parametrize('value', [-1, 2.5, True, '10'])
def test_request_max_tokens_must_be_a_count(value):
    s = InterpretationServer(questions={'q': {'prompt': 'p', 'fields': ['int:id']}},
                             backends={'fake': FakeBackend({'answers': ['{"id": 1}']})})
    result = s.interpret(request('q', max_tokens=value))
    assert not result.success and 'max_tokens' in result.message


def test_request_max_tokens_reaches_the_llm_manager_body():
    from behavior_interpretation.interpreters.llm.backends import create_backend
    from behavior_interpretation.interpreters.llm.backends import Limits
    backend = create_backend('ollama', {'dialect': 'llm_manager'})
    payload = backend.build_payload('m', 'p', None, [], Limits(5, max_tokens=77))
    assert payload['options']['num_predict'] == 77
    router = create_backend('ollama', {})
    assert 'num_predict' not in json.dumps(router.build_payload('m', 'p', None, [],
                                                                Limits(5, max_tokens=77)))


# --- route_overrides -------------------------------------------------------------------------

def test_route_overrides_replace_the_route_of_a_question():
    local = FakeBackend({'answers': ['{"id": 1}']})
    cloud = FakeBackend({'answers': ['{"id": 2}']})
    questions = {'q': {'prompt': 'p', 'fields': ['int:id'], 'route': 'local',
                       'variants': {'v': {'route': 'local', 'prompt': 'pv'}}},
                 'other': {'prompt': 'o', 'fields': ['int:id'], 'route': 'local'}}
    s = InterpretationServer(questions=questions, backends={'local': local, 'cloud': cloud},
                             route_overrides={'q': 'cloud'})
    assert s.interpret(request('q')).answer == '{"id": 2}'
    assert s.interpret(request('q', variant='v')).answer == '{"id": 2}'   # over the variant
    assert s.interpret(request('other')).answer == '{"id": 1}'
    assert [c['prompt'] for c in cloud.calls] == ['p', 'pv'] and len(local.calls) == 1
    # Without overrides, the questions file's routes
    s = InterpretationServer(questions=questions, backends={'local': local, 'cloud': cloud})
    assert s.interpret(request('q')).answer == '{"id": 1}'


def test_route_overrides_from_the_questions_file(tmp_path):
    path = tmp_path / 'questions.yaml'
    path.write_text('questions:\n  q: {prompt: p, fields: ["int:id"], route: a}\n'
                    'route_overrides: {q: b}\n')
    a, b = FakeBackend({'answers': ['{"id": 1}']}), FakeBackend({'answers': ['{"id": 2}']})
    s = InterpretationServer(questions_file=str(path), backends={'a': a, 'b': b})
    assert s.interpret(request('q')).answer == '{"id": 2}'
    # The server's parameter takes precedence over the file
    s = InterpretationServer(questions_file=str(path), backends={'a': a, 'b': b},
                             route_overrides={'q': 'a'})
    assert s.interpret(request('q')).answer == '{"id": 1}'


@pytest.mark.parametrize('overrides', [{'nope': 'a'}, {'q': 'missing'}])
def test_route_overrides_errors(overrides):
    with pytest.raises(ConfigError):
        InterpretationServer(questions={'q': {'prompt': 'p', 'fields': ['int:id']}},
                             backends={'a': FakeBackend({'answers': ['{"id": 1}']})},
                             route_overrides=overrides)
