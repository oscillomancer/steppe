# `masking` commands

Estimate where one sound masks another.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `masking@1`

Estimate where one sound masks another (critical bands).

masker (e.g. the kick) and maskee (e.g. the bass): renders or files at the same sample rate. Per Bark band: % of the maskee's active frames masked and by how much; the three worst bands. An estimate for corrective mixing, not a psychoacoustic model of record.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `masker` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `maskee` | string |  |  |  | A render id (rd_1) or an audio file path. |

### Result

```json
{
  "type": "object",
  "title": "masking",
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

**A render masking itself** (`synth_and_render`)

```json
{
  "masker": "rd_1",
  "maskee": "rd_1"
}

```

Result:

```json
{
  "result": "<any>"
}

```
