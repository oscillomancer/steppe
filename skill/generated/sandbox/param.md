# `param` commands

Read and set plug-in parameters in physical units.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `param.get@1`

Read parameter values (physical value and display text).

keys: semantic keys (e.g. "filter.cutoff"); omit for all. Third-party plugins: CLAP ids or names; values are CLAP values.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `keys` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "param_values",
  "properties": {
    "values": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["values"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_param`: no parameter with that key Hint: closest key in the hint; see plugin.describe

### Examples

**Read one value** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "keys": ["gain"]
}

```

Result:

```json
{
  "values": {
    "gain": {
      "value": 0.0,
      "text": "<any>"
    }
  }
}

```

## `param.set@1`

Set parameters in physical units; returns the values as applied.

values: {key: number | "unit string" | enum key | bool}, e.g. {"filter.cutoff": "1.2 kHz", "filter.mode": "lowpass"}. All values are validated first; nothing changes if any is invalid. Third-party plugins: CLAP id or name keys with CLAP values or {"text": "..."}. A value that does not apply with the new values says "active": false (plugin.describe detail full says what makes it apply). Use patch.merge to change several sections at once.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `values` | value |  |  |  | Key -> value. |

### Result

```json
{
  "type": "object",
  "title": "param_values",
  "properties": {
    "values": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["values"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_param`: no parameter with that key Hint: closest key in the hint
- `params.out_of_range`: a value is outside its range Hint: the hint gives the valid range
- `params.parse`: a unit string could not be read Hint: the hint shows the expected form

### Examples

**Set a level with a unit string** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "values": {
    "gain": "-6 dB"
  }
}

```

Result:

```json
{
  "values": {
    "gain": {
      "value": -6.0,
      "text": "<any>"
    }
  }
}

```
