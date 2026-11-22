# `tempo` commands

The tempo map: replace its points.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `tempo.set@1`

Replace the tempo map: points {beat, bpm, ramp?}, the first at beat 0.

ramp: true changes the tempo linearly (in beats) to the next point; otherwise it steps.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].bpm` | number or string | bpm | 1.0..1000.0 |  | Quantity strings are range-checked by the server. |
| `points[].ramp` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

**Slow down after 8 bars** (`empty`)

```json
{
  "points": [
    {
      "beat": 0,
      "bpm": 120
    },
    {
      "beat": 32,
      "bpm": 100,
      "ramp": false
    }
  ]
}

```

Result:

```json
{
  "count": 2
}

```
