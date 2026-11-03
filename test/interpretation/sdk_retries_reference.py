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
Record how an official SDK retries, against FailingServer (127.0.0.1, no network).

For each scenario of SCENARIOS, the SDK makes the call that the interaction system's gateway
makes (openai: a streamed chat completion; anthropic: a message; google-genai:
generate_content), with its default retries, against a server that fails as scripted. The
number of requests, the seconds between them and the outcome are written as JSON. The
SDK's jitter is switched off (random() = 0), so that the delays are the SDK's base delays;
the scenario `jitter` keeps it.

    python sdk_retries_reference.py openai|anthropic|gemini > reference.json

test_sdk_retries.py compares the backends with the recorded output (sdk_retries_reference.json)
and, when the SDK can be imported, with the SDK itself.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from failing_server import FailingServer  # noqa: E402

E500 = (500, {})
SCENARIOS = {
    'fail1_500': [E500],
    'fail2_503': [(503, {})] * 2,
    'fail5_500': [E500] * 5,
    'statuses_408_409': [(408, {}), (409, {})],
    'status_400': [(400, {})],
    'status_404': [(404, {})],
    'retry_after_s': [(429, {'retry-after': '1'})],
    'retry_after_ms': [(429, {'retry-after-ms': '300'})],
    'retry_after_long': [(429, {'retry-after': '200'})],
    'retry_after_zero': [(503, {'retry-after': '0'})],
    'should_retry_false': [(500, {'x-should-retry': 'false'})],
    'should_retry_true': [(400, {'x-should-retry': 'true'})],
    'drop2': ['drop', 'drop'],
    'jitter': [E500] * 2,
}

OPENAI_STREAM = (
    'data: {"id":"c","object":"chat.completion.chunk","created":0,"model":"m","choices":'
    '[{"index":0,"delta":{"content":"{\\"id\\": 1}"},"finish_reason":null}]}\n\n'
    'data: {"id":"c","object":"chat.completion.chunk","created":0,"model":"m","choices":[],'
    '"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}}\n\n'
    'data: [DONE]\n\n')
ANTHROPIC_REPLY = json.dumps({
    'id': 'msg', 'type': 'message', 'role': 'assistant', 'model': 'claude-x',
    'content': [{'type': 'text', 'text': '{"id": 1}'}], 'stop_reason': 'end_turn',
    'stop_sequence': None, 'usage': {'input_tokens': 3, 'output_tokens': 2}})
GEMINI_REPLY = json.dumps({
    'candidates': [{'content': {'role': 'model', 'parts': [{'text': '{"id": 1}'}]}}],
    'usageMetadata': {'promptTokenCount': 3, 'candidatesTokenCount': 2},
    'modelVersion': 'gemini-x'})

SUCCESS = {
    'openai': (200, {'Content-Type': 'text/event-stream'}, OPENAI_STREAM),
    'anthropic': (200, {}, ANTHROPIC_REPLY),
    'gemini': (200, {}, GEMINI_REPLY),
}


def _openai(url):
    import openai
    client = openai.OpenAI(api_key='unused', base_url=url + '/v1', timeout=10.0)

    def call():
        stream = client.chat.completions.create(
            model='m', messages=[{'role': 'user', 'content': 'p'}], stream=True,
            stream_options={'include_usage': True})
        return ''.join(c.choices[0].delta.content or '' for c in stream if c.choices)
    return openai, call


def _anthropic(url):
    import anthropic
    client = anthropic.Anthropic(api_key='unused', base_url=url, timeout=10.0)

    def call():
        message = client.messages.create(model='claude-x', max_tokens=4096,
                                         messages=[{'role': 'user', 'content': 'p'}])
        return message.content[0].text
    return anthropic, call


def _gemini(url):
    from google import genai
    from google.genai import types
    client = genai.Client(api_key='unused', http_options=types.HttpOptions(base_url=url))

    def call():
        return client.models.generate_content(model='gemini-x', contents='p').text
    return genai, call


CLIENTS = {'openai': _openai, 'anthropic': _anthropic, 'gemini': _gemini}
# The module whose random() draws the SDK's jitter (google-genai does not retry by default)
RANDOM = {'openai': 'openai._base_client', 'anthropic': 'anthropic._base_client', 'gemini': ''}


def run(sdk, scenario, jitter=False):
    """Run one scenario with the SDK: {requests, gaps, outcome}."""
    server = FailingServer(SCENARIOS[scenario], SUCCESS[sdk])
    module, call = CLIENTS[sdk](server.url)
    random_module = sys.modules.get(RANDOM[sdk])
    saved = getattr(random_module, 'random', None)
    if not jitter and saved is not None:
        random_module.random = lambda: 0.0
    try:
        call()
        outcome = 'ok'
    except Exception as exc:  # noqa: B902 (the outcome is the SDK's exception)
        outcome = type(exc).__name__
    finally:
        if saved is not None:
            random_module.random = saved
        server.close()
    return {'requests': len(server.times), 'gaps': server.gaps(), 'outcome': outcome}


def reference(sdk):
    module, _ = CLIENTS[sdk]('http://127.0.0.1:9')
    result = {'version': getattr(module, '__version__', ''), 'scenarios': {}}
    for scenario in SCENARIOS:
        result['scenarios'][scenario] = run(sdk, scenario, jitter=scenario == 'jitter')
    return result


if __name__ == '__main__':
    print(json.dumps({sys.argv[1]: reference(sys.argv[1])}, indent=1))
