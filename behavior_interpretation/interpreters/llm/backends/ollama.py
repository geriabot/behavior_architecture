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
Ollama's native API (POST <url>/api/chat), local or remote.

Two dialects, each the request of one of the current gateways, key for key and byte for byte:

- `llm_router` (default), attention's router: `format` (the schema with
  additionalProperties false), `stream` false, `options.temperature` (0 by default) and
  `num_ctx`, `keep_alive` only when set; no token cap. The images go in the message, as
  `images: [png, ...]` (none: no `images` key).
- `llm_manager`, the interaction system's gateway: an optional `system_prompt` message,
  `stream` (true by default, as the gateway streams), `keep_alive` (-1 by default), `think`
  (false by default; a level such as "low" for the models that cannot switch it off),
  `format` as given, and `options` with `num_predict` (max_tokens, plus 4096 when the model
  may think), `num_ctx` (8192 by default) and temperature 0 when there is a schema. The
  body is serialized as httpx does.

Options of the route: url (default http://localhost:11434), dialect, temperature, num_ctx,
keep_alive, think, stream, system_prompt, api_key_env (only behind a proxy that needs a key).
`cache_prefix_len` is accepted and ignored, as in the interaction system's gateway (Ollama
reuses the common prefix of the prompts by itself).

warm_up() (route option warmup) loads the model as the gateway did when it started
(OllamaProvider.ensure_model_ready): POST <url>/api/generate with an empty prompt, keep_alive
-1 (or the route's keep_alive) and the num_ctx of the route's requests, up to 3 attempts 1 s
and 2 s apart, 180 s each; an HTTP error is not retried.
"""

import json
import time

from behavior_interpretation.fields import strict_schema
from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult
from behavior_interpretation.interpreters.llm.backends.transport import api_key_from
from behavior_interpretation.interpreters.llm.backends.transport import httpx_body
from behavior_interpretation.interpreters.llm.backends.transport import post_json
from behavior_interpretation.interpreters.llm.backends.transport import post_lines
from behavior_interpretation.interpreters.llm.backends.transport import post_text
from behavior_interpretation.interpreters.llm.backends.transport import urllib_body
from behavior_interpretation.validation import strip_code_fence

# Tokens added to num_predict when the model may think (interaction system's gateway)
REASONING_ALLOWANCE = 4096
DEFAULT_NUM_CTX = 8192
# The gateway's warm-up: the delays between its 3 attempts, and the limit of each
WARMUP_DELAYS_S = (1.0, 2.0)
WARMUP_TIMEOUT_S = 180.0


def normalize_think(think):
    """Read `think` as a bool, or a reasoning level ("low"...); "true"/"false" are bools."""
    if isinstance(think, str):
        text = think.strip().lower()
        if text in ('', 'false', '0', 'no', 'off', 'none'):
            return False
        if text in ('true', '1', 'yes', 'on'):
            return True
        return text
    return bool(think)


class OllamaBackend(Backend):

    kind = 'ollama'

    def url(self):
        return (self.option('url') or 'http://localhost:11434').rstrip('/') + '/api/chat'

    def dialect(self):
        return self.option('dialect', 'llm_router') or 'llm_router'

    def build_payload(self, model, prompt, schema, images, limits):
        if self.dialect() == 'llm_manager':
            return self._payload_llm_manager(model, prompt, schema, images, limits)
        if self.dialect() != 'llm_router':
            raise BackendError(f'ollama: unknown dialect "{self.dialect()}"')
        return self._payload_llm_router(model, prompt, schema, images)

    def _payload_llm_router(self, model, prompt, schema, images):
        message = {'role': 'user', 'content': prompt}
        if images:
            message['images'] = [image.base64 for image in images]
        payload = {'model': model, 'messages': [message]}
        if schema is not None:
            payload['format'] = strict_schema(schema)
        payload['stream'] = False
        payload['options'] = {'temperature': float(self.option('temperature', 0.0))}
        if self.option('num_ctx'):
            payload['options']['num_ctx'] = int(self.option('num_ctx'))
        if self.option('keep_alive'):
            payload['keep_alive'] = self.option('keep_alive')
        return payload

    def _payload_llm_manager(self, model, prompt, schema, images, limits):
        think = normalize_think(self.option('think', False))
        messages = []
        if self.option('system_prompt'):
            messages.append({'role': 'system', 'content': self.option('system_prompt')})
        message = {'role': 'user', 'content': prompt}
        if images:
            message['images'] = [image.base64 for image in images]
        messages.append(message)
        payload = {
            'model': model,
            'messages': messages,
            'stream': bool(self.option('stream', True)),
            'keep_alive': self.option('keep_alive', -1),
            'think': think,
        }
        options = {}
        if limits.max_tokens > 0:
            options['num_predict'] = limits.max_tokens + (REASONING_ALLOWANCE if think else 0)
        num_ctx = int(self.option('num_ctx', DEFAULT_NUM_CTX) or 0)
        if num_ctx:
            options['num_ctx'] = num_ctx
        if schema:
            payload['format'] = schema
            options['temperature'] = 0
        if options:
            payload['options'] = options
        return payload

    def num_ctx(self):
        """Return the context length of the route's requests (0: Ollama's own)."""
        if self.dialect() == 'llm_manager':
            return int(self.option('num_ctx', DEFAULT_NUM_CTX) or 0)
        return int(self.option('num_ctx') or 0)

    def warm_up(self, model):
        payload = {'model': model, 'prompt': '', 'keep_alive': self.option('keep_alive', -1)}
        if self.num_ctx():
            # Loaded with the context of the requests (otherwise the first one reloads it)
            payload['options'] = {'num_ctx': self.num_ctx()}
        url = (self.option('url') or 'http://localhost:11434').rstrip('/') + '/api/generate'
        error = None
        for attempt in range(len(WARMUP_DELAYS_S) + 1):
            try:
                post_text(url, httpx_body(payload), self.headers(), WARMUP_TIMEOUT_S)
                return f"Model '{model}' loaded into memory."
            except BackendError as exc:
                if exc.status:
                    raise BackendError(f'Load failed: HTTP {exc.status}') from exc
                error = exc
            if attempt < len(WARMUP_DELAYS_S):
                time.sleep(WARMUP_DELAYS_S[attempt])
        raise BackendError(f'Failed after {len(WARMUP_DELAYS_S) + 1} attempts: {error}')

    def body(self, payload):
        return httpx_body(payload) if self.dialect() == 'llm_manager' else urllib_body(payload)

    def headers(self):
        headers = {'Content-Type': 'application/json'}
        key = api_key_from(self.options)
        if key:
            headers['Authorization'] = f'Bearer {key}'
        return headers

    def generate(self, model, prompt, schema, images, limits, question=''):
        payload = self.build_payload(model, prompt, schema, images, limits)
        body = self.body(payload)
        if payload.get('stream'):
            return self._streamed(body, limits)
        answer = post_json(self.url(), body, self.headers(), limits.timeout_s)
        if 'error' in answer:
            raise BackendError(f'ollama: {answer["error"]}')
        text = (answer.get('message') or {}).get('content') or ''
        return GenerateResult(strip_code_fence(text), answer.get('prompt_eval_count', 0),
                              answer.get('eval_count', 0), answer.get('model', model))

    def _streamed(self, body, limits):
        text = ''
        prompt_tokens = completion_tokens = 0
        version = ''
        for line in post_lines(self.url(), body, self.headers(), limits.timeout_s):
            try:
                chunk = json.loads(line)
            except json.JSONDecodeError:
                continue
            if 'error' in chunk:
                raise BackendError(f'ollama: {chunk["error"]}')
            text += (chunk.get('message') or {}).get('content') or ''
            version = chunk.get('model', version)
            if chunk.get('done'):
                prompt_tokens = chunk.get('prompt_eval_count', 0)
                completion_tokens = chunk.get('eval_count', 0)
        return GenerateResult(text, prompt_tokens, completion_tokens, version)
