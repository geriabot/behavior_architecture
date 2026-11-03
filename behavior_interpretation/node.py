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
The ROS node `interpretation_server`: the service Interpret, answered by the server.

Service (behavior_architecture/srv/Interpret): `interpretation/interpret`, relative, so that
each system's server lives in the system's namespace (parameter service_name). A second
service, `interpretation/cancel` (behavior_architecture/srv/CancelInterpretation; parameter
cancel_service_name, by default `cancel` next to service_name), cancels the calls in progress
of a request_id.

Parameters (each system launches its own server, configured only here):
  questions_file, templates_file   YAML files (server.py, templates.py)
  templates_files                  [YAML, ...] read after templates_file (later files win)
  question_modules                 [module or path.py, ...]
  (a path may be written package://<package>/<path>, in the package's share directory)
  routes.<route>.backend|model|timeout_s|<option>   e.g. routes.text.backend: ollama
  default_route                    the route of the questions without one
  variants.<question>              the variant used when a request names none
  interpreter_overrides.<question> its chain of interpreters, e.g. "rule:most_centered,llm"
  route_overrides.<question>       its route, e.g. "cloud" (replacing the questions file's)
  log_dir, log_file                the call log ('' = no log; default file calls.jsonl)
  service_name, cancel_service_name  the services ('' cancel: `cancel` next to service_name)

Calls are answered concurrently (the server keeps no state between them).
"""

import os
import posixpath
import signal
import sys
import threading
import traceback

from ament_index_python.packages import get_package_share_directory
from behavior_architecture.srv import CancelInterpretation
from behavior_architecture.srv import Interpret
from behavior_interpretation.call_log import LOG_FILE
from behavior_interpretation.server import InterpretationServer
from rcl_interfaces.msg import ParameterDescriptor
from rcl_interfaces.msg import ParameterType
import rclpy
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import ExternalShutdownException
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node

PARAMETERS = {
    'questions_file': '',
    'templates_file': '',
    'default_route': '',
    'log_dir': '',
    'log_file': LOG_FILE,
    'service_name': 'interpretation/interpret',
    'cancel_service_name': '',
}


def cancel_service_for(service_name):
    """Return the cancel service next to an interpret service (interpretation/cancel)."""
    return posixpath.join(posixpath.dirname(service_name), 'cancel')


def resolve_path(path):
    """Resolve package://<package>/<path> to the package's share directory."""
    prefix = 'package://'
    if not path or not path.startswith(prefix):
        return path
    package, _, relative = path[len(prefix):].partition('/')
    return os.path.join(get_package_share_directory(package), relative)


def nested(parameters):
    """{'text.backend': p, 'text.model': q} -> {'text': {'backend': .., 'model': ..}}."""
    tree = {}
    for dotted, parameter in parameters.items():
        node = tree
        parts = dotted.split('.')
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = parameter.value
    return tree


class InterpretationServerNode(Node):

    def __init__(self, **kwargs):
        super().__init__('interpretation_server', allow_undeclared_parameters=True,
                         automatically_declare_parameters_from_overrides=True, **kwargs)
        for name, default in PARAMETERS.items():
            if not self.has_parameter(name):
                self.declare_parameter(name, default)
        for name in ('question_modules', 'templates_files'):
            if not self.has_parameter(name):
                self.declare_parameter(name, [], ParameterDescriptor(
                    type=ParameterType.PARAMETER_STRING_ARRAY))

        value = self.get_parameter
        self.server = InterpretationServer(
            questions_file=resolve_path(value('questions_file').value),
            templates_file=resolve_path(value('templates_file').value),
            templates_files=[resolve_path(f) for f in (value('templates_files').value or [])
                             if f],
            question_modules=[resolve_path(m) for m in (value('question_modules').value or [])
                              if m],
            routes=nested(self.get_parameters_by_prefix('routes')),
            default_route=value('default_route').value,
            variants=nested(self.get_parameters_by_prefix('variants')),
            interpreter_overrides=nested(self.get_parameters_by_prefix('interpreter_overrides')),
            route_overrides=nested(self.get_parameters_by_prefix('route_overrides')),
            log_dir=value('log_dir').value,
            log_file=value('log_file').value or LOG_FILE,
            node=self)

        group = ReentrantCallbackGroup()
        self.service = self.create_service(
            Interpret, value('service_name').value, self.on_interpret, callback_group=group)
        self.cancel_service = self.create_service(
            CancelInterpretation,
            value('cancel_service_name').value or cancel_service_for(value('service_name').value),
            self.on_cancel, callback_group=group)
        for route in self.server.routes.values():
            self.get_logger().info(f'route {route.describe()}')
        for name in sorted(self.server.definitions):
            question = self.server.question(name, self.server.variants.get(name, ''))
            chain = ','.join(f'{n}:{o}' if o else n for n, o in self.server.chain(question))
            self.get_logger().info(f'question {name} -> {chain}')
        if self.server.log.enabled:
            self.get_logger().info(f'call log: {self.server.log.path}')
        self.get_logger().info(f'Interpretation server ready on {self.service.srv_name}')

    def on_cancel(self, request, response):
        if not request.request_id:
            response.success = False
            response.message = 'request_id is empty'
            return response
        response.cancelled = self.server.cancel(request.request_id)
        response.success = True
        response.message = f'{response.cancelled} calls in progress cancelled'
        self.get_logger().info(f'cancel {request.request_id}: {response.message}')
        return response

    def on_interpret(self, request, response):
        try:
            result = self.server.interpret(request)
        except Exception as exc:  # noqa: B902 (a failed call must still get a response)
            self.get_logger().error(f'{request.question}: {exc}\n{traceback.format_exc()}')
            response.success = False
            response.message = f'Internal error: {exc}'
            return response
        response.success = result.success
        response.answer = result.answer
        response.source = result.source
        response.fallback_reason = result.fallback_reason
        response.message = result.message
        reason = f' ({result.fallback_reason})' if result.fallback_reason else ''
        if result.success:
            self.get_logger().info(f'{request.question}: {result.source}{reason} '
                                   f'{result.answer or result.message}')
        else:
            self.get_logger().warning(f'{request.question}: failed: {result.message}')
        return response


def main(args=None):
    """Run the server until Ctrl-C or a shutdown; exit quietly, with code 0."""
    rclpy.init(args=args)
    node = None
    try:
        node = InterpretationServerNode()
        executor = MultiThreadedExecutor()
        executor.add_node(node)
        executor.spin()
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        # Under ros2 run or ros2 launch, a Ctrl-C in the terminal reaches the server twice (from
        # the terminal and from its parent): a second interrupt must not cut the cleanup short
        _ignore_interrupts()
        if node is not None:
            node.destroy_node()
        rclpy.try_shutdown()
    return 0


def _ignore_interrupts():
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGINT, signal.SIG_IGN)


if __name__ == '__main__':
    sys.exit(main())
