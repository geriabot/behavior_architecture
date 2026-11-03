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
The `rule` interpreter: deterministic code instead of a model.

The rule is the one named in the chain (`rule:most_centered`), else the question's `rule`,
else the question class's answer_without_model(context). Rules are registered by the question
modules (`@rule('most_centered')`) and receive the Call (question, context, text, images,
fields). They return the answer (an object, or the value of the only field) or None to
decline, for example when the call is not one they apply to.
"""

from behavior_interpretation.interpreters.base import check_answer
from behavior_interpretation.interpreters.base import Interpreter
from behavior_interpretation.interpreters.base import Outcome
from behavior_interpretation.questions import ConfigError
from behavior_interpretation.questions import Question
from behavior_interpretation.validation import InvalidAnswer


class RuleInterpreter(Interpreter):

    name = 'rule'

    def __init__(self, rules):
        self.rules = rules

    def _rule_name(self, question, option):
        return option or question.spec.get('rule') or ''

    def check(self, question, option):
        name = self._rule_name(question, option)
        if name and name not in self.rules:
            raise ConfigError(f'{question.name}: no rule "{name}" in the question modules '
                              f'(rules: {sorted(self.rules)})')
        if not name and type(question).answer_without_model is Question.answer_without_model:
            raise ConfigError(f'{question.name}: the rule interpreter needs a rule (rule: or '
                              'rule:<name> in the chain) or a class with answer_without_model')

    def answer(self, question, call, option=''):
        name = self._rule_name(question, option)
        call.details['rule'] = name or 'answer_without_model'
        try:
            if name:
                answer = self.rules[name](call)
            else:
                answer = question.answer_without_model(call.context)
        except InvalidAnswer as exc:
            return Outcome.failed('invalid', f'rule {name}: {exc}')
        if answer is None:
            return Outcome.declined(f'rule {name or "answer_without_model"} declined')
        if not isinstance(answer, dict):
            if len(call.fields) != 1:
                return Outcome.failed('invalid', f'rule {name}: a value that is not an object '
                                      'needs exactly one field')
            answer = {call.fields[0].name: answer}
        try:
            return Outcome.answered(check_answer(question, call, answer))
        except InvalidAnswer as exc:
            return Outcome.failed('invalid', f'rule {name}: {exc}')
