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
JSON over HTTP with the standard library only (urllib), with and without streaming.

The body is serialized by the caller, so that each backend sends the same bytes as the
gateway it reproduces: `json.dumps(payload)` for attention's router (urllib), compact UTF-8
for the httpx client of the interaction system's gateway (`httpx_body`). A route with
`sdk_retries` repeats a failed request as the gateway's SDK did (retries.py).
"""

import json
import os
import socket
import time
import urllib.error
import urllib.request

from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout
from behavior_interpretation.interpreters.llm.backends.retries import call_with_retries


def urllib_body(payload):
    """Serialize as llm_router does: json.dumps with its default separators."""
    return json.dumps(payload).encode()


def httpx_body(payload):
    """Serialize as httpx does for json=payload: compact, UTF-8 not escaped."""
    return json.dumps(payload, ensure_ascii=False, separators=(',', ':'),
                      allow_nan=False).encode('utf-8')


def api_key_from(options, default_variables=()):
    """Read the API key from the variable named in api_key_env (or the defaults)."""
    variable = options.get('api_key_env') or ''
    if variable:
        key = os.environ.get(variable, '')
        if not key:
            raise BackendError(f'The environment variable {variable} (api_key_env) is empty')
        return key
    for name in default_variables:
        if os.environ.get(name):
            return os.environ[name]
    return None


def _open(url, body, headers, timeout):
    request = urllib.request.Request(url, data=body, headers=headers, method='POST')
    try:
        return urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as exc:
        text = exc.read().decode(errors='replace')[:500]
        error = BackendError(f'{url} answered HTTP {exc.code}: {text}')
        error.status = exc.code
        error.headers = {name.lower(): value for name, value in (exc.headers or {}).items()}
        raise error from exc
    except (socket.timeout, TimeoutError) as exc:
        raise BackendTimeout(f'No answer from {url} in {timeout:.1f} s') from exc
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, (socket.timeout, TimeoutError)):
            raise BackendTimeout(f'No answer from {url} in {timeout:.1f} s') from exc
        raise _unreachable(f'Cannot reach {url}: {exc.reason}') from exc
    except (ConnectionError, OSError) as exc:
        raise _unreachable(f'Cannot reach {url}: {exc}') from exc


def _unreachable(message):
    error = BackendError(message)
    error.connection = True
    return error


def _read(url, body, headers, timeout):
    try:
        with _open(url, body, headers, timeout) as response:
            return response.read().decode()
    except (socket.timeout, TimeoutError) as exc:
        raise BackendTimeout(f'No answer from {url} in {timeout:.1f} s') from exc


def post_json(url, body, headers, timeout, retries=None, limits=None):
    """
    POST the body and return the JSON object of the answer.

    `retries` (a RetryPolicy) repeats a failed request as an SDK would, within `timeout`;
    `limits` gives the call's cancellation and receives the number of retries.
    """
    if retries is None:
        text = _read(url, body, headers, timeout)
    else:
        text = call_with_retries(lambda left: _read(url, body, headers, left), timeout, retries,
                                 limits)
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise BackendError(f'{url} did not answer JSON: {text[:200]!r}') from exc


def post_lines(url, body, headers, timeout, retries=None, limits=None):
    """
    POST the body and yield the lines of a streamed answer (NDJSON or SSE).

    With `retries`, a request that fails before the answer starts is repeated (as for
    post_json); a stream cut once it has started is not.
    """
    deadline = time.monotonic() + timeout
    try:
        if retries is None:
            opened = _open(url, body, headers, timeout)
        else:
            opened = call_with_retries(lambda left: _open(url, body, headers, left), timeout,
                                       retries, limits)
        with opened as response:
            for raw in response:
                if time.monotonic() > deadline:
                    raise BackendTimeout(f'No complete answer from {url} in {timeout:.1f} s')
                line = raw.decode(errors='replace').strip()
                if line:
                    yield line
    except (socket.timeout, TimeoutError) as exc:
        raise BackendTimeout(f'No answer from {url} in {timeout:.1f} s') from exc
