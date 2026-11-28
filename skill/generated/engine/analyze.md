# `analyze` commands

Analyze renders and tracks: compare, masking, loudness, spectra.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `analyze.compare@1`

Compare two renders or track outputs by perceptual descriptors.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `reference` | string |  |  |  |  |
| `test` | string |  |  |  |  |

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
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["notable", "ltas_rms_db", "summary"],
  "additionalProperties": false
}

```

### Examples

**Compare a track with itself** (`song_with_item`)

```json
{
  "reference": "tr_1",
  "test": "tr_1"
}

```

Result:

```json
{
  "notable": [],
  "ltas_rms_db": 0.0,
  "summary": "<any>"
}

```

## `analyze.masking@1`

Estimate masking between two tracks over the same range.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `masker_track` | string |  |  |  |  |
| `maskee_track` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |

### Result

```json
{}

```

### Examples

**One track against itself** (`song_with_fx`)

```json
{
  "masker_track": "tr_1",
  "maskee_track": "tr_1"
}

```

Result:

```json
"<any>"

```

## `analyze.render@1`

Analyze a render or audio file: loudness, spectrum, envelope, stereo.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `render` | string |  |  |  |  |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. One of: summary, standard, full. |

### Result

```json
{}

```

### Examples

**Analyze a mix** (`song_with_render`)

```json
{
  "render": "rd_1"
}

```

Result:

```json
"<any>"

```

## `analyze.track@1`

Render one track's output offline and analyze it.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tap` | string |  |  | "post_fader" | One of: post_fader, pre_fader, pre_fx. |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. One of: summary, standard, full. |

### Result

```json
{}

```

### Examples

**Analyze a track** (`song_with_item`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
"<any>"

```
