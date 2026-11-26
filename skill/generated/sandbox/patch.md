# `patch` commands

An instance's patch document: get, set, merge.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `patch.get@1`

Get an instance's patch document (changed values or all).

detail changed (default): only values that differ from the defaults, still a complete patch. detail full: every value. Semantic plugins only.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `detail` | string |  |  | "changed" | One of: changed, full. |

### Result

```json
{
  "type": "object",
  "title": "patch",
  "properties": {
    "patch": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use param.get / presets

### Examples

**Get the changed values** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `patch.merge@1`

Merge a partial patch (RFC 7396) into an instance.

patch: only the values to change, e.g. {"params": {"filter.cutoff": "800 Hz"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "patch_merge",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply now"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use param.set
- `params.out_of_range`: a value is outside its range Hint: the hint gives the valid range

### Examples

**Turn it down** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "patch": {
    "params": {
      "gain": "-12 dB"
    }
  }
}

```

Result:

```json
{
  "changed": {
    "params": {
      "gain": -12.0
    }
  },
  "description": "<any>"
}

```

## `patch.set@1`

Replace an instance's whole patch; returns a description of the result.

patch: a complete patch document (see plugin.describe detail full for its schema); missing values take their defaults. Validated fully before anything changes. For partial edits use patch.merge.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "description",
  "properties": {
    "description": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use presets
- `schema.invalid`: the patch has problems Hint: details list each with its path

### Examples

**Reset to defaults** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "patch": {
    "schema": "supple.test-gain-patch/1.0"
  }
}

```

Result:

```json
{
  "description": "<any>"
}

```
