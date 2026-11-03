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
The interpretation server without ROS: the questions, the interpreters and one call.

One call: look up the question and its variant (the request's, else the server's
configuration), apply the launch-time override of its interpreters, read the context, the
fields and the images, and try the question's chain of interpreters in order. The first that
answers gives the answer (source: its name). One that declines passes the call to the next;
one that fails (no valid answer) ends the chain. Without an answer, the question's fallback
(source "fallback") or nothing (source "invalid"), with the reason. Nothing is kept between
calls; every call is written to the call log.

cancel(request_id) cancels the calls in progress with that request_id (for example when the
dialogue that asked them has ended): each returns at once with source "invalid" and the reason
"cancelled", whatever its fallback. The server knows only the calls in progress: a cancel
does not affect later calls of the same request_id.

The configuration (all of it given when the server starts, never by a request):

  questions_file   YAML with `questions:` and, optionally, `routes:`, `default_route:`,
                   `templates:`, `variants:`, `interpreter_overrides:`, `route_overrides:`,
                   `question_modules:`
  templates_file   the prompt templates (templates.py)
  templates_files  more templates files, read after templates_file; a later file replaces the
                   templates of the same name of the earlier ones
  question_modules Python modules with Question subclasses, rules and formatters
  routes           {name: {backend, model, timeout_s, <backend options>}}
  variants         {question: variant}: the variant used when a request names none
  interpreter_overrides  {question: chain}, e.g. {"identify_target": "rule:most_centered"}
  route_overrides  {question: route}: the route of the question, replacing its definition's
                   and its variants'; {question: {route: <route>, variants: false}} replaces
                   only its definition's (a variant that names its own route keeps it)
  log_dir          where the call log is written ('' = no log)

