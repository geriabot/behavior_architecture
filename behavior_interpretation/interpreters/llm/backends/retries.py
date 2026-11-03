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
The automatic retries of the official SDKs that the interaction system's gateway used.

The gateway called the APIs through the SDKs with their default retries, so a route of the
`llm_manager` dialect repeats an HTTP request as the SDK would (route option `sdk_retries`):

- openai (3.39) and anthropic (0.84): at most 2 retries, after a connection error or an HTTP
  status 408, 409, 429 or >= 500 (the header `x-should-retry: true|false` decides first). The
  delay is the server's `retry-after-ms` or `retry-after` (seconds or a date) when it is more
  than 0 and at most 120 s (openai) or 60 s (anthropic); otherwise 0.5 s doubled at each
  retry, at most 8 s, less a random 0 to 25 %. openai does not retry when the server asks for
  more than 120 s; anthropic then waits its own delay.
- google-genai (2.33): no retries (the client has no retry options by default).

Unlike the SDKs, which give each request the whole timeout, the requests and the delays
between them share the limit of the call (the route's timeout_s, or what is left of the
request's budget): a request is sent only while time is left, a delay that would reach the
limit is not waited (the last error is raised), and a request that times out is therefore not
repeated. A cancelled call sends no further request.
"""

import email.utils
import math
from random import random
import time

from behavior_interpretation.interpreters.llm.backends.base import BackendError
from behavior_interpretation.interpreters.llm.backends.base import BackendTimeout


class RetryPolicy:
    """When an SDK repeats a failed request, and how long it waits before."""

    def __init__(self, name, max_retries=2, initial_delay_s=0.5, max_delay_s=8.0,
                 max_retry_after_s=60.0, no_retry_after_longer_wait=False):
        self.name = name
        self.max_retries = max_retries
        self.initial_delay_s = initial_delay_s
        self.max_delay_s = max_delay_s
        self.max_retry_after_s = max_retry_after_s
        # openai: a server that asks for a longer wait than max_retry_after_s is not retried
        self.no_retry_after_longer_wait = no_retry_after_longer_wait

    def should_retry(self, error):
        if isinstance(error, BackendTimeout):
            return False   # the limit of the call has passed
        if getattr(error, 'connection', False):
            return True
        if not error.status:
            return False   # not an HTTP error (an answer that cannot be read...)
        headers = error.headers or {}
        retry_after = retry_after_s(headers)
        if self.no_retry_after_longer_wait and retry_after is not None and \
                math.isfinite(retry_after) and retry_after > self.max_retry_after_s:
            return False
        should = headers.get('x-should-retry')
        if should in ('true', 'false'):
            return should == 'true'
        return error.status in (408, 409, 429) or error.status >= 500

    def delay_s(self, retries_taken, headers=None):
        retry_after = retry_after_s(headers or {})
        if retry_after is not None and math.isfinite(retry_after) and \
                0 < retry_after <= self.max_retry_after_s:
            return retry_after
        base = min(self.initial_delay_s * pow(2.0, min(retries_taken, 1000)), self.max_delay_s)
        return max(0.0, base * (1 - 0.25 * random()))


OPENAI_SDK = RetryPolicy('openai', max_retry_after_s=120.0, no_retry_after_longer_wait=True)
ANTHROPIC_SDK = RetryPolicy('anthropic', max_retry_after_s=60.0)
GENAI_SDK = RetryPolicy('google-genai', max_retries=0)


def retry_after_s(headers):
    """Return the wait that the server asks for (retry-after-ms, retry-after) in s, or None."""
    for name, divisor in (('retry-after-ms', 1000.0), ('retry-after', 1.0)):
        value = headers.get(name)
        if value is None:
            continue
        try:
            return float(value) / divisor
        except ValueError:
            continue
    value = headers.get('retry-after')
    try:
        date = email.utils.parsedate_tz(value)
        if date is None:
            return None
        return float(email.utils.mktime_tz(date) - time.time())
    except (TypeError, ValueError, OverflowError, OSError):
        return None


def _flag(value):
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    if text in ('true', '1', 'yes', 'on'):
        return True
    if text in ('false', '0', 'no', 'off'):
        return False
    raise BackendError(f'sdk_retries must be true or false, not {value!r}')


def route_policy(backend, policy):
    """Return `policy` if the route has sdk_retries (default: in the llm_manager dialect)."""
    value = backend.options.get('sdk_retries')
    if value is None or value == '':
        enabled = backend.dialect() == 'llm_manager'
    else:
        enabled = _flag(value)
    return policy if enabled else None


def call_with_retries(send, timeout_s, policy, limits=None):
    """
    Return send(timeout) with the policy's retries, all within timeout_s.

    `send` makes one request with the time it is given. `limits` (optional) receives the
    number of retries made (`sdk_retries`) and gives the call's cancellation (`cancelled`).
    """
    deadline = time.monotonic() + timeout_s
    cancelled = getattr(limits, 'cancelled', None)
    retries_taken = 0
    while True:
        left = deadline - time.monotonic()
        if left <= 0.0:
            raise BackendTimeout(f'No answer in {timeout_s:.1f} s ({retries_taken} retries)')
        try:
            return send(left)
        except BackendError as error:
            if retries_taken >= policy.max_retries or not policy.should_retry(error):
                raise
            delay = policy.delay_s(retries_taken, error.headers)
            if time.monotonic() + delay >= deadline:
                raise
            if cancelled is not None:
                if cancelled.wait(delay):
                    raise
            else:
                time.sleep(delay)
            retries_taken += 1
            if limits is not None:
                limits.sdk_retries = retries_taken
