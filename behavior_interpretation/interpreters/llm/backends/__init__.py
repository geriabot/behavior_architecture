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

"""The model backends of the llm interpreter, by type."""

from behavior_interpretation.interpreters.llm.backends.base import Backend
from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout
from behavior_interpretation.interpreters.llm.backends.base import GenerateResult
from behavior_interpretation.interpreters.llm.backends.base import Limits
from behavior_interpretation.interpreters.llm.backends.fake import FakeBackend
from behavior_interpretation.interpreters.llm.backends.gemini import GeminiBackend
from behavior_interpretation.interpreters.llm.backends.llama_ros import LlamaRosBackend
from behavior_interpretation.interpreters.llm.backends.ollama import OllamaBackend
from behavior_interpretation.interpreters.llm.backends.openai import OpenAIBackend
from behavior_interpretation.interpreters.llm.backends.recorded import RecordedBackend

BACKENDS = {
    backend.kind: backend
    for backend in (OllamaBackend, OpenAIBackend, GeminiBackend, LlamaRosBackend, FakeBackend,
                    RecordedBackend)
}


def create_backend(kind, options=None, node=None):
    """Create a backend of this type (ollama, openai, gemini, llama_ros, fake or recorded)."""
    if kind not in BACKENDS:
        raise ValueError(f'Unknown backend "{kind}" (one of {sorted(BACKENDS)})')
    return BACKENDS[kind](options, node)


__all__ = ['BACKENDS', 'Backend', 'BackendError', 'BackendTimeout', 'GenerateResult', 'Limits',
           'create_backend']
