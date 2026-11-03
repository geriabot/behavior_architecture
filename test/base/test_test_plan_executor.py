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

"""test_plan_executor runs the trees of a directory, in file-name order, to the end."""

import os
import signal
import subprocess
import threading

import pytest

EXECUTABLE = os.environ.get('TEST_PLAN_EXECUTOR', '')
PLUGIN = os.environ.get('TEST_PLAN_PLUGIN', '')

TREE = ('<root BTCPP_format="4"><BehaviorTree ID="{id}"><Sequence>'
        '<Sleep msec="100"/><{last}/></Sequence></BehaviorTree></root>')


def run(directory, done, timeout=30.0):
    """Run test_plan_executor on a directory until a line contains `done`; its output."""
    process = subprocess.Popen(
        [EXECUTABLE, str(directory), PLUGIN], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    lines = []
    finished = threading.Event()

    def read():
        for line in process.stdout:
            lines.append(line)
            if done in line:
                finished.set()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    finished.wait(timeout)
    process.send_signal(signal.SIGINT)
    try:
        process.wait(10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    reader.join(5)
    return finished.is_set(), ''.join(lines)


@pytest.mark.skipif(not EXECUTABLE, reason='TEST_PLAN_EXECUTOR is not set')
def test_runs_every_tree_in_order(tmp_path):
    (tmp_path / 'step_01.xml').write_text(TREE.format(id='First', last='AlwaysSuccess'))
    (tmp_path / 'step_02.xml').write_text(TREE.format(id='Second', last='AlwaysSuccess'))
    finished, output = run(tmp_path, 'All behavior trees executed successfully')
    assert finished, output
    assert output.count('Behavior tree (test_bt_runner): SUCCESS') == 2, output
    assert output.index('Executing behavior tree: step_01.xml') < \
        output.index('Executing behavior tree: step_02.xml'), output


@pytest.mark.skipif(not EXECUTABLE, reason='TEST_PLAN_EXECUTOR is not set')
def test_stops_at_a_failing_tree(tmp_path):
    (tmp_path / 'step_01.xml').write_text(TREE.format(id='First', last='AlwaysFailure'))
    (tmp_path / 'step_02.xml').write_text(TREE.format(id='Second', last='AlwaysSuccess'))
    finished, output = run(tmp_path, 'State: FAILURE')
    assert finished, output
    assert 'Behavior tree failed' in output and 'step_01.xml' in output, output
    assert 'Executing behavior tree: step_02.xml' not in output, output


@pytest.mark.skipif(not EXECUTABLE, reason='TEST_PLAN_EXECUTOR is not set')
def test_reports_a_tree_that_cannot_be_built(tmp_path):
    (tmp_path / 'step_01.xml').write_text(TREE.format(id='Sleep', last='AlwaysSuccess'))
    finished, output = run(tmp_path, 'State: FAILURE')
    assert finished, output
    assert "'Sleep'" in output and 'registered BT node' in output, output
