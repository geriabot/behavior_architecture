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
Answers replayed from call logs (call_log.py), by question and prompt.

Every attempt of a logged call is replayed in order: its raw answer, or its error (a timeout
is replayed as a timeout), so that a system behaves as it did when the log was written. When
the same question and prompt were logged more than once, the answers are given in the order
of the log, and the last one is repeated. A call that is not in the logs fails (model_down)
with a message that says so.

Options of the route: log_files (a file or a directory with calls.jsonl, or a list of them).
"""

import collections
import threading

from behavior_interpretation.call_log import read_calls
from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult


class RecordedBackend(Backend):

    kind = 'recorded'

    def __init__(self, options=None, node=None):
        super().__init__(options, node)
        files = self.option('log_files', [])
        if isinstance(files, str):
            files = [files]
        files = [f for f in files if f]
        if not files:
            raise BackendError('recorded: no log_files')
        self._answers = collections.defaultdict(list)
        self._next = collections.Counter()
        self._lock = threading.Lock()
        for path in files:
            for call in read_calls(path):
                if 'prompt' not in call:
                    continue  # answered without the model (a rule, or a malformed request)
                key = (call.get('question', ''), call['prompt'])
                for attempt in call.get('attempts') or []:
                    self._answers[key].append((attempt, call.get('model_version', '')))

    def __len__(self):
        return sum(len(answers) for answers in self._answers.values())

    def generate(self, model, prompt, schema, images, limits, question=''):
        key = (question, prompt)
        with self._lock:
            answers = self._answers.get(key)
            if not answers:
                raise BackendError(f'recorded: no recorded answer for the question "{question}" '
                                   'with this prompt')
            index = min(self._next[key], len(answers) - 1)
            self._next[key] += 1
        attempt, version = answers[index]
        error = attempt.get('error') or ''
        if error == 'timeout':
            raise BackendTimeout('recorded: the recorded call timed out')
        if error or attempt.get('raw') is None:
            raise BackendError(f'recorded: the recorded call failed ({error or "no answer"})')
        return GenerateResult(attempt['raw'], attempt.get('prompt_tokens', 0),
                              attempt.get('completion_tokens', 0), version)
