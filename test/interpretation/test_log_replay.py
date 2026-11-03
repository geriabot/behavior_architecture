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

"""The call log, and the replay of a log with the recorded backend."""

import json
import os
from types import SimpleNamespace

from behavior_interpretation import InterpretationServer
from behavior_interpretation.call_log import read_calls
from behavior_interpretation.interpreters.llm.backends import FakeBackend

QUESTIONS = {
    'identify': {'prompt': 'Which of {ids}? {text}', 'fields': ['int:id'],
                 'choices_from': 'context.ids', 'uses_image': 'optional', 'max_retries': 1,
                 'fallback': -1},
    'needs': {'prompt': 'Needs {text}?', 'fields': ['bool:arm']},
}


def request(question, text, ids=(1, 2), images=()):
    return SimpleNamespace(question=question, request_id='dialogue-7', text=text,
                           context=json.dumps({'ids': list(ids)}), images=list(images),
                           budget_s=0.0, variant='')


def image(value):
    return SimpleNamespace(encoding='rgb8', width=1, height=1, step=3, data=bytes([value] * 3))


def run(server):
    """Make the same five calls (with a retry, a timeout and a fallback among them)."""
    return [
        server.interpret(request('identify', 'a', images=[image(1)])),
        server.interpret(request('identify', 'b', images=[image(1)])),
        server.interpret(request('identify', 'c')),
        server.interpret(request('needs', 'x')),
        server.interpret(request('needs', 'y')),
    ]


def test_log_and_replay(tmp_path):
    log_dir = str(tmp_path / 'log')
    fake = FakeBackend({'by_question': {
        'identify': ['{"id": 2}', 'nonsense', '{"id": 1}', '!timeout', '{"id": 9}'],
        'needs': ['{"arm": true}', '!down']}, 'model_version': 'fake:1'})
    original = run(InterpretationServer(questions=QUESTIONS, backends={'m': fake},
                                        log_dir=log_dir))
    assert [(r.source, r.answer, r.fallback_reason) for r in original] == [
        ('llm', '{"id": 2}', ''),
        ('llm', '{"id": 1}', ''),             # retried after an answer that is not JSON
        ('fallback', '{"id": -1}', 'invalid'),  # a timeout, then an id not offered
        ('llm', '{"arm": true}', ''),
        ('invalid', '', 'model_down'),
    ]

    calls = read_calls(log_dir)
    assert len(calls) == 5
    first = calls[0]
    for key in ('time', 'question', 'request_id', 'prompt', 'images', 'raw_answer', 'answer',
                'source', 'fallback_reason', 'latency_s', 'tokens', 'model_version',
                'attempts', 'interpreters', 'route', 'backend'):
        assert key in first, key
    assert (first['question'], first['request_id'], first['prompt']) == \
        ('identify', 'dialogue-7', 'Which of [1, 2]? a')
    assert (first['raw_answer'], first['answer'], first['model_version']) == \
        ('{"id": 2}', {'id': 2}, 'fake:1')
    assert [a.get('error') for a in calls[2]['attempts']] == ['timeout', '']
    # The same image twice: one file
    assert calls[0]['images'] == calls[1]['images']
    assert os.listdir(os.path.join(log_dir, 'images')) == [
        os.path.basename(calls[0]['images'][0])]

    # The replay, without a model, takes the same decisions
    replay = InterpretationServer(
        questions=QUESTIONS, routes={'m': {'backend': 'recorded', 'log_files': log_dir}})
    replayed = run(replay)
    assert [(r.source, r.answer, r.fallback_reason) for r in replayed] == \
        [(r.source, r.answer, r.fallback_reason) for r in original]
    # A call that is not in the log
    missing = replay.interpret(request('needs', 'never asked'))
    assert (missing.source, missing.fallback_reason) == ('invalid', 'model_down')
    assert 'no recorded answer' in missing.message


def test_no_log_dir_writes_nothing(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    fake = FakeBackend({'answers': ['{"arm": false}']})
    InterpretationServer(questions=QUESTIONS, backends={'m': fake}).interpret(
        request('needs', 'x'))
    assert os.listdir(tmp_path) == []
