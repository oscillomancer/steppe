# `render` commands

Render notes, a MIDI file or audio through the chain.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `render@1`

Render notes, a MIDI file or an audio file through the chain to a WAV.

notation: a phrase in the notation (see the notation guide), e.g. "voice lead; 1: C4:q E4:q G4:h | C5:w". midi_file / audio_file: paths (audio at the session rate). bars/seconds override the length; the tail after it is rendered until silence (tail_s caps it). Returns the render id for analyze/compare and the WAV path.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `notation` | string |  |  |  | Notation text. |
| `midi_file` | string |  |  |  |  |
| `audio_file` | string |  |  |  |  |
| `bars` | number |  | ..1000.0 |  |  |
| `seconds` | number |  | ..600.0 |  |  |
| `tempo` | number or string | bpm | 1.0..999.0 |  | Quantity strings are range-checked by the server. |
| `tail_s` | number |  | 0.0..60.0 |  |  |
| `name` | string |  |  |  |  |
| `analyze` | string |  |  | "none" | Also analyze the render (see the analyze command); summary is compact (≤ 400 tokens). One of: none, summary, standard, full. |

### Result

```json
{
  "type": "object",
  "title": "render_result",
  "properties": {
    "render": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "peak_dbfs": {
      "type": "number"
    },
    "tail_s": {
      "type": "number"
    },
    "tail_capped": {
      "type": "boolean"
    },
    "latency_samples": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "anyOf": [
          {
            "type": "string"
          },
          {
            "type": "object"
          }
        ]
      }
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "analysis": {}
  },
  "required": ["render", "path", "seconds", "peak_dbfs", "tail_s", "tail_capped", "latency_samples"],
  "additionalProperties": false
}

```

### Errors

- `notation.*`: the notation does not compile Hint: the error has line:col and a hint
- `render.empty_chain`: nothing is loaded Hint: plugin.load first
- `render.not_instrument`: notes given but the first plugin is an effect Hint: load an instrument first
- `render.input_rate`: audio_file at another sample rate Hint: session.set sample_rate

### Examples

**Render a phrase** (`synth_loaded`)

```json
{
  "notation": "voice lead; 1: A4:q E5:q r:h"
}

```

Result:

```json
{
  "render": "rd_1",
  "path": "<any>",
  "seconds": "<any>",
  "peak_dbfs": "<any>",
  "tail_s": "<any>",
  "tail_capped": false,
  "latency_samples": 0
}

```
