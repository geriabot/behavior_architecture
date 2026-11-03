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

"""
The SDK-like retries of the llm_manager dialect (sdk_retries), against FailingServer.

The number of requests and the seconds between them are compared with the official SDKs':
with the behavior recorded by sdk_retries_reference.py (sdk_retries_reference.json), and with
the SDK itself when it can be imported. No model and no network: 127.0.0.1 only.
"""

import json
import os
import threading
import time
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.call_log import read_calls
from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.llm.backends import BackendTimeout
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import Limits
from behavior_interpretation.interpreters.llm.backends import retries
from behavior_interpretation.questions import ConfigError
from failing_server import FailingServer
import pytest
import sdk_retries_reference as reference

REFERENCE = json.load(open(os.path.join(os.path.dirname(__file__),
                                        'sdk_retries_reference.json'), encoding='utf-8'))
BACKEND = {'openai': 'openai', 'anthropic': 'anthropic', 'gemini': 'gemini'}
MODEL = {'openai': 'm', 'anthropic': 'claude-x', 'gemini': 'gemini-x'}
# Seconds of difference allowed between a delay and the SDK's (scheduling, a request's time)
TOLERANCE_S = 0.15


@pytest.fixture(autouse=True)
def keys(monkeypatch):
    monkeypatch.setenv('ANTHROPIC_API_KEY', 'test-key')
    monkeypatch.setenv('GEMINI_API_KEY', 'test-key')


@pytest.fixture
def no_jitter(monkeypatch):
    monkeypatch.setattr(retries, 'random', lambda: 0.0)


def _backend(sdk, url, **options):
    base = url + '/v1' if sdk == 'openai' else url
    return create_backend(BACKEND[sdk], {'url': base, 'dialect': 'llm_manager', **options})


def run_backend(sdk, scenario, timeout_s=10.0, **options):
    """Run one scenario with the backend: {requests, gaps, outcome}, as reference.run()."""
    server = FailingServer(reference.SCENARIOS[scenario], reference.SUCCESS[sdk])
    try:
        backend = _backend(sdk, server.url, **options)
        backend.generate(MODEL[sdk], 'p', None, [], Limits(timeout_s, 50))
        outcome = 'ok'
    except BackendError as exc:
        outcome = type(exc).__name__
    finally:
        server.close()
    return {'requests': len(server.times), 'gaps': server.gaps(), 'outcome': outcome}


def assert_same_pattern(ours, sdk):
    assert ours['requests'] == sdk['requests'], (ours, sdk)
    assert (ours['outcome'] == 'ok') == (sdk['outcome'] == 'ok'), (ours, sdk)
    for mine, theirs in zip(ours['gaps'], sdk['gaps']):
        assert abs(mine - theirs) <= TOLERANCE_S, (ours, sdk)


# --- The recorded behavior of the SDKs --------------------------------------------------------

SCENARIOS = [s for s in reference.SCENARIOS if s != 'jitter']


@pytest.mark.parametrize('sdk', ['openai', 'anthropic', 'gemini'])
@pytest.mark.parametrize('scenario', SCENARIOS)
def test_requests_and_delays_as_the_recorded_sdk(sdk, scenario, no_jitter):
    assert_same_pattern(run_backend(sdk, scenario), REFERENCE[sdk]['scenarios'][scenario])


@pytest.mark.parametrize('sdk', ['openai', 'anthropic'])
def test_jitter_within_the_sdks_range(sdk):
    # The base delays are 0.5 s and 1 s, less a random 0 to 25 %, in the SDK and here
    for gaps in (run_backend(sdk, 'jitter')['gaps'],
                 REFERENCE[sdk]['scenarios']['jitter']['gaps']):
        assert len(gaps) == 2
        for gap, base in zip(gaps, (0.5, 1.0)):
            assert 0.75 * base - 0.01 <= gap <= base + TOLERANCE_S, gaps


def test_reference_versions():
    # The SDKs of the interaction system's gateway (openai and google-genai in its
    # environment; anthropic, which its environment lacks, from the user site)
    assert {sdk: REFERENCE[sdk]['version'] for sdk in REFERENCE} == {
        'openai': '3.39.0', 'anthropic': '0.84.0', 'gemini': '2.33.0'}


# --- The SDK itself, when it can be imported --------------------------------------------------

def _sdk_available(sdk):
    try:
        reference.CLIENTS[sdk]('http://127.0.0.1:9')
        return True
    except ImportError:
        return False


@pytest.mark.parametrize('sdk', ['openai', 'anthropic', 'gemini'])
@pytest.mark.parametrize('scenario', ['fail2_503', 'retry_after_long', 'should_retry_true',
                                      'drop2'])
def test_requests_and_delays_as_the_sdk_itself(sdk, scenario, no_jitter):
    if not _sdk_available(sdk):
        pytest.skip(f'the {sdk} SDK is not installed')
    assert_same_pattern(run_backend(sdk, scenario), reference.run(sdk, scenario))


# --- Defaults and the option ------------------------------------------------------------------

@pytest.mark.parametrize('kind', ['openai', 'gemini'])
def test_llm_router_dialect_does_not_retry(kind):
    server = FailingServer([(500, {})] * 3, reference.SUCCESS[kind])
    try:
        backend = create_backend(kind, {'url': server.url})   # llm_router: attention's
        assert backend.retries is None
        with pytest.raises(BackendError):
            backend.generate(MODEL[kind], 'p', None, [], Limits(5))
        assert len(server.times) == 1
    finally:
        server.close()


