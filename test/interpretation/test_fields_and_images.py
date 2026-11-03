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

"""Fields, schemas, the output-format block, templates, parsing and the image encoding."""

import json
from types import SimpleNamespace
import zlib

from behavior_interpretation.fields import build_schema
from behavior_interpretation.fields import Field
from behavior_interpretation.fields import FieldError
from behavior_interpretation.fields import output_format_json
from behavior_interpretation.fields import parse_fields
from behavior_interpretation.fields import strict_schema
from behavior_interpretation.fields import to_gemini_dialect
from behavior_interpretation.images import EncodedImage
from behavior_interpretation.images import image_to_png
from behavior_interpretation.templates import fill_template
from behavior_interpretation.templates import flatten_templates
from behavior_interpretation.validation import InvalidAnswer
from behavior_interpretation.validation import parse_answer
from behavior_interpretation.validation import validate_fields
import pytest

# The output fields of llm_router_msgs/GetLLMResponse (key, value), as attention sends them
LLM_ROUTER_FIELDS = [('attention_action_id', 'int'), ('class', 'str'), ('confidence', 'float'),
                     ('person', 'bool'), ('ids', 'int-arr'), ('names', 'str-arr'),
                     ('scores', 'float-arr'), ('flags', 'boll-arr')]


def llm_router_schema_text(mode, output_fields):
    """llm_router.LLMRouter._build_output_fields_json, transcribed as the reference."""
    single = {'local': {'int': 'integer', 'str': 'string', 'float': 'number', 'bool': 'boolean'},
              'remote-gemini': {'int': 'INTEGER', 'str': 'STRING', 'float': 'NUMBER',
                                'bool': 'BOOLEAN'}}
    arrays = {'int-arr': 'int', 'str-arr': 'str', 'float-arr': 'float', 'boll-arr': 'bool'}
    words = {'array': {'local': 'array', 'remote-gemini': 'ARRAY'},
             'object': {'local': 'object', 'remote-gemini': 'OBJECT'}}
    properties = {}
    required = []
    for key, value in output_fields:
        if value in single[mode]:
            properties[key] = {'type': single[mode][value]}
        else:
            properties[key] = {'type': words['array'][mode],
                               'items': {'type': single[mode][arrays[value]]}}
        required.append(key)
    return json.dumps({'type': words['object'][mode], 'properties': properties,
                       'required': required}, indent=2)


def test_schema_is_llm_routers_byte_for_byte():
    fields = parse_fields([{'key': k, 'value': v} for k, v in LLM_ROUTER_FIELDS])
    assert json.dumps(build_schema(fields), indent=2) == \
        llm_router_schema_text('local', LLM_ROUTER_FIELDS)
    assert json.dumps(build_schema(fields, 'gemini'), indent=2) == \
        llm_router_schema_text('remote-gemini', LLM_ROUTER_FIELDS)
    # The schema sent to Ollama and OpenAI-compatible APIs: parsed back and with
    # additionalProperties false, as llm_router's ask_llm_callback does
    sent = json.loads(llm_router_schema_text('local', LLM_ROUTER_FIELDS))
    sent['additionalProperties'] = False
    assert json.dumps(strict_schema(build_schema(fields))) == json.dumps(sent)
    # The Gemini dialect of a standard schema is the one built directly
    assert to_gemini_dialect(build_schema(fields)) == build_schema(fields, 'gemini')


def test_field_notations():
    expected = [Field('id', 'int'), Field('names', 'str[]')]
    assert parse_fields(['int:id', 'str[]:names']) == expected
    assert parse_fields(['int:id', 'str-arr:names']) == expected
    assert parse_fields({'id': 'int', 'names': 'str[]'}) == expected
    specs = [{'name': 'id', 'type': 'integer'}, {'key': 'names', 'value': 'str-arr'}]
    assert parse_fields(specs) == expected
    assert parse_fields('["int:id", "str[]:names"]') == expected
    with pytest.raises(FieldError):
        parse_fields(['double:x'])
    with pytest.raises(FieldError):
        parse_fields(['id'])


def test_output_format_is_attentions_nlohmann_dump():
    # AttentionIntelligence::build_output_format_json: nlohmann's dump(2), keys sorted
    text = output_format_json(parse_fields(['str:zeta', 'int[]:alpha']))
    assert text == (
        '{\n'
        '  "properties": {\n'
        '    "alpha": {\n'
        '      "items": {\n'
        '        "type": "integer"\n'
        '      },\n'
        '      "type": "array"\n'
        '    },\n'
        '    "zeta": {\n'
        '      "type": "string"\n'
        '    }\n'
        '  },\n'
        '  "required": [\n'
        '    "zeta",\n'
        '    "alpha"\n'
        '  ],\n'
        '  "type": "object"\n'
        '}')
    assert output_format_json([]) == \
        '{\n  "properties": {},\n  "required": [],\n  "type": "object"\n}'


