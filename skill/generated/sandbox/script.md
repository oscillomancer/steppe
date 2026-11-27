# `script` commands

Run a Lua script on the command bus.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `script.run@1`

Run a Lua script on the command bus.

Every call the script makes goes through the command registry, so it is validated, transactional and in the undo history like a call from the GUI or an agent. Use it for loops, conditionals and arithmetic that the command set cannot express; use batch for a fixed list of steps. Not transactional itself: undo the edits it made with history.undo.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `code` | string |  |  |  | The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays). |
| `name` | string |  |  |  | The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name. |
| `budget_ms` | number |  | 1.0..10000.0 |  | Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count. |
| `args` | value |  |  |  | Converted to the global table `args`. |

### Result

```json
{
  "type": "object",
  "title": "script_run_result",
  "properties": {
    "result": {
      "description": "What the script returned (null if it returned nothing)."
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "What the script printed."
    },
    "log_dropped": {
      "type": "integer",
      "description": "Cumulative lines evicted from this script's retained log."
    },
    "script": {
      "type": "string",
      "description": "The name it ran under."
    },
    "instructions": {
      "type": "integer",
      "description": "Lua instructions the run executed."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["result", "log", "log_dropped", "script", "instructions"],
  "additionalProperties": false
}

```

### Errors

- `script.syntax`: the source does not compile Hint: fix the Lua syntax at the reported line
- `script.budget`: the run spent its instruction budget Hint: raise budget_ms or make the script do less between waits
- `script.memory`: the run hit the script's memory cap Hint: raise memory_limit_bytes or hold less data
- `script.error`: the script raised a Lua error Hint: read the message and the traceback
- `script.arguments`: a command was called with something other than a table Hint: call it with a table of arguments, e.g. q.track.add{name = "Bass"}
- `script.unavailable`: no command dispatcher is attached to the script Hint: run the script through a host that provides a Runtime

### Examples

**A script that computes, with no commands** (`empty`)

```json
{
  "code": "local total = 0\nfor i = 1, 4 do total = total + i end\nreturn {total = total, steps = {1, 2, 3, 4}}"
}

```

Result:

```json
{
  "result": {
    "total": 10,
    "steps": [1, 2, 3, 4]
  },
  "log": [],
  "log_dropped": 0,
  "script": "<any>",
  "instructions": "<any>"
}

```
