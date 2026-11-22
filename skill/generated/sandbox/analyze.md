# `analyze` commands

Analyze a render or audio file: loudness, spectrum, pitch.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `analyze@1`

Analyze a render or audio file: loudness, spectrum, pitch, envelope, stereo.

source: rd_<n> or a file path. Report supple.analysis-report/1.0: loudness (LUFS, LRA, true peak), spectral (centroid, rolloff, flatness, tilt, named band levels relative to the total), harmonic (f0, note, harmonics in dB relative to the fundamental), envelope (attack, decay to -20 dB, release after note-off, onsets), stereo (width, correlation). note_off_s defaults to the end of a render's input. Envelope values (attack, decay, release) describe single notes: render one note to measure them.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. One of: summary, standard, full. |
| `note_off_s` | number |  | 0.0..3600.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "analysis",
  "properties": {
    "report": {},
    "summary": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["report", "summary"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_render`: no such render Hint: session.get lists the renders
- `audio.*`: the file cannot be read Hint: WAV, FLAC or MP3

### Examples

**Analyze the last render** (`synth_and_render`)

```json
{
  "source": "rd_1"
}

```

Result:

```json
{
  "report": "<any>",
  "summary": "<any>"
}

```
