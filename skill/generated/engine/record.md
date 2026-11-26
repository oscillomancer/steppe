# `record` commands

How a recording pass over an item is kept (take or items).

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `record.commit@1`

Record a recording's commit (or a recovery's) as a replayable edit.

The takes as the device delivered them and the punch range then; placing them again builds the same items, takes and sources. Audio files must still be there.

Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `takes` | array |  |  |  |  |
| `takes[].track` | string |  |  |  | Track id, e.g. "tr_1". |
| `takes[].file` | string |  |  |  | Relative to the media folder when inside it |
| `takes[].hash` | string |  |  |  |  |
| `takes[].start_sample` | integer |  | -281474976710656.0..281474976710656.0 |  |  |
| `takes[].frames` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].segments` | array |  |  |  |  |
| `takes[].loop_start` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].loop_end` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].overflow_frames` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].sample_rate` | number |  |  |  |  |
| `takes[].midi` | boolean |  |  |  |  |
| `takes[].events` | array |  |  |  |  |
| `punch` | object |  |  |  |  |
| `punch.enabled` | boolean |  |  |  |  |
| `punch.start` | number |  |  |  |  |
| `punch.end` | number |  |  |  |  |
| `stack` | boolean |  |  | false | A pass over an item becomes a take of it (ADR-0103) |

### Result

```json
{}

```

### Examples

## `record.get@1`

How a recording pass over an item is kept (stack_takes).

stack_takes true: recording over an item makes a new take of it, heard where it was recorded (the rest of the item plays what it did; the item's extent does not change); false: every pass is laid as new items over what is there. A machine setting, not part of the project, that the engine keeps while it runs; the GUI keeps its Preferences choice between runs and sets it at start, and a headless session keeps nothing.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "record_settings",
  "properties": {
    "stack_takes": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["stack_takes"],
  "additionalProperties": false
}

```

### Examples

**The default** (`empty`)

```json
{}

```

Result:

```json
{
  "stack_takes": true
}

```

## `record.set@1`

Choose whether recording over an item makes a take of it; returns it.

stack_takes true (the default): a pass that starts inside an unlocked, non-looping item of the track becomes a new take of that item, heard over the stretch the pass covers while the rest of the item keeps playing what it did (a comp's pieces all get it; a pass over the whole item is heard whole); false: every pass is laid as new items, layered over what is there. Not an undo step and not saved in the project. The setting lasts for the process; a client that keeps preferences may store it and set it again at start (the GUI keeps its Preferences row's choice), and a headless session does not. It is read when a recording is committed and stored in that commit, so undo, redo and merges do not depend on it later.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `stack_takes` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "record_settings",
  "properties": {
    "stack_takes": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["stack_takes"],
  "additionalProperties": false
}

```

### Examples

**Layer passes as items again** (`empty`)

```json
{
  "stack_takes": false
}

```

Result:

```json
{
  "stack_takes": false
}

```
