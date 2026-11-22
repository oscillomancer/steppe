# `autoitem` commands

Automation items on envelopes: add, duplicate, set, pool, remove.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `autoitem.add@1`

Put an automation item on an envelope: a block that replaces it there.

position (beat) or bar, length in beats. points (beats from the item's start, values in the envelope's units) over source_length beats (default: length), repeated while loop (default true) and the item is longer; without points it captures what the envelope plays over that range. Duplicate it pooled (autoitem.duplicate) to edit several at once. Track, route and plugin envelopes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `length` | number |  |  | 4.0 |  |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |
| `source_length` | number |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**A one-bar swell, looped over two bars** (`song_with_env`)

```json
{
  "env": "en_1",
  "bar": 1,
  "length": 8,
  "source_length": 4,
  "points": [
    {
      "beat": 0,
      "value": -12
    },
    {
      "beat": 4,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "id": "ai_1",
  "env": "en_1",
  "position": 0.0,
  "length": 8.0,
  "source_length": 4.0,
  "loop": true,
  "pool": 0,
  "baseline": 0.0,
  "amplitude": 1.0,
  "name": "",
  "points": [
    {
      "beat": 0.0,
      "value": -12.0
    },
    {
      "beat": 4.0,
      "value": 0.0
    }
  ]
}

```

## `autoitem.duplicate@1`

Copy an automation item (pooled by default: edits to either reach both).

position: where the copy starts (default: right after the item).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |
| `position` | number |  | 0.0.. |  |  |
| `pooled` | boolean |  |  | true |  |

### Result

```json
{}

```

### Examples

**A pooled copy right after it** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
"<any>"

```

## `autoitem.list@1`

List automation items (all, or one envelope's), with their points.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |

### Result

```json
{}

```

### Examples

**None yet** (`empty`)

```json
{}

```

Result:

```json
{
  "items": []
}

```

## `autoitem.remove@1`

Remove an automation item (the envelope under it plays again).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "additionalProperties": false
}

```

### Examples

**Remove** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
{}

```

## `autoitem.set@1`

Move, resize, loop, scale or rewrite an automation item.

points and source_length reach every item of its pool; position, length, loop, baseline, amplitude (values play as baseline + amplitude x (value - baseline)) and name are the item's own.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |
| `position` | number |  | 0.0.. |  |  |
| `length` | number |  |  |  |  |
| `source_length` | number |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `baseline` | number |  |  |  |  |
| `amplitude` | number |  | -2.0..2.0 |  |  |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Two beats later** (`song_with_autoitem`)

```json
{
  "item": "ai_1",
  "position": 2
}

```

Result:

```json
"<any>"

```

## `autoitem.unpool@1`

Take an automation item out of its pool (its points stay).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "additionalProperties": false
}

```

### Examples

**Unpool** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
{}

```
