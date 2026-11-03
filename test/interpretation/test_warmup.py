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

"""The warm-up of a route's model (route option warmup), against FailingServer (no model)."""

import json
import threading
import time

from behavior_interpretation import InterpretationServer
from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import FakeBackend
from behavior_interpretation.interpreters.llm.backends import ollama
from behavior_interpretation.questions import ConfigError
from failing_server import FailingServer
import pytest

LOADED = (200, {}, '{"model":"m","created_at":"t","response":"","done":true,'
                   '"done_reason":"load"}')


@pytest.fixture
def quick(monkeypatch):
    monkeypatch.setattr(ollama, 'WARMUP_DELAYS_S', (0.1, 0.2))


def test_ollama_loads_the_model_as_the_gateway_did():
    server = FailingServer([], LOADED)
    try:
        backend = create_backend('ollama', {'url': server.url, 'dialect': 'llm_manager'})
        assert backend.warm_up('qwen2.5:7b') == "Model 'qwen2.5:7b' loaded into memory."
        request, = server.requests
        assert request['path'] == '/api/generate'
        # OllamaProvider.ensure_model_ready's payload, with the num_ctx of the requests
        assert json.loads(request['body']) == {'model': 'qwen2.5:7b', 'prompt': '',
                                               'keep_alive': -1, 'options': {'num_ctx': 8192}}
        # The route's own num_ctx and keep_alive; llm_router sends num_ctx only when set
        create_backend('ollama', {'url': server.url, 'dialect': 'llm_manager', 'num_ctx': 0,
                                  'keep_alive': '30m'}).warm_up('m')
        create_backend('ollama', {'url': server.url}).warm_up('m')
        create_backend('ollama', {'url': server.url, 'num_ctx': 4096}).warm_up('m')
        assert [json.loads(r['body']) for r in server.requests[1:]] == [
            {'model': 'm', 'prompt': '', 'keep_alive': '30m'},
            {'model': 'm', 'prompt': '', 'keep_alive': -1},
            {'model': 'm', 'prompt': '', 'keep_alive': -1, 'options': {'num_ctx': 4096}}]
    finally:
        server.close()


def test_ollama_attempts(quick):
    # A model that cannot be reached: 3 attempts, 1 s and 2 s apart (here 0.1 and 0.2 s)
    server = FailingServer(['drop', 'drop'], LOADED)
    try:
        assert 'loaded' in create_backend('ollama', {'url': server.url}).warm_up('m')
        assert len(server.times) == 3
        assert [round(g, 1) for g in server.gaps()] == [0.1, 0.2]
    finally:
        server.close()
    server = FailingServer(['drop'] * 3, LOADED)
    try:
        with pytest.raises(BackendError, match='Failed after 3 attempts'):
            create_backend('ollama', {'url': server.url}).warm_up('m')
        assert len(server.times) == 3
    finally:
        server.close()
    # An HTTP error ends it at once
    server = FailingServer([(404, {})], LOADED)
    try:
        with pytest.raises(BackendError, match='Load failed: HTTP 404'):
            create_backend('ollama', {'url': server.url}).warm_up('m')
        assert len(server.times) == 1
    finally:
        server.close()


def test_cloud_backends_have_nothing_to_load():
    for kind in ('openai', 'gemini', 'anthropic', 'fake'):
        assert 'nothing to load' in create_backend(kind, {}).warm_up('m')


class SlowWarmup(FakeBackend):

    def __init__(self, delay_s, error=None):
        super().__init__({'answers': ['{"id": 1}']})
        self.delay_s = delay_s
        self.error = error

    def warm_up(self, model):
        time.sleep(self.delay_s)
        if self.error:
            raise BackendError(self.error)
        return f'{model} loaded'


QUESTIONS = {'q': {'prompt': 'p', 'fields': ['int:id'], 'route': 'a'}}


def test_the_server_answers_while_the_model_loads():
    reports = []
    done = threading.Event()

    def report(ok, message):
        reports.append((ok, message))
        if len(reports) == 2:
            done.set()

    s = InterpretationServer(
        questions=QUESTIONS,
        routes={'a': {'model': 'm', 'warmup': True}, 'b': {'warmup': 'true'},
                'c': {'warmup': False}},
        backends={'a': SlowWarmup(0.5), 'b': SlowWarmup(0.1, 'no model'),
                  'c': SlowWarmup(0.0)})
    start = time.monotonic()
    threads = s.start_warmups(report)
    assert time.monotonic() - start < 0.1 and len(threads) == 2
    assert s.warmups == {'a': 'loading', 'b': 'loading'}
    assert s.interpret({'question': 'q'}).answer == '{"id": 1}'   # without waiting
    assert done.wait(5.0)
    assert s.warmups == {'a': 'ready', 'b': 'failed'}
    by_route = {message.split(' ')[1]: (ok, message) for ok, message in reports}
    assert by_route['a'][0] and by_route['a'][1].endswith('m loaded')
    assert not by_route['b'][0] and by_route['b'][1].endswith('no model')


def test_no_warmup_by_default():
    s = InterpretationServer(questions=QUESTIONS, backends={'a': SlowWarmup(0.0)})
    assert s.start_warmups() == [] and s.warmups == {}
    assert not s.routes['a'].warmup


def test_an_invalid_warmup_is_a_configuration_error():
    with pytest.raises(ConfigError, match='warmup'):
        InterpretationServer(questions=QUESTIONS, routes={'a': {'warmup': 'soon'}},
                             backends={'a': SlowWarmup(0.0)})