def test_the_option_switches_the_retries(no_jitter):
    assert run_backend('openai', 'fail1_500', sdk_retries=False)['requests'] == 1
    assert run_backend('anthropic', 'fail1_500', sdk_retries='false')['requests'] == 1
    # Opted in on the llm_router dialect: the openai SDK's retries
    completion = json.dumps({'choices': [{'message': {'content': '{"id": 1}'}}]})
    server = FailingServer([(503, {})], (200, {}, completion))
    try:
        backend = create_backend('openai', {'url': server.url, 'sdk_retries': True})
        assert backend.generate('m', 'p', None, [], Limits(5)).text == '{"id": 1}'
        assert len(server.times) == 2
    finally:
        server.close()
    assert create_backend('anthropic', {}).retries is retries.ANTHROPIC_SDK
    assert create_backend('gemini', {'dialect': 'llm_manager'}).retries is retries.GENAI_SDK
    assert create_backend('ollama', {'dialect': 'llm_manager'}).options.get('sdk_retries') is None


def test_an_invalid_option_is_a_configuration_error():
    with pytest.raises(ConfigError):
        InterpretationServer(questions={'q': {'prompt': 'p', 'fields': ['int:id']}},
                             routes={'r': {'backend': 'anthropic', 'model': 'm',
                                           'sdk_retries': 'maybe'}})


def test_retry_after_forms():
    assert retries.retry_after_s({'retry-after-ms': '250', 'retry-after': '9'}) == 0.25
    assert retries.retry_after_s({'retry-after-ms': 'x', 'retry-after': '1.5'}) == 1.5
    date = time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(time.time() + 30))
    assert 25 < retries.retry_after_s({'retry-after': date}) <= 30
    assert retries.retry_after_s({'retry-after': 'soon'}) is None
    assert retries.retry_after_s({}) is None


# --- The limit of the call, the budget and cancellation ---------------------------------------

def _server(routes_options, log_dir=''):
    return InterpretationServer(
        questions={'q': {'prompt': 'p', 'fields': ['int:id'], 'route': 'claude'}},
        routes={'claude': {'backend': 'anthropic', 'model': 'claude-x', **routes_options}},
        log_dir=log_dir)


def _request(**fields):
    values = {'question': 'q', 'request_id': '', 'context': '', 'text': '', 'images': [],
              'budget_s': 0.0, 'variant': ''}
    values.update(fields)
    return SimpleNamespace(**values)


def test_retries_succeed_within_the_call_and_are_logged(tmp_path, no_jitter):
    server = FailingServer([(529, {}), (500, {})], reference.SUCCESS['anthropic'])
    try:
        s = _server({'url': server.url}, str(tmp_path))
        result = s.interpret(_request())
        assert (result.source, result.answer) == ('llm', '{"id": 1}')
        assert len(server.times) == 3
        attempt, = read_calls(str(tmp_path))[0]['attempts']
        assert attempt['sdk_retries'] == 2 and attempt['latency_s'] >= 1.4
    finally:
        server.close()


@pytest.mark.parametrize('limit', ['timeout_s', 'budget_s'])
def test_a_delay_past_the_limit_is_not_waited(limit, no_jitter):
    # Requests at 0 s and 0.5 s; the next delay (1 s) would pass the limit of 0.8 s
    server = FailingServer([(500, {})] * 5, reference.SUCCESS['anthropic'])
    try:
        options = {'url': server.url, 'timeout_s': 0.8 if limit == 'timeout_s' else 30.0}
        start = time.monotonic()
        result = _server(options).interpret(
            _request(budget_s=0.8 if limit == 'budget_s' else 0.0))
        elapsed = time.monotonic() - start
        assert (result.source, result.fallback_reason) == ('invalid', 'model_down')
        assert len(server.times) == 2 and elapsed < 0.8
    finally:
        server.close()


@pytest.mark.parametrize('limit, reason', [('timeout_s', 'timeout'), ('budget_s', 'budget')])
def test_a_retry_gets_only_the_time_left(limit, reason, no_jitter):
    # The retry (at 0.5 s) waits for an answer that takes 3 s: the call ends at its limit
    server = FailingServer([(500, {})], reference.SUCCESS['anthropic'], delay_s=3.0)
    try:
        options = {'url': server.url, 'timeout_s': 1.5 if limit == 'timeout_s' else 30.0}
        start = time.monotonic()
        result = _server(options).interpret(
            _request(budget_s=1.5 if limit == 'budget_s' else 0.0))
        elapsed = time.monotonic() - start
        assert (result.source, result.fallback_reason) == ('invalid', reason)
        assert len(server.times) == 2 and 1.4 <= elapsed < 2.2
    finally:
        server.close()


def test_a_cancelled_call_sends_no_further_request():
    server = FailingServer([(503, {'retry-after': '1'})] * 3, reference.SUCCESS['anthropic'])
    try:
        s = _server({'url': server.url})
        threading.Timer(0.3, s.cancel, args=('dialogue-1',)).start()
        start = time.monotonic()
        result = s.interpret(_request(request_id='dialogue-1'))
        assert result.fallback_reason == 'cancelled' and time.monotonic() - start < 0.8
        time.sleep(1.2)
        assert len(server.times) == 1
    finally:
        server.close()


def test_no_time_left_is_a_timeout():
    with pytest.raises(BackendTimeout):
        retries.call_with_retries(lambda left: None, 0.0, retries.OPENAI_SDK)
