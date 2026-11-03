# Interpretation

Interpretation answers the narrow questions that an orchestrator or a behavior tree asks about
unstructured input: which behavior fits a request, which values of a form a sentence gives,
which detection in an image is the target. A question is declared once in YAML; the
`interpretation_server` builds the prompt, asks a model (or applies a rule), validates the
answer against the declared fields and returns it, or returns a fallback, or reports that no
valid answer came back. It keeps no state between questions and commands nothing: the asker
decides what to do with the answer.

This document is the user guide and reference. The design is described in
[ARCHITECTURE.md](../ARCHITECTURE.md#interpretation), and the design record in
[INTERPRETATION_DESIGN.md](INTERPRETATION_DESIGN.md).

## Contents

- [Concepts](#concepts)
- [Running a server](#running-a-server)
- [Server parameters](#server-parameters)
- [The questions file](#the-questions-file)
- [The templates file](#the-templates-file)
- [Routes and backends](#routes-and-backends)
- [Question modules](#question-modules)
- [The service](#the-service)
- [Asking from an orchestrator](#asking-from-an-orchestrator)
- [Asking from a tree](#asking-from-a-tree)
- [Linking](#linking)
- [Call log and replay](#call-log-and-replay)
- [Worked example](#worked-example)
- [Troubleshooting](#troubleshooting)

## Concepts

| Term | Meaning |
|---|---|
| question | A named, declared question: its prompt template, the fields of its answer, how the answer is validated, its interpreters and its fallback. |
| interpreter | What answers a question: `llm` (a model) or `rule` (code). A question names one interpreter or a chain of them, tried in order. |
| route | A backend, a model and the backend's options. A question answered by `llm` names a route. |
| backend | The client of a model API: `ollama`, `openai`, `gemini`, `anthropic`, `llama_ros`, or `fake` and `recorded`, which need no model. |
| variant | A named set of changes to a question's definition (another template, another route...), chosen by the request or by the server's configuration. |
| context | A JSON object sent with a request: the values that the template, the fields and the validation of the question use. |
| source | Who gave the answer: the interpreter (`llm`, `rule`), `fallback`, or `invalid` when there is no answer. |
| fallback reason | Why there is no answer from an interpreter: `timeout`, `invalid`, `model_down`, `budget`, `declined` or `cancelled`. |

## Running a server

Each system runs its own server, in its own namespace, configured at launch:

```bash
ros2 launch behavior_architecture interpretation.launch.py \
  namespace:=my_robot params_file:=/path/to/interpretation_params.yaml
```

| Launch argument | Default | Meaning |
|---|---|---|
| `namespace` | `""` | Namespace of the server node. |
| `params_file` | `config/interpretation/example_params.yaml` | ROS parameters file of the node `interpretation_server`. |
| `questions_file`, `templates_file`, `log_dir` | `""` | When given, take precedence over the same parameters of `params_file`. |

Without arguments, the launch file starts the example of `config/interpretation/`: the questions
`choose_behavior` and `extract_slots`, answered by the `fake` backend, which returns the scripted
answer `{"behavior_id": -1}`.

`mission_executor.launch.py` starts a server too with `interpretation:=true`, with the arguments
`interpretation_params_file` and `interpretation_namespace`. The server runs by default in the
namespace of `mission_executor` (its argument `namespace`, by default none), where the
orchestrator and the BT nodes resolve the relative service name:

```bash
ros2 launch behavior_architecture mission_executor.launch.py config_file:=/path/to/mission.yaml \
  namespace:=my_robot \
  interpretation:=true interpretation_params_file:=/path/to/interpretation_params.yaml
# mission_executor's nodes and the service /my_robot/interpretation/interpret, in /my_robot
```

A system that launches its server in a namespace of its own (`interpretation_namespace:=ns`, or
`interpretation.launch.py namespace:=ns`) gives its askers the absolute service name
(`/ns/interpretation/interpret`).

The server validates its whole configuration when it starts (every question and each of its
variants, templates, routes, rules and modules) and exits with the reason if any of them cannot
be used. Once ready, it logs each route, each question with its chain of interpreters, the path
of the call log and `Interpretation server ready on interpretation/interpret`. Ctrl-C stops it;
the process exits with code 0.

## Server parameters

```yaml
/**/interpretation_server:
  ros__parameters:
    questions_file: package://my_robot/config/questions.yaml
    templates_file: package://my_robot/config/templates.yaml
    question_modules: ["my_robot.interpretation"]
    default_route: text
    routes:
      text:
        backend: ollama
        model: "qwen2.5vl:7b"
        url: "http://localhost:11434"
        keep_alive: 30m
        timeout_s: 180.0
      cloud:
        backend: openai
        model: gpt-4o-mini
        api_key_env: OPENAI_API_KEY
    variants:
      choose_behavior: terse
    interpreter_overrides:
      identify_target: "rule:most_centered"
    route_overrides:
      choose_behavior: cloud
    log_dir: /tmp/interpretation
```

| Parameter | Default | Meaning |
|---|---|---|
| `questions_file` | `""` | The questions ([The questions file](#the-questions-file)). Required: a server without questions exits. |
| `templates_file` | `""` | The prompt templates ([The templates file](#the-templates-file)). |
| `templates_files` | `[]` | More templates files, read in order after `templates_file`: a template of a later file replaces the one of the same name of the earlier files (for example a system's templates over the shared ones). In a parameters file an empty list is written `[""]`. |
| `question_modules` | `[]` | Python modules, by import name or by the path of a `.py` file, with question classes, rules and formatters ([Question modules](#question-modules)). In a parameters file an empty list is written `[""]`. |
| `routes.<route>.<key>` | | The routes ([Routes and backends](#routes-and-backends)). |
| `default_route` | `""` | The route of the questions that name none. Empty: the first route in alphabetical order. |
| `variants.<question>` | | The variant used when a request names none. |
| `interpreter_overrides.<question>` | | The chain of interpreters of the question, replacing its definition, for example `"rule:most_centered,llm"`. |
| `route_overrides.<question>` | | The route of the question, replacing its definition and its variants' (for example to answer one question with another model without editing the questions file). It must be one of the routes. |
| `log_dir` | `""` | Directory of the call log; `""` writes no log. |
| `log_file` | `calls.jsonl` | File name of the call log in `log_dir`. |
| `service_name` | `interpretation/interpret` | Name of the service, relative to the node's namespace. |
| `cancel_service_name` | `""` | Name of the cancel service ([Cancelling calls](#cancelling-calls)); `""`: `cancel` next to `service_name` (`interpretation/cancel`). |

A file parameter accepts `package://<package>/<path>`, resolved in the share directory of the
package. A path starting with `~` is expanded for `log_dir` and for module files.

The questions file may also contain `routes`, `default_route`, `templates`, `variants`,
`interpreter_overrides`, `route_overrides` and `question_modules`. The server parameters take precedence over the
file, route by route and question by question.

## The questions file

```yaml
questions:
  identify_target:
    interpreter: [rule, llm]
    template: identify_target_with_image
    fields: ["int:id"]
    choices_from: context.detections
    uses_image: optional
    max_images: 1
    max_retries: 1
    fallback: none
    route: vision
    max_tokens: 50
    rule: most_centered
    variants:
      no_image: {template: identify_target, uses_image: false}
```

| Key | Default | Meaning |
|---|---|---|
| `interpreter` | `llm` | One interpreter, a list (`[rule, llm]`) or a comma-separated string (`"rule:most_centered,llm"`). `rule:<name>` names the rule to use. |
| `template` | `""` | Key of the prompt template in the templates file. |
| `prompt` | `""` | The prompt template written inline; used instead of `template`. |
| `fields` | none | The fields of the answer ([Fields](#fields)). |
| `fields_from` | `""` | A path into the context (`context.<key>`) that holds the fields, for questions whose fields come with each call. |
| `schema` | none | A JSON schema of the answer, sent to the model as is. Without `fields`, any JSON object is accepted. |
| `choices_from` | `""` | A path into the context that holds the valid values of the choice field: a list of values, a list of objects (their `id`), or a mapping (its keys). |
| `choices_field` | `""` | The field that `choices_from` constrains; required when the answer has more than one field. |
| `choices_extra` | `[-1]` | Values that are valid besides the choices, such as `-1` for "none of them". |
| `uses_image` | `false` | `true`: a request must bring at least one image; `false`: the images of a request are ignored; `optional`: they are sent if present. |
| `max_images` | `1` | The most images a request may bring; more is a malformed request. |
| `max_retries` | `0` | Further attempts after a failed or invalid answer of the model. |
| `fallback` | none | `none`, or the answer to give when there is no valid one. A value that is not an object needs exactly one field and becomes `{<field>: <value>}`. |
| `route` | `""` | The route of the `llm` interpreter; empty: `default_route`. |
| `max_tokens` | `0` | Token cap of the answer (0: none). A request's `max_tokens` replaces it for that call. Sent only by the `llm_manager` dialects. |
| `parse` | `loose` | How the model's text is read: `loose`, `strict` or `text` ([Validation](#validation), [Text answers](#text-answers)). |
| `allow_empty` | `false` | With `parse: text`, an empty text (or only blanks) is a valid answer. |
| `validate` | `fields` | `fields`: the answer must have the fields, typed; `object`: the parsed object is returned as it is ([Validation](#validation)). |
| `rule` | `""` | The rule of the `rule` interpreter when the chain does not name one. Empty: the question class's `answer_without_model()`. |
| `output_formatter` | `""` | A formatter, registered by a question module, that writes `{output_format}`. |
| `class` | `""` | A `Question` subclass of a question module for this question. |
| `variants` | `{}` | `<name>: {<key>: <value>, ...}`: the keys that the variant changes. |

Any other key is a configuration error.

### Fields

A field has a name and a type: `int`, `str`, `float`, `bool`, or an array of one of them
(`int[]`, `str[]`, `float[]`, `bool[]`). The names are the system's own. A list of fields can be
written in three forms:

```yaml
fields: ["int:behavior_id", "str[]:reasons"]          # "type:name"
fields: {behavior_id: int, reasons: "str[]"}          # a mapping, in order
fields: [{name: behavior_id, type: int}]              # or {key: <name>, value: <type>}
```

The aliases `integer`, `string`, `number`, `boolean` and `int-arr`, `str-arr`, `float-arr`,
`bool-arr` are accepted. With `fields_from`, the context holds a list in one of those forms, for
example `{"slots": ["str:name", "int:age"]}` with `fields_from: context.slots`.

From the fields the server builds the JSON schema sent to the model (every field required) and
the default `{output_format}` block of the prompt.

### Validation

A model's answer is parsed tolerantly: a `<think>...</think>` block before it, a code fence
around it, text around the JSON object, or a list (whose first object is taken) are accepted. It
is then validated strictly:

- Every field must be present with its type. A value of the wrong JSON type that reads as the
  right one is converted (`"3"` for an `int`, `"true"` for a `bool`, a number for a `str`).
  Keys that are not fields are dropped.
- With `choices_from`, the choice field must be one of the choices or of `choices_extra`.
- The question class's `validate()`, if any, may reject the answer or change it.

The answers of rules are validated the same way.

Two keys of a question change this, for a system that must read its answers exactly as its
former gateway did:

| Key | Value | Effect |
|---|---|---|
| `parse` | `strict` | The whole text must be JSON (`json.loads`): an object, or a list whose first item is an object. A `<think>` block or text around the JSON makes the answer invalid. A code fence is removed only where `llm_router` removed it: by the `ollama` and `openai` backends, not by `gemini` (in the `llm_router` dialect) or `llama_ros`. |
| `validate` | `object` | The parsed object is returned as it is: any keys, any JSON types, `{}` included. The fields (or `fields_from`) still make the schema and `{output_format}`; `choices_from` and the class's `validate()` still apply. The asker checks the answer. |

`parse: strict` with `validate: object` is what the attention system's `llm_router` returned:
every key of the object, which the asker then read field by field.

A question class may also read the text itself, by overriding `parse(text)` ([Question
modules](#question-modules)): it returns the object, or raises `InvalidAnswer`, and the object
is then validated as above.

### Text answers

A question whose answer is free text (a line to be said, a summary) has `parse: text`. The
model's text is not parsed: the answer is the object `{"text": "<the text>"}`, with the text as
the backend returns it (the `llm_router` dialects of `ollama` and `openai` remove a code fence
around a whole answer, and the streamed ones remove the blanks at its ends).

```yaml
questions:
  say_line:
    template: say_line
    parse: text
    max_retries: 1
    fallback: "Sorry, could you say that again?"
```

- No schema is sent to the model (no structured output), and `{output_format}` is empty unless
  the question has an `output_formatter`. `fields`, `fields_from` and `schema` are
  configuration errors; the answer has the one field `text` (a `str`).
- An empty text, or one of blanks only, is invalid (reason `invalid`) unless the question has
  `allow_empty: true`.
- Retries, fallbacks and `invalid` work as for any other question: a fallback that is not an
  object becomes `{"text": <value>}`, a rule may return the text as a string, and the class's
  `validate()` receives `{"text": ...}`.
- The `llm_router` dialect of `gemini` always asks for JSON (`responseMimeType`); a text
  question is answered there as the model writes it within that constraint.

## The templates file

```yaml
templates:
  choose_behavior: "A robot was asked: {text}\nBehaviors:\n{offered_behaviors}\n{output_format}"
  extract_slots: "A person said: {text}\nGive the values it states, as JSON:\n{output_format}"
```

| Slot | Value |
|---|---|
| `{<key>}` | The value of that key of the context: a string as it is, anything else as JSON. |
| `{text}` | The request's `text` (or the context's `text` when the request has none). |
| `{output_format}` | The JSON schema of the answer, indented by 2 with sorted keys, or the text of the question's `output_formatter`. |

A slot without a value is left empty, and `{{` and `}}` write a literal brace. The file may also
be a nested mapping, for example a ROS parameters file with the templates under
`ros__parameters`: every text is found by its dotted path and, when its name is unique in the
file, by its name.

The templates are read, in this order, from the `templates` of the questions file, from
`templates_file` and from each file of `templates_files`; a later source replaces the templates
of the same name of the earlier ones.

## Routes and backends

Every route has `backend` (required), `model` and `timeout_s` (default 120 s, the limit of one
attempt). The other keys are options of the backend.

| Backend | Options (default) |
|---|---|
| `ollama` | `url` (`http://localhost:11434`), `dialect` (`llm_router`), `temperature` (0), `num_ctx`, `keep_alive`, `think` (`false`), `stream` (`true` in `llm_manager`), `system_prompt`, `api_key_env` (only behind a proxy that needs a key) |
| `openai` | `url` (`https://api.openai.com/v1`; any OpenAI-compatible API), `dialect` (`llm_router`), `api_key_env` (`OPENAI_API_KEY` when `url` is not set), `temperature` (0), `reasoning_effort` (`none`), `system_prompt` |
| `gemini` | `url` (`https://generativelanguage.googleapis.com`), `dialect` (`llm_router`), `api_key_env` (`GEMINI_API_KEY`, then `GOOGLE_API_KEY`), `system_prompt` |
| `anthropic` | `url` (`https://api.anthropic.com`), `dialect` (`llm_manager`, the only one), `api_key_env` (`ANTHROPIC_API_KEY`), `effort` (`low`; `""` sends none), `system_prompt` |
| `llama_ros` | `action` (`/llama/generate_response`); needs llama_ros and its action server |
| `fake` | `answers` (a list, used in order, the last one repeated), `by_question` (`{<question>: [answers]}`, used before `answers`), `delay_s` (0), `model_version` (`fake`). The answer `!timeout` simulates a timeout and `!down` an unreachable model. |
| `recorded` | `log_files`: a call log file, a directory with `calls.jsonl`, or a list of them ([Call log and replay](#call-log-and-replay)) |

An API key is read from the environment variable named in `api_key_env`; it is never a
parameter.

### Dialects

The `ollama`, `openai` and `gemini` backends have two dialects, `llm_router` and `llm_manager`.
Each reproduces the request of an existing LLM gateway, so that a system that moves to
interpretation sends a model the same request as before:

| | `llm_router` (default) | `llm_manager` |
|---|---|---|
| Ollama | `stream` false; `format` with `additionalProperties: false`; `options.temperature`; `num_ctx` and `keep_alive` only when set; no token cap | optional system message; `stream` (default true); `keep_alive` (default -1); `think`; `options.num_predict` (`max_tokens`, plus 4096 when the model may think) and `num_ctx` (default 8192) |
| OpenAI-compatible | text part followed by one `image_url` part per image; `temperature`; strict `response_format` named `answer` | optional system message; streamed with usage; `reasoning_effort`; `max_completion_tokens` (at least 4096 with reasoning); `response_format` named `response`, strict only when every property is required; the body of the openai SDK, byte for byte (keys `model`, `stream`, `messages`, then the others in alphabetical order) |
| Gemini | `responseMimeType` JSON with the schema in Gemini's form; images as JPEG at quality 95 (needs OpenCV and numpy) | role `user`; optional `systemInstruction`; `maxOutputTokens`; thinking budget 0; images as PNG |

The `anthropic` backend (Claude models, Messages API) has only the `llm_manager` dialect, the
request of the interaction system's Claude provider: `max_tokens` (`max_tokens`, at least
4096, since thinking counts toward it), one user message (with one PNG `image` block per image
after the text), `system` (`system_prompt`), `output_config` with `effort` and, with a schema,
`format` (`json_schema`, with `additionalProperties: false` on every object), and, for the
models that support it, the server-side refusal fallback (`fallbacks: default` and its beta
header). A schema that the API rejects (HTTP 400) is sent once more without `format`. The
request is not streamed, and carries no prompt-caching markers.

Images are sent in the order of the request, encoded once as lossless PNG without resizing
(except as noted for Gemini). Supported encodings: `rgb8`, `bgr8`, `8UC3` (read as RGB),
`rgba8`, `bgra8`, `mono8`, `8UC1`.

### Limits and budget

Each attempt is limited by the route's `timeout_s` and by what remains of the request's
`budget_s` (0: no budget). A timed-out attempt fails with `timeout`, or with `budget` when the
budget was the shorter limit; a budget spent before an attempt also fails with `budget`. An
unreachable model or an HTTP error fails with `model_down`; an answer that does not validate
fails with `invalid`. Failed attempts are retried up to `max_retries`.

## Question modules

A question module, listed in `question_modules`, can define:

- **Rules**: functions decorated with `@rule('<name>')` (or a module-level
  `RULES = {name: function}`). A rule receives the call (`call.question`, `call.context`,
  `call.text`, `call.images`, `call.fields`, `call.budget_s`, `call.variant`,
  `call.request_id`) and returns the answer as an object, or the value of the only field, or
  `None` to decline.
- **Formatters**: functions decorated with `@formatter('<name>')` (or `FORMATTERS`) that write
  the `{output_format}` block from the list of fields.
- **Question classes**: subclasses of `Question`, bound to a question by their class attribute
  `name` or by the `class` key of its definition.

```python
# my_robot/interpretation.py
from behavior_interpretation import Question, formatter, rule
from behavior_interpretation.validation import InvalidAnswer


@rule('most_centered')
def most_centered(call):
    """Answer only when the only field asked is the id of a detection; decline otherwise."""
    if [field.name for field in call.fields] != ['id']:
        return None
    detections = call.context.get('detections', [])
    if not detections:
        return -1
    return min(detections, key=lambda d: abs(d['x'] - 320))['id']


@formatter('names')
def names(fields):
    return 'Answer with: ' + ', '.join(field.name for field in fields)


class ExtractSlots(Question):
    name = 'extract_slots'

    def validate(self, answer, context):
        if answer.get('age', 0) < 0:
            raise InvalidAnswer('age: negative')
        return answer
```

| `Question` hook | Default |
|---|---|
| `build_prompt(context, text)` | Fill the template with the context, `text` and `output_format`. |
| `parse(text)` | Read the model's text as the question's `parse` says (`loose`, `strict` or `text`). Return an object; raise `InvalidAnswer` to reject the text. |
| `format_output(fields)` | The JSON schema of the fields, or the question's `output_formatter`. |
| `validate(answer, context)` | Return the answer; raise `InvalidAnswer` to reject it. |
| `fallback(context, reason)` | The question's `fallback`, or `None` (source `invalid`). |
| `answer_without_model(context)` | `None`. The `rule` interpreter calls it when no rule is named. |

## The service

`behavior_architecture/srv/Interpret`:

| Request field | Type | Meaning |
|---|---|---|
| `question` | `string` | The question's name. |
| `request_id` | `string` | Who asks (a request, a dialogue); written to the log only. |
| `context` | `string` | A JSON object (`""` is `{}`). |
| `text` | `string` | Optional free text, available to the template as `{text}`. |
| `images` | `sensor_msgs/Image[]` | Zero or more images; used only by questions that use images. |
| `budget_s` | `float64` | Time left for the answer; 0: only the per-attempt limit. |
| `variant` | `string` | A variant of the question; `""`: the server's configuration. |
| `max_tokens` | `int32` | The token cap of this call; 0: the question's `max_tokens`. Like the question's, it is sent only by the `llm_manager` dialects. Negative: the request is malformed. |

| Response field | Type | Meaning |
|---|---|---|
| `success` | `bool` | `false` only when the request cannot be taken: an unknown question or variant, a context that is not a JSON object, missing or excess images, missing `fields_from` or `choices_from` keys. |
| `answer` | `string` | A JSON object valid for the question, from an interpreter or the fallback; `""` when the source is `invalid`. |
| `source` | `string` | The interpreter that answered, `fallback` or `invalid`. |
| `fallback_reason` | `string` | `timeout`, `invalid`, `model_down`, `budget`, `declined`, `cancelled` ([Cancelling calls](#cancelling-calls)), or `""` when an interpreter answered. |
| `message` | `string` | A readable explanation (the route and attempt, errors, why the request was rejected). |

With the example server running, the service can be called from the command line:

```bash
ros2 service call /interpretation/interpret behavior_architecture/srv/Interpret \
  "{question: choose_behavior, context: '{\"offered_ids\": [1, 2]}', text: 'Follow me'}"
# success=True, answer='{"behavior_id": -1}', source='llm'

ros2 service call /interpretation/interpret behavior_architecture/srv/Interpret \
  "{question: extract_slots, context: '{\"slots\": [\"str:name\"]}', text: 'I am Ana'}"
# success=True, answer='', source='invalid', fallback_reason='invalid'
#   (the scripted answer has no field "name", and the question has no fallback)
```

### Cancelling calls

A second service, `interpretation/cancel` (`behavior_architecture/srv/CancelInterpretation`),
cancels the calls in progress that were asked with a `request_id`, for example when the
dialogue that asked them has ended:

| Field | Type | Meaning |
|---|---|---|
| `request_id` (request) | `string` | The `request_id` of the calls to cancel. |
| `success` | `bool` | `false` only when `request_id` is empty. |
| `cancelled` | `int32` | How many calls in progress were cancelled (0: none was). |
| `message` | `string` | A readable explanation. |

Each cancelled call returns at once with `success` true, source `invalid` and fallback reason
`cancelled`, whatever its fallback, and is not retried; its model's answer, if it comes, is
dropped. Only the calls in progress are cancelled: a later call with the same `request_id` is
answered as usual. A call without a `request_id` cannot be cancelled. Nothing changes for a
system that never calls the service.

```bash
ros2 service call /interpretation/cancel behavior_architecture/srv/CancelInterpretation \
  "{request_id: dialogue-7}"
# success=True, cancelled=1, message='1 calls in progress cancelled'
```

## Asking from an orchestrator

`InterpretationClient` (header `behavior_architecture/interpretation/interpretation_client.hpp`,
namespace `behavior_architecture::interpretation`) asks without blocking. It is owned by the
orchestrator and uses the orchestrator's node, which `mission_executor` spins.

```cpp
#include "behavior_architecture/interpretation/interpretation_client.hpp"

namespace interp = behavior_architecture::interpretation;

// Members
std::unique_ptr<interp::InterpretationClient> client_;
interp::InterpretationHandle slots_;

// In the constructor
client_ = std::make_unique<interp::InterpretationClient>(this);

// In a state that needs the answer: ask once...
slots_ = client_->ask("extract_slots", R"({"slots": ["str:name", "int:age"]})", heard_text);

// ...and poll in the following cycles
if (slots_.ready()) {
  const auto result = slots_.result();
  if (result.valid()) {
    // result.answer: {"name": ..., "age": ...}; result.source: "llm", "rule" or "fallback"
  } else {
    // result.success false: no response (result.message); else source "invalid"
  }
}
```

| Member | Description |
|---|---|
| `InterpretationClient(node, service_name = "interpretation/interpret", timeout = 120 s)` | `node`: an `rclcpp::Node`, an `rclcpp_lifecycle::LifecycleNode`, or a pointer to either. The service name is resolved in the node's namespace. `timeout` limits the wait for the service and for the response, from the call to `ask()`. |
| `ask(InterpretationRequest)` | Ask; returns a handle at once. `InterpretationRequest` has `question`, `context`, `text`, `images`, `budget_s`, `variant`, `request_id` and `max_tokens` (0: the question's). |
| `ask(question, context, text = "", images = {}, budget_s = 0)` | The same, with the common fields. |
| `choose_behavior(question, offered, context = "", text = "", budget_s = 0, variant = "")` | Ask the system's question that chooses a behavior. Adds `offered_behaviors` (`[{id, name, description}]`) and `offered_ids` (`[id]`) to the context. Throws `std::invalid_argument` if the context is not a JSON object. |
| `behavior_choice(result, offered, answer_field = "")` (static) | The chosen id, or `-1` with the reason: no response, no valid answer, none fits (`-1`), or an id that was not offered. `answer_field`: the field with the id (`""`: the only field). |
| `offered_context(offered, context)` (static) | The context with the offered behaviors added. |
| `cancel(request_id)` | Cancel the server's calls in progress with this `request_id` ([Cancelling calls](#cancelling-calls)); returns a `CancellationHandle` at once. The client of the cancel service is created at the first call. |
| `cancel_service_for(service_name)` (static) | The cancel service next to an interpret service: `interpretation/cancel` for the default. |
| `service_is_ready()`, `service_name()` | The state and the resolved name of the service. |

| `InterpretationHandle` | Description |
|---|---|
| `poll()` | `PENDING`, `DONE` or `FAILED`. Sends the request once the service is available; never waits. |
| `ready()` | `poll() != PENDING`. |
| `result()` | An `InterpretationResult`: `success`, `answer`, `source`, `fallback_reason`, `message`, and `valid()` (a response with an answer from an interpreter or a fallback). After `FAILED`, `success` is false and `message` says why. |
| `cancel()` | Forget the question; its response, if any, is ignored. |
| `valid()` | The handle belongs to a question (it is not default-constructed). |

| `CancellationHandle` | Description |
|---|---|
| `poll()`, `ready()` | As for `InterpretationHandle`. The request is sent once the cancel service is available: poll the handle until `ready()` to make sure that it is sent. |
| `cancelled()` | How many calls the server cancelled (after `DONE`). |
| `message()` | The server's message, or why the cancellation failed. |

```cpp
// When a dialogue ends: its questions still in progress return at once (source "invalid",
// fallback_reason "cancelled")
cancelling_ = client_->cancel(dialogue_id);
// ...and, in the following cycles, until cancelling_.ready()
```

A handle fails, without blocking, when the service does not appear, or does not answer, before
the client's timeout.

## Asking from a tree

Add the plugin library to the mission's `plugin_libraries`:

```yaml
plugin_libraries:
  - "libbehavior_architecture_interpretation_bt_nodes.so"
```

The nodes take the ROS node from the blackboard key `node` (an `rclcpp::Node::SharedPtr`, which
`mission_executor` stores); a tree without it fails to build.

### Interpret

| Port | Direction | Default | Meaning |
|---|---|---|---|
| `question` | in | | The question (required). |
| `variant` | in | `""` | A variant of the question. |
| `context` | in | `""` | A JSON object: written in the attribute, or read from the blackboard (`{key}`) as a string ([Passing a JSON context](#passing-a-json-context)). |
| `context_keys` | in | `""` | Blackboard entries added to the context, comma-separated: `key` (under its name) or `name=key`. |
| `text` | in | `""` | Free text. |
| `images` | in | | A blackboard entry of type `std::vector<sensor_msgs::msg::Image>`. |
| `budget_s` | in | `0` | Time left for the answer (0: no budget). |
| `max_tokens` | in | `0` | Token cap of this call (0: the question's `max_tokens`). |
| `wait` | in | `true` | `true`: `RUNNING` until the answer arrives. `false`: `SUCCESS` at once; the question in progress is stored under `result_key`. |
| `result_key` | in | `""` | With `wait="false"`, the blackboard key of the question in progress (`""`: `interpretation_pending_<question>`). |
| `request_id` | in | `""` | Who asks, for the server's log. |
| `service` | in | `interpretation/interpret` | The service, resolved in the namespace of the ROS node. |
| `timeout_s` | in | `120` | The client's timeout. |
| `fail_on_invalid` | in | `true` | Return `FAILURE` when there is no valid answer (source `invalid`). |
| `answer` | out | `{interpretation_answer}` | The answer (JSON). |
| `source` | out | `{interpretation_source}` | The source. |
| `fallback_reason` | out | `{interpretation_fallback_reason}` | The fallback reason. |
| `message` | out | `{interpretation_message}` | The server's or the client's message. |

With `wait="true"`, the node returns `SUCCESS` with a valid answer, and `FAILURE` when the server
did not answer, rejected the request (`success` false), or gave no valid answer and
`fail_on_invalid` is true. Halting the node cancels the question.

### InterpretationResult

Picks up the answer of an `Interpret` with `wait="false"`.

| Port | Direction | Default | Meaning |
|---|---|---|---|
| `question` | in | `""` | The question asked, for the default key. |
| `result_key` | in | `""` | The key given to `Interpret`, if any. |
| `wait` | in | `true` | `true`: `RUNNING` until the answer arrives. `false`: `FAILURE` if it has not arrived. |
| `fail_on_invalid` | in | `true` | As for `Interpret`. |
| `answer`, `source`, `fallback_reason`, `message` | out | as for `Interpret` | The result. |

It returns `FAILURE` when nothing was asked under the key.

```xml
<Sequence>
  <Interpret question="identity_feature" context="{feature_context}" images="{frames}"
             wait="false"/>
  <SubTree ID="ApproachPerson"/>
  <InterpretationResult question="identity_feature" answer="{feature}"/>
</Sequence>
```

### Passing a JSON context

The context is written in one of three ways, which can be combined:

```xml
<!-- A JSON object in the attribute, enclosed in single quotes (the XML then needs no escaping) -->
<Interpret question="extract_slots" context='{"slots": ["str:name", "int:age"]}' text="{heard}"/>

<!-- A JSON object, as a string, from a blackboard entry -->
<Interpret question="extract_slots" context="{slots_context}" text="{heard}"/>

<!-- Blackboard entries added to the context by name: {"offered_ids": <ids>, "task": <task>} -->
<Interpret question="choose_behavior" context_keys="offered_ids=ids, task" text="{heard}"/>
```

BehaviorTree.CPP reads a port value enclosed in braces as the name of a blackboard entry; a
`context` that is a JSON object is therefore recognized and taken as written, and any other
`{key}` is read from the blackboard. A double-quoted attribute also works with the quotes written
as `&quot;`.

`context_keys` adds each entry to the context (`context`, or `{}` without it), replacing a member
of the same name: a string that holds a JSON object or array is added as JSON, any other string as
a string, and numbers, booleans, vectors of them and vectors of strings as JSON. A missing entry,
an entry of another type, or a `context` that is not a JSON object makes the node fail before it
asks, with the reason in `message`.

## Linking

`find_package(behavior_architecture)` sets the default variables
(`behavior_architecture_LIBRARIES`, `_INCLUDE_DIRS`, `_TARGETS`, `_DEPENDENCIES`) to the base
library and its dependencies only. Interpretation is linked by naming it:

| CMake name | Contents |
|---|---|
| `behavior_architecture::interpretation` | `InterpretationClient`, with the generated C++ interface and its dependencies. |
| `${behavior_architecture_INTERPRETATION_TARGETS}` | The same, as a list. |
| `${behavior_architecture_INTERPRETATION_INTERFACE_TARGETS}` | Only the generated interface (`behavior_architecture/srv/interpret.hpp`). |
| `behavior_architecture_INTERPRETATION_FOUND` | `FALSE` when a dependency of interpretation is missing. |

```cmake
find_package(behavior_architecture REQUIRED)
target_link_libraries(my_orchestrator
  ${behavior_architecture_LIBRARIES}            # the base, as before
  behavior_architecture::interpretation)        # the client
```

`behavior_architecture::interpretation` does not include the base library, which is linked as
usual. The BT nodes' library is not exported or linked: runners load it by name. From Python,
the interface is `from behavior_architecture.srv import Interpret`.

## Call log and replay

With `log_dir` set, every call is appended as one JSON line to `<log_dir>/calls.jsonl`, and every
image is saved once as `<log_dir>/images/<sha256>.png`.

| Key | Content |
|---|---|
| `time` | UTC, ISO 8601. |
| `question`, `variant`, `request_id`, `context`, `text`, `budget_s` | The request (and `max_tokens`, when the request gives one). |
| `images` | Paths of the images, relative to `log_dir`. |
| `interpreters`, `interpreter` | The chain tried, and the interpreter that answered or failed last (`""` if every one declined). |
| `route`, `backend`, `model`, `prompt`, `schema` | What the model received (when `llm` was called). |
| `rule` | The rule applied (when `rule` was called). |
| `attempts` | Every attempt: `raw`, `error`, `latency_s`, `prompt_tokens`, `completion_tokens`. |
| `raw_answer` | The text of the last attempt. |
| `answer`, `source`, `fallback_reason`, `message`, `success` | The response. |
| `latency_s`, `tokens`, `model_version` | The whole call's latency, the tokens of all its attempts, and the model version that the backend reported. |

The `recorded` backend replays logs: for each call it finds the logged calls with the same
question and prompt and replays their attempts in order (a logged timeout as a timeout, a logged
error as `model_down`), repeating the last one; a call that is not in the logs fails with
`model_down`. A replay therefore reproduces the decisions of a run without a model, provided the
prompts are the same.

```yaml
routes:
  text:
    backend: recorded
    log_files: ["/tmp/interpretation/calls.jsonl"]
```

## Worked example

This example runs a mission whose trees ask the two example questions, with the `fake` backend,
logs the calls and replays them. It needs only the base.

```bash
source ~/ws/install/setup.bash
mkdir -p ~/interpretation_example && cd ~/interpretation_example

# A server: the example questions and templates, scripted answers, a call log
cat > interpretation_params.yaml <<EOF
/**/interpretation_server:
  ros__parameters:
    questions_file: package://behavior_architecture/config/interpretation/example_questions.yaml
    templates_file: package://behavior_architecture/config/interpretation/example_templates.yaml
    default_route: text
    routes:
      text:
        backend: fake
        model: ""
        answers: ['{"behavior_id": 2}']
        by_question:
          extract_slots: ['{"name": "Ana", "age": 34}']
    log_dir: $PWD/log
EOF

# First behavior: extract the values of a form from a sentence, waiting for the answer
cat > extract.xml <<'EOF'
<root BTCPP_format="4">
  <BehaviorTree ID="ExtractSlots">
    <Sequence>
      <Interpret question="extract_slots" context='{"slots": ["str:name", "int:age"]}'
                 text="My name is Ana and I am 34." answer="{slots}" source="{slots_source}"/>
    </Sequence>
  </BehaviorTree>
</root>
EOF

# Second behavior: choose a behavior, asking first and picking the answer up later
cat > choose.xml <<'EOF'
<root BTCPP_format="4">
  <BehaviorTree ID="ChooseBehavior">
    <Sequence>
      <Interpret question="choose_behavior"
                 context='{"offered_ids": [1, 2], "offered_behaviors": [{"id": 1, "name": "greet"}, {"id": 2, "name": "follow"}]}'
                 text="Come with me" wait="false"/>
      <Sleep msec="200"/>
      <InterpretationResult question="choose_behavior" answer="{choice}"/>
    </Sequence>
  </BehaviorTree>
</root>
EOF

# The mission: the example orchestrator "simple" runs state1_runner, then state2_runner
cat > mission.yaml <<EOF
orchestrator_type: "simple"
plugin_libraries:
  - "libbehavior_architecture_interpretation_bt_nodes.so"
behaviors:
  - name: "state1_runner"
    behavior_file: "$PWD/extract.xml"
  - name: "state2_runner"
    behavior_file: "$PWD/choose.xml"
EOF

ros2 launch behavior_architecture mission_executor.launch.py config_file:=$PWD/mission.yaml \
  interpretation:=true interpretation_params_file:=$PWD/interpretation_params.yaml
```

The server logs `extract_slots: llm {"name": "Ana", "age": 34}` and
`choose_behavior: llm {"behavior_id": 2}`; both runners report `SUCCESS`, and the orchestrator
reaches `State: STOP - All behaviors completed`. Stop the launch with Ctrl-C.
`log/calls.jsonl` holds the two calls, with their prompts:

```bash
python3 -c "import json; [print(c['question'], c['source'], c['answer']) for c in map(json.loads, open('log/calls.jsonl'))]"
```

To replay the run from the log, replace the route by a `recorded` one and launch again; the
answers are the same:

```bash
cat > replay_params.yaml <<EOF
/**/interpretation_server:
  ros__parameters:
    questions_file: package://behavior_architecture/config/interpretation/example_questions.yaml
    templates_file: package://behavior_architecture/config/interpretation/example_templates.yaml
    default_route: text
    routes:
      text:
        backend: recorded
        model: ""
        log_files: ["$PWD/log/calls.jsonl"]
EOF

ros2 launch behavior_architecture mission_executor.launch.py config_file:=$PWD/mission.yaml \
  interpretation:=true interpretation_params_file:=$PWD/replay_params.yaml
```

To answer with a model instead, replace the route by one of a real backend, for example
`{backend: ollama, model: "qwen2.5:7b", url: "http://localhost:11434"}` with an Ollama server
that has pulled the model.

## Troubleshooting

**The server exits at start with a configuration error.** The message names the question and the
cause: an unknown key in a definition, a template that is not in the templates file, a route that
does not exist, a rule that no module registers, a module that cannot be imported, or a variant
that does not exist.

**`the service /.../interpretation/interpret is not available`.** No server runs in the namespace
of the asking node. `mission_executor.launch.py` starts the server in the namespace of
`mission_executor` (`namespace`); with another `interpretation_namespace:=ns`, or a server
launched apart, the server is `/ns/interpretation/interpret`: set the BT port `service` (or the
client's `service_name`) to that absolute name.

**`success` is false.** The request cannot be taken; `message` says why: an unknown question or
variant, a context that is not a JSON object, the key of `fields_from` or `choices_from` missing
from the context, an image missing for `uses_image: true`, or more images than `max_images`.

**Source `invalid` with reason `invalid`.** The model answered, but not with the declared fields
or with an allowed choice. The log's `attempts[].raw` shows what it wrote. `max_retries`, a
clearer template or a `fallback` are the usual remedies.

**Reason `model_down`.** The backend could not reach the model (the URL, the API key in the
variable named by `api_key_env`, the model name), or, with `recorded`, the call is not in the
logs because its prompt changed.

**Reason `timeout` or `budget`.** The attempt exceeded the route's `timeout_s` or the request's
`budget_s`. Local models may need a longer `timeout_s` on their first call, while they load.

**A tree with `Interpret` fails to build outside `mission_executor`.** The blackboard has no
`node`; store an `rclcpp::Node::SharedPtr` under `node`, and spin that node, before creating the
tree.
