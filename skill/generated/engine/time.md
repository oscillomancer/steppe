# `time` commands

Convert seconds, beats and bars to one another.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `time.convert@1`

Convert a seconds, beat, or bar position to all three time domains.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `seconds` | number |  | 0.0.. |  |  |
| `beat` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "time_position",
  "properties": {
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "bar": {
      "type": "integer"
    },
    "beat_in_bar": {
      "type": "integer"
    },
    "fraction": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["seconds", "beat", "bar", "beat_in_bar", "fraction"],
  "additionalProperties": false
}

```

### Examples

**Bar two** (`empty`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "seconds": 2,
  "beat": 4,
  "bar": 2,
  "beat_in_bar": 1,
  "fraction": 0
}

```
