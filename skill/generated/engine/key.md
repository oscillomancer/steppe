# `key` commands

Key points of the song context: set or remove one at a beat.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `key.remove@1`

Remove the key point at a beat.



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

**Remove the key at the start** (`song_with_context`)

```json
{
  "at": 0
}

```

Result:

```json
{}

```

## `key.set@1`

Set the key from a beat: tonic and scale.

Adds a key point at `at` (project beats, rounded to 1/960) or changes the one there. `to` moves the point at `at` to another beat and keeps its id. tonic is a letter and at most one # or b (Eb, F#); scale is one of: major, minor, harmonic_minor, melodic_minor, dorian, phrygian, lydian, mixolydian, locrian, major_pentatonic, minor_pentatonic, blues, chromatic. A key that needs a double accidental (G# major) is refused: use the enharmonic one (Ab major). Notation, the piano roll and midi.edit read it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |
| `tonic` | string |  |  |  |  |
| `scale` | string |  |  |  |  |
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

**E-flat major from the start** (`empty`)

```json
{
  "at": 0,
  "tonic": "Eb",
  "scale": "major"
}

```

Result:

```json
{
  "id": "cx_1"
}

```
