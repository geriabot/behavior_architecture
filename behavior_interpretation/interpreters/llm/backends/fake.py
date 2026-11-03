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
Scripted answers, for tests and for running a system without a model.

Options of the route:
  answers      the answers, in order (the last one is repeated); an object is sent as JSON
  by_question  {question: [answers]}, used before `answers` for that question
  delay_s      how long each answer takes (a call whose limit is shorter times out)
  model_version  what the answers say answered them (default "fake")
An answer "!timeout" raises a timeout and "!down" a model that cannot be reached.

In Python, `script` may also be a function(model, prompt, schema, images, limits, question)
that returns the text. Every call is kept in `calls`, for tests.
"""

import json
import threading
import time

from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult


class FakeBackend(Backend):

    kind = 'fake'

    def __init__(self, options=None, node=None, script=None):
        super().__init__(options, node)
        self.script = script
        self.calls = []
        self._next = {}
        self._lock = threading.Lock()

    def _scripted(self, question):
        by_question = self.option('by_question', {}) or {}
        key = question if question in by_question else ''
        answers = by_question[question] if key else self.option('answers', []) or []
        if not isinstance(answers, list):
            answers = [answers]
        if not answers:
            raise BackendError('fake: no scripted answer')
        with self._lock:
            index = self._next.get(key, 0)
            self._next[key] = index + 1
        answer = answers[min(index, len(answers) - 1)]
        return answer if isinstance(answer, str) else json.dumps(answer)

    def generate(self, model, prompt, schema, images, limits, question=''):
        with self._lock:
            self.calls.append({'model': model, 'prompt': prompt, 'schema': schema,
                               'images': list(images), 'limits': limits, 'question': question})
        delay = float(self.option('delay_s', 0.0) or 0.0)
        if delay > 0.0:
            time.sleep(min(delay, limits.timeout_s))
            if delay > limits.timeout_s:
                raise BackendTimeout(f'fake: no answer in {limits.timeout_s:.3f} s')
        if self.script is not None:
            text = self.script(model=model, prompt=prompt, schema=schema, images=images,
                               limits=limits, question=question)
        else:
            text = self._scripted(question)
        if text == '!timeout':
            raise BackendTimeout('fake: scripted timeout')
        if text == '!down':
            raise BackendError('fake: scripted model down')
        return GenerateResult(text, len(prompt.split()), len(str(text).split()),
                              self.option('model_version', 'fake'))
