# `optimize` commands

Tune parameters toward a reference sound or a target.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `optimize.start@1`

Tune continuous parameters toward a reference sound or a profile (a job).

You choose the structure (patch) and which knobs matter; CMA-ES tunes the numbers. params: "auto" (continuous parameters of the modules present, max 32) or keys, optionally {key, min, max} to narrow the search. "auto" searches around each parameter's current value (±2 octaves for frequency, time and pitch-interval quantities, ±25 % of the range otherwise): name keys with min/max when the answer may be far from the current patch. target: {"reference": rd_<n> or file} and/or {"profile": supple.analysis-profile document}; a reference target starts from a smaller initial step than a profile, because the patch is already close. phrase: notation rendered per candidate (default "1: C3:w"; use the reference's phrase when matching a reference). Returns a job: poll job.wait; the result has the best loss per term, the failed-candidate count, changes, a merge patch ({"params": {...}}) for patch.merge, and the best candidate's render.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  |  |
| `params` | value |  |  | "auto" | "auto" or a list of keys / {key, min, max}. |
| `target` | value |  |  |  | {reference?, profile?} |
| `phrase` | string |  |  |  |  |
| `seconds` | number |  | ..30.0 |  |  |
| `tail_s` | number |  | 0.0..10.0 |  |  |
| `evaluations` | integer |  | 10.0..5000.0 |  |  |
| `max_seconds` | number |  | 1.0..600.0 |  |  |
| `seed` | integer |  | 0.0..1e+09 | 0 |  |

### Result

```json
{
  "type": "object",
  "title": "optimize_started",
  "properties": {
    "job": {
      "type": "string"
    },
    "status": {
      "type": "string"
    },
    "params": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "render": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["job", "status", "params", "render"],
  "additionalProperties": false
}

```

### Errors

- `optimize.not_semantic`: the plugin has no semantic schema Hint: use a stock plugin
- `optimize.target`: no reference or profile Hint: {"reference": "rd_1"}
- `optimize.too_many`: more than 32 parameters Hint: name the ones that matter

### Examples

**Tune toward a profile** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "evaluations": 20,
  "max_seconds": 5,
  "target": {
    "profile": {
      "schema": "supple.analysis-profile/1.0",
      "targets": {
        "spectral.centroid_hz.median": {
          "target": 800,
          "tolerance": 100
        }
      }
    }
  }
}

```

Result:

```json
{
  "job": "<any>",
  "status": "running",
  "params": "<any>",
  "render": "<any>"
}

```
