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
Parsing of a model's answer and its validation against the question's fields.

Parsing is loose by default: a code fence around the JSON, a reasoning block before it, or
text around the object are tolerated, and a list is read as its first object (as llm_router
does). With `parse: strict` the whole text must be JSON, as llm_router read it. With
`parse: text` the text is not parsed: the answer is {"text": <the text>}.
Validation is strict about what the asker receives: every field present, with its type. A
value written as the wrong JSON type but readable as the right one is converted (the string
"3" for an int field: llm_router passes every value as a string and attention reads it with
std::stoi, so these answers are valid today). With `validate: object` the parsed object is
returned as it is, and the asker checks it.
"""

import json
import math
import re


class InvalidAnswer(ValueError):
    """An answer that cannot be used: not JSON, a field missing or of the wrong type."""


_THINK = re.compile(r'^\s*<think>.*?</think>', re.DOTALL)
_INT = re.compile(r'^[+-]?\d+$')


def strip_code_fence(text):
    """Remove the ```json ... ``` that some models write even when asked for JSON only."""
    text = text.strip()
    if text.startswith('```'):
        text = text.split('\n', 1)[1] if '\n' in text else ''
        text = text.rsplit('```', 1)[0]
    return text.strip()


def _first_object(text):
    """Return the first balanced {...} of a text, or None."""
    start = text.find('{')
    while start >= 0:
        depth = 0
        in_string = False
        escaped = False
        for i in range(start, len(text)):
            char = text[i]
            if in_string:
                if escaped:
                    escaped = False
                elif char == '\\':
                    escaped = True
                elif char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == '{':
                depth += 1
            elif char == '}':
                depth -= 1
                if depth == 0:
                    try:
                        return json.loads(text[start:i + 1])
                    except json.JSONDecodeError:
                        break
        start = text.find('{', start + 1)
    return None


def parse_answer(text):
    """Return the JSON object of a model's answer; InvalidAnswer if there is none."""
    if text is None:
        raise InvalidAnswer('empty answer')
    text = strip_code_fence(_THINK.sub('', text))
    if not text:
        raise InvalidAnswer('empty answer')
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        parsed = _first_object(text)
        if parsed is None:
            raise InvalidAnswer(f'not JSON: {text[:200]!r}') from None
    if isinstance(parsed, list):
        if not parsed:
            raise InvalidAnswer('an empty list')
        parsed = parsed[0]
    if not isinstance(parsed, dict):
        raise InvalidAnswer('not a JSON object or a list with one object')
    return parsed


def parse_answer_strict(text):
    """
    Return the JSON object of a model's answer, read as llm_router read it.

    Raise InvalidAnswer if there is none. The whole text must be JSON: no reasoning block or
    text around it (a code fence is removed, or not, by the backend, as llm_router did). A
    list gives its first item.
    """
    if text is None:
        raise InvalidAnswer('empty answer')
    try:
        parsed = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        raise InvalidAnswer(f'not JSON: {str(text)[:200]!r}') from None
    if isinstance(parsed, list):
        if not parsed:
            raise InvalidAnswer('an empty list')
        parsed = parsed[0]
    if not isinstance(parsed, dict):
        raise InvalidAnswer('not a JSON object or a list with one object')
    return parsed


# The field of the answer of a question with `parse: text`
TEXT_FIELD = 'text'


def parse_text(text):
    """Return the text as it is, as {"text": <the text>} (an empty text is checked later)."""
    return {TEXT_FIELD: '' if text is None else str(text)}


PARSERS = {'loose': parse_answer, 'strict': parse_answer_strict, 'text': parse_text}


def _coerce(value, kind, name):
    if kind == 'int':
        if isinstance(value, bool):
            raise InvalidAnswer(f'{name}: a bool where an int was expected')
        if isinstance(value, int):
            return value
        if isinstance(value, float) and math.isfinite(value) and value.is_integer():
            return int(value)
        if isinstance(value, str) and _INT.match(value.strip()):
            return int(value.strip())
    elif kind == 'float':
        if isinstance(value, bool):
            raise InvalidAnswer(f'{name}: a bool where a float was expected')
        if isinstance(value, (int, float)):
            return float(value)
        if isinstance(value, str):
            try:
                number = float(value.strip())
            except ValueError:
                pass
            else:
                if math.isfinite(number):
                    return number
    elif kind == 'bool':
        if isinstance(value, bool):
            return value
        if isinstance(value, str) and value.strip().lower() in ('true', 'false'):
            return value.strip().lower() == 'true'
    elif kind == 'str':
        if isinstance(value, str):
            return value
        if isinstance(value, bool):
            return 'true' if value else 'false'
        if isinstance(value, (int, float)):
            return str(value)
    raise InvalidAnswer(f'{name}: {value!r} is not a valid {kind}')


def validate_fields(answer, fields):
    """Return the answer with exactly the question's fields, in order and typed."""
    clean = {}
    for field in fields:
        if field.name not in answer:
            raise InvalidAnswer(f'the field "{field.name}" is missing')
        value = answer[field.name]
        if field.array:
            if not isinstance(value, list):
                raise InvalidAnswer(f'{field.name}: {value!r} is not an array')
            clean[field.name] = [_coerce(item, field.type, field.name) for item in value]
        else:
            clean[field.name] = _coerce(value, field.type, field.name)
    return clean


def resolve_path(data, path):
    """Return the value of a dotted path (`context.detection_ids`: data['detection_ids'])."""
    parts = path.split('.')
    if parts and parts[0] == 'context':
        parts = parts[1:]
    value = data
    for part in parts:
        if isinstance(value, dict) and part in value:
            value = value[part]
        elif isinstance(value, list) and part.lstrip('-').isdigit():
            value = value[int(part)]
        else:
            raise KeyError(path)
    return value


def choices_of(value):
    """Return the choices of a context value: a list of values, or of objects with `id`."""
    if isinstance(value, dict):
        value = list(value)
    if not isinstance(value, list):
        return [value]
    return [item['id'] if isinstance(item, dict) and 'id' in item else item for item in value]


def check_choice(answer, field_name, choices, extra):
    """Raise InvalidAnswer unless answer[field_name] is one of the choices or `extra`."""
    value = answer.get(field_name)
    allowed = list(choices) + list(extra or [])
    if value in allowed and not (isinstance(value, bool) and
                                 not any(isinstance(a, bool) for a in allowed)):
        return
    raise InvalidAnswer(f'{field_name}: {value!r} is not one of the choices {allowed}')
