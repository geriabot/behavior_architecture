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

"""Cancelling the calls in progress of a request (InterpretationServer.cancel)."""

import threading
import time
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.call_log import read_calls
from behavior_interpretation.interpreters.llm.backends import FakeBackend

QUESTION = {'prompt': 'p', 'fields': ['int:id'], 'max_retries': 3, 'fallback': -1}


def request(request_id, question='q'):
    return SimpleNamespace(question=question, request_id=request_id, context='', text='',
                           images=[], budget_s=0.0, variant='')


def in_background(server, req):
    """Start a call in a thread; return a function that waits for its result."""
    box = {}
    thread = threading.Thread(target=lambda: box.update(result=server.interpret(req)))
    thread.start()

    def result(timeout=5.0):
        thread.join(timeout)
        assert not thread.is_alive(), 'the call did not return'
        return box['result']
    return result


def wait_until(condition, timeout=5.0):
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, 'timed out'
        time.sleep(0.01)


def slow_server(tmp_path=None, delay_s=10.0):
    fake = FakeBackend({'answers': ['{"id": 1}'], 'delay_s': delay_s})
    server = InterpretationServer(questions={'q': QUESTION}, backends={'fake': fake},
                                  log_dir=str(tmp_path) if tmp_path else '')
    return server, fake


def test_cancel_returns_the_calls_in_progress_at_once(tmp_path):
    server, fake = slow_server(tmp_path)
    first = in_background(server, request('dialogue-1'))
    second = in_background(server, request('dialogue-1'))
    other = in_background(server, request('dialogue-2'))
    wait_until(lambda: len(fake.calls) == 3)
    start = time.monotonic()
    assert server.cancel('dialogue-1') == 2
    for result in (first(), second()):
        # invalid, whatever the question's fallback; no retry
        assert (result.success, result.source, result.fallback_reason, result.answer) == (
            True, 'invalid', 'cancelled', '')
    assert time.monotonic() - start < 1.0
    assert len(fake.calls) == 3
    # The other request goes on, and a cancel of nothing in progress cancels nothing
    assert server.cancel('dialogue-1') == 0
    assert server.cancel('') == 0
    assert server.cancel('dialogue-2') == 1
    assert other().fallback_reason == 'cancelled'
    logged = read_calls(str(tmp_path))
    assert [c['fallback_reason'] for c in logged] == ['cancelled'] * 3
    assert logged[0]['attempts'][0]['error'] == 'cancelled'


def test_later_calls_of_the_request_are_not_affected():
    server, _ = slow_server(delay_s=0.0)
    assert server.cancel('dialogue-1') == 0
    assert server.interpret(request('dialogue-1')).answer == '{"id": 1}'


def test_calls_without_request_id_run_in_the_callers_thread():
    threads = []

    def script(**kwargs):
        threads.append(threading.current_thread())
        return '{"id": 1}'

    fake = FakeBackend({}, script=script)
    server = InterpretationServer(questions={'q': QUESTION}, backends={'fake': fake})
    assert server.interpret(request('')).source == 'llm'
    assert threads == [threading.current_thread()]
    assert server.interpret(request('d')).source == 'llm'
    assert threads[1] is not threading.current_thread()


def test_errors_of_a_cancellable_call_are_the_same():
    fake = FakeBackend({'answers': ['!down', '!timeout', 'not json', '{"id": 2}']})
    server = InterpretationServer(questions={'q': dict(QUESTION, max_retries=3)},
                                  backends={'fake': fake}, log_dir='')
    result = server.interpret(request('d'))
    assert (result.source, result.answer) == ('llm', '{"id": 2}')
    assert len(fake.calls) == 4