start_warmups() loads the models of the routes with `warmup: true` in the background.
"""

import json
import math
import threading
import time

from behavior_interpretation.call_log import CallLog
from behavior_interpretation.call_log import LOG_FILE
from behavior_interpretation.images import EncodedImage
from behavior_interpretation.interpreters.base import Call
from behavior_interpretation.interpreters.base import Outcome
from behavior_interpretation.interpreters.llm import LLMInterpreter
from behavior_interpretation.interpreters.llm import Route
from behavior_interpretation.interpreters.llm.backends import BackendError
from behavior_interpretation.interpreters.rule import RuleInterpreter
from behavior_interpretation.questions import ConfigError
from behavior_interpretation.questions import ModuleContents
from behavior_interpretation.questions import parse_interpreters
from behavior_interpretation.questions import RequestError
from behavior_interpretation.questions import resolve_spec
from behavior_interpretation.templates import flatten_templates
from behavior_interpretation.templates import load_templates
import yaml


class Result:
    """The response of a call (the fields of Interpret.srv's response)."""

    def __init__(self, success, answer=None, source='', fallback_reason='', message=''):
        self.success = success
        self.answer_object = answer
        self.answer = '' if answer is None else json.dumps(answer, ensure_ascii=False)
        self.source = source
        self.fallback_reason = fallback_reason
        self.message = message

    def __repr__(self):
        return (f'Result(success={self.success}, answer={self.answer!r}, source={self.source!r}, '
                f'fallback_reason={self.fallback_reason!r}, message={self.message!r})')


def _get(request, name, default):
    if isinstance(request, dict):
        return request.get(name, default)
    return getattr(request, name, default)


def _route_override(name, value):
    """
    Read a route override: `route`, or `{route: <route>, variants: <bool>}`.

    Return (route, whether it also replaces the routes of the question's variants); the
    plain form replaces them.
    """
    if not isinstance(value, dict):
        return (value or '', True)
    unknown = set(value) - {'route', 'variants'}
    if unknown:
        raise ConfigError(f'route_overrides: {name}: unknown keys {sorted(unknown)} '
                          f'(route, variants)')
    variants = value.get('variants', True)
    if isinstance(variants, str) and variants.strip().lower() in ('true', 'false'):
        variants = variants.strip().lower() == 'true'
    if not isinstance(variants, bool):
        raise ConfigError(f'route_overrides: {name}: variants must be true or false, '
                          f'not {variants!r}')
    route = value.get('route') or ''
    if not isinstance(route, str):
        raise ConfigError(f'route_overrides: {name}: route must be the name of a route')
    return (route, variants)


def _merge(base, override):
    merged = dict(base or {})
    merged.update(override or {})
    return merged


class InterpretationServer:

    def __init__(self, questions=None, questions_file='', templates=None, templates_file='',
                 question_modules=(), routes=None, default_route='', variants=None,
                 interpreter_overrides=None, log_dir='', log_file=LOG_FILE, node=None,
                 backends=None, interpreters=None, route_overrides=None, templates_files=()):
        """
        Build the server; ConfigError if the configuration cannot answer its questions.

        `backends` ({route: Backend}) and `interpreters` ({name: Interpreter}) give objects
        instead of building them (tests, or an interpreter of a system's own).
        """
        data = {}
        if questions_file:
            with open(questions_file, encoding='utf-8') as stream:
                data = yaml.safe_load(stream) or {}
            if not isinstance(data, dict) or not isinstance(data.get('questions', {}), dict):
                raise ConfigError(f'{questions_file}: expected a mapping with `questions:`')
        self.definitions = _merge(data.get('questions'), questions)
        if not self.definitions:
            raise ConfigError('No questions (questions_file or questions)')

        self.templates = flatten_templates(data.get('templates') or {})
        self.templates.update(load_templates(templates_file))
        for path in templates_files or ():
            if path:
                self.templates.update(load_templates(path))
        self.templates.update(templates or {})

        modules = list(data.get('question_modules') or []) + list(question_modules or [])
        self.modules = ModuleContents(modules)

        route_configs = _merge(data.get('routes'), routes)
        backends = backends or {}
        self.routes = {}
        for name in sorted(set(route_configs) | set(backends)):
            self.routes[name] = Route(name, route_configs.get(name), node, backends.get(name))
        default_route = default_route or data.get('default_route') or ''
        if default_route and default_route not in self.routes:
            raise ConfigError(f'default_route "{default_route}" is not one of the routes')

        self.interpreters = {
            'llm': LLMInterpreter(self.routes, default_route),
            'rule': RuleInterpreter(self.modules.rules),
        }
        self.interpreters.update(interpreters or {})

        self.variants = {k: v for k, v in _merge(data.get('variants'), variants).items() if v}
        self.overrides = {k: v for k, v in
                          _merge(data.get('interpreter_overrides'), interpreter_overrides).items()
                          if v}
        self.route_overrides = {}   # question -> (route, whether it replaces its variants')
        for name, value in _merge(data.get('route_overrides'), route_overrides).items():
            override = _route_override(name, value)
            if override[0]:
                self.route_overrides[name] = override
        for table, label in ((self.variants, 'variants'),
                             (self.overrides, 'interpreter_overrides'),
                             (self.route_overrides, 'route_overrides')):
            unknown = set(table) - set(self.definitions)
            if unknown:
                raise ConfigError(f'{label}: unknown questions {sorted(unknown)}')
        for name, (route, _) in self.route_overrides.items():
            if route not in self.routes:
                raise ConfigError(f'route_overrides: {name}: no route "{route}" '
                                  f'(routes: {sorted(self.routes)})')

        self.log = CallLog(log_dir, log_file)
        self.warmups = {}        # route -> loading, ready or failed (start_warmups)
        self._in_progress = {}   # request_id -> the calls in progress, for cancel()
        self._in_progress_lock = threading.Lock()
        self._check()

    # --- Configuration ----------------------------------------------------------------------

    def question(self, name, variant=''):
        """Build the Question of a question with a variant (and the overrides) applied."""
        definition = self.definitions[name]
        route, over_variants = self.route_overrides.get(name, ('', True))
        if route and not over_variants and isinstance(definition, dict):
            # The definition's route only: a variant that names its own route keeps it
            definition = dict(definition, route=route)
        spec = resolve_spec(name, definition, variant)
        if name in self.overrides:
            spec['interpreter'] = self.overrides[name]
        if route and over_variants:
            spec['route'] = route
        question_class = self.modules.question_class(name, spec)
        return question_class(name, spec, self.templates, self.modules.formatters)

    def chain(self, question):
        chain = parse_interpreters(question.spec.get('interpreter'))
        for interpreter, _ in chain:
            if interpreter not in self.interpreters:
                raise ConfigError(f'{question.name}: unknown interpreter "{interpreter}" '
                                  f'(interpreters: {sorted(self.interpreters)})')
        return chain

    def _check(self):
        """Every question, with each of its variants, can be answered (at start, not later)."""
        for name, definition in self.definitions.items():
            variants = [''] + sorted((definition or {}).get('variants') or {})
            if name in self.variants and self.variants[name] not in variants:
                raise ConfigError(f'variants: the question {name} has no variant '
                                  f'"{self.variants[name]}"')
            for variant in variants:
                try:
                    question = self.question(name, variant)
                except RequestError as exc:
                    raise ConfigError(str(exc)) from exc
                if not question.spec.get('fields_from'):
                    try:
                        question.fields({})
                    except ValueError as exc:
                        raise ConfigError(f'{name}: {exc}') from exc
                for interpreter, option in self.chain(question):
                    self.interpreters[interpreter].check(question, option)

    # --- Warm-up ----------------------------------------------------------------------------

    def start_warmups(self, report=None):
        """
        Load the model of every route with `warmup: true`, each in a thread of its own.

        Return the threads at once (the server answers meanwhile). `report(ok, message)` is
        called when each warm-up ends; `warmups` holds each route's state: loading, ready or
        failed.
        """
        threads = []
        for route in self.routes.values():
            if not route.warmup:
                continue
            self.warmups[route.name] = 'loading'
            thread = threading.Thread(target=self._warm_up, args=(route, report), daemon=True,
                                      name=f'interpretation-warmup-{route.name}')
            thread.start()
            threads.append(thread)
        return threads

    def _warm_up(self, route, report):
        start = time.monotonic()
        try:
            message = route.backend.warm_up(route.model)
            ok = True
        except BackendError as exc:
            message = str(exc)
            ok = False
        except Exception as exc:  # noqa: B902 (a warm-up must not end the server)
            message = f'{type(exc).__name__}: {exc}'
            ok = False
        self.warmups[route.name] = 'ready' if ok else 'failed'
        if report is not None:
            report(ok, f'route {route.describe()}: warm-up '
                       f'{"done" if ok else "failed"} in {time.monotonic() - start:.1f} s: '
                       f'{message}')

    # --- One call ---------------------------------------------------------------------------

    def interpret(self, request):
        """Answer a request (Interpret.Request, or any object or dict with its fields)."""
        name = _get(request, 'question', '')
        variant = _get(request, 'variant', '') or self.variants.get(name, '')
        call = Call(name, _get(request, 'request_id', ''), None, _get(request, 'text', '') or '',
                    (), _get(request, 'budget_s', 0.0) or 0.0, variant)
        record = {'question': name, 'variant': variant, 'request_id': call.request_id,
                  'text': call.text, 'budget_s': call.budget_s}
        max_tokens = _get(request, 'max_tokens', 0) or 0
        if max_tokens:
            record['max_tokens'] = max_tokens  # only when given: other calls log as before
        self._track(call, True)
        try:
            result = self._interpret(call, request, record)
        except (RequestError, ConfigError) as exc:
            result = Result(False, message=str(exc))
        finally:
            self._track(call, False)
        record.update({'answer': result.answer_object, 'source': result.source,
                       'fallback_reason': result.fallback_reason, 'message': result.message,
                       'success': result.success})
        self._write_log(call, record)
        return result

    def _interpret(self, call, request, record):
        if call.question not in self.definitions:
            raise RequestError(f'Unknown question "{call.question}" '
                               f'(questions: {sorted(self.definitions)})')
        if not math.isfinite(call.budget_s) or call.budget_s < 0.0:
            raise RequestError(f'budget_s must be 0 or positive, not {call.budget_s}')
        max_tokens = _get(request, 'max_tokens', 0) or 0
        if not isinstance(max_tokens, int) or isinstance(max_tokens, bool) or max_tokens < 0:
            raise RequestError(f'max_tokens must be 0 or a positive integer, not {max_tokens!r}')
        call.max_tokens = max_tokens
        question = self.question(call.question, call.variant)

        context_text = _get(request, 'context', '') or ''
        try:
            call.context = json.loads(context_text) if context_text.strip() else {}
        except json.JSONDecodeError as exc:
            raise RequestError(f'The context is not JSON: {exc}') from exc
        if not isinstance(call.context, dict):
            raise RequestError('The context must be a JSON object')
        record['context'] = call.context

        call.fields = question.fields(call.context)
        call.images = self._images(question, list(_get(request, 'images', []) or []))
        record['images'] = [self.log.save_image(image) for image in call.images] \
            if self.log.enabled else [image.sha256 for image in call.images]

        chain = self.chain(question)
        record['interpreters'] = [f'{n}:{o}' if o else n for n, o in chain]
        outcome = Outcome.declined()
        messages = []
        for interpreter, option in chain:
            record['interpreter'] = interpreter
            outcome = self.interpreters[interpreter].answer(question, call, option)
            if outcome.message:
                messages.append(f'{interpreter}: {outcome.message}')
            if outcome.kind == Outcome.ANSWERED:
                return Result(True, outcome.answer, interpreter, '', '; '.join(messages))
            if outcome.kind == Outcome.FAILED:
                break
        else:
            record['interpreter'] = ''
        reason = outcome.reason if outcome.kind == Outcome.FAILED else 'declined'
        if reason == 'cancelled':
            return Result(True, None, 'invalid', reason, '; '.join(messages))
        fallback = question.fallback(call.context, reason)
        if fallback is not None:
            return Result(True, fallback, 'fallback', reason, '; '.join(messages))
        return Result(True, None, 'invalid', reason, '; '.join(messages))

    def cancel(self, request_id):
        """Cancel the calls in progress with this request_id; return how many there were."""
        if not request_id:
            return 0
        with self._in_progress_lock:
            calls = list(self._in_progress.get(request_id, ()))
        for call in calls:
            call.cancelled.set()
        return len(calls)

    def _track(self, call, active):
        if not call.request_id:
            return
        with self._in_progress_lock:
            calls = self._in_progress.setdefault(call.request_id, set())
            if active:
                calls.add(call)
            else:
                calls.discard(call)
                if not calls:
                    del self._in_progress[call.request_id]

    def _images(self, question, images):
        mode = question.uses_image()
        if mode == 'false':
            return []
        if mode == 'true' and not images:
            raise RequestError(f'{question.name}: the question needs an image')
        limit = int(question.spec.get('max_images', 1))
        if len(images) > limit:
            raise RequestError(f'{question.name}: {len(images)} images, the question takes at '
                               f'most {limit} (max_images)')
        try:
            return [image if isinstance(image, EncodedImage) else EncodedImage(image)
                    for image in images]
        except ValueError as exc:
            raise RequestError(f'{question.name}: {exc}') from exc

    def _write_log(self, call, record):
        details = dict(call.details)
        attempts = details.get('attempts') or []
        record.update(details)
        record['raw_answer'] = attempts[-1].get('raw') if attempts else None
        record['latency_s'] = round(call.elapsed(), 4)
        record['tokens'] = {
            'prompt': sum(a.get('prompt_tokens', 0) for a in attempts),
            'completion': sum(a.get('completion_tokens', 0) for a in attempts),
        }
        record.setdefault('model_version', '')
        self.log.write(record)
