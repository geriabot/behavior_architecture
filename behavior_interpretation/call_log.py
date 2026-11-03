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
The call log: one JSON line per call, and every image saved once, by its hash.

<log_dir>/calls.jsonl has one object per call with these keys:

  time             UTC, ISO 8601
  question         the question asked; `variant`, the variant used ('' for none)
  request_id       who asked (from the request)
  context, text, budget_s   what the request brought (images: see `images`)
  interpreters     the chain that was tried, e.g. ["rule:most_centered", "llm"]
  interpreter      the one that answered or failed last ('' if every one declined)
  route, backend, model     the route of the llm interpreter (if it was called)
  prompt, schema   what the model received
  images           paths, relative to log_dir, of the PNG files (images/<sha256>.png)
  attempts         [{raw, error, latency_s, prompt_tokens, completion_tokens}], in order
  raw_answer       the text of the last attempt
  answer           the answer given to the asker (an object, or null if invalid)
  source           the interpreter that answered, or "fallback" or "invalid"
  fallback_reason  "timeout", "invalid", "model_down", "budget", "declined" or ''
  latency_s        of the whole call; tokens: {prompt, completion} over all the attempts
  model_version    as the backend reported it (e.g. the model and digest that answered)
  message          a readable explanation

The `recorded` backend answers from these files (by question and prompt).
"""

import datetime
import json
import os
import threading

LOG_FILE = 'calls.jsonl'
IMAGES_DIR = 'images'


class CallLog:
    """Appends the calls to <log_dir>/<file_name>; nothing is written without a log_dir."""

    def __init__(self, log_dir='', file_name=LOG_FILE):
        self.log_dir = os.path.expanduser(log_dir) if log_dir else ''
        self.path = os.path.join(self.log_dir, file_name) if self.log_dir else ''
        self._lock = threading.Lock()
        if self.log_dir:
            os.makedirs(os.path.join(self.log_dir, IMAGES_DIR), exist_ok=True)

    @property
    def enabled(self):
        return bool(self.log_dir)

    def save_image(self, image):
        """Save the image's PNG (only the first time); return its path relative to log_dir."""
        relative = os.path.join(IMAGES_DIR, image.sha256 + '.png')
        if not self.log_dir:
            return relative
        path = os.path.join(self.log_dir, relative)
        with self._lock:
            if not os.path.exists(path):
                temporary = path + '.tmp'
                with open(temporary, 'wb') as stream:
                    stream.write(image.png)
                os.replace(temporary, path)
        return relative

    def write(self, record):
        if not self.log_dir:
            return
        record = dict(record)
        now = datetime.datetime.now(datetime.timezone.utc)
        record.setdefault('time', now.isoformat(timespec='milliseconds'))
        line = json.dumps(record, ensure_ascii=False, default=str)
        with self._lock:
            with open(self.path, 'a', encoding='utf-8') as stream:
                stream.write(line + '\n')


def read_calls(path):
    """Read the calls of a log file (or of <dir>/calls.jsonl for a directory)."""
    if os.path.isdir(path):
        path = os.path.join(path, LOG_FILE)
    calls = []
    with open(path, encoding='utf-8') as stream:
        for number, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            try:
                calls.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f'{path}:{number}: not a JSON line ({exc})') from exc
    return calls
