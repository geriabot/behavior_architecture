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
The questions a system declares: their definition in YAML and, optionally, a class.

```yaml
questions:
  identify_target:
    interpreter: llm             # or rule, or a chain: [rule, llm] / "rule:most_centered,llm"
    template: additional_information_with_image   # key of the templates file (or `prompt:`)
    fields: ["int:id"]           # the answer's fields; or fields_from: context.inputs;
                                 # or schema: {...} (a JSON schema sent as it is)
    choices_from: context.detection_ids   # the (single, or choices_field) field must be one
    choices_extra: [-1]          #   of these, or of choices_extra (default [-1])
    uses_image: optional         # true, false (default) or optional
    max_images: 1                # default 1
    max_retries: 0               # default 0: no retry
    fallback: none               # none (the result is `invalid`) or a value
    route: behavior_input_with_image   # the llm interpreter's route (default_route if absent)
    max_tokens: 50               # 0: no cap
    rule: most_centered          # the rule of the `rule` interpreter (default: the class's
                                 #   answer_without_model)
    output_formatter: ''         # a formatter registered by a question module
    class: ''                    # a Question subclass of a question module
    variants:
      no_image: {template: additional_information, uses_image: false}
```

A question module (server parameter `question_modules`) may define Question subclasses (bound
to a question by their `name`, or by the `class` of its definition), rules (functions
decorated with `@rule('name')`, or a module-level `RULES = {name: function}`) and output
formatters (`@formatter('name')`, or `FORMATTERS`).
"""

import copy
import importlib
import importlib.util
import inspect
import os

from behavior_interpretation.fields import dump_like_nlohmann
from behavior_interpretation.fields import output_format_json
from behavior_interpretation.fields import parse_fields
from behavior_interpretation.templates import fill_template
from behavior_interpretation.templates import slot_value
from behavior_interpretation.validation import choices_of
from behavior_interpretation.validation import resolve_path

DEFAULTS = {
    'interpreter': 'llm',
    'template': '',
    'prompt': '',
    'fields': None,
    'fields_from': '',
    'schema': None,
    'choices_from': '',
    'choices_field': '',
    'choices_extra': [-1],
    'uses_image': False,
    'max_images': 1,
    'max_retries': 0,
    'fallback': None,
    'route': '',
    'max_tokens': 0,
    'rule': '',
    'output_formatter': '',
    'class': '',
    'variants': {},
}

USES_IMAGE = {True: 'true', False: 'false', 'true': 'true', 'false': 'false',
              'optional': 'optional'}


class ConfigError(ValueError):
    """A question, route or module that the server cannot use (found when it starts)."""


class RequestError(ValueError):
    """A request the question cannot take (a malformed context, a missing image...)."""


def rule(name):
    """Register the decorated function as a rule: f(call) returns an answer or None."""
    def register(function):
        function.interpretation_rule = name
        return function
    return register


def formatter(name):
    """Register the decorated function as an output formatter: f(fields) returns text."""
    def register(function):
        function.interpretation_formatter = name
        return function
    return register


def parse_interpreters(value):
    """[(interpreter, option)] of `llm`, `rule:most_centered,llm`, [rule, llm]..."""
    if value is None or value == '':
        value = 'llm'
    if isinstance(value, str):
        items = value.split(',')
    elif isinstance(value, (list, tuple)):
        items = list(value)
    else:
        raise ConfigError(f'interpreter: cannot read {value!r}')
    chain = []
    for item in items:
        item = str(item).strip()
        if not item:
            continue
        name, _, option = item.partition(':')
        chain.append((name.strip(), option.strip()))
    if not chain:
        raise ConfigError(f'interpreter: no interpreter in {value!r}')
    return chain


def _fallback_value(value):
    """None for `none` (or no fallback), else the value."""
    if value is None or (isinstance(value, str) and value.strip().lower() in ('none', '')):
        return None
    return value


class Question:
    """
    A question: its definition and the hooks a system may override in a subclass.

    The hooks receive the request's context (a dict) and text. Overriding them is only
    needed for questions with logic of their own; the definition covers the rest.
    """

    # The question this class answers, when the definition does not name it with `class`
    name = ''

    def __init__(self, name, spec, templates, formatters=None):
        self.name = name
        self.spec = spec
        self.templates = templates
        self.formatters = formatters or {}

    # --- Hooks ------------------------------------------------------------------------------

    def build_prompt(self, context, text):
        """
        Build the prompt: the question's template with the request's values in its slots.

        The slots are the keys of the context (strings as they are, the rest as JSON),
        `text` and `output_format` (the block of format_output).
        """
        template = self.template_text()
        values = {key: slot_value(value) for key, value in context.items()}
        if text or 'text' not in values:
            values['text'] = text
        values['output_format'] = self.output_format(context)
        return fill_template(template, values)

    def format_output(self, fields):
        """Write the `{output_format}` block of these fields (default: attention's JSON)."""
        name = self.spec.get('output_formatter')
        if name:
            return self.formatters[name](fields)
        return output_format_json(fields)

    def validate(self, answer, context):
        """Check the answer and return it (raise InvalidAnswer to reject it)."""
        return answer

    def fallback(self, context, reason):
        """Return the answer when no valid one came back, or None (the result: `invalid`)."""
        value = copy.deepcopy(_fallback_value(self.spec.get('fallback')))
        if value is None:
            return None
        if not isinstance(value, dict):
            fields = self.fields(context)
            if len(fields) != 1:
                raise ConfigError(f'{self.name}: a fallback that is not an object needs '
                                  'exactly one field')
            value = {fields[0].name: value}
        return value

    def answer_without_model(self, context):
        """
        Answer without the model (a rule): an answer, or None to decline.

        Used by the `rule` interpreter when no rule is named.
        """
        return None

    # --- What the server needs --------------------------------------------------------------

    def template_text(self):
        if self.spec.get('prompt'):
            return self.spec['prompt']
        name = self.spec.get('template')
        if not name:
            raise ConfigError(f'{self.name}: no template and no prompt')
        if name not in self.templates:
            raise ConfigError(f'{self.name}: the template "{name}" is not in the templates')
        return self.templates[name]

    def fields(self, context):
        """Return the fields of the answer: declared, or taken from the call's context."""
        if self.spec.get('fields_from'):
            try:
                value = resolve_path(context, self.spec['fields_from'])
            except (KeyError, IndexError, TypeError):
                raise RequestError(
                    f'{self.name}: the context has no {self.spec["fields_from"]} '
                    '(fields_from)') from None
            try:
                return parse_fields(value)
            except ValueError as exc:
                raise RequestError(f'{self.name}: fields_from: {exc}') from None
        return parse_fields(self.spec.get('fields'))

    def schema(self, context):
        """Return the raw JSON schema of the definition, or None (built from the fields)."""
        return copy.deepcopy(self.spec.get('schema'))

    def output_format(self, context):
        schema = self.schema(context)
        if schema is not None:
            return dump_like_nlohmann(schema)
        return self.format_output(self.fields(context))

    def uses_image(self):
        return USES_IMAGE[self.spec.get('uses_image', False)]

    def interpreters(self):
        return parse_interpreters(self.spec.get('interpreter'))

    def choices(self, context):
        """(field name, choices, extra) of choices_from, or None."""
        path = self.spec.get('choices_from')
        if not path:
            return None
        try:
            choices = choices_of(resolve_path(context, path))
        except (KeyError, IndexError, TypeError):
            raise RequestError(f'{self.name}: the context has no {path} (choices_from)') from None
        field_name = self.spec.get('choices_field')
        if not field_name:
            fields = self.fields(context)
            if len(fields) != 1:
                raise ConfigError(f'{self.name}: choices_from with several fields needs '
                                  'choices_field')
            field_name = fields[0].name
        return field_name, choices, self.spec.get('choices_extra') or []


def resolve_spec(name, definition, variant=''):
    """Apply the defaults and, if given, a variant to the definition of a question."""
    if not isinstance(definition, dict):
        raise ConfigError(f'{name}: the definition must be a mapping')
    unknown = set(definition) - set(DEFAULTS)
    if unknown:
        raise ConfigError(f'{name}: unknown keys {sorted(unknown)}')
    spec = copy.deepcopy(DEFAULTS)
    spec.update(copy.deepcopy(definition))
    if variant:
        variants = spec.get('variants') or {}
        if variant not in variants:
            raise RequestError(f'{name}: no variant "{variant}" (variants: {sorted(variants)})')
        changes = variants[variant] or {}
        unknown = set(changes) - set(DEFAULTS) - {'variants'}
        if unknown:
            raise ConfigError(f'{name}, variant {variant}: unknown keys {sorted(unknown)}')
        spec.update(copy.deepcopy(changes))
    if spec.get('uses_image', False) not in USES_IMAGE:
        raise ConfigError(f'{name}: uses_image must be true, false or optional')
    return spec


def load_module(name):
    """Import a question module, by its import name or by the path of its .py file."""
    if name.endswith('.py') or os.sep in name:
        path = os.path.expanduser(name)
        module_name = 'interpretation_module_' + os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None:
            raise ConfigError(f'Cannot load the question module {name}')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    return importlib.import_module(name)


class ModuleContents:
    """The classes, rules and formatters of the question modules."""

    def __init__(self, modules=()):
        self.classes = {}       # class name -> class
        self.by_question = {}   # question name -> class (by its `name`)
        self.rules = {}
        self.formatters = {}
        for module_name in modules:
            if not module_name:
                continue
            try:
                module = load_module(module_name)
            except ImportError as exc:
                raise ConfigError(f'Cannot import the question module {module_name}: '
                                  f'{exc}') from exc
            self.add(module)

    def add(self, module):
        for _, value in inspect.getmembers(module):
            if inspect.isclass(value) and issubclass(value, Question) and value is not Question:
                self.classes[value.__name__] = value
                self.classes[f'{value.__module__}.{value.__name__}'] = value
                if value.name:
                    self.by_question[value.name] = value
            elif callable(value) and getattr(value, 'interpretation_rule', None):
                self.rules[value.interpretation_rule] = value
            elif callable(value) and getattr(value, 'interpretation_formatter', None):
                self.formatters[value.interpretation_formatter] = value
        self.rules.update(getattr(module, 'RULES', {}) or {})
        self.formatters.update(getattr(module, 'FORMATTERS', {}) or {})

    def question_class(self, name, spec):
        class_name = spec.get('class')
        if class_name:
            if class_name not in self.classes:
                raise ConfigError(f'{name}: the class "{class_name}" is not in the question '
                                  'modules')
            return self.classes[class_name]
        return self.by_question.get(name, Question)
