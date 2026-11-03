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
The `llm` interpreter: the question's prompt to the model of its route, and its answer.

One call: build the prompt (the question's template, or its class's build_prompt), call the
backend of the question's route with the structured-output schema (none for `parse: text`)
and the images (if the question uses them), read the text with the question's parse()
(loosely, strictly with `parse: strict`, as text with `parse: text`, or as its class reads it)
and validate (the fields, unless `validate: object`, choices_from and the class's validate).
A failed or invalid answer is retried up to the question's max_retries (0 = no retry). Each
attempt is limited by the route's timeout_s and by what is left of the request's budget
(budget_s 0 cuts nothing); when the budget is spent the call fails with the reason `budget`.
The token cap is the request's max_tokens, or the question's when the request gives none (0).

A route names a backend, a model and the backend's options:

  routes:
    text: {backend: ollama, model: qwen2.5vl:7b, url: http://localhost:11434, timeout_s: 180}
"""

import math
import time

from behavior_interpretation.fields import build_schema
from behavior_interpretation.interpreters.base import check_answer
from behavior_interpretation.interpreters.base import Interpreter
from behavior_interpretation.interpreters.base import Outcome
from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.llm.backends import BackendTimeout
from behavior_interpretation.interpreters.llm.backends import create_backend
from behavior_interpretation.interpreters.llm.backends import Limits
from behavior_interpretation.questions import ConfigError
from behavior_interpretation.questions import Question
from behavior_interpretation.validation import InvalidAnswer

DEFAULT_TIMEOUT_S = 120.0


class Route:
    """A backend and a model, with the backend's options and the limit of each call."""

    def __init__(self, name, config, node=None, backend=None):
        config = dict(config or {})
        self.name = name
        self.backend_type = config.pop('backend', '') or config.pop('type', '')
        self.model = config.pop('model', '')
        self.timeout_s = float(config.pop('timeout_s', DEFAULT_TIMEOUT_S) or DEFAULT_TIMEOUT_S)
        self.options = config
        if backend is not None:
            self.backend = backend
            self.backend_type = backend.kind
        else:
            if not self.backend_type:
                raise ConfigError(f'route {name}: no backend')
            try:
                self.backend = create_backend(self.backend_type, config, node)
            except (ValueError, BackendError) as exc:
                raise ConfigError(f'route {name}: {exc}') from exc

    def describe(self):
        return f'{self.name} ({self.backend_type}{", " + self.model if self.model else ""})'


class LLMInterpreter(Interpreter):

    name = 'llm'

    def __init__(self, routes, default_route=''):
        self.routes = routes
        self.default_route = default_route or (next(iter(routes)) if routes else '')

    def route_of(self, question):
        name = question.spec.get('route') or self.default_route
        if name not in self.routes:
            raise ConfigError(
                f'{question.name}: no route "{name}" (routes: {sorted(self.routes)})')
        return self.routes[name]

    def check(self, question, option):
        self.route_of(question)
        if type(question).build_prompt is Question.build_prompt:
            question.template_text()  # the template must exist

    def schema(self, question, call):
        if question.text_answer():
            return None  # no structured output: the model writes text
        schema = question.schema(call.context)
        if schema is not None:
            return schema
        return build_schema(call.fields, 'json')

    def answer(self, question, call, option=''):
        route = self.route_of(question)
        prompt = question.build_prompt(call.context, call.text)
        schema = self.schema(question, call)
        attempts = []
        call.details.update({'route': route.name, 'backend': route.backend_type,
                             'model': route.model, 'prompt': prompt, 'schema': schema,
                             'attempts': attempts})
        retries = max(0, int(question.spec.get('max_retries', 0) or 0))
        reason = ''
        message = ''
        for number in range(retries + 1):
            remaining = call.remaining()
            if remaining <= 0.0:
                reason = 'budget'
                message = f'the budget ({call.budget_s:.3f} s) was spent before attempt ' \
                          f'{number + 1}'
                break
            timeout = min(route.timeout_s, remaining)
            budget_bound = not math.isinf(remaining) and remaining < route.timeout_s
            limits = Limits(timeout, call.max_tokens or question.spec.get('max_tokens', 0))
            attempt = {'raw': None, 'error': ''}
            attempts.append(attempt)
            start = time.monotonic()
            try:
                result = route.backend.generate(route.model, prompt, schema, call.images,
                                                limits, question=question.name)
            except BackendTimeout as exc:
                attempt['error'] = 'timeout'
                attempt['latency_s'] = round(time.monotonic() - start, 4)
                reason = 'budget' if budget_bound else 'timeout'
                message = str(exc)
                continue
            except BackendError as exc:
                attempt['error'] = 'model_down'
                attempt['message'] = str(exc)
                attempt['latency_s'] = round(time.monotonic() - start, 4)
                reason = 'model_down'
                message = str(exc)
                continue
            attempt.update({'raw': result.text,
                            'latency_s': round(time.monotonic() - start, 4),
                            'prompt_tokens': result.prompt_tokens,
                            'completion_tokens': result.completion_tokens})
            call.details['model_version'] = result.model_version
            try:
                answer = check_answer(question, call, question.parse(result.text))
            except InvalidAnswer as exc:
                attempt['invalid'] = str(exc)
                reason = 'invalid'
                message = f'invalid answer: {exc}'
                continue
            return Outcome.answered(answer, f'{route.describe()}, attempt {number + 1}')
        return Outcome.failed(reason, message)
