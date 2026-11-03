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
The fields of an answer, the JSON schema built from them and the output-format block.

A field has a name (the system's own, e.g. `attention_action_id`) and a type: `int`, `str`,
`float`, `bool` or an array of one of them (`int[]`; attention's spellings `int-arr`,
`str-arr`, `float-arr`, `bool-arr` and `boll-arr` are also accepted). They are written as
`"int:id"`, as `{name: type}`, or as `{key: name, value: type}` (the KeyValue of attention's
router).

The schemas are the same, key for key and in the same order, as the ones attention's router
(llm_router) builds from its output fields, so that a model receives the same request.
"""

import json

SCALAR_TYPES = ('int', 'str', 'float', 'bool')

_ALIASES = {
    'integer': 'int', 'string': 'str', 'number': 'float', 'boolean': 'bool',
    'int-arr': 'int[]', 'str-arr': 'str[]', 'float-arr': 'float[]', 'bool-arr': 'bool[]',
    'boll-arr': 'bool[]',  # the spelling of BOOL_ARRAY in llm_router_msgs
}

# JSON schema types, standard and in the dialect of the Gemini API
_JSON_TYPES = {'int': 'integer', 'str': 'string', 'float': 'number', 'bool': 'boolean'}
_DIALECTS = {
    'json': {'object': 'object', 'array': 'array', 'types': _JSON_TYPES},
    'gemini': {'object': 'OBJECT', 'array': 'ARRAY',
               'types': {k: v.upper() for k, v in _JSON_TYPES.items()}},
}


class FieldError(ValueError):
    """A field that cannot be read."""


class Field:
    """One field of an answer: a name, a scalar type and whether it is an array."""

    def __init__(self, name, kind):
        kind = _ALIASES.get(str(kind).strip(), str(kind).strip())
        self.array = kind.endswith('[]')
        self.type = kind[:-2] if self.array else kind
        self.type = _ALIASES.get(self.type, self.type)
        if self.type not in SCALAR_TYPES:
            raise FieldError(f'Field "{name}": unknown type "{kind}" (one of {SCALAR_TYPES} '
                             'or an array of one of them, e.g. int[])')
        if not str(name):
            raise FieldError(f'A field of type "{kind}" has no name')
        self.name = str(name)

    @property
    def kind(self):
        return self.type + ('[]' if self.array else '')

    def __eq__(self, other):
        return isinstance(other, Field) and (self.name, self.kind) == (other.name, other.kind)

    def __repr__(self):
        return f'Field({self.kind}:{self.name})'


def parse_field(spec):
    """Read a Field from "type:name", {"name": .., "type": ..} or {"key": .., "value": ..}."""
    if isinstance(spec, Field):
        return spec
    if isinstance(spec, str):
        if ':' not in spec:
            raise FieldError(f'Field "{spec}": expected "type:name", e.g. "int:id"')
        kind, name = spec.split(':', 1)
        return Field(name.strip(), kind)
    if isinstance(spec, dict):
        if 'name' in spec and 'type' in spec:
            return Field(spec['name'], spec['type'])
        if 'key' in spec and 'value' in spec:
            return Field(spec['key'], spec['value'])
        if len(spec) == 1:
            ((name, kind),) = spec.items()
            return Field(name, kind)
    raise FieldError(f'Cannot read the field {spec!r}')


def parse_fields(spec):
    """Read the fields of a list of field specs, or of a mapping {name: type} (in order)."""
    if spec is None:
        return []
    if isinstance(spec, str):
        spec = json.loads(spec) if spec.strip().startswith(('[', '{')) else [spec]
    if isinstance(spec, dict) and not ({'name', 'type'} <= set(spec) or
                                       {'key', 'value'} <= set(spec)):
        return [Field(name, kind) for name, kind in spec.items()]
    if isinstance(spec, dict):
        spec = [spec]
    if not isinstance(spec, (list, tuple)):
        raise FieldError(f'Cannot read the fields {spec!r}')
    return [parse_field(item) for item in spec]


def build_field_schema(field, dialect='json'):
    words = _DIALECTS[dialect]
    if field.array:
        return {'type': words['array'], 'items': {'type': words['types'][field.type]}}
    return {'type': words['types'][field.type]}


def build_schema(fields, dialect='json'):
    """
    Build the JSON schema of an answer with these fields, as llm_router builds it.

    `dialect` is `json` (llama_ros, Ollama, OpenAI-compatible APIs) or `gemini` (the
    upper-case types of the Gemini API). Every field is required. The strict structured
    outputs of OpenAI-compatible APIs also need `additionalProperties: false`, which the
    backends that send it add (strict_schema).
    """
    properties = {}
    required = []
    for field in fields:
        properties[field.name] = build_field_schema(field, dialect)
        required.append(field.name)
    return {'type': _DIALECTS[dialect]['object'], 'properties': properties,
            'required': required}


def strict_schema(schema):
    """Copy the schema with `additionalProperties: false` at its top, as llm_router does."""
    schema = json.loads(json.dumps(schema))
    schema['additionalProperties'] = False
    return schema


def to_gemini_dialect(schema):
    """Write the schema with the upper-case type names of the Gemini API (`type` only)."""
    if isinstance(schema, list):
        return [to_gemini_dialect(item) for item in schema]
    if not isinstance(schema, dict):
        return schema
    converted = {}
    for key, value in schema.items():
        if key == 'type' and isinstance(value, str):
            converted[key] = value.upper()
        elif key == 'properties' and isinstance(value, dict):
            converted[key] = {name: to_gemini_dialect(sub) for name, sub in value.items()}
        else:
            converted[key] = to_gemini_dialect(value)
    return converted


def dump_like_nlohmann(value):
    """Write JSON as nlohmann::json::dump(2): indent 2, keys sorted, UTF-8 not escaped."""
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False)


def output_format_json(fields):
    """
    Write the default `{output_format}` block: the fields' schema as attention does.

    It reproduces AttentionIntelligence::build_output_format_json (nlohmann's dump(2) of
    {type, properties, required}), which prints the keys of every object sorted.
    """
    return dump_like_nlohmann(build_schema(fields, 'json'))
