# `profile` commands

Score a render against a target descriptor profile.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `profile.evaluate@1`

Score a render against a target descriptor profile.

profile: {"schema": "supple.analysis-profile/1.0", "targets": {"<report key>": {"target": x, "tolerance": t, "weight"?: w} | {"min"?: a, "max"?: b}}, "ltas"?: [31 relative dB]}. Keys are dotted report paths such as spectral.centroid_hz.median or harmonic.harmonics_db.3. Returns per-target pass and normalized distance, and a weighted loss (0 = on target).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `profile` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "profile_result",
  "properties": {
    "result": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["result"],
  "additionalProperties": false
}

```

### Examples

**A brightness target** (`synth_and_render`)

```json
{
  "source": "rd_1",
  "profile": {
    "schema": "supple.analysis-profile/1.0",
    "targets": {
      "spectral.centroid_hz.median": {
        "target": 1500,
        "tolerance": 500
      }
    }
  }
}

```

Result:

```json
{
  "result": "<any>"
}

```
