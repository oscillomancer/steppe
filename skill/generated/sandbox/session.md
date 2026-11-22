# `session` commands

The sandbox session: settings, chain, instances; reset.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `session.get@1`

Show the session: settings, chain, instances and renders.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "session",
  "properties": {
    "settings": {},
    "chain": {},
    "instances": {},
    "renders": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings", "chain", "instances", "renders"],
  "additionalProperties": false
}

```

### Examples

**An empty session** (`empty`)

```json
{}

```

Result:

```json
{
  "settings": {
    "sample_rate": 48000.0,
    "block_size": 256,
    "tempo": 120.0,
    "meter": "4/4",
    "tail_limit_s": 5.0
  },
  "chain": [],
  "instances": {},
  "renders": {}
}

```

## `session.reset@1`

Unload every plugin, delete renders and restore default settings.

Not undoable. Saved presets are kept.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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

**Start over** (`synth_loaded`)

```json
{}

```

Result:

```json
{}

```

## `session.set@1`

Change session settings (rate, block size, tempo, meter, tail limit).

Only the given fields change. A rate or block size change reactivates plugins at the next render.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `sample_rate` | number or string | Hz | 8000.0..384000.0 |  | Quantity strings are range-checked by the server. |
| `block_size` | integer |  | 1.0..8192.0 |  |  |
| `tempo` | number or string | bpm | 1.0..999.0 |  | Quantity strings are range-checked by the server. |
| `meter` | string |  |  |  |  |
| `tail_limit_s` | number |  | 0.0..60.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "settings",
  "properties": {
    "settings": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings"],
  "additionalProperties": false
}

```

### Examples

**Slow down, 3/4** (`empty`)

```json
{
  "tempo": "90 bpm",
  "meter": "3/4"
}

```

Result:

```json
{
  "settings": {
    "sample_rate": 48000.0,
    "block_size": 256,
    "tempo": 90.0,
    "meter": "3/4",
    "tail_limit_s": 5.0
  }
}

```
