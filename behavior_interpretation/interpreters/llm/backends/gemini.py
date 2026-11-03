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
The Gemini API (POST <url>/v1beta/models/<model>:generateContent), over plain HTTP.

Two dialects:

- `llm_router` (default), the request of attention's Gemini bridge (google_gemini_bridge_cpp):
  `contents` with the text part and one `inline_data` part per image, and `generationConfig`
  with responseMimeType application/json and the schema in Gemini's dialect (upper-case
  types), serialized as nlohmann's dump() (compact, keys sorted). Each image goes as the bridge
  sends it: converted to bgr8, not resized, JPEG at quality 95 by OpenCV, mime type image/jpeg.
- `llm_manager`, the request of the interaction system's gateway (google-genai SDK):
  role "user", an optional `systemInstruction` (system_prompt), and `generationConfig` with
  maxOutputTokens, responseMimeType, responseSchema and thinkingBudget 0. The SDK's body is
  reproduced as JSON content, not byte for byte.

With `sdk_retries` (by default in the `llm_manager` dialect only), the request is retried as
the google-genai SDK does by default: never (retries.py).

Options of the route: url (default https://generativelanguage.googleapis.com), dialect,
api_key_env (default GEMINI_API_KEY, then GOOGLE_API_KEY), system_prompt, sdk_retries.
"""

import json

from behavior_interpretation.fields import to_gemini_dialect
from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult
from behavior_interpretation.interpreters.llm.backends.retries import GENAI_SDK
from behavior_interpretation.interpreters.llm.backends.retries import route_policy
from behavior_interpretation.interpreters.llm.backends.transport import api_key_from
from behavior_interpretation.interpreters.llm.backends.transport import httpx_body
from behavior_interpretation.interpreters.llm.backends.transport import post_json
from behavior_interpretation.validation import strip_code_fence

DEFAULT_URL = 'https://generativelanguage.googleapis.com'


def nlohmann_body(payload):
    """Serialize as nlohmann::json::dump() does: compact, keys sorted, UTF-8 as it is."""
    return json.dumps(payload, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode('utf-8')


class GeminiBackend(Backend):

    kind = 'gemini'

    def __init__(self, options=None, node=None):
        super().__init__(options, node)
        self.retries = route_policy(self, GENAI_SDK)

    def url(self, model):
        base = (self.option('url') or DEFAULT_URL).rstrip('/')
        return f'{base}/v1beta/models/{model}:generateContent'

    def dialect(self):
        return self.option('dialect', 'llm_router') or 'llm_router'

    def build_payload(self, model, prompt, schema, images, limits):
        parts = [{'text': prompt}]
        for image in images:
            if self.dialect() == 'llm_router':
                # As attention's bridge: JPEG, quality 95, image/jpeg
                parts.append({'inline_data': {'mime_type': 'image/jpeg', 'data': image.jpeg_base64}})
            else:
                parts.append({'inline_data': {'mime_type': 'image/png', 'data': image.base64}})
        if self.dialect() == 'llm_router':
            config = {'responseMimeType': 'application/json'}
            if schema is not None:
                config['responseSchema'] = to_gemini_dialect(schema)
            return {'contents': [{'parts': parts}], 'generationConfig': config}
        if self.dialect() != 'llm_manager':
            raise BackendError(f'gemini: unknown dialect "{self.dialect()}"')
        payload = {'contents': [{'role': 'user', 'parts': parts}]}
        if self.option('system_prompt'):
            payload['systemInstruction'] = {'parts': [{'text': self.option('system_prompt')}]}
        config = {}
        if limits.max_tokens > 0:
            config['maxOutputTokens'] = limits.max_tokens
        if schema:
            config['responseMimeType'] = 'application/json'
            config['responseSchema'] = to_gemini_dialect(schema)
        config['thinkingConfig'] = {'thinkingBudget': 0}
        payload['generationConfig'] = config
        return payload

    def body(self, payload):
        return nlohmann_body(payload) if self.dialect() == 'llm_router' else httpx_body(payload)

    def headers(self):
        key = api_key_from(self.options, ('GEMINI_API_KEY', 'GOOGLE_API_KEY'))
        if not key:
            raise BackendError('gemini: no API key (GEMINI_API_KEY, or api_key_env)')
        return {'x-goog-api-key': key, 'Content-Type': 'application/json'}

    def generate(self, model, prompt, schema, images, limits, question=''):
        payload = self.build_payload(model, prompt, schema, images, limits)
        answer = post_json(self.url(model), self.body(payload), self.headers(), limits.timeout_s,
                           self.retries, limits)
        if answer.get('error'):
            raise BackendError(f'gemini: {answer["error"]}')
        text = None
        for candidate in answer.get('candidates') or []:
            for part in (candidate.get('content') or {}).get('parts') or []:
                if 'text' in part and not part.get('thought'):
                    text = part['text']
                    break
            if text is not None:
                break
        if text is None:
            raise BackendError('gemini: no candidates in the answer')
        usage = answer.get('usageMetadata') or {}
        if self.dialect() != 'llm_router':
            text = strip_code_fence(text)
        # llm_router: the text as attention's bridge returned it (llm_router parsed it without
        # removing a code fence; parse: loose removes it)
        return GenerateResult(text, usage.get('promptTokenCount', 0),
                              usage.get('candidatesTokenCount', 0),
                              answer.get('modelVersion', model))
