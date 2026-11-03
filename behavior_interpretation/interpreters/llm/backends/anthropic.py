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
Claude models through the Claude API (POST <url>/v1/messages), over plain HTTP.

One dialect, `llm_manager`: the request of the interaction system's gateway (its Claude
provider, through the anthropic SDK's beta messages, without streaming):

- `max_tokens`: the call's cap, at least 4096 (thinking counts toward it); `messages`: one user
  message with the prompt (a list with the image blocks after the text when there are images);
  `system` when the route has a `system_prompt`.
- `output_config`: `effort` (route option, "low" by default; "" leaves it out) and, with a
  schema, `format` {type json_schema, schema with additionalProperties false on every object}.
- For the models that support it, the server-side refusal fallback: `fallbacks` "default" and
  the beta header `anthropic-beta: server-side-fallback-2026-07-01`.
- A request with a schema that the API rejects (HTTP 400) is sent once more without `format`,
  as the gateway does.

The body has the gateway's content; it is not compared byte for byte with the SDK's
serialization. The gateway's prompt caching (`cache_prefix_len`) is not reproduced: the
interpretation server sends no cache markers. Images are sent as PNG blocks (the gateway
sends none).

With `sdk_retries` (true by default, since the dialect is `llm_manager`), a failed request
is repeated as the anthropic SDK does (retries.py).

Options of the route: url (default https://api.anthropic.com), dialect (llm_manager),
api_key_env (default ANTHROPIC_API_KEY), effort, system_prompt, sdk_retries.
"""

import copy
import json

from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult
from behavior_interpretation.interpreters.llm.backends.retries import ANTHROPIC_SDK
from behavior_interpretation.interpreters.llm.backends.retries import route_policy
from behavior_interpretation.interpreters.llm.backends.transport import api_key_from
from behavior_interpretation.interpreters.llm.backends.transport import httpx_body
from behavior_interpretation.interpreters.llm.backends.transport import post_json

DEFAULT_URL = 'https://api.anthropic.com'
API_VERSION = '2023-06-01'
# Thinking tokens count toward max_tokens (the gateway's floor)
MIN_MAX_TOKENS = 4096
# The server-side refusal fallback, on the models that support it (the gateway's list)
FALLBACK_BETA = 'server-side-fallback-2026-07-01'
FALLBACK_MODELS = frozenset(
    {'claude-fable-5-1', 'claude-opus-5-5', 'claude-opus-5', 'claude-sonnet-5-5'})


def claude_schema(schema):
    """Return the schema with additionalProperties false on every object (as the gateway)."""
    def fix(node):
        if isinstance(node, list):
            return [fix(n) for n in node]
        if not isinstance(node, dict):
            return node
        out = {k: fix(v) for k, v in node.items()}
        if out.get('type') == 'object' or 'properties' in out:
            out['additionalProperties'] = False
        return out

    return fix(copy.deepcopy(schema))


class AnthropicBackend(Backend):

    kind = 'anthropic'

    def __init__(self, options=None, node=None):
        super().__init__(options, node)
        self.retries = route_policy(self, ANTHROPIC_SDK)

    def url(self):
        return (self.option('url') or DEFAULT_URL).rstrip('/') + '/v1/messages'

    def dialect(self):
        return self.option('dialect', 'llm_manager') or 'llm_manager'

    def build_payload(self, model, prompt, schema, images, limits):
        if self.dialect() != 'llm_manager':
            raise BackendError(f'anthropic: unknown dialect "{self.dialect()}"')
        content = prompt
        if images:
            content = [{'type': 'text', 'text': prompt}] + [
                {'type': 'image',
                 'source': {'type': 'base64', 'media_type': 'image/png', 'data': image.base64}}
                for image in images]
        payload = {
            'model': model,
            'max_tokens': max(limits.max_tokens, MIN_MAX_TOKENS),
            'messages': [{'role': 'user', 'content': content}],
            'output_config': {},
        }
        if self.option('system_prompt'):
            payload['system'] = self.option('system_prompt')
        effort = self.option('effort', 'low')
        if effort:
            payload['output_config']['effort'] = effort
        if schema:
            payload['output_config']['format'] = {'type': 'json_schema',
                                                  'schema': claude_schema(schema)}
        if model in FALLBACK_MODELS:
            payload['fallbacks'] = 'default'
        return payload

    def headers(self, model):
        key = api_key_from(self.options, ('ANTHROPIC_API_KEY',))
        if not key:
            raise BackendError('anthropic: no API key (ANTHROPIC_API_KEY, or api_key_env)')
        headers = {'x-api-key': key, 'anthropic-version': API_VERSION,
                   'Content-Type': 'application/json'}
        if model in FALLBACK_MODELS:
            headers['anthropic-beta'] = FALLBACK_BETA
        return headers

    def generate(self, model, prompt, schema, images, limits, question=''):
        payload = self.build_payload(model, prompt, schema, images, limits)
        headers = self.headers(model)
        try:
            answer = post_json(self.url(), httpx_body(payload), headers, limits.timeout_s,
                               self.retries, limits)
        except BackendError as exc:
            # A schema the API cannot compile: once more without it (the gateway's retry)
            if exc.status != 400 or 'format' not in payload['output_config']:
                raise
            payload['output_config'] = {k: v for k, v in payload['output_config'].items()
                                        if k != 'format'}
            answer = post_json(self.url(), httpx_body(payload), headers, limits.timeout_s,
                               self.retries, limits)
        if answer.get('type') == 'error' or answer.get('error'):
            raise BackendError(f'anthropic: {json.dumps(answer.get("error", answer))[:500]}')
        usage = answer.get('usage') or {}
        cache_read = usage.get('cache_read_input_tokens') or 0
        prompt_tokens = (usage.get('input_tokens') or 0) + cache_read + \
            (usage.get('cache_creation_input_tokens') or 0)
        if answer.get('stop_reason') == 'refusal':
            raise BackendError('anthropic: Claude declined the request')
        text = ''.join(block.get('text', '') for block in answer.get('content') or []
                       if block.get('type') == 'text').strip()
        if not text:
            raise BackendError(f'anthropic: empty reply (stop_reason='
                               f'{answer.get("stop_reason")})')
        return GenerateResult(text, prompt_tokens, usage.get('output_tokens', 0),
                              answer.get('model', model))
