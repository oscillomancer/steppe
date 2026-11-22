# `compare` commands

Compare two renders or files by their descriptors.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `compare@1`

Compare two renders or files: notable descriptor changes and timbre distance.

Notable changes use fixed thresholds (loudness 1 LU, centroid 10 %, band levels 1.5 dB, harmonics 3 dB, f0 10 cents, ...). ltas_rms_db is the RMS difference of the 1/3-octave spectra after removing the level offset: a pure gain change reads 0. loss is the score the optimizer minimizes and the evaluation scorer judges: the test is loudness-matched to the reference, both are analyzed at their note-off times (note_off_s reports them), and loss.total is the number to get down. A render's note-off is its own input end; give reference_note_off_s / test_note_off_s for files, and one unknown side borrows the other's (note_off_assumed).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `reference` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `test` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `reference_note_off_s` | number |  | 0.0..3600.0 |  | The reference's note-off; defaults to a render's input end. |
| `test_note_off_s` | number |  | 0.0..3600.0 |  | The test's note-off; defaults to a render's input end. |

### Result

```json
{
  "type": "object",
  "title": "comparison",
  "properties": {
    "notable": {},
    "ltas_rms_db": {
      "type": "number"
    },
    "summary": {
      "type": "string"
    },
    "loss": {
      "description": "The score the optimizer minimizes: total, per-term contributions, and the note-off_s used per side."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["notable", "ltas_rms_db", "summary", "loss"],
  "additionalProperties": false
}

```

### Examples

**A render against itself** (`synth_and_render`)

```json
{
  "reference": "rd_1",
  "test": "rd_1"
}

```

Result:

```json
{
  "notable": [],
  "ltas_rms_db": 0.0,
  "summary": "<any>",
  "loss": {
    "total": "<any>",
    "terms": "<any>",
    "note_off_s": {
      "reference": "<any>",
      "test": "<any>"
    }
  }
}

```
