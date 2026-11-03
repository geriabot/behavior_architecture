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
An HTTP server on 127.0.0.1 that fails as scripted, and records when each request arrived.

Each request (POST or GET) takes the next scripted failure; when they are spent, it gets the
success reply. A failure is (status, headers) or 'drop' (the connection is closed without a
response, as a connection error). The same server is used for the backends' retries and for
the reference behavior of the SDKs (sdk_retries_reference.py).
"""

import http.server
import threading
import time


class _Server(http.server.ThreadingHTTPServer):

    daemon_threads = True    # close() does not wait for a reply in progress
    block_on_close = False


class FailingServer:

    def __init__(self, failures=(), success=(200, {}, b'{}'), delay_s=0.0):
        self.failures = list(failures)
        self.success = success
        self.delay_s = delay_s   # how long the success reply takes
        self.times = []          # monotonic time of each request
        self.requests = []       # {'method', 'path', 'headers', 'body'}
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):

            protocol_version = 'HTTP/1.1'

            def _answer(self):
                length = int(self.headers.get('Content-Length') or 0)
                body = self.rfile.read(length) if length else b''
                server.times.append(time.monotonic())
                server.requests.append({'method': self.command, 'path': self.path,
                                        'headers': dict(self.headers), 'body': body})
                reply = server.failures.pop(0) if server.failures else None
                if reply == 'drop':
                    self.close_connection = True
                    return
                if reply is None:
                    status, headers, content = server.success
                    time.sleep(server.delay_s)
                else:
                    status, headers = reply
                    content = b'{"error": {"message": "scripted failure"}}'
                data = content.encode() if isinstance(content, str) else content
                self.send_response(status)
                for name, value in (headers or {}).items():
                    self.send_header(name, value)
                if not any(name.lower() == 'content-type' for name in headers or {}):
                    self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_POST = _answer
            do_GET = _answer

            def log_message(self, *args):
                pass

        self.server = _Server(('127.0.0.1', 0), Handler)
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def gaps(self):
        """Seconds between consecutive requests."""
        return [round(b - a, 4) for a, b in zip(self.times, self.times[1:])]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
