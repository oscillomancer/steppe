# `chord` commands

Chord points of the song context: set or remove one at a beat.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `chord.remove@1`

Remove the chord point at a beat.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |

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

**Remove the chord at the start** (`song_with_context`)

```json
{
  "at": 0
}

```

Result:

```json
{}

```

## `chord.set@1`

Set the chord from a beat; "" ends it.

Adds a chord point at `at` (project beats, rounded to 1/960) or changes the one there. `to` moves the point at `at` to another beat and keeps its id. symbol is the notation's chord grammar: a root, a quality and an optional /bass (Cm7, Bb, F#7#9, C/E); an empty symbol means no chord from here. The symbol is stored in canonical form (Cmin7 becomes Cm7).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |
| `symbol` | string |  |  |  |  |
| `to` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**C minor seventh at bar 2** (`empty`)

```json
{
  "at": 4,
  "symbol": "Cm7"
}

```

Result:

```json
{
  "id": "cx_1"
}

```
