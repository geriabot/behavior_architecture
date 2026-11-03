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
Prompt templates: named slots `{name}`, filled as attention's fill_template does.

A slot without a value is left empty, and `{{` and `}}` write a single brace. The templates
file is a YAML mapping of names to texts; a nested mapping (for example a ROS parameters
file with `prompts:` under `ros__parameters`) is also read: every text is found by its
dotted path and, when the name is unique in the file, by its own name.
"""

import json

import yaml


def fill_template(template, values):
    """Replace each `{name}` of the template by values[name] ('' if it has none)."""
    output = []
    i = 0
    n = len(template)
    while i < n:
        pair = template[i:i + 2]
        if pair in ('{{', '}}'):
            output.append(template[i])
            i += 2
        elif template[i] == '{':
            end = template.find('}', i)
            if end < 0:
                break  # an unclosed slot: the rest of the template is not written
            value = values.get(template[i + 1:end])
            if value is not None:
                output.append(value)
            i = end + 1
        else:
            output.append(template[i])
            i += 1
    return ''.join(output)


def slot_value(value):
    """Write a context value in a template: strings as they are, the rest as JSON."""
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False)


def flatten_templates(data, prefix=''):
    """Map every text of a nested mapping by its dotted path and, if unique, its name."""
    paths = {}

    def walk(node, path):
        if isinstance(node, dict):
            for key, value in node.items():
                walk(value, f'{path}.{key}' if path else str(key))
        elif isinstance(node, str):
            paths[path] = node

    walk(data, prefix)
    leaves = {}
    for path, text in paths.items():
        leaves.setdefault(path.rsplit('.', 1)[-1], []).append(text)
    templates = {name: texts[0] for name, texts in leaves.items() if len(texts) == 1}
    templates.update(paths)
    return templates


def load_templates(path):
    """Load the templates of a YAML file (see the module's docstring)."""
    if not path:
        return {}
    with open(path, encoding='utf-8') as stream:
        data = yaml.safe_load(stream) or {}
    if not isinstance(data, dict):
        raise ValueError(f'{path}: the templates file must be a YAML mapping')
    if isinstance(data.get('templates'), dict):
        data = data['templates']
    return flatten_templates(data)
