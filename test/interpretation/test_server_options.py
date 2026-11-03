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
