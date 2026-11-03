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
The abstraction of a model backend: generate(model, prompt, schema, images, limits) -> text.

`schema` is a standard JSON schema (as built from the fields, without additionalProperties)
or the raw schema of the question; each backend translates it into its own dialect. `images`
are EncodedImage (PNG, in the order of the request). A backend raises BackendTimeout when the
limit passes and BackendError when the model cannot be reached or answers with an error.
"""


class BackendError(RuntimeError):
    """The model could not answer (not reachable, an HTTP error, an empty answer)."""

    reason = 'model_down'
    status = 0  # the HTTP status of an HTTP error (0: not an HTTP error)


class BackendTimeout(BackendError):
    """The limit of the call passed before the answer."""

    reason = 'timeout'


class Limits:
    """The limits of one call to a backend."""

    def __init__(self, timeout_s=120.0, max_tokens=0):
        self.timeout_s = float(timeout_s)
        self.max_tokens = int(max_tokens or 0)


class GenerateResult:
    """The text of an answer, with what the backend reports about it."""

    def __init__(self, text, prompt_tokens=0, completion_tokens=0, model_version=''):
        self.text = text
        self.prompt_tokens = int(prompt_tokens or 0)
        self.completion_tokens = int(completion_tokens or 0)
        self.model_version = model_version or ''


class Backend:
    """A backend, built from the options of a route (its parameters other than backend)."""

    kind = ''

    def __init__(self, options=None, node=None):
        self.options = dict(options or {})
        self.node = node

    def option(self, name, default=None):
        value = self.options.get(name, default)
        return default if value is None else value

    def generate(self, model, prompt, schema, images, limits, question=''):
        """Return the model's answer (GenerateResult); `question` is for the replays."""
        raise NotImplementedError
