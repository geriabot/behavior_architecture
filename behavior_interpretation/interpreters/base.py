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
The interface of an interpreter, and what it receives (Call) and returns (Outcome).

An interpreter answers a question, declines it (not applicable: the next interpreter of the
question's chain is tried) or fails (no valid answer: the question's fallback, or `invalid`).
Adding one means a subclass of Interpreter registered in the server under its name; neither
the service nor the callers change.
"""

import math
import time

from behavior_interpretation.validation import check_choice
from behavior_interpretation.validation import InvalidAnswer
from behavior_interpretation.validation import validate_fields


class Call:
    """One request, as the interpreters see it (the server keeps nothing of it after)."""

    def __init__(self, question, request_id='', context=None, text='', images=(),
                 budget_s=0.0, variant='', fields=()):
        self.question = question
        self.request_id = request_id
        self.context = context or {}
        self.text = text
        self.images = list(images)   # EncodedImage, only if the question uses them
        self.budget_s = float(budget_s or 0.0)
        self.variant = variant
        self.fields = list(fields)
        self.start = time.monotonic()
        self.details = {}            # what an interpreter adds to the call's log line

    def elapsed(self):
        return time.monotonic() - self.start

    def remaining(self):
        """Seconds left of the budget (infinite without a budget)."""
        if self.budget_s <= 0.0:
            return math.inf
        return self.budget_s - self.elapsed()


class Outcome:
    """What an interpreter returns: an answer, a decline or a failure (with its reason)."""

    ANSWERED = 'answered'
    DECLINED = 'declined'
    FAILED = 'failed'

    def __init__(self, kind, answer=None, reason='', message=''):
        self.kind = kind
        self.answer = answer
        self.reason = reason
        self.message = message

    @classmethod
    def answered(cls, answer, message=''):
        return cls(cls.ANSWERED, answer=answer, message=message)

    @classmethod
    def declined(cls, message=''):
        return cls(cls.DECLINED, message=message)

    @classmethod
    def failed(cls, reason, message=''):
        return cls(cls.FAILED, reason=reason, message=message)


def check_answer(question, call, answer):
    """
    Validate an answer for the question: fields, choices_from and the class's validate.

    A question with a raw schema and no fields only needs an object. Raises InvalidAnswer.
    """
    if not isinstance(answer, dict):
        raise InvalidAnswer('the answer is not an object')
    if call.fields:
        answer = validate_fields(answer, call.fields)
    choices = question.choices(call.context)
    if choices is not None:
        check_choice(answer, *choices)
    return question.validate(answer, call.context)


class Interpreter:
    """An interpreter. `option` is what the chain gives it (`rule:<option>`)."""

    name = ''

    def check(self, question, option):
        """Raise ConfigError when the server starts if this question cannot be answered."""

    def answer(self, question, call, option=''):
        raise NotImplementedError
