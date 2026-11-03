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

"""The server process ends quietly, with code 0, on Ctrl-C, also when it arrives twice."""

import signal
import subprocess
import sys
import threading
import time

import pytest

pytest.importorskip('rclpy')
pytest.importorskip('behavior_architecture.srv')


def start_server(tmp_path):
    questions = tmp_path / 'questions.yaml'
    questions.write_text('questions:\n  q:\n    prompt: "{text}"\n    fields: ["int:id"]\n')
    process = subprocess.Popen(
        [sys.executable, '-c', 'import sys; from behavior_interpretation.node import main; '
         'sys.exit(main())',
         '--ros-args', '-r', '__ns:=/shutdown_test', '-p', f'questions_file:={questions}',
         '-p', 'routes.text.backend:=fake'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    ready = threading.Event()

    def read():
        for line in process.stdout:
            lines.append(line)
            if 'Interpretation server ready' in line:
                ready.set()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    assert ready.wait(30), ''.join(lines)
    return process, reader, lines


@pytest.mark.parametrize('interrupts, gap_s', [(1, 0.0), (2, 0.0), (2, 0.01), (3, 0.005)])
def test_ctrl_c_exits_cleanly(tmp_path, interrupts, gap_s):
    process, reader, lines = start_server(tmp_path)
    for _ in range(interrupts):
        process.send_signal(signal.SIGINT)
        time.sleep(gap_s)
    try:
        code = process.wait(20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        pytest.fail('the server did not exit: ' + ''.join(lines))
    reader.join(5)
    output = ''.join(lines)
    assert code == 0, output
    assert 'Traceback' not in output, output