def test_fill_template_as_attention():
    values = {'task': 'bring the cup', 'empty': ''}
    assert fill_template('Task: {task}.{missing}{empty} {{literal}}', values) == \
        'Task: bring the cup. {literal}'
    assert fill_template('no slots', {}) == 'no slots'
    assert fill_template('cut {open', {}) == 'cut '


def test_templates_from_a_ros_parameters_file():
    data = {'/node': {'ros__parameters': {'prompts': {'a': 'A {x}', 'b': 'B'}}},
            'other': {'b': 'B2'}}
    templates = flatten_templates(data)
    assert templates['a'] == 'A {x}'
    assert templates['/node.ros__parameters.prompts.b'] == 'B'
    assert 'b' not in templates  # not unique: only by its path


def test_loose_parsing():
    assert parse_answer('{"id": 3}') == {'id': 3}
    assert parse_answer('```json\n{"id": 3}\n```') == {'id': 3}
    assert parse_answer('<think>hmm {no}</think>{"id": 3}') == {'id': 3}
    assert parse_answer('The answer is {"id": 3, "s": "a}b"} as asked') == {'id': 3, 's': 'a}b'}
    assert parse_answer('[{"id": 1}, {"id": 2}]') == {'id': 1}
    for text in ('', 'no json', '[]', '3', None):
        with pytest.raises(InvalidAnswer):
            parse_answer(text)


def test_validation_types():
    fields = parse_fields(['int:id', 'float:score', 'bool:ok', 'str:name', 'int[]:ids'])
    answer = {'id': '3', 'score': 1, 'ok': 'true', 'name': 7, 'ids': [1, 2.0], 'extra': 1}
    assert validate_fields(answer, fields) == {
        'id': 3, 'score': 1.0, 'ok': True, 'name': '7', 'ids': [1, 2]}
    for bad in ({'id': True}, {'id': 'three'}, {'id': 2.5}, {}):
        with pytest.raises(InvalidAnswer):
            validate_fields(bad, parse_fields(['int:id']))
    with pytest.raises(InvalidAnswer):
        validate_fields({'ids': 3}, parse_fields(['int[]:ids']))


def _image(encoding, width, height, channels, padding=0):
    step = width * channels + padding
    data = bytes((7 * i + 3) % 256 for i in range(step * height))
    return SimpleNamespace(encoding=encoding, width=width, height=height, step=step, data=data)


def _decode_png(png):
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    position = 8
    chunks = {}
    while position < len(png):
        length = int.from_bytes(png[position:position + 4], 'big')
        kind = png[position + 4:position + 8]
        payload = png[position + 8:position + 8 + length]
        crc = int.from_bytes(png[position + 8 + length:position + 12 + length], 'big')
        assert crc == zlib.crc32(kind + payload) & 0xffffffff
        chunks[kind] = payload
        position += 12 + length
    header = chunks[b'IHDR']
    width = int.from_bytes(header[0:4], 'big')
    height = int.from_bytes(header[4:8], 'big')
    color_type = header[9]
    return width, height, color_type, zlib.decompress(chunks[b'IDAT'])


@pytest.mark.parametrize('encoding,channels,color_type', [
    ('rgb8', 3, 2), ('bgr8', 3, 2), ('8UC3', 3, 2), ('rgba8', 4, 6), ('bgra8', 4, 6),
    ('mono8', 1, 0)])
def test_png_is_lossless_and_unresized(encoding, channels, color_type):
    image = _image(encoding, 5, 3, channels, padding=2)
    width, height, kind, raw = _decode_png(image_to_png(image))
    assert (width, height, kind) == (5, 3, color_type)
    for y in range(3):
        row = raw[y * (5 * channels + 1):(y + 1) * (5 * channels + 1)]
        assert row[0] == 0  # filter type 0
        source = image.data[y * image.step:y * image.step + 5 * channels]
        for x in range(5):
            pixel = list(source[x * channels:(x + 1) * channels])
            if encoding in ('bgr8', 'bgra8'):
                pixel[0], pixel[2] = pixel[2], pixel[0]
            assert list(row[1 + x * channels:1 + (x + 1) * channels]) == pixel


def test_unsupported_encoding_and_hash():
    with pytest.raises(ValueError):
        image_to_png(_image('16UC1', 2, 2, 2))
    first = EncodedImage(_image('rgb8', 2, 2, 3))
    second = EncodedImage(_image('rgb8', 2, 2, 3))
    assert first.sha256 == second.sha256 and first.base64 == second.base64
