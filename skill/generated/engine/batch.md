# `batch` commands

Run several commands atomically (in the DAW, one undo step).

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `batch@1`

Run commands atomically.

Run commands atomically.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `label` | string |  |  |  |  |
| `steps` | array |  |  |  |  |
| `steps[].command` | string |  |  |  |  |
| `steps[].args` | object |  |  |  |  |
| `steps[].as` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  }
}

```

### Examples

**Empty batch** (`empty`)

```json
{
  "steps": []
}

```

Result:

```json
{
  "label": "",
  "results": []
}

```
