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
Any OpenAI-compatible API (POST <url>/chat/completions): OpenAI, vLLM, llama.cpp, Groq...

Two dialects, each the request of one of the current gateways:

- `llm_router` (default), attention's router: the content is the text followed by one
  `image_url` part per image (none: the text part alone), `temperature` (0 by default) and
  `response_format` json_schema named "answer", strict, with additionalProperties false.
- `llm_manager`, the interaction system's gateway (the openai SDK): the content is the
  prompt (a list with the image parts only when there are images), an optional
  `system_prompt` message, `stream` with usage, `reasoning_effort` ("none" by default),
  `max_completion_tokens` (at least 4096 when reasoning is on) and `response_format` named
  "response", strict only when every property is required. The body is the one the openai
  Python SDK sends, byte for byte: its keys in the SDK's order (model, stream, messages, then
  the others in alphabetical order), serialized as httpx does.

With `sdk_retries` (by default in the `llm_manager` dialect only), a request that fails
before the answer starts is repeated as the openai SDK does (retries.py).

Options of the route: url (base, default https://api.openai.com/v1), dialect, api_key_env
(default OPENAI_API_KEY, if set), temperature, reasoning_effort, system_prompt, sdk_retries.
"""

import copy
import json

from behavior_interpretation.fields import strict_schema
from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult
from behavior_interpretation.interpreters.llm.backends.retries import OPENAI_SDK
from behavior_interpretation.interpreters.llm.backends.retries import route_policy
from behavior_interpretation.interpreters.llm.backends.transport import api_key_from
from behavior_interpretation.interpreters.llm.backends.transport import httpx_body
from behavior_interpretation.interpreters.llm.backends.transport import post_json
from behavior_interpretation.interpreters.llm.backends.transport import post_lines
from behavior_interpretation.interpreters.llm.backends.transport import urllib_body
from behavior_interpretation.validation import strip_code_fence

_MIN_REASONING_MAX_TOKENS = 4096


def manager_strict_schema(schema):
    """
    Prepare the schema for Structured Outputs as the interaction system's gateway does.

    additionalProperties false on every object; strict only if every property is required.
    """
    strict = True

    def fix(node):
        nonlocal strict
        if isinstance(node, list):
            return [fix(n) for n in node]
        if not isinstance(node, dict):
            return node
        out = {k: fix(v) for k, v in node.items()}
        if out.get('type') == 'object' or 'properties' in out:
            out['additionalProperties'] = False
            if set(out.get('properties', {})) - set(out.get('required', [])):
                strict = False
        return out

    return fix(copy.deepcopy(schema)), strict


# The order in which the openai SDK writes the first keys of a chat completion (the others
# follow in alphabetical order), as chat.completions.create() builds the body (openai 3.39)
SDK_FIRST_KEYS = ('model', 'stream', 'service_tier', 'messages')


def sdk_ordered(payload):
    """Return the payload with its keys in the order of the openai SDK's request body."""
    first = [key for key in SDK_FIRST_KEYS if key in payload]
    rest = sorted(key for key in payload if key not in SDK_FIRST_KEYS)
    return {key: payload[key] for key in first + rest}


def _image_parts(images):
    return [{'type': 'image_url', 'image_url': {'url': f'data:image/png;base64,{image.base64}'}}
            for image in images]


class OpenAIBackend(Backend):

    kind = 'openai'

    def __init__(self, options=None, node=None):
        super().__init__(options, node)
        self.retries = route_policy(self, OPENAI_SDK)

    def url(self):
        return (self.option('url') or 'https://api.openai.com/v1').rstrip('/') + \
            '/chat/completions'

    def dialect(self):
        return self.option('dialect', 'llm_router') or 'llm_router'

    def build_payload(self, model, prompt, schema, images, limits):
        if self.dialect() == 'llm_manager':
            return self._payload_llm_manager(model, prompt, schema, images, limits)
        if self.dialect() != 'llm_router':
            raise BackendError(f'openai: unknown dialect "{self.dialect()}"')
        content = [{'type': 'text', 'text': prompt}] + _image_parts(images)
        payload = {
            'model': model,
            'messages': [{'role': 'user', 'content': content}],
            'temperature': float(self.option('temperature', 0.0)),
        }
        if schema is not None:
            payload['response_format'] = {
                'type': 'json_schema',
                'json_schema': {'name': 'answer', 'schema': strict_schema(schema), 'strict': True},
            }
        return payload

    def _payload_llm_manager(self, model, prompt, schema, images, limits):
        effort = self.option('reasoning_effort', 'none')
        reasoning = effort not in ('', 'none')
        messages = []
        if self.option('system_prompt'):
            messages.append({'role': 'system', 'content': self.option('system_prompt')})
        content = prompt
        if images:
            content = [{'type': 'text', 'text': prompt}] + _image_parts(images)
        messages.append({'role': 'user', 'content': content})
        payload = {
            'model': model,
            'messages': messages,
            'stream': True,
            'stream_options': {'include_usage': True},
        }
        if effort:
            payload['reasoning_effort'] = effort
        if limits.max_tokens > 0:
            payload['max_completion_tokens'] = (
                max(limits.max_tokens, _MIN_REASONING_MAX_TOKENS) if reasoning
                else limits.max_tokens)
        if schema:
            fixed, strict = manager_strict_schema(schema)
            payload['response_format'] = {
                'type': 'json_schema',
                'json_schema': {'name': 'response', 'schema': fixed, 'strict': strict},
            }
        return sdk_ordered(payload)

    def headers(self):
        headers = {'Content-Type': 'application/json'}
        key = api_key_from(self.options, ('OPENAI_API_KEY',) if not self.option('url') else ())
        if key:
            headers['Authorization'] = f'Bearer {key}'
        return headers

    def generate(self, model, prompt, schema, images, limits, question=''):
        payload = self.build_payload(model, prompt, schema, images, limits)
        if payload.get('stream'):
            return self._streamed(httpx_body(payload), limits, model)
        answer = post_json(self.url(), urllib_body(payload), self.headers(), limits.timeout_s,
                           self.retries, limits)
        if answer.get('error'):
            raise BackendError(f'openai: {answer["error"]}')
        choices = answer.get('choices') or [{}]
        text = (choices[0].get('message') or {}).get('content') or ''
        usage = answer.get('usage') or {}
        return GenerateResult(strip_code_fence(text), usage.get('prompt_tokens', 0),
                              usage.get('completion_tokens', 0), answer.get('model', model))

    def _streamed(self, body, limits, model):
        text = ''
        refusal = ''
        usage = {}
        version = model
        for line in post_lines(self.url(), body, self.headers(), limits.timeout_s, self.retries,
                               limits):
            if not line.startswith('data:'):
                continue
            data = line[len('data:'):].strip()
            if data == '[DONE]':
                break
            try:
                chunk = json.loads(data)
            except json.JSONDecodeError:
                continue
            if chunk.get('error'):
                raise BackendError(f'openai: {chunk["error"]}')
            version = chunk.get('model', version)
            usage = chunk.get('usage') or usage
            for choice in chunk.get('choices') or []:
                delta = choice.get('delta') or {}
                text += delta.get('content') or ''
                refusal += delta.get('refusal') or ''
        if refusal:
            raise BackendError(f'openai declined the request: {refusal}')
        return GenerateResult(text.strip(), usage.get('prompt_tokens', 0),
                              usage.get('completion_tokens', 0), version)
