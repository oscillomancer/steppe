# Command reference

Generated from the command registry.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

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

## `arrangement.build@1`

Lay a song out from scenes, each for some bars, in order.

scene.insert for each entry of sequence in turn, each starting where the last ended, from bar or at (default bar 1), with a section for each. A scene may appear more than once. Every entry's refusal (scene.empty, scene.insert.period, scene.insert.occupied) names its index in path, and changes nothing; an empty sequence or one of more than 256 entries is scene.sequence. Each item warns context.mismatch as scene.insert's does. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `sequence` | array |  |  |  |  |
| `sequence[].scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `sequence[].bars` | integer |  | 1.0.. |  |  |
| `at` | number |  | 0.0.. |  | Start in beats, on a bar line. |
| `bar` | integer |  | 1.0..1e+05 |  | Start bar, 1-based [1]. |
| `pooled` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "arrangement_build",
  "properties": {
    "segments": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "arrangement_segment",
        "properties": {
          "scene": {
            "type": "string"
          },
          "region": {
            "type": "string"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          },
          "items": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "laid_clip",
              "properties": {
                "clip": {
                  "type": "string"
                },
                "item": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                }
              },
              "required": ["clip", "item", "track"],
              "additionalProperties": false
            }
          }
        },
        "required": ["scene", "region", "start", "end", "items"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["segments"],
  "additionalProperties": false
}

```

### Errors

- `scene.sequence`: the sequence is empty or longer than 256 entries Hint: give 1 to 256 entries of {scene, bars}
- `scene.empty`: a scene in the sequence has no clips Hint: add clips with clip.add
- `scene.insert.occupied`: items are already in an entry's span Hint: move or remove them, or start at another bar
- `scene.insert.period`: a repeating clip no single looping item reproduces Hint: loop the clip over its whole content, or ask for fewer bars

### Examples

**A verse for four bars from bar 5** (`song_with_clip`)

```json
{
  "sequence": [
    {
      "scene": "sc_1",
      "bars": 4
    }
  ],
  "bar": 5
}

```

Result:

```json
{
  "segments": "<any>"
}

```

## `audio.quantize@1`

Quantize detected audio hits together using stretch markers or slices.

Consumes stored source hits. Detect absent/stale hits with take.hits.detect. Aligned items share one timing map; warp mode jointly processes their channels. Slice mode requires unwarped input; strength 0 splits without moving hits.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |
| `grid` | number |  | 0.0009765625..1024.0 |  |  |
| `strength` | number |  | 0.0..1.0 | 1.0 |  |
| `swing` | number |  | 0.0..1.0 | 0.0 |  |
| `crossfade` | number |  | 0.0..0.05 | 0.005 |  |
| `mode` | string |  |  | "warp" | One of: warp, slice. |
| `reference` | string |  |  | "" |  |
| `hits` | string |  |  | "reference" | One of: reference, merged. |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Warp to sixteenths** (`song_with_hits`)

```json
{
  "items": ["it_1"],
  "grid": 0.25
}

```

Result:

```json
"<any>"

```

**Split without moving** (`song_with_hits`)

```json
{
  "items": ["it_1"],
  "grid": 0.25,
  "mode": "slice",
  "strength": 0
}

```

Result:

```json
"<any>"

```

## `audio.warp.detach@1`

Detach selected items from jointly processed warp bundles.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Clear warp** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{}

```

## `audio.warp.reset@1`

Remove selected items' warping while keeping detected hits.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Clear warp** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{}

```

## `audition.get@1`

Whether an audition plays: path, length, position (seconds).



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "audition_status",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "decoding": {
      "type": "boolean"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "position": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "decoding", "path", "seconds", "position"],
  "additionalProperties": false
}

```

### Examples

**Nothing playing** (`empty`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "decoding": false,
  "path": "",
  "seconds": 0.0,
  "position": 0.0
}

```

## `audition.play@1`

Play an audio file once on outputs 1/2, outside the project.

A WAV, FLAC or MP3 file plays from its start, once, on the device's outputs 1 and 2 (mono on both; a wider file's first two channels), whether the transport is stopped or playing. It does not pass the project's tracks, masters or meters, and renders and recordings never contain it. The file's start, up to 28,800,000 frames at the higher of its rate and the device's (ten minutes at 48 kHz, 2.5 at 192 kHz), is read and resampled to the device's rate before it sounds (decoding: true meanwhile; truncated: true when the file is longer). path: absolute, or a name in the project's media folder. A second call replaces what plays; it ends at the file's end, with audition.stop, when the engine stops or another project is opened. Not an undo step, not saved.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "audition",
  "properties": {
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "sample_rate": {
      "type": "number"
    },
    "channels": {
      "type": "integer"
    },
    "truncated": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "seconds", "sample_rate", "channels", "truncated"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `audition.file`: the file is missing, unreadable or not WAV, FLAC or MP3 Hint: give a WAV, FLAC or MP3 file: an absolute path, or a name in the project's media folder

### Examples

**A file in the project's media folder** (`audition_file`)

```json
{
  "path": "tone.wav"
}

```

Result:

```json
{
  "path": "<any>",
  "seconds": 2.0,
  "sample_rate": 48000.0,
  "channels": 2,
  "truncated": false
}

```

## `audition.stop@1`

Stop the file audition (fading out over 5 ms); returns its state.

Stopping when nothing plays is not an error. Not an undo step.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "audition_status",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "decoding": {
      "type": "boolean"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "position": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "decoding", "path", "seconds", "position"],
  "additionalProperties": false
}

```

### Examples

**Nothing playing** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "decoding": false,
  "path": "",
  "seconds": 0.0,
  "position": 0.0
}

```

## `autoitem.add@1`

Put an automation item on an envelope: a block that replaces it there.

position (beat) or bar, length in beats. points (beats from the item's start, values in the envelope's units) over source_length beats (default: length), repeated while loop (default true) and the item is longer; without points it captures what the envelope plays over that range. Duplicate it pooled (autoitem.duplicate) to edit several at once. Track, route and plugin envelopes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `length` | number |  |  | 4.0 |  |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |
| `source_length` | number |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**A one-bar swell, looped over two bars** (`song_with_env`)

```json
{
  "env": "en_1",
  "bar": 1,
  "length": 8,
  "source_length": 4,
  "points": [
    {
      "beat": 0,
      "value": -12
    },
    {
      "beat": 4,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "id": "ai_1",
  "env": "en_1",
  "position": 0.0,
  "length": 8.0,
  "source_length": 4.0,
  "loop": true,
  "pool": 0,
  "baseline": 0.0,
  "amplitude": 1.0,
  "name": "",
  "points": [
    {
      "beat": 0.0,
      "value": -12.0
    },
    {
      "beat": 4.0,
      "value": 0.0
    }
  ]
}

```

## `autoitem.duplicate@1`

Copy an automation item (pooled by default: edits to either reach both).

position: where the copy starts (default: right after the item).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |
| `position` | number |  | 0.0.. |  |  |
| `pooled` | boolean |  |  | true |  |

### Result

```json
{}

```

### Examples

**A pooled copy right after it** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
"<any>"

```

## `autoitem.list@1`

List automation items (all, or one envelope's), with their points.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |

### Result

```json
{}

```

### Examples

**None yet** (`empty`)

```json
{}

```

Result:

```json
{
  "items": []
}

```

## `autoitem.remove@1`

Remove an automation item (the envelope under it plays again).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

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

**Remove** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
{}

```

## `autoitem.set@1`

Move, resize, loop, scale or rewrite an automation item.

points and source_length reach every item of its pool; position, length, loop, baseline, amplitude (values play as baseline + amplitude x (value - baseline)) and name are the item's own.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |
| `position` | number |  | 0.0.. |  |  |
| `length` | number |  |  |  |  |
| `source_length` | number |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `baseline` | number |  |  |  |  |
| `amplitude` | number |  | -2.0..2.0 |  |  |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Two beats later** (`song_with_autoitem`)

```json
{
  "item": "ai_1",
  "position": 2
}

```

Result:

```json
"<any>"

```

## `autoitem.unpool@1`

Take an automation item out of its pool (its points stay).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

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

**Unpool** (`song_with_autoitem`)

```json
{
  "item": "ai_1"
}

```

Result:

```json
{}

```

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

## `branch.create@1`

Start a branch from a head or an earlier version and switch to it.

Agents: work on agent/<you>/<topic> branches, then branch.diff and branch.merge into main. Merging replays your edits onto main even if main moved; conflicting edits are reported.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `version` | integer |  | 1.0.. |  | Start at this version of the current branch's history (history.list; a render result's version) instead of a head |
| `switch` | boolean |  |  | true |  |

### Result

```json
{
  "type": "object",
  "title": "branch_name",
  "properties": {
    "name": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Errors

- `branch.exists`: a branch with that name exists Hint: pick another name or switch to it

### Examples

**An agent branch** (`song`)

```json
{
  "name": "agent/claude/darker"
}

```

Result:

```json
{
  "name": "agent/claude/darker"
}

```

## `branch.delete@1`

Delete a branch (not main, not the current one).



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

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

**Delete** (`branch_ahead`)

```json
{
  "name": "agent/claude/darker"
}

```

Result:

```json
{}

```

## `branch.diff@1`

What differs between two branch heads: added, removed, changed objects.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `a` | string |  |  |  |  |
| `b` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_diff",
  "properties": {
    "changes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "diff_entry",
        "properties": {
          "op": {
            "type": "string"
          },
          "id": {
            "type": "string"
          },
          "fields": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["op", "id", "fields"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changes"],
  "additionalProperties": false
}

```

### Examples

**Main against itself** (`song`)

```json
{
  "a": "main",
  "b": "main"
}

```

Result:

```json
{
  "changes": []
}

```

## `branch.list@1`

List branches and the current one.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "branch_list",
  "properties": {
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "current": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["branches", "current"],
  "additionalProperties": false
}

```

### Examples

**List** (`empty`)

```json
{}

```

Result:

```json
{
  "branches": ["main"],
  "current": "main"
}

```

## `branch.merge@1`

Merge a branch into another (default: main) by replaying its edits.

Fast-forwards when the target has not moved. Otherwise replays the branch's edits; IDs it created are renumbered where needed (see remapped). If both sides changed the same field of an object, nothing is merged and the error lists each conflict.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `into` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_merge",
  "properties": {
    "fast_forward": {
      "type": "boolean"
    },
    "up_to_date": {
      "type": "boolean"
    },
    "version": {
      "type": "integer"
    },
    "remapped": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["fast_forward", "up_to_date", "version", "remapped"],
  "additionalProperties": false
}

```

### Errors

- `branch.conflict`: both sides changed the same thing Hint: details: one per conflict with the object

### Examples

**Merge an agent branch** (`branch_ahead`)

```json
{
  "from": "agent/claude/darker"
}

```

Result:

```json
{
  "fast_forward": true,
  "up_to_date": false,
  "version": "<any>",
  "remapped": {}
}

```

## `branch.render-ab@1`

Render two branch heads over the same range and compare them.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `a` | string |  |  |  |  |
| `b` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..600.0 |  |  |

### Result

```json
{}

```

### Examples

**Compare main with itself** (`song`)

```json
{
  "a": "main",
  "b": "main",
  "end_beat": 1
}

```

Result:

```json
{
  "a": "<any>",
  "b": "<any>",
  "comparison": "<any>"
}

```

## `branch.switch@1`

Make another branch current (edits, undo and playback follow it).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_name",
  "properties": {
    "name": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Examples

**Back to main** (`song`)

```json
{
  "name": "main"
}

```

Result:

```json
{
  "name": "main"
}

```

## `cable.add@1`

Connect an output to an input or a knob.

from: "mo_N.<output>"; to: "mo_N.<input>" or "mo_N.<knob>" (a modulation cable: amount is the knob's movement at a signal of 1, in octaves for a frequency, the knob's unit otherwise; default an octave or half the range), or a plugin module's plug-in parameter "fx_N.<key>" (CLAP modulation, amount a fraction of its range, default 0.5; it must be modulatable). A numeric CLAP id is also accepted (fx_3.12), with no leading zeros except 0; a numeric semantic key takes precedence. Several cables into one input sum; a mono cable reaches every voice of a poly module. feedback: it closes a loop, so it carries the previous frame (a one-sample delay).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |
| `amount` | number |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.port`: no such output, input or knob Hint: rack.get detail full
- `cable.exists`: the two ends are joined already Hint: cable.set
- `cable.rack`: the ends are in different racks Hint: use In and Out modules

### Examples

**Notes' gate opens the VCA** (`song_with_rack`)

```json
{
  "from": "mo_2.gate",
  "to": "mo_3.cv"
}

```

Result:

```json
{
  "cable": "cb_2",
  "feedback": false
}

```

**Modulate a knob** (`song_with_rack`)

```json
{
  "from": "mo_2.modwheel",
  "to": "mo_3.level",
  "amount": 12
}

```

Result:

```json
{
  "cable": "cb_2",
  "feedback": false
}

```

## `cable.remove@1`

Remove a cable, by id or by its two ends.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `cable` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Unplug** (`song_with_rack`)

```json
{
  "cable": "cb_1"
}

```

Result:

```json
{}

```

**Unplug by ends** (`song_with_rack`)

```json
{
  "from": "mo_3.out",
  "to": "mo_1.l"
}

```

Result:

```json
{}

```

## `cable.set@1`

Set a modulation cable's amount or a cable's colour.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `cable` | string |  |  |  |  |
| `amount` | number |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Colour it** (`song_with_rack`)

```json
{
  "cable": "cb_1",
  "color": "violet"
}

```

Result:

```json
{}

```

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

## `clip.add@1`

Add a clip to a scene's cell: from notation, a file or an item.

One source at most: notation (as item.add's), midi_file, audio_file, or item (a copy of that item's takes; pooled shares its MIDI instead). Without one, an empty MIDI clip of one bar. A MIDI clip is measured in beats, as long as its notes rounded up to whole bars; an audio clip in seconds of take time, as long as the file. From an item, the length, loop and take envelopes convert at the tempo where the item sits (ADR-0097); the clip is named as the item is (its active take's name) unless name is given. Clips live on channels, one per (track, scene). A clip keeps the key and chords it was written against (context): as a lead sheet in clip bars ("1: key D minor {Dm}:w"), read from the project at a place (context_at), under the item it copies, or from the {SYMBOL} chords of its notation (a stated context holding chords wins over them, clip.chords_ignored). follow (none, key or chords) needs a MIDI clip with a key at its start (clip.no_key, clip.follow_audio); until placement follows it, only the mismatch warning on laying a clip uses the context (ADR-0098).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `notation` | string |  |  |  |  |
| `audio_file` | string |  |  |  |  |
| `midi_file` | string |  |  |  |  |
| `item` | string |  |  |  | Item id, e.g. "it_1". |
| `pooled` | boolean |  |  | false |  |
| `name` | string |  |  |  |  |
| `context` | string |  |  |  | The key and chords the clip was written against, as a lead sheet in clip bars (bar 1 is the clip's start): "1: key D minor {Dm}:w". "" is none. |
| `context_at` | object |  |  |  | A place in the project, by bar or in beats: the clip takes the key and chords holding there, and the changes up to its length after it. |
| `context_at.bar` | integer |  | 1.0..1e+06 |  |  |
| `context_at.at` | number |  | 0.0.. |  |  |
| `follow` | string |  |  |  | How the clip's notes are mapped when it is laid on the timeline (ADR-0098): none (default), key or chords. Needs a MIDI clip with a key at its start. One of: none, key, chords. |

### Result

```json
{
  "type": "object",
  "title": "clip_add",
  "properties": {
    "clip": {
      "type": "string"
    },
    "takes": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "source": {
      "type": "string"
    },
    "kind": {
      "type": "string"
    },
    "length": {
      "type": "number"
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
    }
  },
  "required": ["clip", "takes", "kind", "length"],
  "additionalProperties": false
}

```

### Errors

- `clip.track`: the track is not a channel Hint: put clips on a channel
- `clip.scene`: no such scene Hint: see scene.list, or add one with scene.add
- `clip.occupied`: the cell holds a clip Hint: choose an empty cell
- `clip.source`: more than one source Hint: give one of notation, audio_file, midi_file or item
- `clip.mixed`: the item holds MIDI and audio takes Hint: copy one kind
- `clip.unsupported`: a warped take Hint: reset its warp first
- `clip.memory`: the clips' audio would not fit Hint: use shorter audio
- `clip.limit`: the project has 16384 clips Hint: remove clips first
- `clip.context_args`: context and context_at together Hint: give one: the text, or a place to read
- `clip.context_syntax`: the context text does not read Hint: write a lead sheet in clip bars: 1: key D minor {Dm}:w
- `clip.context_range`: a context point at or past the clip's end Hint: move it earlier or lengthen the clip
- `clip.context_audio`: a chord, or a key after the start, on an audio clip Hint: an audio clip's context is a key at the start of bar 1
- `clip.no_key`: follow without a key at the clip's start Hint: give the clip a context with a key at its start
- `clip.follow_audio`: follow on an audio clip Hint: follow is for MIDI clips

### Examples

**A chorus bass line** (`song_with_clip`)

```json
{
  "track": "tr_1",
  "scene": "sc_2",
  "notation": "voice bass; 1: C2:h G2:h"
}

```

Result:

```json
{
  "clip": "cl_2",
  "takes": ["tk_3"],
  "source": "mi_3",
  "kind": "midi",
  "length": 4.0
}

```

**A bass line written in D minor, following the key when laid** (`song_with_clip`)

```json
{
  "track": "tr_1",
  "scene": "sc_2",
  "notation": "voice bass; 1: D2:h A1:h",
  "context": "1: key D minor {Dm}:w",
  "follow": "key"
}

```

Result:

```json
{
  "clip": "cl_2",
  "takes": ["tk_3"],
  "source": "mi_3",
  "kind": "midi",
  "length": 4.0
}

```

## `clip.duplicate@1`

Copy a clip and its takes to an empty cell.

The copy's MIDI is its own unless pooled, which shares it (both clips then edit the same notes).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `pooled` | boolean |  |  | false |  |

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

### Errors

- `clip.occupied`: the cell holds a clip Hint: choose an empty cell
- `clip.limit`: the project has 16384 clips Hint: remove clips first

### Examples

**Into the chorus** (`song_with_clip`)

```json
{
  "clip": "cl_1",
  "scene": "sc_2"
}

```

Result:

```json
{
  "id": "cl_2"
}

```

## `clip.move@1`

Move a clip to an empty cell in another scene, channel or both.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |

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

### Errors

- `clip.occupied`: the cell holds a clip Hint: choose an empty cell
- `clip.track`: the track is not a channel Hint: choose a channel

### Examples

**To the chorus** (`song_with_clip`)

```json
{
  "clip": "cl_1",
  "scene": "sc_2"
}

```

Result:

```json
{}

```

## `clip.remove@1`

Remove a clip with its takes (unpooled sources go too).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |

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

**Remove** (`song_with_clip`)

```json
{
  "clip": "cl_1"
}

```

Result:

```json
{}

```

## `clip.set@1`

Set a clip's name, colour, mute, gain, length, loop, mode or quantize.

length and loop are in the clip's unit: beats for MIDI, seconds of take time for audio. loop: {on, start?, length?}, the window that repeats inside the clip from the take's start. mode loop repeats every length; once plays it once. quantize (bar, beat, 1/2 … 1/32, none) overrides the project's launch_quantize; null returns to it. context (a lead sheet in clip bars; "" clears it) or context_at (read from the project at a bar or beat) sets the key and chords the clip was written against, and follow (none, key, chords) how it is mapped when laid; shortening a clip drops the points past its end (clip.context_dropped), and a context left with no key at the start resets follow to none (clip.follow_reset).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `gain_db` | number |  | -150.0..24.0 |  |  |
| `length` | number |  |  |  |  |
| `loop` | object |  |  |  |  |
| `loop.on` | boolean |  |  |  |  |
| `loop.start` | number |  | 0.0.. |  |  |
| `loop.length` | number |  |  |  |  |
| `mode` | string |  |  |  | One of: loop, once. |
| `quantize` | string or null |  |  |  |  |
| `context` | string |  |  |  | As clip.add's: the lead sheet in clip bars; "" clears the context. |
| `context_at` | object |  |  |  | A place in the project, by bar or in beats: the clip takes the key and chords holding there, and the changes up to its length after it. |
| `context_at.bar` | integer |  | 1.0..1e+06 |  |  |
| `context_at.at` | number |  | 0.0.. |  |  |
| `follow` | string |  |  |  | How the clip's notes are mapped when it is laid on the timeline (ADR-0098): none (default), key or chords. Needs a MIDI clip with a key at its start. One of: none, key, chords. |

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

### Errors

- `clip.length`: a length or loop length not above 0 Hint: give a positive length in the clip's unit
- `clip.context_args`: context and context_at together Hint: give one: the text, or a place to read
- `clip.context_syntax`: the context text does not read Hint: write a lead sheet in clip bars: 1: key D minor {Dm}:w
- `clip.context_range`: a context point at or past the clip's end Hint: move it earlier or lengthen the clip
- `clip.context_audio`: a chord, or a key after the start, on an audio clip Hint: an audio clip's context is a key at the start of bar 1
- `clip.no_key`: follow without a key at the clip's start Hint: give the clip a context with a key at its start
- `clip.follow_audio`: follow on an audio clip Hint: follow is for MIDI clips

### Examples

**Play once, on the beat** (`song_with_clip`)

```json
{
  "clip": "cl_1",
  "mode": "once",
  "quantize": "beat"
}

```

Result:

```json
{}

```

## `context.at@1`

What key, chord and section hold at a beat or bar.

Give `at` (project beats) or `bar` (1-based). The key and chord are the last points at or before it; a section is a region with section true that covers it (the latest start wins). known false means the point is there but its scale or chord symbol is not one this build reads.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_at",
  "properties": {
    "beat": {
      "type": "number"
    },
    "bar": {
      "type": "integer"
    },
    "beat_in_bar": {
      "type": "number"
    },
    "key": {
      "type": "object",
      "title": "context_key",
      "properties": {
        "id": {
          "type": "string"
        },
        "tonic": {
          "type": "string"
        },
        "scale": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "tonic", "scale", "from_beat", "known"],
      "additionalProperties": false
    },
    "chord": {
      "type": "object",
      "title": "context_chord",
      "properties": {
        "id": {
          "type": "string"
        },
        "symbol": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "symbol", "from_beat", "known"],
      "additionalProperties": false
    },
    "section": {
      "type": "object",
      "title": "context_section",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "start": {
          "type": "number"
        },
        "end": {
          "type": "number"
        }
      },
      "required": ["id", "name", "start", "end"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["beat", "bar", "beat_in_bar"],
  "additionalProperties": false
}

```

### Examples

**At bar 2** (`song_with_context`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "beat": 4,
  "bar": 2,
  "beat_in_bar": 1,
  "key": {
    "id": "cx_1",
    "tonic": "Eb",
    "scale": "major",
    "from_beat": 0,
    "known": true
  },
  "chord": {
    "id": "cx_3",
    "symbol": "Cm7",
    "from_beat": 4,
    "known": true
  },
  "section": {
    "id": "rg_1",
    "name": "Verse",
    "start": 0,
    "end": 8
  }
}

```

## `context.get@1`

List the key and chord points in a range of beats.

Points from `from` (default 0) to `to` (default: the end), by beat, with the key and chord that hold at `from`, and `text`: the same as a lead sheet in project bars, one line per bar, for context.write. In it `{Cm7}:h` is a chord point at the cursor, `r:h` a chord point with no chord, `~:h` a hold (no point), `key Eb major` a key point at the cursor; a bar's durations fill the bar. The bars run from the one holding `from` to the one holding `to` (exclusive when `to` is a bar line), or to the last point.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | number |  | 0.0.. |  |  |
| `to` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_get",
  "properties": {
    "text": {
      "type": "string"
    },
    "points": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "context_point_row",
        "properties": {
          "id": {
            "type": "string"
          },
          "type": {
            "type": "string"
          },
          "beat": {
            "type": "number"
          },
          "bar": {
            "type": "integer"
          },
          "tonic": {
            "type": "string"
          },
          "scale": {
            "type": "string"
          },
          "symbol": {
            "type": "string"
          }
        },
        "required": ["id", "type", "beat", "bar"],
        "additionalProperties": false
      }
    },
    "key": {
      "type": "object",
      "title": "context_key",
      "properties": {
        "id": {
          "type": "string"
        },
        "tonic": {
          "type": "string"
        },
        "scale": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "tonic", "scale", "from_beat", "known"],
      "additionalProperties": false
    },
    "chord": {
      "type": "object",
      "title": "context_chord",
      "properties": {
        "id": {
          "type": "string"
        },
        "symbol": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "symbol", "from_beat", "known"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["points"],
  "additionalProperties": false
}

```

### Examples

**Nothing yet** (`empty`)

```json
{}

```

Result:

```json
{
  "points": []
}

```

## `context.write@1`

Write key and chord points from a lead sheet.

text is context.get's lead sheet: one line per project bar, 'N: ' then {SYMBOL}:DUR (a chord point at the cursor), r:DUR (a chord point with no chord), ~:DUR (hold: no point) and 'key TONIC SCALE' (a key point at the cursor, no time), where DUR is the notation's duration (q, h., h+q, 480t) and a bar's durations must fill it. Bars run in order from the first named to the last. The write is a diff over that span: points equal in beat and content are left alone (ids kept), points in the span the text omits are removed, the rest are added or replaced. History records the key.set, chord.set and remove operations it came to, so branches that write different bars merge. Keys and chords both follow the text: to leave the key alone, include it. Errors name the bar; nothing is changed when one occurs.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `text` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_write",
  "properties": {
    "first_bar": {
      "type": "integer"
    },
    "last_bar": {
      "type": "integer"
    },
    "added": {
      "type": "integer"
    },
    "replaced": {
      "type": "integer"
    },
    "removed": {
      "type": "integer"
    },
    "unchanged": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["first_bar", "last_bar", "added", "replaced", "removed", "unchanged"],
  "additionalProperties": false
}

```

### Examples

**Two bars of changes** (`empty`)

```json
{
  "text": "1: key Eb major {Ebmaj7}:w\n2: {Cm7}:h {Fm7}:h\n"
}

```

Result:

```json
{
  "first_bar": 1,
  "last_bar": 2,
  "added": 4,
  "replaced": 0,
  "removed": 0,
  "unchanged": 0
}

```

## `crash.acknowledge@1`

Mark a crash report, or a run that left none, as seen.

Writes a small marker beside the report; the report itself is never changed. An acknowledged report no longer counts for the restart notice. It does not end the suspicion of a plug-in: seen is not resolved, and crash.settle does that. Repeating it changes nothing.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report or ended run has that incident id Hint: list the reports with crash.list

### Examples

**Acknowledge a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "acknowledged": true
}

```

## `crash.clear@1`

Delete every finished crash report and record of an ended run.

Removes what crash.list shows as over: reports and unreported runs, with their acknowledgements. It cannot be undone. Live runs, reports still being written and files that are not valid reports are left alone. Returns how many incidents went.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Clear the reports** (`song_with_crash_report`)

```json
{}

```

Result:

```json
{
  "deleted": 1
}

```

## `crash.delete@1`

Delete one crash report, or the record of a run that left none.

Removes the report file, its acknowledgement and an idle run marker. It cannot be undone. A run that is still live or a report still being written is refused.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report or ended run has that incident id Hint: list the reports with crash.list

### Examples

**Delete a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "deleted": true
}

```

## `crash.list@1`

The crash reports earlier runs left, newest first.

Each report says when it was written, which build and role, how the run ended (kind: crash, unexpected_exit, bridge_exit for a plug-in bridge process that died while the app lived, or other for a kind this build does not know), the attribution level (fault_in_image, active_token, concurrent_activity, recent_command, bridge_exit, unknown; a stated confidence, not a verdict) and the plug-in it names, if any, as the plug-in described itself. unreported lists runs that ended and left no report (the supervisor died or could not write). A report counts once its run has ended: reports_pending are still being written; live_runs are running now, this one included. ignored counts files that are not valid reports. limit caps each list (default 50); the _total fields count them all. Reports hold plug-in and command names, never project content.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `limit` | integer |  | 1.0..200.0 |  |  |

### Result

```json
{}

```

### Examples

**The reports left behind** (`song_with_crash_report`)

```json
{}

```

Result:

```json
{
  "reports": "<any>",
  "reports_total": 1,
  "unreported": [],
  "unreported_total": 0,
  "live_runs": 0,
  "reports_pending": 0,
  "ignored": 0
}

```

## `crash.read@1`

One crash report in full.

The report as the supervisor wrote it (schema supple.crash-report/1.0): the run, the plug-ins it had announced, the threads with module-relative frames, the last commands, the attribution and everything it could not collect (unavailable, dropped). Frames are not symbolised: the offline resolver does that. Reports are tens of kilobytes and at most 4 MiB.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report has that incident id Hint: list the reports with crash.list

### Examples

**Read a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "report": "<any>"
}

```

## `crash.settle@1`

Mark the suspicion a crash report raised as resolved.

Writes a small marker beside the report; the report itself is never changed. A settled report no longer makes its plug-in a suspect (project.plugins, project.open's project.suspect_plugin warning). Use it once the plug-in was loaded normally and ran well, or when the person chose to load it normally; crash.acknowledge only says the report was seen. A report whose run has not ended cannot be settled. Repeating it changes nothing.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report has that incident id Hint: list the reports with crash.list

### Examples

**Settle a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "settled": true
}

```

## `dsp.check@1`

Compile a DSP script without saving it; return its diagnostics.

Give source (a whole script) or name (one in the library). ok is true when it compiles; plugin then lists its id, name and parameters as the patch shows them.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_check_result",
  "properties": {
    "ok": {
      "type": "boolean"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["ok", "diagnostics"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of name and source Hint: give exactly one
- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts

### Examples

**Check a script in the library** (`dsp_script`)

```json
{
  "name": "gain"
}

```

Result:

```json
{
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>"
}

```

**A mistake, with its hint** (`empty`)

```json
{
  "source": "struct X : Effect {};"
}

```

Result:

```json
{
  "ok": false,
  "diagnostics": "<any>"
}

```

## `dsp.edit@1`

Replace text in a library script and compile it, as dsp.write.

For a change to part of a script, instead of sending all of it to dsp.write. Each edit replaces old, which must be in the script exactly once, with new (empty deletes it). The edits apply in order, each to the script as the ones before left it, and all or none: when one fails, nothing is saved and the error says where old and the script part. Then as dsp.write: it saves the script, compiles it and returns the diagnostics; instances playing it follow within half a second.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `edits` | array |  |  |  |  |
| `edits[].old` | string |  |  |  | text the script has in one place |
| `edits[].new` | string |  |  |  | what replaces it |

### Result

```json
{
  "type": "object",
  "title": "dsp_write_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "ok": {
      "type": "boolean",
      "description": "it compiled"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "listed": {
      "type": "boolean",
      "description": "plugin.list shows it (the script player is on the plugin path)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "ok", "diagnostics", "listed"],
  "additionalProperties": false
}

```

### Errors

- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts
- `dsp.edit`: an edit's old text is not in the script, or in several places Hint: copy old exactly from dsp.get's source, with enough around it to be unique
- `dsp.size`: the script would be longer than 1 MiB Hint: split it up
- `dsp.id_taken`: another script in the library has its id; not saved Hint: give Info.id a value no other script has

### Examples

**Widen a parameter's range** (`dsp_script`)

```json
{
  "name": "gain",
  "edits": [
    {
      "old": "-24, 24, 0",
      "new": "-36, 36, 0"
    }
  ]
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>",
  "listed": "<any>"
}

```

## `dsp.export@1`

Export a DSP script as a plugin: a CLAP file, or C++ source.

format clap (the default): copies the platform's script player and embeds the script in its trailer (Linux/Windows) or bundle resource (macOS, re-signed ad hoc with the system codesign tool). The copy is a plugin of its own, under the script's id and name, that plays this version of the script in any CLAP host (it reads no library), written to <folder>/<plugin name>.clap (default: the script's folder, under export/) with the licenses to ship beside it (notices). It runs on this platform, as fast as the player it copies (a debug build's is slow). format source: a C++ project of the plugin template that builds the same plugin (same id, parameters and state) as CLAP and VST3, plus AU on macOS, with the script compiled as C++, written to the empty <folder> (default: the script's folder, under export/<script>-source); its README says how to build it, and license (MIT unless GPL-3.0-or-later) is the license of its own files. Export again after changing the script; an export does not follow the library.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `script` | string |  |  |  | a name from dsp.list |
| `format` | string |  |  |  | clap (default): a plugin file; source: a C++ project One of: clap, source. |
| `folder` | string |  |  |  | an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new |
| `license` | string |  |  |  | source: the license of the project's own files (default MIT) One of: MIT, GPL-3.0-or-later. |

### Result

```json
{
  "type": "object",
  "title": "dsp_export_result",
  "properties": {
    "path": {
      "type": "string",
      "description": "the plugin file, or the project's folder"
    },
    "id": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "format": {
      "type": "string",
      "description": "clap or source"
    },
    "player": {
      "type": "string",
      "description": "clap: the script player it was copied from"
    },
    "bytes": {
      "type": "integer",
      "minimum": 0.0
    },
    "notices": {
      "type": "string",
      "description": "clap: the licenses to ship with the plugin"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "id", "name", "format"],
  "additionalProperties": false
}

```

### Errors

- `dsp.sign`: macOS: ad-hoc bundle signing failed or codesign is unavailable Hint: restore the player and system codesign tool, or use format source
- `dsp.compile`: the script does not compile Hint: dsp.check gives its diagnostics
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.player`: the plugin with the script's id is not the script player Hint: give the script an id no other plugin has
- `dsp.notices`: the player's license or notices cannot be read Hint: restore the license and notices beside the player before exporting
- `dsp.path`: the folder is relative, or (source) not empty Hint: give an absolute folder; for source, a new one
- `dsp.plugin_name`: source: the plugin's name cannot name a project Hint: rename it in Info.name
- `dsp.no_parameter`: source: the project's tests need a parameter a host can automate, plainly named Hint: declare one
- `dsp.kit`: source: no export kit beside the stock plugins Hint: build the script-export-kit target

### Examples

**Export a script from the library as a plugin file** (`dsp_player_export`)

```json
{
  "script": "gain"
}

```

Result:

```json
{
  "path": "<any>",
  "id": "com.example.gain",
  "name": "Gain",
  "format": "clap",
  "player": "<any>",
  "bytes": "<any>",
  "notices": "<any>"
}

```

**Export it as a C++ project** (`dsp_script`)

```json
{
  "script": "gain",
  "format": "source"
}

```

Result:

```json
{
  "path": "<any>",
  "id": "com.example.gain",
  "name": "Gain",
  "format": "source"
}

```

## `dsp.get@1`

Read a DSP script from the library.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_get_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "source"],
  "additionalProperties": false
}

```

### Errors

- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts

### Examples

**Read a script** (`dsp_script`)

```json
{
  "name": "gain"
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "source": "<any>"
}

```

## `dsp.list@1`

List the DSP scripts in the library, compiled, with their plugin ids.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "dsp_list_result",
  "properties": {
    "library": {
      "type": "string"
    },
    "scripts": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_list_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          },
          "ok": {
            "type": "boolean",
            "description": "it compiles"
          },
          "errors": {
            "type": "integer",
            "minimum": 0.0
          },
          "id": {
            "type": "string",
            "description": "its plugin's id"
          },
          "plugin_name": {
            "type": "string"
          }
        },
        "required": ["name", "path", "ok", "errors"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["library", "scripts"],
  "additionalProperties": false
}

```

### Examples

**An empty library** (`empty`)

```json
{}

```

Result:

```json
{
  "library": "<any>",
  "scripts": []
}

```

## `dsp.preview@1`

Draw a plugin's editor to a PNG, after a test signal plays through it.

Give plugin (an id) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays signal -- an effect: sine (the default, at frequency_hz), sweep, impulse, noise or silence, peaking at level_db, for seconds, or file (the default with audio_file), the file played whole as dsp.test plays it; an instrument: notes (the default), note or silence, for seconds -- while its editor watches, so meters and the script's draw() show what played. Then the editor is drawn without a window, scale times its design size, and saved as a PNG (path; width and height in pixels): read the file to see it. A script with neither editor() nor draw() shows the automatic editor. pointer, if given, acts on the editor first, in its design units (the picture's pixels at scale 1): move (a hover; a drag after a press), press, release, click, double_click, drag (from x, y to to_x, to_y) and wheel (steps notches, up positive), with shift, ctrl or alt held; edits is a merge patch of what they changed, as a user's edits would. problems says what went wrong in the editor's code (a loop stopped by its budget, an index out of range), with the line. Headless pictures use EGL on Linux, Metal on macOS and WGL on Windows; an unavailable renderer returns dsp.no_picture.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signal` | string |  |  |  | One of: sine, sweep, impulse, noise, silence, notes, note, file. |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `level_db` | number or string | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | number or string | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.0..10.0 | 1.0 |  |
| `scale` | number |  | 0.5..3.0 | 1.0 |  |
| `pointer` | array |  |  |  |  |
| `pointer[].action` | string |  |  |  | One of: move, press, release, click, double_click, drag, wheel. |
| `pointer[].x` | number |  | -1000.0..5000.0 |  | design units: the picture's pixels at scale 1 |
| `pointer[].y` | number |  | -1000.0..5000.0 |  | design units: the picture's pixels at scale 1 |
| `pointer[].to_x` | number |  | -1000.0..5000.0 |  | drag: where it ends |
| `pointer[].to_y` | number |  | -1000.0..5000.0 |  | drag: where it ends |
| `pointer[].steps` | number |  | -20.0..20.0 | 1.0 | wheel: notches turned, up positive |
| `pointer[].shift` | boolean |  |  | false |  |
| `pointer[].ctrl` | boolean |  |  | false |  |
| `pointer[].alt` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_preview_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "path": {
      "type": "string",
      "description": "the PNG"
    },
    "width": {
      "type": "integer",
      "description": "pixels"
    },
    "height": {
      "type": "integer",
      "description": "pixels"
    },
    "edits": {
      "description": "what pointer changed, as a merge patch"
    },
    "problems": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "path", "width", "height", "edits", "problems", "log"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of plugin and script Hint: give exactly one
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.signal`: a signal the plugin cannot play Hint: an effect plays audio signals, an instrument notes
- `dsp.file`: the file signal without audio_file, or a file that is not absolute, is over 10 minutes or holds no audio Hint: give audio_file a WAV, FLAC or MP3, or another signal
- `dsp.pointer`: a drag without its end, or too many steps Hint: give a drag to_x and to_y; at most 64 steps
- `dsp.no_picture`: the plugin cannot draw its editor without a window Hint: scripts can, through the script player; other plugins need the editor-picture extension

### Examples

**See a script's editor** (`dsp_player_editor`)

```json
{
  "script": "gain"
}

```

Result:

```json
{
  "plugin": "com.example.gain",
  "path": "<any>",
  "width": "<any>",
  "height": "<any>",
  "edits": {},
  "problems": [],
  "log": "<any>"
}

```

**Drag a knob, then see the editor** (`dsp_player_editor`)

```json
{
  "script": "gain",
  "pointer": [
    {
      "action": "drag",
      "x": 86,
      "y": 132,
      "to_x": 86,
      "to_y": 92
    }
  ]
}

```

Result:

```json
{
  "plugin": "com.example.gain",
  "path": "<any>",
  "width": "<any>",
  "height": "<any>",
  "edits": {
    "params": {
      "gain": "<any>"
    }
  },
  "problems": [],
  "log": "<any>"
}

```

## `dsp.test@1`

Play test signals or notes through a plugin; measure levels, THD, pitch.

Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays each signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at level_db; and file, audio_file played as it is (resampled to the render's rate, mono on both channels, at most 60 s), the only signal when signals is not given. An instrument, at velocity 0.8: notes (a note, then a chord), note (A4; pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from silence, no output, a note out of tune, sound that never dies away, non-finite samples the framework silenced, a script stopped by its loop budget or indexing outside an array). cpu_percent is measured, so it varies between runs. save: true keeps each output as a WAV (path) for analyze and compare. automation is a list of {param, at_s, value}: semantic patch keys and physical values, as discrete sample-accurate changes applied to every signal (no implicit ramps). automation.max_step is the largest adjacent-sample change in full-scale amplitude; baseline_max_step uses the same render without automation; excess_step is their positive difference. A step above both four times baseline and 0.05 suggests clicks or zipper noise: a screening heuristic, not proof of an artifact or of inaudibility. sidechain supplies a timed key; its middle half is measured against a connected silent key with identical automation: gain_change_db is negative for ducking, null if either output is below -120 dBFS. Slow attack may not settle in that window. Each pass has a fresh instance and initial patch. THD and impulse response are omitted during automation or a non-silent key; gain_db then describes the changing render, not static gain. notes supplies an instrument's sequence ({key, velocity, start_s, length_s}), replacing the notes signal; defaults become notes and silence. Do not combine it with the A4 note signal or select only silence. release_ms starts at its last note-off. Times round to samples at the render rate; spans must fit within seconds. sample_rate renders at that rate instead of the session's (sandbox) or the project's (DAW); latency is in samples at the rate used, which sample_rate in the result reports. meters gives a semantic plugin's read-only parameters (its meters and readouts) by patch key in physical units, as they stood when the signal ended (the tail after it does not move them). traces gives what a script's trace(name, value) calls recorded over the signal, in trace_points intervals of trace_interval_s: for each name, the last value in each interval and the lowest and highest, null where none was given.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signals` | array |  |  |  | default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `automation` | array |  |  |  | discrete physical-value changes on every signal |
| `automation[].param` | string |  |  |  | semantic patch key |
| `automation[].at_s` | number |  | 0.0.. |  |  |
| `automation[].value` | value |  |  |  | physical value, enum key or boolean |
| `sidechain` | object |  |  |  | timed generated key input |
| `sidechain.signal` | string |  |  | "sine" | One of: sine, sweep, impulse, noise, silence. |
| `sidechain.level_db` | number or string | dB | -60.0..0.0 | -6.0 | Quantity strings are range-checked by the server. |
| `sidechain.frequency_hz` | number or string | Hz | 10.0..20000.0 | 100.0 | Quantity strings are range-checked by the server. |
| `sidechain.start_s` | number |  | 0.0.. |  | default: 0.25 * seconds |
| `sidechain.length_s` | number |  |  |  | default: 0.5 * seconds |
| `notes` | array |  |  |  | instrument sequence replacing the notes stimulus |
| `notes[].key` | integer |  | 0.0..127.0 |  |  |
| `notes[].velocity` | number |  | ..1.0 | 0.8 |  |
| `notes[].start_s` | number |  | 0.0.. |  |  |
| `notes[].length_s` | number |  |  |  |  |
| `level_db` | number or string | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | number or string | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.1..10.0 | 1.0 |  |
| `sample_rate` | number or string | Hz | 8000.0..384000.0 |  | default: the session's or project's rate Quantity strings are range-checked by the server. |
| `save` | boolean |  |  | false |  |
| `trace_points` | integer |  | 1.0..4096.0 | 100 | intervals the signal's traces are kept in |

### Result

```json
{
  "type": "object",
  "title": "dsp_test_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "sample_rate": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "string",
          "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:[hH][zZ]|[kK][hH][zZ]|[kK]))?\\s*$"
        }
      ],
      "x-unit": "Hz",
      "x-unit-strings": true,
      "description": "the rate rendered at Quantity strings are range-checked by the server."
    },
    "latency": {
      "type": "integer",
      "description": "samples"
    },
    "signals": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_signal_result",
        "properties": {
          "signal": {
            "type": "string"
          },
          "peak_dbfs": {
            "type": "number"
          },
          "rms_dbfs": {
            "type": "number"
          },
          "dc": {
            "type": "number"
          },
          "clipped": {
            "type": "integer",
            "minimum": 0.0
          },
          "denormals": {
            "type": "integer",
            "minimum": 0.0
          },
          "gain_db": {
            "type": "number",
            "description": "sine: the fundamental out against in"
          },
          "thd_percent": {
            "type": "number"
          },
          "response_db": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "dsp_response_point",
              "properties": {
                "hz": {
                  "type": "number"
                },
                "db": {
                  "type": "number"
                }
              },
              "required": ["hz", "db"],
              "additionalProperties": false
            }
          },
          "pitch_hz": {
            "type": "number",
            "description": "note: the pitch heard (A4 is 440 Hz)"
          },
          "release_ms": {
            "type": "number",
            "description": "an instrument's: sound after the last release"
          },
          "path": {
            "type": "string"
          },
          "automation": {
            "type": "object",
            "title": "dsp_automation_result",
            "properties": {
              "max_step": {
                "type": "number"
              },
              "baseline_max_step": {
                "type": "number"
              },
              "excess_step": {
                "type": "number"
              }
            },
            "required": ["max_step", "baseline_max_step", "excess_step"],
            "additionalProperties": false
          },
          "sidechain": {
            "type": "object",
            "title": "dsp_sidechain_result",
            "properties": {
              "rms_dbfs": {
                "type": "number"
              },
              "reference_rms_dbfs": {
                "type": "number"
              },
              "gain_change_db": {
                "type": ["number", "null"]
              }
            },
            "required": ["rms_dbfs", "reference_rms_dbfs", "gain_change_db"],
            "additionalProperties": false
          },
          "meters": {
            "description": "read-only parameters (meters) by patch key, physical, as of the signal's end"
          },
          "trace_interval_s": {
            "type": "number",
            "description": "each trace interval's length"
          },
          "traces": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "dsp_trace_result",
              "properties": {
                "name": {
                  "type": "string"
                },
                "last": {
                  "description": "each interval's last value, or null"
                },
                "low": {
                  "description": "each interval's lowest"
                },
                "high": {
                  "description": "each interval's highest"
                },
                "non_finite": {
                  "type": "integer",
                  "minimum": 0.0,
                  "description": "NaN or infinity given, not kept"
                }
              },
              "required": ["name", "last", "low", "high", "non_finite"],
              "additionalProperties": false
            },
            "description": "what trace() recorded, by name"
          }
        },
        "required": ["signal", "peak_dbfs", "rms_dbfs", "dc", "clipped", "denormals"],
        "additionalProperties": false
      }
    },
    "cpu_percent": {
      "type": "number",
      "description": "of real time, measured"
    },
    "problems": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "sample_rate", "latency", "signals", "cpu_percent", "problems", "log"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of plugin and script Hint: give exactly one
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.not_effect`: the plugin takes no audio and no notes Hint: test an effect or an instrument
- `dsp.signal`: a signal the plugin cannot play Hint: an effect plays audio signals, an instrument notes
- `dsp.automation`: unavailable parameter or automation time outside the render Hint: choose a writable semantic parameter and a time before seconds
- `dsp.sidechain`: no key input or invalid key timing Hint: choose a plugin with a key and a nonempty span within seconds
- `dsp.notes`: invalid custom sequence or signal selection Hint: use an instrument, notes or notes and silence, and spans within seconds
- `dsp.frequency`: a sine at or above half the sample rate Hint: lower frequency_hz or raise sample_rate
- `dsp.file`: the file signal without audio_file, or a file that is not absolute, is over 10 minutes or holds no audio Hint: give audio_file a WAV, FLAC or MP3, or leave file out of signals

### Examples

**Measure an effect with a sine** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Automate an effect in dB** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"],
  "automation": [
    {
      "param": "gain",
      "at_s": 0.253,
      "value": -12
    },
    {
      "param": "gain",
      "at_s": 0.753,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Measure a timed sidechain** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.keymix",
  "signals": ["sine"],
  "sidechain": {
    "level_db": -6
  }
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Measure an effect at 96 kHz** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"],
  "sample_rate": 96000
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Play a custom note sequence** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.sine",
  "notes": [
    {
      "key": 60,
      "start_s": 0.013,
      "length_s": 0.2
    },
    {
      "key": 67,
      "velocity": 0.6,
      "start_s": 0.303,
      "length_s": 0.4
    }
  ]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

## `dsp.write@1`

Save a DSP script to the library and compile it, with diagnostics.

Writes <library>/<name>/<name>.cpp (replacing it), compiles it and returns the compiler's diagnostics, each with a hint. A script that compiles becomes a plugin of the script player under the id its Info gives: plugin.list shows it (listed) and fx.add loads it. Instances already playing it pick up the change within half a second, crossfaded; one that fails to compile leaves them playing the old code. Language: the dsp-language guide.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | the script's folder and file name |
| `source` | string |  |  |  | the whole C++ file |

### Result

```json
{
  "type": "object",
  "title": "dsp_write_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "ok": {
      "type": "boolean",
      "description": "it compiled"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "listed": {
      "type": "boolean",
      "description": "plugin.list shows it (the script player is on the plugin path)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "ok", "diagnostics", "listed"],
  "additionalProperties": false
}

```

### Errors

- `dsp.name`: not a script name Hint: use lower-case letters, digits, _ or -
- `dsp.size`: the script is longer than 1 MiB Hint: split it up
- `dsp.id_taken`: another script in the library has its id; not saved Hint: give Info.id a value no other script has

### Examples

**Save a gain script** (`empty`)

```json
{
  "name": "gain",
  "source": "#include <supple/script.h>\n\nstruct Gain : Effect {\n    static constexpr Info info = {.name = \"Gain\", .id = \"com.example.gain\"};\n    Param gain = decibels(\"Gain\", -24, 24, 0).smoothed(20).help(\"The level change.\");\n\n    void process(Block& io) {\n        for (int c = 0; c < io.channels; ++c)\n            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));\n    }\n};\n"
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>",
  "listed": "<any>"
}

```

## `engine.devices@1`

List audio devices: names, host APIs, rates, channel counts.

Enumerates PortAudio devices on macOS/Windows without opening a stream. backend defaults to portaudio; dummy returns an empty list without initializing hardware. Use exact host_api and name values with engine.start. Linux uses JACK ports and returns backend.unsupported; engine.inputs lists running JACK inputs.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  | One of: jack, dummy, portaudio. |

### Result

```json
{
  "type": "object",
  "title": "engine_devices_result",
  "properties": {
    "devices": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "audio_device",
        "properties": {
          "host_api": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "inputs": {
            "type": "integer"
          },
          "outputs": {
            "type": "integer"
          },
          "default_rate": {
            "type": "number"
          },
          "default_input": {
            "type": "boolean"
          },
          "default_output": {
            "type": "boolean"
          }
        },
        "required": [
          "host_api",
          "name",
          "inputs",
          "outputs",
          "default_rate",
          "default_input",
          "default_output"
        ],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["devices"],
  "additionalProperties": false
}

```

### Errors

- `backend.unsupported`: native device enumeration is unavailable Hint: use engine.inputs with JACK

### Examples

**Without hardware** (`empty`)

```json
{
  "backend": "dummy"
}

```

Result:

```json
{
  "devices": []
}

```

## `engine.inputs@1`

The device's audio inputs, MIDI devices and master outputs.

audio: one entry per input channel (channel, the 1-based number track.set record_input takes, and the device port it is connected to). midi: the MIDI devices (names track.set record_midi matches; JACK ports or native CoreMIDI/WinMM inputs, fixed until restart). midi_outputs: destination names for the hardware output API (routing and clock generation are separate). outputs: one entry per output channel (channel, the number track.set hw_output takes for a master, and the port it is connected to). While the engine is stopped: running false and empty lists (engine.start opens the device).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Stopped** (`empty`)

```json
{}

```

Result:

```json
{
  "running": false,
  "audio": [],
  "midi": [],
  "outputs": [],
  "midi_outputs": []
}

```

## `engine.start@1`

Start playback through JACK, native PortAudio, or a device-free dummy.

backend: jack (Linux default), portaudio (macOS/Windows native device), or dummy (default elsewhere, no device). For portaudio, host_api, input_device and output_device select exact names from engine.devices. inputs defaults to 0 (playback only); outputs (the device channels masters can play to, track.set hw_output) defaults to one per physical playback port with JACK (2 to 64) and 2 otherwise. rate defaults to the project rate; a rate given is asked of the device (CoreAudio switches the device's rate when it can, WASAPI converts) or, with JACK on PipeWire, suggested to the graph (node.rate: applied only while the devices are idle and the rate is allowed). force_graph (JACK on PipeWire, default false) instead forces block as the graph's quantum, and rate if given, while the engine runs. PortAudio opens up to 16 native MIDI inputs and outputs; engine.inputs lists their names. Dummy is paced like a device. block: the device block in frames (default 256); the device's period may differ and is reported. anticipative (default true): render ahead of the playhead on worker threads (edits are heard up to 200 ms later); false renders everything in the audio callback. The engine runs at the device's rate (media is resampled; renders stay at the project's rate). Background and live thread capacities follows the app's Audio processing threads preference (Automatic by default); live helpers join each callback with no added latency. memory_lock reports the default-off host-process memory policy: requested, active, and a structured OS error on failure. cpu_idle_latency reports the 20 microsecond PM QoS request: requested by the default-off option, or automatic, held on Linux while a bridged plug-in plays live; released on engine stop. thp_opt_out: on Linux the process is kept out of transparent huge pages while the engine runs (advised_only: regions a plug-in advised keep them). A protection failure does not prevent audio startup. MIDI output refusals are in midi_output_warnings; midi_output_latency reports signed audio-minus-MIDI frames per output. MIDI inputs that refuse to open are skipped: midi_warnings reports each device, numeric os_error, message and corrective hint; audio continues.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  | One of: jack, dummy, portaudio. |
| `host_api` | string |  |  |  |  |
| `input_device` | string |  |  |  |  |
| `output_device` | string |  |  |  |  |
| `rate` | number |  | 1.0..768000.0 |  |  |
| `inputs` | integer |  | 0.0..64.0 |  |  |
| `outputs` | integer |  | 2.0..64.0 |  |  |
| `block` | integer |  | 16.0..4096.0 |  |  |
| `anticipative` | boolean |  |  |  |  |
| `force_graph` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "preparing_audio": {
      "type": "boolean"
    },
    "prepared_version": {
      "type": "integer"
    },
    "preparation_version": {
      "type": "integer"
    },
    "preparation_error": {
      "type": "string"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "bridge_late": {
      "type": "integer"
    },
    "bridge_timing": {
      "type": "object",
      "title": "engine_bridge_timing",
      "properties": {
        "calls": {
          "type": "integer"
        },
        "late": {
          "type": "integer"
        },
        "settles": {
          "type": "integer"
        },
        "past_deadline": {
          "type": "integer"
        },
        "spun": {
          "type": "integer"
        },
        "wait_max_us": {
          "type": "number"
        },
        "wait_mean_us": {
          "type": "number"
        },
        "wake_max_us": {
          "type": "number"
        },
        "wake_mean_us": {
          "type": "number"
        },
        "work_max_us": {
          "type": "number"
        },
        "return_max_us": {
          "type": "number"
        },
        "wakes_over_200us": {
          "type": "integer"
        },
        "wakes_over_1ms": {
          "type": "integer"
        },
        "bridge_cpus": {
          "type": "array",
          "items": {
            "type": "integer"
          }
        },
        "migrations": {
          "type": "integer"
        },
        "class_mismatches": {
          "type": "integer"
        },
        "longest_cycle_us": {
          "type": "number"
        },
        "overruns_late_wait": {
          "type": "integer"
        },
        "overruns_slow_bridge": {
          "type": "integer"
        },
        "overruns_not_waiting": {
          "type": "integer"
        }
      },
      "required": [
        "calls",
        "late",
        "settles",
        "past_deadline",
        "spun",
        "wait_max_us",
        "wait_mean_us",
        "wake_max_us",
        "wake_mean_us",
        "work_max_us",
        "return_max_us",
        "wakes_over_200us",
        "wakes_over_1ms",
        "bridge_cpus",
        "migrations",
        "class_mismatches",
        "longest_cycle_us",
        "overruns_late_wait",
        "overruns_slow_bridge",
        "overruns_not_waiting"
      ],
      "additionalProperties": false
    },
    "midi_dropped": {
      "type": "integer"
    },
    "midi_inputs": {},
    "midi_outputs": {},
    "midi_lead_frames": {
      "type": "integer"
    },
    "midi_out_dropped": {
      "type": "integer"
    },
    "midi_output_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_output_latency": {
      "type": "array",
      "items": {
        "type": "integer"
      }
    },
    "midi_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_devices_changed": {
      "type": "boolean"
    },
    "midi_devices_message": {
      "type": "string"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    },
    "live_helpers": {
      "type": "integer"
    },
    "scheduling": {
      "type": "object",
      "title": "engine_scheduling",
      "properties": {
        "callback": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "live": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "renderer": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "background": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        }
      },
      "required": ["callback", "live", "renderer", "background"],
      "additionalProperties": false
    },
    "workgroup": {
      "type": "object",
      "title": "engine_workgroup",
      "properties": {
        "state": {
          "type": "string"
        },
        "joined": {
          "type": "integer"
        },
        "failed": {
          "type": "integer"
        },
        "os_error": {
          "type": "integer"
        },
        "message": {
          "type": "string"
        },
        "hint": {
          "type": "string"
        }
      },
      "required": ["state", "joined", "failed", "os_error", "message", "hint"],
      "additionalProperties": false
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
    "memory_lock": {
      "type": "object",
      "title": "runtime_protection_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active"],
      "additionalProperties": false
    },
    "cpu_idle_latency": {
      "type": "object",
      "title": "cpu_idle_latency_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "automatic": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active", "automatic"],
      "additionalProperties": false
    },
    "thp_opt_out": {
      "type": "object",
      "title": "thp_opt_out_status",
      "properties": {
        "active": {
          "type": "boolean"
        },
        "advised_only": {
          "type": "boolean"
        },
        "inherited": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["active", "advised_only", "inherited"],
      "additionalProperties": false
    },
    "audio_page_faults": {
      "type": "object",
      "title": "audio_page_faults",
      "properties": {
        "scope": {
          "type": "string",
          "pattern": "^(audio_threads|process|none)$"
        },
        "minor": {
          "type": "integer"
        },
        "major": {
          "type": "integer"
        },
        "threads": {
          "type": "integer"
        },
        "saturated": {
          "type": "boolean"
        }
      },
      "required": ["scope", "minor", "threads", "saturated"],
      "additionalProperties": false
    },
    "meter_drops": {
      "type": "object",
      "title": "meter_drops",
      "properties": {
        "renderer": {
          "type": "integer"
        },
        "callback": {
          "type": "integer"
        }
      },
      "required": ["renderer", "callback"],
      "additionalProperties": false
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "disabled_fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "fx whose plug-in this run has disabled (plugin.override): the strip passes audio through"
    },
    "bridge_threads_refused": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "modulation_deferred": {
      "type": "integer",
      "description": "Modulation events that waited past their grid point for room in a block (ADR-0096)"
    },
    "failed_errors": {},
    "sync": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    }
  },
  "required": [
    "running",
    "preparing_audio",
    "prepared_version",
    "preparation_version",
    "preparation_error",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "bridge_late",
    "bridge_timing",
    "midi_dropped",
    "midi_inputs",
    "midi_outputs",
    "midi_lead_frames",
    "midi_out_dropped",
    "midi_output_warnings",
    "midi_output_latency",
    "midi_warnings",
    "midi_devices_changed",
    "midi_devices_message",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses",
    "live_helpers",
    "scheduling",
    "workgroup",
    "warnings",
    "memory_lock",
    "cpu_idle_latency",
    "thp_opt_out",
    "audio_page_faults",
    "meter_drops",
    "failed_fx",
    "disabled_fx",
    "bridge_threads_refused",
    "dropped",
    "voice_dropped",
    "modulation_deferred",
    "failed_errors",
    "sync"
  ],
  "additionalProperties": false
}

```

### Errors

- `engine.running`: the engine is already running Hint: engine.stop first
- `backend.device`: device or host API was not found Hint: choose names from engine.devices
- `backend.config`: device format is invalid Hint: check rate and channel capacities
- `backend.unavailable`: JACK is not installed Hint: install pipewire-jack
- `backend.open`: no JACK server answered Hint: is PipeWire running?
- `backend.unsupported`: backend is unavailable on this platform Hint: select dummy

### Examples

**Without a device** (`empty`)

```json
{
  "backend": "dummy",
  "block": 128
}

```

Result:

```json
{
  "running": true,
  "backend": "dummy",
  "sample_rate": 48000.0,
  "block": 128,
  "period": 128,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "bridge_late": 0,
  "bridge_timing": {
    "calls": 0,
    "late": 0,
    "settles": 0,
    "past_deadline": 0,
    "spun": 0,
    "wait_max_us": 0,
    "wait_mean_us": 0,
    "wake_max_us": 0,
    "wake_mean_us": 0,
    "work_max_us": 0,
    "return_max_us": 0,
    "wakes_over_200us": 0,
    "wakes_over_1ms": 0,
    "bridge_cpus": [],
    "migrations": 0,
    "class_mismatches": 0,
    "longest_cycle_us": 0,
    "overruns_late_wait": 0,
    "overruns_slow_bridge": 0,
    "overruns_not_waiting": 0
  },
  "midi_dropped": 0,
  "midi_warnings": [],
  "midi_out_dropped": 0,
  "midi_output_warnings": [],
  "midi_output_latency": [],
  "midi_inputs": [],
  "midi_outputs": [],
  "midi_lead_frames": 0,
  "midi_devices_changed": false,
  "midi_devices_message": "",
  "preparing_audio": false,
  "preparation_error": "",
  "prepared_version": "<any>",
  "preparation_version": 0,
  "cycles": "<any>",
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": true,
  "anticipation_misses": 0,
  "live_helpers": "<any>",
  "scheduling": {
    "callback": {
      "policy": "normal",
      "priority": 0
    },
    "live": "<any>",
    "renderer": {
      "policy": "normal",
      "priority": 0
    },
    "background": "<any>"
  },
  "workgroup": {
    "state": "none",
    "joined": 0,
    "failed": 0,
    "os_error": 0,
    "message": "",
    "hint": ""
  },
  "warnings": [],
  "memory_lock": {
    "requested": false,
    "active": false
  },
  "cpu_idle_latency": {
    "requested": false,
    "active": false,
    "automatic": false
  },
  "thp_opt_out": "<any>",
  "audio_page_faults": "<any>",
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "disabled_fx": [],
  "bridge_threads_refused": [],
  "dropped": 0,
  "voice_dropped": 0,
  "modulation_deferred": 0,
  "failed_errors": [],
  "sync": "<any>"
}

```

## `engine.status@1`

Engine state: backend, rate, block, device period, latency, xruns.

preparing_audio: source preparation is pending; the previous ready plan stays audible. prepared_version is the last compiled version, preparation_version the pending target (zero when idle); preparation_error explains a failed preparation. plan_latency: plugin delay plus prepared MIDI audio lead in frames; midi_lead_frames reports the latter. midi_inputs/midi_outputs give per-port enabled/timing/loss/warnings, and for outputs what each sends (clock, transport, spp, mtc), as midi.inputs.get/midi.outputs.get. device_latency: frames from the engine's outputs to the device. restart_needed: the device changed rate or went away; engine.stop and engine.start again. midi_devices_changed is advisory: MIDI topology changed but audio keeps running. midi_devices_message asks for an explicit engine restart to pick up the new set; existing port names stay fixed. midi_out_dropped counts hardware output rejections, queue/device loss and pending events discarded at shutdown. midi_output_warnings lists outputs skipped at start; midi_output_latency is signed audio-minus-MIDI latency per output, in frames. sync: external sync (sync.get has the same status with the settings). midi_dropped: MIDI input messages/packets rejected or lost to bounded queues (JACK and native; zero for the dummy backend). midi_warnings lists inputs skipped at start, with device, numeric os_error, message and corrective hint. meter_drops: entries a full ring dropped (the renderer's, the audio callback's) — a track's peak/RMS missed a block, never audio. dropped/voice_dropped count lost plug-in output/voice records. memory_lock reports requested/active and an optional OS error with errno and a corrective hint, including while stopped. Bridges are excluded. cpu_idle_latency reports the owned 20 microsecond request with the same status/error fields, and automatic: held without the option because a bridged plug-in plays live (Linux; an automatic failure is no warning). It affects system idle while held and leaves CPU frequency policy unchanged. thp_opt_out: active, advised_only, inherited (the process was already kept out when the engine started; left as it was, not undone at stop) and an optional error, Linux only (khugepaged collapsing memory would stall audio threads). audio_page_faults: page faults since the engine started; scope audio_threads counts the callback's and live helpers' (Linux), process the whole process's (macOS, Windows, where major is omitted), none while stopped; saturated: the platform's counter stopped at its limit (macOS 32-bit counts), so minor is 0 and major omitted, not counts. live_helpers is the enabled live helper capacity. scheduling: each thread class's policy (none, normal, fifo, rr, high) and priority as applied at start: the device callback, live helpers, renderer and background helpers (real-time below a real-time callback when permitted). workgroup: whether the live helpers joined the device's audio workgroup (macOS CoreAudio; state none, ready, joined, partial or failed, with the helper counts and the last refusal's OS error, message and hint). warnings includes scheduling fallbacks and plan warnings with corrective actions. failed_errors adds structured code, message, path and hint to each failure, and `incident`, the id of the local crash report the bridge's death may have caused (crash.read; the report may not exist), when it died of a signal or an exception. failed_fx: isolated plugins whose process crashed or hung (silent now; fx.restart re-creates them). bridge_late: callback cycles in which a bridged plugin had not answered by the cycle's bridge deadline (its period or 2 ms, whichever is larger); its output was silent for that cycle, and xruns counts each of these cycles too. bridge_threads_refused: isolated plugins whose bridge audio thread was refused the scheduling it asked for (it runs at what it had and can fall behind under load; grant real-time scheduling to the user and restart the engine).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "preparing_audio": {
      "type": "boolean"
    },
    "prepared_version": {
      "type": "integer"
    },
    "preparation_version": {
      "type": "integer"
    },
    "preparation_error": {
      "type": "string"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "bridge_late": {
      "type": "integer"
    },
    "bridge_timing": {
      "type": "object",
      "title": "engine_bridge_timing",
      "properties": {
        "calls": {
          "type": "integer"
        },
        "late": {
          "type": "integer"
        },
        "settles": {
          "type": "integer"
        },
        "past_deadline": {
          "type": "integer"
        },
        "spun": {
          "type": "integer"
        },
        "wait_max_us": {
          "type": "number"
        },
        "wait_mean_us": {
          "type": "number"
        },
        "wake_max_us": {
          "type": "number"
        },
        "wake_mean_us": {
          "type": "number"
        },
        "work_max_us": {
          "type": "number"
        },
        "return_max_us": {
          "type": "number"
        },
        "wakes_over_200us": {
          "type": "integer"
        },
        "wakes_over_1ms": {
          "type": "integer"
        },
        "bridge_cpus": {
          "type": "array",
          "items": {
            "type": "integer"
          }
        },
        "migrations": {
          "type": "integer"
        },
        "class_mismatches": {
          "type": "integer"
        },
        "longest_cycle_us": {
          "type": "number"
        },
        "overruns_late_wait": {
          "type": "integer"
        },
        "overruns_slow_bridge": {
          "type": "integer"
        },
        "overruns_not_waiting": {
          "type": "integer"
        }
      },
      "required": [
        "calls",
        "late",
        "settles",
        "past_deadline",
        "spun",
        "wait_max_us",
        "wait_mean_us",
        "wake_max_us",
        "wake_mean_us",
        "work_max_us",
        "return_max_us",
        "wakes_over_200us",
        "wakes_over_1ms",
        "bridge_cpus",
        "migrations",
        "class_mismatches",
        "longest_cycle_us",
        "overruns_late_wait",
        "overruns_slow_bridge",
        "overruns_not_waiting"
      ],
      "additionalProperties": false
    },
    "midi_dropped": {
      "type": "integer"
    },
    "midi_inputs": {},
    "midi_outputs": {},
    "midi_lead_frames": {
      "type": "integer"
    },
    "midi_out_dropped": {
      "type": "integer"
    },
    "midi_output_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_output_latency": {
      "type": "array",
      "items": {
        "type": "integer"
      }
    },
    "midi_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_devices_changed": {
      "type": "boolean"
    },
    "midi_devices_message": {
      "type": "string"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    },
    "live_helpers": {
      "type": "integer"
    },
    "scheduling": {
      "type": "object",
      "title": "engine_scheduling",
      "properties": {
        "callback": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "live": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "renderer": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "background": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        }
      },
      "required": ["callback", "live", "renderer", "background"],
      "additionalProperties": false
    },
    "workgroup": {
      "type": "object",
      "title": "engine_workgroup",
      "properties": {
        "state": {
          "type": "string"
        },
        "joined": {
          "type": "integer"
        },
        "failed": {
          "type": "integer"
        },
        "os_error": {
          "type": "integer"
        },
        "message": {
          "type": "string"
        },
        "hint": {
          "type": "string"
        }
      },
      "required": ["state", "joined", "failed", "os_error", "message", "hint"],
      "additionalProperties": false
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
    "memory_lock": {
      "type": "object",
      "title": "runtime_protection_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active"],
      "additionalProperties": false
    },
    "cpu_idle_latency": {
      "type": "object",
      "title": "cpu_idle_latency_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "automatic": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active", "automatic"],
      "additionalProperties": false
    },
    "thp_opt_out": {
      "type": "object",
      "title": "thp_opt_out_status",
      "properties": {
        "active": {
          "type": "boolean"
        },
        "advised_only": {
          "type": "boolean"
        },
        "inherited": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            },
            "hint": {
              "type": "string"
            },
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["active", "advised_only", "inherited"],
      "additionalProperties": false
    },
    "audio_page_faults": {
      "type": "object",
      "title": "audio_page_faults",
      "properties": {
        "scope": {
          "type": "string",
          "pattern": "^(audio_threads|process|none)$"
        },
        "minor": {
          "type": "integer"
        },
        "major": {
          "type": "integer"
        },
        "threads": {
          "type": "integer"
        },
        "saturated": {
          "type": "boolean"
        }
      },
      "required": ["scope", "minor", "threads", "saturated"],
      "additionalProperties": false
    },
    "meter_drops": {
      "type": "object",
      "title": "meter_drops",
      "properties": {
        "renderer": {
          "type": "integer"
        },
        "callback": {
          "type": "integer"
        }
      },
      "required": ["renderer", "callback"],
      "additionalProperties": false
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "disabled_fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "fx whose plug-in this run has disabled (plugin.override): the strip passes audio through"
    },
    "bridge_threads_refused": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "modulation_deferred": {
      "type": "integer",
      "description": "Modulation events that waited past their grid point for room in a block (ADR-0096)"
    },
    "failed_errors": {},
    "sync": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    }
  },
  "required": [
    "running",
    "preparing_audio",
    "prepared_version",
    "preparation_version",
    "preparation_error",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "bridge_late",
    "bridge_timing",
    "midi_dropped",
    "midi_inputs",
    "midi_outputs",
    "midi_lead_frames",
    "midi_out_dropped",
    "midi_output_warnings",
    "midi_output_latency",
    "midi_warnings",
    "midi_devices_changed",
    "midi_devices_message",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses",
    "live_helpers",
    "scheduling",
    "workgroup",
    "warnings",
    "memory_lock",
    "cpu_idle_latency",
    "thp_opt_out",
    "audio_page_faults",
    "meter_drops",
    "failed_fx",
    "disabled_fx",
    "bridge_threads_refused",
    "dropped",
    "voice_dropped",
    "modulation_deferred",
    "failed_errors",
    "sync"
  ],
  "additionalProperties": false
}

```

### Examples

**Not running** (`empty`)

```json
{}

```

Result:

```json
{
  "running": false,
  "backend": "",
  "sample_rate": 0.0,
  "block": 0,
  "period": 0,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "bridge_late": 0,
  "bridge_timing": {
    "calls": 0,
    "late": 0,
    "settles": 0,
    "past_deadline": 0,
    "spun": 0,
    "wait_max_us": 0,
    "wait_mean_us": 0,
    "wake_max_us": 0,
    "wake_mean_us": 0,
    "work_max_us": 0,
    "return_max_us": 0,
    "wakes_over_200us": 0,
    "wakes_over_1ms": 0,
    "bridge_cpus": [],
    "migrations": 0,
    "class_mismatches": 0,
    "longest_cycle_us": 0,
    "overruns_late_wait": 0,
    "overruns_slow_bridge": 0,
    "overruns_not_waiting": 0
  },
  "midi_dropped": 0,
  "midi_warnings": [],
  "midi_out_dropped": 0,
  "midi_output_warnings": [],
  "midi_output_latency": [],
  "midi_inputs": [],
  "midi_outputs": [],
  "midi_lead_frames": 0,
  "midi_devices_changed": false,
  "midi_devices_message": "",
  "preparing_audio": false,
  "preparation_error": "",
  "prepared_version": 0,
  "preparation_version": 0,
  "cycles": 0,
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": false,
  "anticipation_misses": 0,
  "live_helpers": 0,
  "scheduling": {
    "callback": {
      "policy": "none",
      "priority": 0
    },
    "live": {
      "policy": "none",
      "priority": 0
    },
    "renderer": {
      "policy": "none",
      "priority": 0
    },
    "background": {
      "policy": "none",
      "priority": 0
    }
  },
  "workgroup": {
    "state": "none",
    "joined": 0,
    "failed": 0,
    "os_error": 0,
    "message": "",
    "hint": ""
  },
  "warnings": [],
  "memory_lock": {
    "requested": false,
    "active": false
  },
  "cpu_idle_latency": {
    "requested": false,
    "active": false,
    "automatic": false
  },
  "thp_opt_out": {
    "active": false,
    "advised_only": false,
    "inherited": false
  },
  "audio_page_faults": {
    "scope": "none",
    "minor": 0,
    "major": 0,
    "threads": 0,
    "saturated": false
  },
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "disabled_fx": [],
  "bridge_threads_refused": [],
  "dropped": 0,
  "voice_dropped": 0,
  "modulation_deferred": 0,
  "failed_errors": [],
  "sync": "<any>"
}

```

## `engine.stop@1`

Stop real-time playback and release the audio device.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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

**Stop** (`engine_running`)

```json
{}

```

Result:

```json
{}

```

## `env.add@1`

Automate a parameter: a track's or route's mixer control, or a plugin's.

Target one of track (param volume|pan|mute; width|pan_left|pan_right|pan_boost on channels/busses; playrate only on tr_0), route (volume|pan|mute), group (VCA volume|mute), or fx (a semantic key such as "gain" or "filter1.cutoff", or for other plugins a CLAP id or parameter name), or module (a rack module's knob by key, values in its unit, a choice by its key; module.describe). points: [{beat|bar|seconds, value, shape?, tension?}]; values in the parameter's unit (dB, %, Hz… — text like "800 Hz" works for plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values move along the parameter's curve (a log cutoff sweeps evenly in pitch). take (param volume|pan|mute): a take envelope, its points in beats from the item's start (it moves with the item), or on a clip's take from the clip's start in beats (MIDI) or seconds (audio); it shapes the take after its take FX (a MIDI take needs an instrument in its take FX). Master playrate points are finite numeric ratios 0.25..4.0; an active nonempty master curve replaces the saved scalar during playback and rendering. Audio sources convert before take FX/live mixing; MIDI and automation follow the same timeline clock. Width and dual pans use -100..100 percent; pan_boost uses 0..1 (on at 0.5), overriding the effective law's boost while the envelope is active and nonempty.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `module` | string |  |  |  | Rack module id, e.g. "mo_1". |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `group` | string |  |  |  | VCA group id, e.g. "gr_1". |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `route` | string |  |  |  | Route id, e.g. "sd_1". |
| `param` | string |  |  |  |  |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].bar` | integer |  | 1.0..2147483647.0 |  |  |
| `points[].seconds` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_add",
  "properties": {
    "env": {
      "type": "string"
    },
    "points": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["env", "points"],
  "additionalProperties": false
}

```

### Examples

**Fade a track out over bar 2** (`song`)

```json
{
  "track": "tr_1",
  "param": "volume",
  "points": [
    {
      "bar": 2,
      "value": 0
    },
    {
      "bar": 3,
      "value": "-60 dB"
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

**Sweep a plugin parameter** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain",
  "points": [
    {
      "beat": 0,
      "value": "-12 dB",
      "shape": "slow"
    },
    {
      "beat": 4,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

**Duck a take halfway through its item** (`song_with_audio`)

```json
{
  "take": "tk_1",
  "param": "volume",
  "points": [
    {
      "beat": 0,
      "value": 0
    },
    {
      "beat": 2,
      "value": "-12 dB"
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

**A rack module's knob: a VCA opening over a bar** (`song_with_rack`)

```json
{
  "module": "mo_3",
  "param": "level",
  "points": [
    {
      "beat": 0,
      "value": -40
    },
    {
      "beat": 4,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

**Master rate envelope** (`empty`)

```json
{
  "track": "tr_0",
  "param": "playrate",
  "points": [
    {
      "beat": 0,
      "value": 1
    },
    {
      "beat": 4,
      "value": 2
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

## `env.list@1`

List envelopes (optionally of one track, route or plugin).



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `object` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_list",
  "properties": {
    "envelopes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "env_entry",
        "properties": {
          "env": {
            "type": "string"
          },
          "target": {
            "type": "string"
          },
          "param": {
            "type": "string"
          },
          "points": {
            "type": "integer"
          },
          "active": {
            "type": "boolean"
          },
          "visible": {
            "type": "boolean"
          },
          "mode": {
            "type": "string"
          }
        },
        "required": ["env", "target", "param", "points", "active", "visible", "mode"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["envelopes"],
  "additionalProperties": false
}

```

### Examples

**All** (`song_with_env`)

```json
{}

```

Result:

```json
{
  "envelopes": [
    {
      "env": "en_1",
      "target": "fx_2",
      "param": "gain",
      "points": 2,
      "active": true,
      "visible": true,
      "mode": "read"
    }
  ]
}

```

## `env.points.set@1`

Replace an envelope's points (all of them, or those in a beat range).

from_beat/to_beat: replace only points with from_beat <= beat < to_beat. On a clip's take or take FX they are in the clip's unit from its start (beats, or seconds on an audio clip).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `points` | array |  |  |  |  |
| `points[].beat` | number |  | 0.0.. |  |  |
| `points[].bar` | integer |  | 1.0..2147483647.0 |  |  |
| `points[].seconds` | number |  | 0.0.. |  |  |
| `points[].value` | value |  |  |  |  |
| `points[].shape` | string |  |  |  | One of: linear, square, slow, fast_start, fast_end, bezier. |
| `points[].tension` | number |  | -1.0..1.0 |  |  |
| `from_beat` | number |  | 0.0.. |  |  |
| `to_beat` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_count",
  "properties": {
    "points": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["points"],
  "additionalProperties": false
}

```

### Examples

**Three points** (`song_with_env`)

```json
{
  "env": "en_1",
  "points": [
    {
      "beat": 0,
      "value": -6
    },
    {
      "beat": 2,
      "value": 0,
      "shape": "square"
    },
    {
      "beat": 3,
      "value": "-3 dB"
    }
  ]
}

```

Result:

```json
{
  "points": 3
}

```

## `env.remove@1`

Remove an envelope (the parameter returns to its set value).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |

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

**Remove** (`song_with_env`)

```json
{
  "env": "en_1"
}

```

Result:

```json
{}

```

## `env.set@1`

Set an envelope's active flag and mode (read, touch, latch, write).

visible shows or hides the envelope's lane in the arrange view (a hidden envelope still plays while active).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `active` | boolean |  |  |  |  |
| `visible` | boolean |  |  |  |  |
| `mode` | string |  |  |  | One of: trim, read, touch, latch, latch_preview, write. |

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

**Mute the automation** (`song_with_env`)

```json
{
  "env": "en_1",
  "active": false
}

```

Result:

```json
{}

```

## `env.write.commit@1`

Record what a write pass wrote into an envelope as a replayable edit.

Points with after < pos <= through go and the given points come in, in the envelope's own units (beats or seconds by its timebase, stored values).

Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `after` | number |  |  |  |  |
| `through` | number |  |  |  |  |
| `points` | array |  |  |  |  |
| `points[].pos` | number |  |  |  |  |
| `points[].value` | number |  |  |  |  |

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

## `fx.add@1`

Insert a plugin into a track's (or a take's) FX chain and return its id.

plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic plugins. chain: main (default; the track's effects) or input (the recorded-input chain). take instead of track: the take's own chain (take FX) — the item plays through it before the track's input, cut to the item; an instrument there plays a MIDI take on its own. An instrument with outputs past its main pair (a multi-out sampler's Kick, Snare...) gets that many more channels on the track, and its pins.out sends each output there; route.matrix lists them and route.add {channels: {from: [3, 4]}} sends one to a buss. Without index, a note effect (an arpeggiator) goes to the front of the track's chain, ahead of what it plays, and an instrument after the note effects there.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `plugin` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |
| `patch` | value |  |  |  |  |
| `chain` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_add",
  "properties": {
    "fx": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "semantic": {
      "type": "boolean"
    },
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["fx", "name", "semantic", "chain"],
  "additionalProperties": false
}

```

### Errors

- `plugin.unknown`: no such plugin Hint: closest ids in the hint; see plugin.list

### Examples

**An instrument on the bass track** (`song`)

```json
{
  "track": "tr_1",
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "Test Sine",
  "semantic": false,
  "chain": ["fx_1"]
}

```

**An effect in the track's input chain** (`song`)

```json
{
  "track": "tr_1",
  "plugin": "com.suppleaudio.test.gain",
  "chain": "input"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["fx_1"]
}

```

**Take FX: a gain on the take of an item** (`song_with_item`)

```json
{
  "take": "tk_1",
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["fx_1"]
}

```

## `fx.chain.delete@1`

Delete a saved FX chain preset by name.

Removes the preset file from the user's chains directory. Tracks that loaded it keep their effects.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_delete_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `chain.unknown`: no preset has that name Hint: use a name from fx.chain.list

### Examples

**Delete the bass chain** (`song_with_saved_chain`)

```json
{
  "name": "Bass"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `fx.chain.list@1`

List saved FX chain presets by name, path and effect count.

Lists valid supple.fx-chain/1.0 files, sorted by name and path. Invalid documents are skipped with warnings. See fx.chain.save and fx.chain.load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_list_result",
  "properties": {
    "chains": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "fx_chain_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          },
          "effects": {
            "type": "integer",
            "minimum": 0.0
          }
        },
        "required": ["name", "path", "effects"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["chains"],
  "additionalProperties": false
}

```

### Examples

**List presets** (`empty`)

```json
{}

```

Result:

```json
{
  "chains": []
}

```

## `fx.chain.load@1`

Append or replace a track's FX chain from a preset in one undo step.

Give exactly one of name (in the user's chains directory) or path (relative to the work directory). mode defaults to append; replace removes the target chain, its envelopes and key routes. Validates every plugin and state before changing the model.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `path` | string |  |  |  |  |
| `chain` | string |  |  | "main" |  |
| `mode` | string |  |  | "append" |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_load_result",
  "properties": {
    "effects": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["effects"],
  "additionalProperties": false
}

```

### Errors

- `chain.source`: name/path missing or both given Hint: give exactly one of name or path
- `chain.plugin_missing`: a saved plugin is not installed Hint: install the named plugin or choose another preset

### Examples

**Load a saved bass chain** (`song_with_saved_chain`)

```json
{
  "track": "tr_1",
  "name": "Bass",
  "mode": "replace"
}

```

Result:

```json
{
  "effects": ["<any>", "<any>"]
}

```

## `fx.chain.save@1`

Save an ordered FX chain as a named preset.

Save main (default) or input FX to the user's chains directory. Preserves semantic patches or opaque base64 state. Refuses empty chains; replace: true explicitly overwrites an existing name.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `chain` | string |  |  | "main" |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_save_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "effects": {
      "type": "integer",
      "minimum": 0.0
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "effects"],
  "additionalProperties": false
}

```

### Errors

- `chain.name`: invalid preset name Hint: use a name without path separators
- `chain.empty`: the chain has no effects Hint: add effects before saving
- `chain.exists`: the name already exists Hint: choose another name or set replace: true

### Examples

**Save the bass chain** (`song_with_chain`)

```json
{
  "track": "tr_1",
  "name": "Bass"
}

```

Result:

```json
{
  "path": "<any>",
  "effects": 2
}

```

## `fx.copy@1`

Copy a plugin and its current settings into a track's FX chain.

Adds a new instance of fx's plugin to track's main (default) or input chain, at index (default: the end), with the source's options (label, bypass, offline, wet, isolation, oversampling, parallel, pdc) and its settings as they are now: the running plugin's own state when the engine has one (editor edits included), else the model's patch or state. Not copied: envelopes, key routes into the source, and, when the track differs, channel pins. The source stays where it is; use fx.move to move it. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `track` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |
| `chain` | string |  |  | "main" |  |

### Result

```json
{
  "type": "object",
  "title": "fx_copy_result",
  "properties": {
    "fx": {
      "type": "string"
    },
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["fx", "chain"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown`: no such plugin instance Hint: use an fx id returned by fx.add, fx.copy or fx.chain.load
- `fx.index`: index is past the end of the chain Hint: use 0 to the chain's size

### Examples

**Duplicate a plugin at the end of its own chain** (`song_with_fx`)

```json
{
  "fx": "fx_1",
  "track": "tr_1"
}

```

Result:

```json
{
  "fx": "<any>",
  "chain": ["fx_1", "fx_2", "<any>"]
}

```

## `fx.describe@1`

Describe an instance and its live problems; full adds parameters and patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `full` | boolean |  |  | false |  |
| `filter` | string |  |  |  | Plain parameters only: list those whose name contains this text (case-insensitive). |

### Result

```json
{
  "type": "object",
  "title": "fx_description",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "description": {
      "type": "string"
    },
    "params": {
      "description": "With full: each parameter; modulatable_by_envelope: true when a route reaches it through its envelope (ADR-0096 M4); modulatable: true when a modulation route can take it (modulation.add)"
    },
    "patch": {},
    "status_available": {
      "type": "boolean",
      "description": "A live instance supplied its current problems"
    },
    "problems": {
      "description": "Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable"
    },
    "sidechain_channels": {
      "type": "integer",
      "description": "The plug-in's key input: route.add {fx} feeds it"
    },
    "latency_samples": {
      "type": "integer",
      "description": "The instance's reported latency, in samples"
    },
    "isolation_effective": {
      "type": "string",
      "description": "Where the instance runs: in_process, shared_bridge or dedicated; inherit resolves to the plug-in isolation preference (stock plug-ins in process), as the instance is made"
    },
    "override": {
      "type": "string",
      "description": "This run's plugin.override for the plug-in, when it is not normal: bridge (it runs on a dedicated bridge) or disable (it is not loaded: the strip passes audio through, a rack module is silent, and there is no isolation_effective or parameter section)"
    },
    "outputs_notes": {
      "type": "boolean",
      "description": "The loaded plug-in sends notes or MIDI: fx.set midi_output chooses replace or merge"
    },
    "params_total": {
      "type": "integer",
      "description": "Plain parameters matching; at most 512 are listed."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "name", "description", "status_available"],
  "additionalProperties": false
}

```

### Examples

**Describe** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "description": "<any>",
  "latency_samples": "<any>",
  "isolation_effective": "in_process",
  "status_available": false
}

```

## `fx.learn.list@1`

List the MIDI controller bindings of plugin parameters.

Every effect's, or one effect's (fx). control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_learn_list",
  "properties": {
    "learn": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["learn"],
  "additionalProperties": false
}

```

### Examples

**Every binding** (`song_with_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "fx": "fx_2",
      "param": "gain",
      "midi": {
        "kind": "cc",
        "channel": 1,
        "number": 21
      },
      "control": "CC 21 · ch 1"
    }
  ]
}

```

## `fx.learn.remove@1`

Remove a plugin parameter's MIDI controller binding.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  |  |

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

**Free the gain** (`song_with_learn`)

```json
{
  "fx": "fx_2",
  "param": "gain"
}

```

Result:

```json
{}

```

## `fx.learn.set@1`

Let a MIDI controller (CC, note, pitch bend) move a plugin parameter.

One binding per parameter (a new one replaces it). midi.device: a substring of the MIDI input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = +1), relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle (above 0 flips it). soft_takeover: an absolute control acts once it reaches the value. when_selected / when_focused / when_visible: only while the effect's track is selected, or its window or editor has the focus or shows (the GUI).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  | A semantic key, or a plain parameter's id or name (fx.describe) |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |
| `when_selected` | boolean |  |  |  |  |
| `when_focused` | boolean |  |  |  |  |
| `when_visible` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_learn_set_result",
  "properties": {
    "param": {
      "type": "string"
    },
    "control": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["param", "control"],
  "additionalProperties": false
}

```

### Examples

**Gain on CC 21** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain",
  "midi": {
    "kind": "cc",
    "channel": 1,
    "number": 21
  }
}

```

Result:

```json
{
  "param": "gain",
  "control": "CC 21 · ch 1"
}

```

## `fx.meters@1`

Read a plugin's meters and readouts, playing now or as a render ended.

The read-only parameters of a plugin: a semantic plugin's by patch key in physical units (an analyzer's loudness, a reverb's RT60, a gate's suggested threshold), any other plugin's by name as CLAP values. from live (the default): the playing instance's, which move while it processes (playback, monitoring); source none while no instance runs (start the engine). from render: as the last render.mix ended, which renders through instances of its own; source none until a render.mix has included this plugin.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `from` | string |  |  | "live" | live: the playing instance; render: the last render.mix One of: live, render. |

### Result

```json
{
  "type": "object",
  "title": "fx_meters",
  "properties": {
    "source": {
      "type": "string",
      "description": "live, render, or none (nothing to read yet)"
    },
    "meters": {
      "description": "Read-only values by patch key (physical) or name"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["source", "meters"],
  "additionalProperties": false
}

```

### Examples

**Read meters** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "source": "<any>",
  "meters": "<any>"
}

```

## `fx.move@1`

Move a plugin within a track chain or to another track.

Works on the track's chain, on its input chain (an input-chain effect stays on its track) and on a take's chain (take FX stay on their take: index only).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `index` | integer |  | 0.0.. |  |  |

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

**Move a gain first** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "index": 0
}

```

Result:

```json
{}

```

## `fx.param.get@1`

Read one saved plugin parameter by semantic key or numeric CLAP id.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `param` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_param_result",
  "properties": {
    "value": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["value"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain"
}

```

Result:

```json
{
  "value": "<any>"
}

```

## `fx.param.set@1`

Set semantic parameters in units, or plain plugin parameters by name/id.

Plain CLAP/VST3 values use the declared range or plugin text; read-only parameters refuse edits.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `values` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Set the gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "values": {
    "gain": -3
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.patch.get@1`

Read a semantic plugin's saved patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `changed_only` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `fx.patch.list@1`

List an instance's read-only factory patches, by name.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_list_result",
  "properties": {
    "patches": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patches"],
  "additionalProperties": false
}

```

### Examples

**List factory patches** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "patches": []
}

```

## `fx.patch.merge@1`

Merge a partial patch into a plugin instance; returns the effective change.

Semantic plugins only. Values in physical units: {"params": {"gain": "-6 dB"}}. changed holds only the members the merge changed: against the saved patch, or for an instance never saved against the plug-in's defaults (a value set to what it already is changes nothing).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Turn the gain down** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "params": {
      "gain": "-6 dB"
    }
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.patch.set@1`

Replace a semantic patch, or apply a factory patch by name.

Give exactly one of patch or name. Factory names come from fx.patch.list; omitted parameters take defaults.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `patch` | value |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Replace gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "schema": "supple.test-gain-patch/1.0",
    "params": {
      "gain": -6
    }
  }
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `fx.preset.delete@1`

Delete one of the user's presets for a plugin; factory presets stay.

Removes the user preset's file. Factory presets belong to the plugin and cannot be deleted. Instances that loaded the preset keep their settings.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_delete_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown_preset`: no user preset has that name Hint: choose a user preset from fx.preset.list
- `fx.preset_factory`: factory presets cannot be deleted Hint: delete only presets whose source is user

### Examples

**Delete a saved preset** (`song_with_preset`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `fx.preset.list@1`

List a plugin's presets: its factory ones, then the user's.

source factory: the plugin's own patches in its order (read-only; the same as fx.patch.list's names). source user: the user's saved presets for this plugin, sorted by name, with their file path. See fx.preset.load and fx.preset.save.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_list_result",
  "properties": {
    "presets": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "fx_preset_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "source": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["name", "source"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["presets"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown`: no such plugin instance Hint: use an fx id from fx.add

### Examples

**List presets** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "presets": []
}

```

## `fx.preset.load@1`

Apply a factory or user preset to a plugin instance in one undo step.

name from fx.preset.list. source (factory or user) is needed only when both have the name. A factory preset or a semantic plugin's user preset replaces the whole patch (omitted parameters take defaults); a plain plugin's user preset is its saved state. Options such as wet and bypass are not part of a preset.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_load_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "source"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown_preset`: no preset has that name Hint: choose a name from fx.preset.list
- `fx.preset_ambiguous`: a factory and a user preset share the name Hint: add source: factory or source: user
- `fx.preset_invalid`: the preset file is not valid for this plugin Hint: save the preset again

### Examples

**Load a saved preset** (`song_with_preset`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "name": "Quiet",
  "source": "user"
}

```

## `fx.preset.save@1`

Save a plugin instance's current settings as a named user preset.

Saves what the running plugin has now (editor edits included): the patch of a semantic plugin, else its state. The preset is the user's, for every instance of this plugin in any project. A factory preset's name cannot be taken; replace: true overwrites a user preset of that name.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_save_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `fx.preset_name`: invalid preset name Hint: use a name without path separators
- `fx.preset_factory`: a factory preset has the name Hint: choose another name; factory presets are read-only
- `fx.preset_exists`: a user preset has the name Hint: choose another name or set replace: true

### Examples

**Save the gain's settings** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `fx.remove@1`

Remove a plugin instance (and its envelopes).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

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

**Remove** (`song_with_fx`)

```json
{
  "fx": "fx_1"
}

```

Result:

```json
{}

```

## `fx.restart@1`

Re-create a plugin instance from its saved patch/state (after a crash).

engine.status lists failed_fx: isolated plugins whose process crashed or hung. The new instance starts from the model's patch or state.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

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

**Restart** (`song_with_fx`)

```json
{
  "fx": "fx_1"
}

```

Result:

```json
{}

```

## `fx.set@1`

Set instance options: bypass, offline, wet, label, isolation, pins, MIDI.

isolation: in_process, dedicated (the plugin runs in a process of its own: a crash silences only it), shared_bridge (one process for all such plugins), inherit (the project default: in_process). pins: which of the track's channels (1-based; track.set channels adds 3-128) the plugin reads ({in: [l, r]}), writes ({out: [l, r]}) and keys from ({key: [3, 4]}: a detector on 3/4, for a plugin with a sidechain input); {} restores 1/2 in and out. out past its first two entries: a track channel for each of the plugin's auxiliary output channels in order (0: unconnected); fx.add sets them for a multi-out instrument. midi_output, for a plug-in with note output (fx.describe outputs_notes): replace (default) gives the plug-ins after it, and the track's post-FX MIDI sends, only its output; merge adds its output to the MIDI that reached it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `bypass` | boolean |  |  |  |  |
| `offline` | boolean |  |  |  |  |
| `wet` | number or string | % | 0.0..100.0 |  | Quantity strings are range-checked by the server. |
| `label` | string |  |  |  |  |
| `isolation` | string |  |  |  | One of: inherit, in_process, shared_bridge, dedicated. |
| `pins` | object |  |  |  |  |
| `pins.in` | array |  |  | [] |  |
| `pins.out` | array |  |  | [] |  |
| `pins.key` | array |  |  | [] |  |
| `midi_output` | string |  |  |  | One of: replace, merge. |

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

**Half wet** (`song_with_fx`)

```json
{
  "fx": "fx_1",
  "wet": "50%"
}

```

Result:

```json
{}

```

**Key the plugin from channels 3/4** (`song_with_keymix_4ch`)

```json
{
  "fx": "fx_1",
  "pins": {
    "key": [3, 4]
  }
}

```

Result:

```json
{}

```

## `fx.state.capture@1`

Record a plug-in's own state change as a replayable edit.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plug-in id, e.g. "fx_1". |
| `state` | string |  |  |  | The state's blob hash, as the plug-in's state field holds it |

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

## `group.add@1`

Create a named track group with optional links and members.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `active` | boolean |  |  |  |  |
| `members` | array |  |  |  |  |
| `links` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_ref",
  "properties": {
    "group": {
      "type": "string",
      "pattern": "^gr_[0-9]+$"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["group"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Group the drums** (`song`)

```json
{
  "name": "Drums",
  "members": ["tr_1"]
}

```

Result:

```json
{
  "group": "gr_1"
}

```

## `group.describe@1`

Describe a track group's membership, links and VCA ancestry.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_entry",
  "properties": {
    "group": {
      "type": "string",
      "pattern": "^gr_[0-9]+$"
    },
    "state": {
      "type": "object",
      "title": "track_group",
      "properties": {
        "name": {
          "type": "string",
          "default": ""
        },
        "color": {
          "type": "string",
          "default": ""
        },
        "active": {
          "type": "boolean",
          "default": true
        },
        "members": {
          "type": "array",
          "items": {
            "type": "string",
            "pattern": "^[a-z]{2}_[0-9]+$"
          },
          "default": []
        },
        "links": {
          "type": "array",
          "items": {
            "type": "string",
            "pattern": "^(volume|pan|mute|solo|armed|monitor|selection|edit)$"
          },
          "default": ["volume", "mute", "solo"]
        },
        "vca": {
          "type": "object",
          "title": "vca",
          "properties": {
            "volume_db": {
              "type": "number",
              "minimum": -150.0,
              "maximum": 12.0,
              "default": 0.0
            },
            "mute": {
              "type": "boolean",
              "default": false
            },
            "solo": {
              "type": "boolean",
              "default": false
            },
            "followers": {
              "type": "array",
              "items": {
                "type": "string",
                "pattern": "^[a-z]{2}_[0-9]+$"
              },
              "default": []
            },
            "learn": {
              "type": "array",
              "items": {
                "type": "object",
                "title": "control_learn",
                "properties": {
                  "param": {
                    "type": "string"
                  },
                  "midi": {
                    "type": "object",
                    "title": "midi_control",
                    "properties": {
                      "kind": {
                        "type": "string",
                        "enum": ["cc", "note", "pitch_bend", "program"]
                      },
                      "channel": {
                        "type": "integer",
                        "minimum": 1.0,
                        "maximum": 16.0
                      },
                      "number": {
                        "type": "integer",
                        "minimum": 0.0,
                        "maximum": 127.0,
                        "default": 0
                      },
                      "device": {
                        "type": "string",
                        "default": ""
                      }
                    },
                    "required": ["kind", "channel"],
                    "additionalProperties": false
                  },
                  "mode": {
                    "type": "string",
                    "enum": ["absolute", "relative1", "relative2", "relative3", "toggle"],
                    "default": "absolute"
                  },
                  "soft_takeover": {
                    "type": "boolean",
                    "default": false
                  }
                },
                "required": ["param", "midi"],
                "additionalProperties": false
              },
              "default": []
            }
          },
          "additionalProperties": false
        }
      },
      "additionalProperties": false
    },
    "controllers": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["group", "state", "controllers"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Describe drums** (`song_with_group`)

```json
{
  "group": "gr_1"
}

```

Result:

```json
{
  "group": "gr_1",
  "state": "<any>",
  "controllers": []
}

```

## `group.list@1`

List track groups, linked attributes and optional VCA controls.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "group_list",
  "properties": {
    "groups": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "group_entry",
        "properties": {
          "group": {
            "type": "string",
            "pattern": "^gr_[0-9]+$"
          },
          "state": {
            "type": "object",
            "title": "track_group",
            "properties": {
              "name": {
                "type": "string",
                "default": ""
              },
              "color": {
                "type": "string",
                "default": ""
              },
              "active": {
                "type": "boolean",
                "default": true
              },
              "members": {
                "type": "array",
                "items": {
                  "type": "string",
                  "pattern": "^[a-z]{2}_[0-9]+$"
                },
                "default": []
              },
              "links": {
                "type": "array",
                "items": {
                  "type": "string",
                  "pattern": "^(volume|pan|mute|solo|armed|monitor|selection|edit)$"
                },
                "default": ["volume", "mute", "solo"]
              },
              "vca": {
                "type": "object",
                "title": "vca",
                "properties": {
                  "volume_db": {
                    "type": "number",
                    "minimum": -150.0,
                    "maximum": 12.0,
                    "default": 0.0
                  },
                  "mute": {
                    "type": "boolean",
                    "default": false
                  },
                  "solo": {
                    "type": "boolean",
                    "default": false
                  },
                  "followers": {
                    "type": "array",
                    "items": {
                      "type": "string",
                      "pattern": "^[a-z]{2}_[0-9]+$"
                    },
                    "default": []
                  },
                  "learn": {
                    "type": "array",
                    "items": {
                      "type": "object",
                      "title": "control_learn",
                      "properties": {
                        "param": {
                          "type": "string"
                        },
                        "midi": {
                          "type": "object",
                          "title": "midi_control",
                          "properties": {
                            "kind": {
                              "type": "string",
                              "enum": ["cc", "note", "pitch_bend", "program"]
                            },
                            "channel": {
                              "type": "integer",
                              "minimum": 1.0,
                              "maximum": 16.0
                            },
                            "number": {
                              "type": "integer",
                              "minimum": 0.0,
                              "maximum": 127.0,
                              "default": 0
                            },
                            "device": {
                              "type": "string",
                              "default": ""
                            }
                          },
                          "required": ["kind", "channel"],
                          "additionalProperties": false
                        },
                        "mode": {
                          "type": "string",
                          "enum": ["absolute", "relative1", "relative2", "relative3", "toggle"],
                          "default": "absolute"
                        },
                        "soft_takeover": {
                          "type": "boolean",
                          "default": false
                        }
                      },
                      "required": ["param", "midi"],
                      "additionalProperties": false
                    },
                    "default": []
                  }
                },
                "additionalProperties": false
              }
            },
            "additionalProperties": false
          },
          "controllers": {
            "type": "array",
            "items": {
              "type": "string",
              "pattern": "^[a-z]{2}_[0-9]+$"
            }
          }
        },
        "required": ["group", "state", "controllers"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["groups"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**List groups** (`empty`)

```json
{}

```

Result:

```json
{
  "groups": []
}

```

## `group.members.set@1`

Replace a group's track membership; an empty array clears it.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `members` | array |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Assign a track** (`song_with_group`)

```json
{
  "group": "gr_1",
  "members": ["tr_1"]
}

```

Result:

```json
{}

```

## `group.remove@1`

Remove a track group and its VCA, keeping its tracks.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Remove** (`song_with_vca`)

```json
{
  "group": "gr_1"
}

```

Result:

```json
{}

```

## `group.set@1`

Change a group's name, color, links or active state.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `active` | boolean |  |  |  |  |
| `links` | array |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Suspend linked edits** (`song_with_group`)

```json
{
  "group": "gr_1",
  "active": false
}

```

Result:

```json
{}

```

## `group.targets@1`

Resolve linked tracks or matching items for an edit or selection.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `attribute` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_targets",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "items": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks", "items"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Resolve volume links** (`song_with_group`)

```json
{
  "track": "tr_1",
  "attribute": "volume"
}

```

Result:

```json
{
  "tracks": ["tr_1"],
  "items": []
}

```

## `history.list@1`

List the current branch's recent versions (newest first).



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_list",
  "properties": {
    "versions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "history_entry",
        "properties": {
          "version": {
            "type": "integer"
          },
          "label": {
            "type": "string"
          },
          "origin": {
            "type": "string"
          },
          "squashed_steps": {
            "type": "integer"
          }
        },
        "required": ["version", "label", "origin", "squashed_steps"],
        "additionalProperties": false
      }
    },
    "can_undo": {
      "type": "boolean"
    },
    "can_redo": {
      "type": "boolean"
    },
    "truncated": {
      "type": "boolean"
    },
    "retained_bytes": {
      "type": "integer"
    },
    "budget_bytes": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["versions", "can_undo", "can_redo", "truncated", "retained_bytes", "budget_bytes"],
  "additionalProperties": false
}

```

### Examples

**List** (`song`)

```json
{}

```

Result:

```json
{
  "versions": "<any>",
  "can_undo": true,
  "can_redo": false,
  "truncated": false,
  "retained_bytes": "<any>",
  "budget_bytes": 1073741824
}

```

## `history.redo@1`

Redo the last undone change.

Steps the current branch forward one version. The result is the version that is current afterwards and its label: the step that was redone.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Redo** (`song_undone`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```

## `history.undo@1`

Undo the last change on the current branch.

Steps the current branch back one version. The result is the version that is current afterwards and its label: the step before the one undone, not the undone step (history.list shows both).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Undo** (`song`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```

## `item.add@1`

Add an item on a track: notation, a MIDI or audio file, or a clip's copy.

position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h" — every bar must add up; the item is as long as the written bars unless length (beats) is given. audio_file: a path at the project's sample rate; with the app's copy-imports setting on, the project keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, every track merged, channels kept); its notes keep their beats and follow the project's tempo (the file's tempo and meter are not applied; a warning says when they differ), and the item is as long as the file unless length is given. clip: a copy of that clip's takes, as long as the clip, its loop and take envelopes converted at the tempo where the item lands (ADR-0097); the item is named as the clip is (its active take's name) when the clip has a name; the notes are copied as they are, and context.mismatch warns when the clip's key is not the project's under the item (ADR-0098). A master holds audio items only (prints, played after its fader); busses hold none.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `position` | number |  | 0.0.. |  | Beats. |
| `bar` | integer |  | 1.0..2147483647.0 |  | Start at this bar (1-based). |
| `length` | number |  |  |  | Beats. |
| `notation` | string |  |  |  |  |
| `audio_file` | string |  |  |  |  |
| `midi_file` | string |  |  |  |  |
| `clip` | string |  |  |  | Lay a copy of this clip's takes here. |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_add",
  "properties": {
    "item": {
      "type": "string"
    },
    "take": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "position": {
      "type": "number"
    },
    "length": {
      "type": "number"
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
    }
  },
  "required": ["item", "take", "position", "length"],
  "additionalProperties": false
}

```

### Errors

- `notation.*`: the notation does not compile Hint: line:col and a hint in the error
- `tr.unknown`: no such track Hint: see track.list

### Examples

**A bass line at bar 1** (`song`)

```json
{
  "track": "tr_1",
  "bar": 1,
  "notation": "voice bass; 1: C2:q r:q C2:q r:q"
}

```

Result:

```json
{
  "item": "it_1",
  "take": "tk_1",
  "source": "mi_1",
  "position": 0.0,
  "length": 4.0
}

```

## `item.apply-fx@1`

Render an item through its take and track FX into a new audio take.

Renders the heard take alone (other items on the track silent) through its take FX and the track's main chain, before the fader and pan, over the item's span: MIDI becomes audio through the track's instrument. The file goes to the project's media folder. mode take: a new take, heard, beside the old one (MIDI stays a take). mode replace: the new take takes the old one's place. The item's fades and gain still apply when it plays; the track FX still process the result, so bypass or move them if they should not apply twice.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `mode` | string |  |  | "take" |  |

### Result

```json
{
  "type": "object",
  "title": "item_apply_fx_result",
  "properties": {
    "take": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["take", "source", "path"],
  "additionalProperties": false
}

```

### Examples

**MIDI through the instrument, as a new take** (`song_with_fx`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "take": "<any>",
  "source": "<any>",
  "path": "<any>"
}

```

## `item.consolidate@1`

Consolidate selected audio and MIDI per track in a background job.

Partitions by track and active source kind. Audio bakes active takes, item controls, warp and take FX/automation to float WAV; track processing remains live. MIDI items on a track become one item (each item's heard take, gaps empty, overlaps layered); a lone MIDI item with several MIDI takes layers them into one take; other lone MIDI stays unchanged. Select complete linked/warp bundles on matching lanes/groups. One undo step after successful job publication. No item-count cap; existing render/source sample budgets apply. job.get returns resulting items/files.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `item.consolidate_*`: invalid material, stale inputs or failed capture Hint: repair the selected sources/layout and retry consolidation
- `item.selection`: selection omits linked peers Hint: select the full group or suspend its edit link
- `warp.peers`: incomplete or invalid warp bundle Hint: select every bundle owner or detach it first

### Examples

**Consolidate the active audio take** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{
  "job": "<any>"
}

```

## `item.consolidate.commit@1`

Attach consolidated audio and merge MIDI as one replayable edit.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |
| `expected` | string |  |  |  |  |
| `media` | array |  |  |  |  |
| `media[].item` | string |  |  |  |  |
| `media[].source` | object |  |  |  |  |

### Result

```json
{}

```

### Examples

## `item.duplicate@1`

Copy an item and its takes to a new position (default: right after).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `to` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_duplicate_result",
  "properties": {
    "id": {
      "type": "string"
    },
    "linked": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "default": []
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

**Repeat** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "id": "it_2"
}

```

## `item.explode-takes@1`

Spread an item's takes onto new tracks, or in turn on its track.

In recording order. mode tracks: the first take stays on the item's track and each other take gets a new channel right below it (named after the take) at the same time. mode serial: the takes follow each other on the item's own track, each as long as the item. Every take becomes a one-take item with its FX and envelopes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `mode` | string |  |  | "tracks" |  |

### Result

```json
{
  "type": "object",
  "title": "item_explode_takes_result",
  "properties": {
    "items": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["items", "tracks"],
  "additionalProperties": false
}

```

### Examples

**Takes onto tracks** (`song_with_takes`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "items": ["it_1", "<any>", "<any>"],
  "tracks": ["<any>", "<any>"]
}

```

**Takes one after another** (`song_with_takes`)

```json
{
  "item": "it_1",
  "mode": "serial"
}

```

Result:

```json
{
  "items": ["it_1", "<any>", "<any>"],
  "tracks": []
}

```

## `item.export@1`

Write an item's raw audio (WAV) or its MIDI (SMF) to a file.

Exports the active take over the part the item covers (trims and splits respected). Audio: the source's own samples, rate and channels, from the source position at the item's start to the one at its end (playrate, unpreserved pitch and stretch markers decide that span but are not applied; a looping item ignores markers and repeats its source as heard; outside the source is silence). No gain, fades, pan, channel mode, FX or track processing. 16/24-bit PCM WAV sources keep their format; others are 32-bit float. MIDI: what the item plays (loops unrolled, notes ending by its end) from 0, at the source PPQ, with the tempo and meter at the item's start. Relative paths resolve against the work directory; a missing extension is added; existing files are replaced, the project's media never. A clip exports as the item it would make at the start: one period, looped as an item loops.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `path` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_export_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "format": {
      "type": "string"
    },
    "sample_format": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "channels": {
      "type": "integer"
    },
    "frames": {
      "type": "integer"
    },
    "notes": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "format"],
  "additionalProperties": false
}

```

### Errors

- `item.unknown`: no such item Hint: use an id from session.query {kind: "item"}
- `item.export_empty`: an empty take, or no source audio under the item Hint: pick a take with material (take.activate) or move the item onto its audio
- `item.export_source`: the source file cannot be read Hint: restore or relink the source
- `item.export_path`: no path, or it names the project's media Hint: choose another file
- `item.export_extension`: the extension does not match the item's kind Hint: use .wav for audio items and .mid for MIDI items
- `item.export_too_long`: the export exceeds the in-memory sample budget Hint: shorten the item

### Examples

**The right half of a split item: one second of its source** (`song_split_audio`)

```json
{
  "item": "it_2",
  "path": "right-half.wav"
}

```

Result:

```json
{
  "path": "<any>",
  "format": "wav",
  "sample_format": "float32",
  "sample_rate": 48000.0,
  "channels": 2,
  "frames": 48000
}

```

**A MIDI item as a Standard MIDI File** (`song_with_item`)

```json
{
  "item": "it_1",
  "path": "bass"
}

```

Result:

```json
{
  "path": "<any>",
  "format": "mid",
  "notes": 2
}

```

## `item.heal@1`

Heal compatible consecutive audio pieces without rendering.

One chain per track, complete linked/warp selection, matching stored audio/take/FX properties and continuous source maps. Accepts touching split pieces and unchanged linear Slice-at-hits overlaps up to 50 ms. Keep all left takes, outer fades and full warp maps; remove interior slice fades. Nonlinear envelope seams that cannot be recovered refuse. Stored FX states must match exactly; no live plugin queries or audio rendering. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `item.heal_*`: selection cannot be restored as a continuous source Hint: select unchanged split pieces or use item.consolidate instead
- `item.selection`: a linked or warp peer is unselected Hint: include every affected peer or suspend the edit link

### Examples

**Heal an audio split** (`song_split_audio`)

```json
{
  "items": ["it_1", "it_2"]
}

```

Result:

```json
{
  "items": ["it_1"]
}

```

## `item.mix-to-track@1`

Mix the selected items through their tracks onto a new muted channel.

Background job. Only the given items sound; each plays through its channel in full (take FX, item gain/fades, track FX, automation, fader, pan, Console 9 encode), and the sum is decoded by a neutral Console 9 Buss, as a fresh buss fed by those channels hears it. Sends, parent busses, mute switches and solo are ignored. Span: earliest item start to latest end, no tail. Result: a project-rate stereo float WAV in the media folder on a new muted channel after the last source (under their common parent, else the main master), one item at the span's start. Source items stay. One undo step; job.get returns track, item and file.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `item.mix_not_channel`: an item is not on a channel Hint: select items on channels only
- `item.mix_empty`: nothing selected plays material Hint: select items with audio or MIDI
- `track.frozen`: an item is on a frozen channel Hint: unfreeze the channel first
- `item.mix_*`: stale selection, failed render or media Hint: repair and retry the mix

### Examples

**Mix an audio item to a new channel** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{
  "job": "<any>"
}

```

## `item.mix-to-track.commit@1`

Put a finished item mix on a new muted channel as one replayable edit.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |
| `expected` | string |  |  |  |  |
| `start` | number |  |  |  |  |
| `end` | number |  |  |  |  |
| `media` | object |  |  |  |  |
| `media.path` | string |  |  |  |  |
| `media.hash` | string |  |  |  |  |
| `media.format` | string |  |  |  |  |
| `media.sample_rate` | number |  | 0.0.. |  |  |
| `media.channels` | integer |  | 0.0.. |  |  |
| `media.frames` | integer |  | 0.0.. |  |  |

### Result

```json
{}

```

### Examples

## `item.move@1`

Move an item in time (position in beats or bar) and/or to another track.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

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

**Two bars later** (`song_with_item`)

```json
{
  "item": "it_1",
  "bar": 3
}

```

Result:

```json
{}

```

## `item.paste@1`

Copy items as they were in a version to tracks and beats.

version: where the items are read (default: now) — a GUI copy or cut remembers the version, so a paste reproduces the items as copied even after they were removed or edited. items: [{item, track, position}] (beats; channels only). Takes, fades, take FX and MIDI are copied; an audio source the project no longer has is restored.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `version` | integer |  | 1.0.. |  |  |
| `items` | array |  |  |  |  |
| `items[].item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `items[].track` | string |  |  |  | Track id, e.g. "tr_1". |
| `items[].position` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_paste_result",
  "properties": {
    "items": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["items"],
  "additionalProperties": false
}

```

### Examples

**Paste after itself** (`song_with_item`)

```json
{
  "items": [
    {
      "item": "it_1",
      "track": "tr_1",
      "position": 16
    }
  ]
}

```

Result:

```json
{
  "items": ["it_2"]
}

```

## `item.remove@1`

Remove an item with its takes (unpooled sources go too).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |

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

**Remove** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{}

```

## `item.set@1`

Set an item's position, length, playback, fades, name, color, or lock.

Positions and lengths are in beats; fades are in seconds. Name is the active take's name. A locked item refuses every edit except {"locked": false}. timebase: what the item follows when the tempo changes — beats (moves and stretches with it), time (stays at its seconds), beats_position (its start follows the beat, its length stays in seconds) or track (inherit); the item keeps its place now.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `length` | number |  | 0.0.. |  |  |
| `mute` | boolean |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `locked` | boolean |  |  |  | The arrange view marks locked items. |
| `gain_db` | number or string | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `fade_in` | number |  | 0.0.. |  |  |
| `fade_in_shape` | string |  |  |  | One of: linear, slow, fast, slow_fast, fast_slow, cos, exp. |
| `fade_out_shape` | string |  |  |  | One of: linear, slow, fast, slow_fast, fast_slow, cos, exp. |
| `fade_out` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `timebase` | string |  |  |  | One of: track, time, beats, beats_position. |

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

**Lower an item** (`song_with_item`)

```json
{
  "item": "it_1",
  "gain_db": "-3 dB"
}

```

Result:

```json
{}

```

**Keep an item at its time when the tempo changes** (`song_with_item`)

```json
{
  "item": "it_1",
  "timebase": "time"
}

```

Result:

```json
{}

```

## `item.split@1`

Split an item at a beat, optionally at the prior audio zero crossing.

snap defaults to none. zero_crossing searches the active take's prepared playback source up to 100 ms before at, before FX/fades/gain. After channel mode, use the louder stereo channel by window energy (left on ties), never a stereo sum unless MonoMix is selected. Choose the latest exact zero or sign-change sample boundary; never search forward or outside the item. Without a crossing, split at at. MIDI and empty takes refuse zero_crossing; the GUI skips them. Current prepared audio must be available; start the engine and retry after preparation. Linked items use the initiating item's resolved split time. History records that time for replay.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `at` | number |  | 0.0.. |  |  |
| `snap` | string |  |  | "none" | One of: none, zero_crossing. |

### Result

```json
{
  "type": "object",
  "title": "item_split_result",
  "properties": {
    "left": {
      "type": "string"
    },
    "right": {
      "type": "string"
    },
    "linked_left": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "default": []
    },
    "linked_right": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "default": []
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["left", "right"],
  "additionalProperties": false
}

```

### Errors

- `item.split_range`: the cursor is outside the item's interior Hint: choose a beat strictly after the start and before the end
- `item.split_not_audio`: zero_crossing needs an active audio take Hint: use snap none to split MIDI or empty items at the cursor
- `item.split_source_pending`: current source audio is not prepared Hint: start the engine, wait for source preparation and retry
- `item.split_source`: the audio source could not be prepared Hint: relink or repair the audio source and retry

### Examples

**Split halfway** (`song_with_item`)

```json
{
  "item": "it_1",
  "at": 2
}

```

Result:

```json
{
  "left": "it_1",
  "right": "it_2"
}

```

**Split at a prior audio zero crossing** (`song_with_prepared_audio`)

```json
{
  "item": "it_1",
  "at": 2,
  "snap": "zero_crossing"
}

```

Result:

```json
{
  "left": "it_1",
  "right": "it_2"
}

```

## `job.cancel@1`

Cancel a job.

Cancel a job.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

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

**Cancel job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "cancelled",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.get@1`

Get a job's status and progress.

Get a job's status and progress.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

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

**Get render job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.list@1`

List session jobs.

List session jobs.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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

**List jobs** (`empty`)

```json
{}

```

Result:

```json
{
  "jobs": []
}

```

## `job.status@1`

Get a job's status.

Get a job's status.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

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

**Get job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.wait@1`

Wait for a job.

Wait for a job. Blocks for up to timeout_s seconds (default 30) until the job finishes; a larger timeout_s is accepted and clamped to 30, and the result then carries "timeout_clamped_s": 30. Otherwise the result is what job.status returns.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |
| `timeout_s` | number |  | 0.0.. |  | Seconds to block (default 30). A larger value is clamped to 30 and reported as timeout_clamped_s. |

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

**Wait for job** (`job_running`)

```json
{
  "job": "jb_1",
  "timeout_s": 0
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

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

## `launch.arrangement@1`

Return tracks to the arrangement at the next boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. Returning does not chase notes: an arrangement note already under way stays silent until its next start. Without track: every track not following the arrangement, or queued to leave it. A frozen track already follows the arrangement: nothing changes.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: a master playrate is active Hint: set the master playrate to 1 and turn off its envelope
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating Hint: launch again in a moment

### Examples

**Nothing left the arrangement** (`song_with_clip_running`)

```json
{}

```

Result:

```json
{
  "action": "arrangement",
  "tracks": [],
  "started": false
}

```

## `launch.capture@1`

Start or end capturing launches for the timeline.

While on, every applied launch is kept with the running time it applied at, and what each track played when the capture began. on: true begins at the position heard now (stopped: where playing starts); on: false ends it there. A stop, a seek or a loop change ends a capture that has run; one that has not run begins again at the new position. launch.commit writes the capture onto the timeline. from and to: its span on the timeline in beats, laid out by running time.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `on` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "launch_capture",
  "properties": {
    "on": {
      "type": "boolean"
    },
    "from": {
      "type": "number"
    },
    "to": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["on"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "on": true
}

```

Result:

```json
{
  "on": true,
  "from": 0,
  "to": 0
}

```

## `launch.clip@1`

Play a clip on its track from the next quantize boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. Launching while stopped starts the transport. The track leaves the arrangement (or the clip it played) there: its sounding notes end and audio crossfades over 5 ms.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: a master playrate is active Hint: set the master playrate to 1 and turn off its envelope
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating Hint: launch again in a moment
- `track.frozen`: the clip's track is frozen Hint: track.unfreeze first
- `launch.memory`: the clip's audio did not fit the budget Hint: use shorter audio or fewer files in clips

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "clip": "cl_1"
}

```

Result:

```json
{
  "action": "clip",
  "tracks": [
    {
      "track": "tr_1",
      "clip": "cl_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": true
}

```

## `launch.commit@1`

Write the captured performance onto the timeline as items.

One undo step. The capture (launch.capture) is laid out linearly from where it began, by running time, whatever the transport loop did: a launch heard s seconds into the capture lands s seconds after its start. On each track that left the arrangement, its clip, stopped and held stretches replace the arrangement there (its items are split, trimmed or removed); each clip becomes copies of its takes at the phase it was heard, its MIDI copied unless pooled, and the 5 ms switch crossfades become item fades. Tracks that followed the arrangement are untouched, and so is the arrangement a loop repeated. A scene launched whole (launch.scene: every clip that could play started on one boundary) also writes a section named after it, from that boundary until one of its tracks is launched again or the capture ends, as scene.insert does; clips launched singly write none. A launched clip is not followed (until ADR-0098's X4): its items copy what was heard, and context.mismatch warns for each whose clip's key is not the project's under it. A running capture ends first; the capture stays, so committing again writes it again over itself. performance is what history records: leave it out and launch.commit fills it from the capture.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `pooled` | boolean |  |  | false | Share the clips' MIDI with the items (pooled) instead of copying it |
| `performance` | object |  |  |  | What history records; launch.commit fills it from the capture |
| `performance.sample_rate` | number |  | ..1536000.0 |  |  |
| `performance.start` | integer |  | 0.0..140737488355328.0 |  |  |
| `performance.frames` | integer |  | 0.0..140737488355328.0 |  |  |
| `performance.loop` | object |  |  |  |  |
| `performance.launches` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "launch_commit",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "items": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "sections": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "committed_section",
        "properties": {
          "scene": {
            "type": "string"
          },
          "region": {
            "type": "string"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          }
        },
        "required": ["scene", "region", "start", "end"],
        "additionalProperties": false
      }
    },
    "from": {
      "type": "number"
    },
    "to": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks", "items", "sections", "from", "to"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.capture`: nothing has been captured Hint: launch.capture {on: true}, play and launch clips, then launch.commit
- `launch.commit`: a captured track or clip is gone Hint: undo the change, or capture the performance again
- `item.locked`: a locked item is in the way Hint: unlock it with item.set first
- `clip.unsupported`: a clip's loop window has no item equivalent Hint: set its loop to the whole content, or off

### Examples

**A captured bar of cl_1** (`song_with_clip`)

```json
{
  "performance": {
    "sample_rate": 48000,
    "start": 0,
    "frames": 96000,
    "launches": [
      {
        "frame": 0,
        "track": "tr_1",
        "action": "clip",
        "clip": "cl_1",
        "scene": "sc_1",
        "anchor": 0,
        "anchor_beat": 0
      }
    ]
  }
}

```

Result:

```json
{
  "tracks": ["tr_1"],
  "items": ["<any>"],
  "sections": [
    {
      "scene": "sc_1",
      "region": "<any>",
      "start": 0,
      "end": 4
    }
  ],
  "from": 0,
  "to": 4
}

```

## `launch.scene@1`

Play every clip of a scene together at one boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. All the scene's tracks switch at the same boundary, the project's launch_quantize. Tracks with no clip in the scene keep playing what they play. A frozen track is skipped with a track.frozen warning, a clip past the memory budget with a launch.memory warning.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: a master playrate is active Hint: set the master playrate to 1 and turn off its envelope
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating Hint: launch again in a moment
- `launch.scene`: the scene has no clip that can play Hint: add clips to the scene, or unfreeze their tracks

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "scene": "sc_1"
}

```

Result:

```json
{
  "action": "clip",
  "tracks": [
    {
      "track": "tr_1",
      "clip": "cl_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": true
}

```

## `launch.state@1`

What each track with clips plays, and what it has queued.

plays is arrangement, clip, held (a once clip past its length, silent) or stopped, as heard. position is the playing clip's place in its period, in beats for a MIDI clip and seconds for an audio clip. queued is a launch not heard yet, with the song position it applies at. Tracks without clips always follow the arrangement and are not listed. capturing: launch.capture is on.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "launch_state",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "capturing": {
      "type": "boolean"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_track_state",
        "properties": {
          "track": {
            "type": "string"
          },
          "plays": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          },
          "position": {
            "type": "number"
          },
          "queued": {
            "type": "object",
            "title": "launch_queued",
            "properties": {
              "action": {
                "type": "string"
              },
              "clip": {
                "type": "string"
              },
              "beat": {
                "type": "number"
              },
              "seconds": {
                "type": "number"
              }
            },
            "required": ["action", "beat", "seconds"],
            "additionalProperties": false
          }
        },
        "required": ["track", "plays"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "capturing", "tracks"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**Before any launch** (`song_with_clip_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "capturing": false,
  "tracks": [
    {
      "track": "tr_1",
      "plays": "arrangement"
    }
  ]
}

```

## `launch.stop@1`

Stop a track's clip, or all clips, at the next boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. A stopped track is silent until a clip is launched or it returns to the arrangement. Without track: every track a clip holds or is queued to hold. A frozen track already follows the arrangement: nothing changes.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: a master playrate is active Hint: set the master playrate to 1 and turn off its envelope
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating Hint: launch again in a moment
- `launch.track`: the track has no clips in the running plan Hint: mute the track to silence it

### Examples

**One track, while stopped** (`song_with_clip_running`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{
  "action": "stop",
  "tracks": [
    {
      "track": "tr_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": false
}

```

## `log.list@1`

Recent commands with origin, arguments and outcome (the command log).

Newest last. Who did what: origin is gui, agent:<client>/<model>, mcp:<client>, lua:<script>, rpc. queries: include read-only commands (left out by default). since: only entries after this seq (poll with the last seq seen). origin: entries whose origin starts with this. The log keeps the last 2000 commands.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `limit` | integer |  | 1.0..2000.0 |  |  |
| `since` | integer |  | 0.0.. |  |  |
| `queries` | boolean |  |  | false |  |
| `origin` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**The last commands** (`song`)

```json
{
  "limit": 5
}

```

Result:

```json
{
  "entries": "<any>",
  "last_seq": "<any>"
}

```

## `marker.add@1`

Add a named marker in beats.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `pos` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Add** (`empty`)

```json
{
  "pos": 4,
  "name": "Verse"
}

```

Result:

```json
{
  "id": "mk_1"
}

```

## `marker.list@1`

List rulers in this project.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "ruler_list",
  "properties": {
    "markers": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "ruler_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "timebase": {
            "type": "string",
            "enum": ["time", "beats", "beats_position"]
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "pos": {
            "type": "number"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          },
          "section": {
            "type": "boolean"
          }
        },
        "required": ["id", "timebase", "name", "color"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["markers"],
  "additionalProperties": false
}

```

### Examples

**List empty** (`empty`)

```json
{}

```

Result:

```json
{
  "markers": []
}

```

## `marker.remove@1`

Remove a ruler.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Marker id, e.g. "mk_1". |

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

**Remove** (`song_with_marker`)

```json
{
  "id": "mk_1"
}

```

Result:

```json
{}

```

## `marker.set@1`

Edit a ruler's position, name, or color.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Marker id, e.g. "mk_1". |
| `pos` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Rename** (`song_with_marker`)

```json
{
  "id": "mk_1",
  "name": "Bridge"
}

```

Result:

```json
{}

```

## `midi.edit@1`

Edit a MIDI item's notes and CCs: move, resize, quantize, fit to key or chord.

ops run in order and are one undo step. notes: indices into midi.notes' list as it was before this command (every op refers to that list, so later ops are not affected by earlier removals). Beats are from the item's start. add {pitch, start, length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid), strength? 0-1, ends?}; cc.set {number, points [{beat, value}], channel?}; cc.clear {number, from?, to?}. Scale ops read the context track (key.set, chord.set) at the project beat each note starts, in the order of the ops: transpose_degrees {notes, degrees, key?} moves notes by scale degrees (a pitch outside the scale first goes down to the scale); snap_to_scale {notes? (all), mode? nearest|down|up, key?} moves notes outside the scale to it (nearest: a tie goes down); fit_to_chord {notes? (all), chord?} moves each note to the nearest tone of the chord (a tie goes down; a slash bass counts). key is "Eb major", chord is "Cm7"; given, they replace the context. A note that is looped or shared by another item in a different key or chord is refused with midi.context_varies; none holding is midi.no_key or midi.no_chord. On a clip they read the clip's own context (the key and chords it was written against, clip.set {context}) at the clip beat each note sounds, never the project's. Returns the new indices of the notes the ops added or changed (to keep a selection).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `ops` | array |  |  |  |  |
| `ops[].op` | string |  |  |  | One of: add, remove, move, resize, velocity, quantize, cc.set, cc.clear, transpose_degrees, snap_to_scale, fit_to_chord. |
| `ops[].notes` | array |  |  |  |  |
| `ops[].pitch` | integer |  | 0.0..127.0 |  |  |
| `ops[].start` | number |  |  |  |  |
| `ops[].length` | number |  |  |  |  |
| `ops[].velocity` | integer |  | 1.0..127.0 |  |  |
| `ops[].channel` | integer |  | 1.0..16.0 |  |  |
| `ops[].beats` | number |  |  |  |  |
| `ops[].semitones` | integer |  | -127.0..127.0 |  |  |
| `ops[].length_delta` | number |  |  |  |  |
| `ops[].start_delta` | number |  |  |  |  |
| `ops[].velocity_delta` | integer |  | -127.0..127.0 |  |  |
| `ops[].grid` | number |  |  |  |  |
| `ops[].strength` | number |  | 0.0..1.0 |  |  |
| `ops[].ends` | boolean |  |  |  |  |
| `ops[].number` | integer |  | 0.0..127.0 |  |  |
| `ops[].points` | array |  |  |  |  |
| `ops[].from` | number |  |  |  |  |
| `ops[].to` | number |  |  |  |  |
| `ops[].degrees` | integer |  | -127.0..127.0 |  |  |
| `ops[].mode` | string |  |  |  | One of: nearest, down, up. |
| `ops[].key` | string |  |  |  |  |
| `ops[].chord` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Add a note and move the first one up an octave** (`song_with_item`)

```json
{
  "item": "it_1",
  "ops": [
    {
      "op": "add",
      "pitch": 64,
      "start": 0.5,
      "length": 0.5
    },
    {
      "op": "move",
      "notes": [0],
      "semitones": 12
    }
  ]
}

```

Result:

```json
{
  "notes": "<any>"
}

```

**Move the first note up two degrees of C major** (`song_with_item`)

```json
{
  "item": "it_1",
  "ops": [
    {
      "op": "transpose_degrees",
      "notes": [0],
      "degrees": 2,
      "key": "C major"
    }
  ]
}

```

Result:

```json
{
  "notes": "<any>"
}

```

## `midi.get@1`

Read an item's or clip's MIDI as notation, with its key and chords.

Pitch names follow the key that holds at the item's start (none: the default spelling). context lists the key and the chords over the item's span, each from the bar of the item (item_bar, the notation's) and of the project (project_bar) where it starts; the first of each is the one already holding at the item's start. Omitted when the project has no key or chords there. A clip has no place in time: its pitch names follow its own key at its start, and its context is the key and chords it was written against: runs by clip bar (item_bar, no project_bar) and beat from its start, and text, its lead sheet in clip bars (clip.set {context} takes it back). follow (none, key, chords) is a clip's.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |

### Result

```json
{
  "type": "object",
  "title": "midi_get",
  "properties": {
    "text": {
      "type": "string"
    },
    "context": {
      "type": "object",
      "title": "midi_context",
      "properties": {
        "keys": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "context_run",
            "properties": {
              "item_bar": {
                "type": "integer"
              },
              "project_bar": {
                "type": "integer"
              },
              "beat": {
                "type": "number"
              },
              "what": {
                "type": "string"
              }
            },
            "required": ["item_bar", "what"],
            "additionalProperties": false
          }
        },
        "chords": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "context_run",
            "properties": {
              "item_bar": {
                "type": "integer"
              },
              "project_bar": {
                "type": "integer"
              },
              "beat": {
                "type": "number"
              },
              "what": {
                "type": "string"
              }
            },
            "required": ["item_bar", "what"],
            "additionalProperties": false
          }
        },
        "text": {
          "type": "string"
        }
      },
      "required": ["keys", "chords"],
      "additionalProperties": false
    },
    "follow": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["text"],
  "additionalProperties": false
}

```

### Examples

**Read the bass line** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "text": "<any>"
}

```

## `midi.inputs.get@1`

Hardware MIDI inputs, timing, settings, loss and startup warnings.

ports contains name, zero-based index (-1 absent), enabled, offset_ms, latency_ms, dropped and structured warnings. A disabled input feeds nothing, the sync chase included; offset_ms moves played and recorded MIDI, not the chase (sync.set has its own offset). Settings are machine preferences keyed by exact port name; they are not project state.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**No devices** (`empty`)

```json
{}

```

Result:

```json
{
  "ports": []
}

```

## `midi.inputs.set@1`

Change one machine MIDI port's enabled state and timing offset.

port is a unique case-insensitive match to a present or recorded name; settings use its exact spelling. Omitted members keep their values; a record back at every default is removed. offset_ms is signed -1000..1000 milliseconds. Positive offsets delay. Inputs retain the existing channel input mechanism. Machine settings: not an undo step, not saved in the project. Returns the updated ports list.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `port` | string |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `offset_ms` | number |  | -1000.0..1000.0 |  |  |

### Result

```json
{}

```

### Examples

**Disable a recorded port** (`midi_ports`)

```json
{
  "port": "Test port",
  "enabled": false
}

```

Result:

```json
{
  "ports": "<any>"
}

```

## `midi.notes@1`

List a MIDI item's or clip's notes and CC points (beats from its start).

Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their position in this list. start/length/beat are in beats from the item's start; channel is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them too. For writing whole phrases, midi.write with notation is usually better.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |

### Result

```json
{}

```

### Examples

**Notes of a MIDI item** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "item": "it_1",
  "ppq": 960,
  "length": "<any>",
  "position": "<any>",
  "notes": "<any>",
  "cc": "<any>"
}

```

## `midi.outputs.get@1`

Hardware MIDI outputs: settings, clock/MTC, timing, loss, warnings.

ports contains name, zero-based index (-1 absent), enabled, offset_ms, reset_on_stop, latency_ms (audio minus MIDI), dropped (routed MIDI, clock and MTC, and device loss) and structured warnings. lead_ms is the shared lead added to all audio, including monitoring. Clock output (ADR-0073): clock, transport, spp, mtc and clock_while_stopped as set; sends, what goes out now; suppressed, what is set but not sent back to the sync master's device; delay_ms (plan latency + latency + offset) and sent, the clock and MTC messages since the engine started. Settings are machine preferences keyed by exact port name; they are not project state.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**No devices** (`empty`)

```json
{}

```

Result:

```json
{
  "ports": []
}

```

## `midi.outputs.set@1`

Change one MIDI output: enabled, offset, reset, clock, SPP, MTC.

port is a unique case-insensitive match to a present or recorded name; settings use its exact spelling. Omitted members keep their values; a record back at every default is removed. offset_ms is signed -1000..1000 milliseconds. Positive offsets delay. reset_on_stop defaults true. clock sends MIDI clock (24 per quarter note) while playing; transport Start, Continue and Stop; spp Song Position before Continue and on locates; mtc MIDI time code at the project's timecode; clock_while_stopped keeps clocking at the tempo while stopped. Clock and MTC leave with the audio they belong to, after the plan's latency, the output's latency and offset_ms; nothing is sent during a count-in. Applying settings releases disabled/changed output routes. Machine settings: not an undo step, not saved in the project. Returns the updated ports list.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `port` | string |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `offset_ms` | number |  | -1000.0..1000.0 |  |  |
| `reset_on_stop` | boolean |  |  |  |  |
| `clock` | boolean |  |  |  |  |
| `transport` | boolean |  |  |  |  |
| `spp` | boolean |  |  |  |  |
| `mtc` | boolean |  |  |  |  |
| `clock_while_stopped` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**Disable a recorded port** (`midi_ports`)

```json
{
  "port": "Test port",
  "enabled": false
}

```

Result:

```json
{
  "ports": "<any>"
}

```

## `midi.write@1`

Replace an item's or clip's MIDI with notation (keeping its place and length).

The item or clip keeps its length: notation longer than it is stored but does not play past its end, and midi.beyond_length warns. Lengthen it first (item.set length, clip.set length), or write the notation to fit.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `notation` | string |  |  |  |  |

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

**Rewrite the bass line** (`song_with_item`)

```json
{
  "item": "it_1",
  "notation": "voice bass; 1: C2:e C2:e r:q G1:h"
}

```

Result:

```json
{}

```

## `modulate@1`

Modulate a parameter or mixer level: LFO, follower, random, macro.

The whole gesture in one undo step. It takes the first rack homed on the target's track that holds a Mod Out, else the first rack named Modulators, else adds one on a new channel named Modulators (rack: choose one instead). It adds the source module (lfo: an LFO; random: an LFO's Random Steps preset; macro: a Macro, its first knob; follower: a Follower fed by an In module and a send from the track from), cables it into the first free Mod Out input (adding a Mod Out when none is free) and routes that input to the target. {module, input} routes a Mod Out input already patched. For a macro, knob names the knob to move. depth as modulation.add; default a quarter of a plug-in parameter's range, 6 dB, or 50 percent. A parameter reached through its envelope (fx.describe: modulatable_by_envelope) gets a one-point envelope at its current value first when it has none. A rack that would close a loop (it already drives the track a follower hears, or hears the target) is passed over for the next, or a new Modulators rack. Only a real cycle is refused (modulation.cycle): a follower on a track the target feeds, itself included. target: {fx, param} (a plug-in parameter by key, name or CLAP id; fx.describe full marks the modulatable ones), {track, param: volume|pan|width} or {route, param: volume}. The source module is left set to its defaults: change it with module.set {module, params} on the module this returns (an LFO's rate, shape or sync; a follower's gain and threshold; a macro's macro_1, which sits at 0 until moved, so a macro does nothing at first; module.describe {type} lists the knobs) and the route's depth with modulation.set.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `source` | string or object |  |  |  |  |
| `source.module` | string |  |  |  |  |
| `source.input` | string |  |  |  |  |
| `depth` | number |  |  |  |  |
| `from` | string |  |  |  | The track a follower listens to. |
| `rack` | string |  |  |  | Rack id, e.g. "rk_1". |

### Result

```json
{
  "type": "object",
  "title": "modulated",
  "properties": {
    "modulation": {
      "type": "string"
    },
    "rack": {
      "type": "string"
    },
    "module": {
      "type": "string"
    },
    "mod_out": {
      "type": "string"
    },
    "input": {
      "type": "string"
    },
    "send": {
      "type": "string"
    },
    "channel": {
      "type": "string",
      "description": "The channel made for a new rack."
    },
    "knob": {
      "type": "string",
      "description": "A macro's knob to move, by key."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["modulation"],
  "additionalProperties": false
}

```

### Errors

- `modulation.cycle`: a follower on a track the target feeds Hint: listen to another track, or modulate a buss it feeds

### Examples

**An LFO on a gain** (`song_with_mod_out`)

```json
{
  "target": {
    "fx": "fx_1",
    "param": "gain"
  },
  "source": "lfo"
}

```

Result:

```json
{
  "modulation": "mr_1",
  "rack": "rk_1",
  "module": "mo_3",
  "mod_out": "mo_2",
  "input": "in_1"
}

```

## `modulation.add@1`

Route a rack's Mod Out to a plug-in parameter or mixer level.

depth is the offset at a signal of 1: for a plug-in parameter a fraction of its CLAP range (-1..1). A parameter fx.describe marks modulatable takes the offset directly; one marked modulatable_by_envelope (automatable, continuous, not modulatable, or any VST3's) takes it through its active envelope, which must exist (modulation.envelope; modulate adds one), and for a VST3 depth is a fraction of its normalized range; for a track's or send's volume in dB (-48..48); for a track's pan or width in percent (-200..200). The offset adds to the value automation or the fader gives, and the sum stays in the target's range.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | object |  |  |  |  |
| `source.module` | string |  |  |  | Mod Out module id, e.g. "mo_1". |
| `source.input` | string |  |  |  | One of the Mod Out's inputs, in_1 .. in_16. |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `depth` | number |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `lane` | boolean |  |  |  | Show the target's automation lane in the timeline while the route exists (default true). It never changes the sound. |

### Result

```json
{
  "type": "object",
  "title": "modulation_added",
  "properties": {
    "modulation": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["modulation"],
  "additionalProperties": false
}

```

### Examples

**Add** (`song_with_mod_out`)

```json
{
  "source": {
    "module": "mo_2",
    "input": "in_1"
  },
  "target": {
    "fx": "fx_1",
    "param": "gain"
  },
  "depth": 0.25
}

```

Result:

```json
{
  "modulation": "mr_1"
}

```

## `modulation.list@1`

List modulation routes by target or source rack.

Each route with its source signal's range (bipolar -1..1, or unipolar 0..1 when every cable into the Mod Out input is unipolar, a gate or a phase), fed_by (each cable into that input: its module, output and module type) and, when the plan cannot apply it, inert: source (nothing is cabled into the input), target (the parameter cannot take it, or the target's track is frozen and its level is outside the print), envelope (a parameter reached through its envelope, which is gone or off) or latency (its plug-in changed latency while modulated; editing a route into it turns them back on).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Target track id, e.g. "tr_1". |
| `fx` | string |  |  |  | Target plug-in instance id, e.g. "fx_1". |
| `rack` | string |  |  |  | Source rack id, e.g. "rk_1". |

### Result

```json
{
  "type": "object",
  "title": "modulation_list",
  "properties": {
    "modulations": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "modulation_entry",
        "properties": {
          "modulation": {
            "type": "string"
          },
          "source": {
            "type": "object",
            "title": "modulation_source",
            "properties": {
              "module": {
                "type": "string",
                "pattern": "^mo_[0-9]+$",
                "description": "Mod Out module id, e.g. \"mo_1\"."
              },
              "input": {
                "type": "string",
                "pattern": "^in_([1-9]|1[0-6])$",
                "description": "One of the Mod Out's inputs, in_1 .. in_16."
              }
            },
            "required": ["module", "input"],
            "additionalProperties": false
          },
          "target": {
            "type": "object",
            "title": "modulation_target",
            "properties": {
              "fx": {
                "type": "string",
                "pattern": "^fx_[0-9]+$",
                "description": "Plugin instance id, e.g. \"fx_1\"."
              },
              "track": {
                "type": "string",
                "pattern": "^tr_[0-9]+$",
                "description": "A track's volume, pan or width."
              },
              "route": {
                "type": "string",
                "pattern": "^sd_[0-9]+$",
                "description": "A send's volume."
              },
              "param": {
                "type": "string",
                "minLength": 1,
                "description": "A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route."
              }
            },
            "required": ["param"],
            "additionalProperties": false
          },
          "depth": {
            "type": "number"
          },
          "enabled": {
            "type": "boolean"
          },
          "lane": {
            "type": "boolean",
            "description": "The route asks for its target's lane to be shown (a display flag; the sound is the same either way)"
          },
          "range": {
            "type": "string"
          },
          "fed_by": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "modulation_fed_by",
              "properties": {
                "module": {
                  "type": "string"
                },
                "output": {
                  "type": "string"
                },
                "type": {
                  "type": "string"
                }
              },
              "required": ["module", "output", "type"],
              "additionalProperties": false
            }
          },
          "inert": {
            "type": "string"
          }
        },
        "required": [
          "modulation",
          "source",
          "target",
          "depth",
          "enabled",
          "lane",
          "range",
          "fed_by"
        ],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["modulations"],
  "additionalProperties": false
}

```

### Examples

**List** (`song_with_modulation`)

```json
{}

```

Result:

```json
{
  "modulations": [
    {
      "modulation": "mr_1",
      "source": {
        "module": "mo_2",
        "input": "in_1"
      },
      "target": {
        "fx": "fx_1",
        "param": "gain"
      },
      "depth": 0.25,
      "enabled": true,
      "lane": true,
      "range": "bipolar",
      "fed_by": [],
      "inert": "source"
    }
  ]
}

```

## `modulation.remove@1`

Remove a modulation route.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modulation` | string |  |  |  | Modulation route id, e.g. "mr_1". |

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

**Remove** (`song_with_modulation`)

```json
{
  "modulation": "mr_1"
}

```

Result:

```json
{}

```

## `modulation.set@1`

Change a modulation route's source, target, depth, enabled or lane.

Omitted fields keep their values. A depth drag sends Origin.gesture, so the drag is one undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modulation` | string |  |  |  | Modulation route id, e.g. "mr_1". |
| `source` | object |  |  |  |  |
| `source.module` | string |  |  |  | Mod Out module id, e.g. "mo_1". |
| `source.input` | string |  |  |  | One of the Mod Out's inputs, in_1 .. in_16. |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `depth` | number |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `lane` | boolean |  |  |  | Show or hide the target's automation lane for this route. It never changes the sound. |

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

**Deepen** (`song_with_modulation`)

```json
{
  "modulation": "mr_1",
  "depth": 0.5
}

```

Result:

```json
{}

```

## `modulation.telemetry@1`

Read each modulation route's live offset and span.

offset: the route's target's current offset, the sum of every route on it, as the engine last applied it (a plug-in parameter's as a fraction of its range, a volume's in dB, a pan's or width's in percent); absent while no running plan applies the route. low and high: what this route alone can add, depth times its signal range (bipolar -1..1, unipolar 0..1), in the same unit; absent while nothing is cabled into its Mod Out input.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Target track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "modulation_telemetry",
  "properties": {
    "modulations": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "modulation_reading",
        "properties": {
          "modulation": {
            "type": "string"
          },
          "offset": {
            "type": "number"
          },
          "low": {
            "type": "number"
          },
          "high": {
            "type": "number"
          }
        },
        "required": ["modulation"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["modulations"],
  "additionalProperties": false
}

```

### Examples

**Stopped, nothing cabled into the route's input yet** (`song_with_modulation`)

```json
{}

```

Result:

```json
{
  "modulations": [
    {
      "modulation": "mr_1"
    }
  ]
}

```

## `module.add@1`

Add a module to a rack: a type with knob values.

type: an id or a short name ("vca", "notes"; module.list). x, y: its top-left corner in the rack (default: right of the others). params: knob values by key in the knob's units (a choice by its key, a switch as a bool). Notes: source (the track whose MIDI it plays; default the rack's channel) and device (a live MIDI input it always plays; "" any). plugin (instead of type): a plug-in id (fx.scan) for a plugin module, which shows the plug-in's name and opens its editor; its ports are in_l, in_r, out_l, out_r and, for instruments, pitch, gate and velocity. preset: a factory preset (module.describe), params on top. Returns its size and keys.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `type` | string |  |  |  |  |
| `label` | string |  |  |  |  |
| `preset` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |
| `params` | value |  |  |  |  |
| `source` | string |  |  |  |  |
| `device` | string |  |  |  |  |
| `plugin` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown_type`: no such module type Hint: module.list
- `rack.param`: a knob or value the type does not have Hint: module.describe
- `rack.limit`: too many modules Hint: remove modules first

### Examples

**A VCA, half open** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "type": "vca",
  "params": {
    "level": -6
  }
}

```

Result:

```json
{
  "module": "mo_4",
  "type": "com.suppleaudio.rack.vca",
  "x": "<any>",
  "y": 40.0,
  "width": "<any>",
  "height": "<any>",
  "inputs": ["in", "cv"],
  "outputs": ["out"],
  "knobs": ["level", "response"]
}

```

**An instrument plug-in, played by cables from Notes** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "module": "mo_4",
  "type": "com.suppleaudio.rack.plugin",
  "x": "<any>",
  "y": "<any>",
  "width": "<any>",
  "height": "<any>",
  "inputs": ["in_l", "in_r", "pitch", "gate", "velocity"],
  "outputs": ["out_l", "out_r"],
  "knobs": [],
  "fx": "fx_1",
  "plugin": "Test Sine"
}

```

## `module.check@1`

Compile a module without saving; return diagnostics and description.

source (a whole module) or name (one in the library). ok is true when it compiles; type then describes it as module.describe would.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.unknown`: no such library module Hint: module.list

### Examples

**A mistake, with its hint** (`empty`)

```json
{
  "source": "struct X : Module { void process(int frames) {} };"
}

```

Result:

```json
{
  "ok": false,
  "diagnostics": "<any>"
}

```

**A library module** (`song_with_module`)

```json
{
  "name": "wobble"
}

```

Result:

```json
{
  "ok": true,
  "diagnostics": [],
  "type": "<any>"
}

```

## `module.copy@1`

Copy modules, their current plug-in settings and internal cables.

Replace this engine's clipboard, independent of the project and undo. Module order is retained; cables to unselected modules are omitted.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modules` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.unknown`: a selected module no longer exists Hint: use rack.get ids
- `module.copy`: a module occurs twice Hint: select each module once

### Examples

**Copy a VCA and its Out, with their cable** (`song_with_rack`)

```json
{
  "modules": ["mo_3", "mo_1"]
}

```

Result:

```json
{
  "modules": 2,
  "cables": 1
}

```

## `module.describe@1`

A module type's knobs, inputs and outputs, with units and help.

type (an id or a short name, "vca") or module (a module's type). Knobs: key, name, kind (number, integer, choice, toggle), unit, min, max, default, choices (keys), group, help. Ports: key, name, signal (audio, pitch in octaves from C4, gate, cv -1..1, unipolar 0..1, phase 0..1), help, and an input's normal (its value with nothing connected). A cable into a knob moves it by amount per unit of signal: octaves for a frequency, the knob's own unit otherwise.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  |  |  |
| `module` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**The VCA** (`empty`)

```json
{
  "type": "vca"
}

```

Result:

```json
{
  "type": "com.suppleaudio.rack.vca",
  "name": "VCA",
  "origin": "stock",
  "vendor": "Supple Audio",
  "version": "1.0.0",
  "description": "<any>",
  "tags": ["amplifier", "utility"],
  "width": "<any>",
  "height": "<any>",
  "knobs": "<any>",
  "inputs": "<any>",
  "outputs": "<any>"
}

```

## `module.duplicate@1`

Duplicate modules, with the cables between them.

The copies sit dx, dy from the originals (default 0, 40 below the lowest). cables (default true): cables among the given modules are copied too. Plugin modules copy their plug-in with its state.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modules` | array |  |  |  |  |
| `dx` | number |  | -1e+06..1e+06 |  |  |
| `dy` | number |  | -1e+06..1e+06 |  |  |
| `cables` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**Copy the VCA** (`song_with_rack`)

```json
{
  "modules": ["mo_3"]
}

```

Result:

```json
{
  "modules": ["mo_4"],
  "cables": []
}

```

## `module.edit@1`

Edit a library module in place: replace text, then save and compile it.

name (or type, a library or project module's id). A project type is edited into the library's entry of the same id, or a free name when the id is absent. Each edit replaces old, which must be in the source exactly once, with new (empty deletes it), in order and all or none. Then as module.write: saved, compiled, and racks playing it rebuilt.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `type` | string |  |  |  |  |
| `edits` | array |  |  |  |  |
| `edits[].old` | string |  |  |  | text the source has in one place |
| `edits[].new` | string |  |  |  | what replaces it |

### Result

```json
{}

```

### Errors

- `module.unknown`: no such library module Hint: module.list
- `module.edit`: an edit's old text is not in the source, or in several Hint: copy old exactly from module.get
- `module.built_in`: built-in modules are not edited Hint: module.fork

### Examples

**Slow the tremolo down** (`song_with_module`)

```json
{
  "name": "wobble",
  "edits": [
    {
      "old": "0.1, 20, 4",
      "new": "0.1, 20, 2"
    }
  ]
}

```

Result:

```json
{
  "name": "wobble",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```

## `module.fork@1`

Copy a module type's source into the library under a new id.

type: the one to copy (stock or library). id: the copy's ("com.<you>.<name>"). name: its library name (default: the id's last part). The copy is saved and compiled as module.write does; then module.edit changes it.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  |  |  |
| `id` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown_type`: no such type Hint: module.list
- `module.host`: a host module has no source Hint: module.describe
- `module.exists`: the library has a module of that name Hint: give name

### Examples

**Your own VCA to change** (`empty`)

```json
{
  "type": "vca",
  "id": "com.example.vca2"
}

```

Result:

```json
{
  "name": "vca2",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```

## `module.get@1`

A module type's source, not a placed module's knobs (rack.get reads those).

Reads a module type's source, for writing your own; the knobs of a module placed in a rack are in rack.get (detail full lists every knob key; module.describe lists a type's knobs and ranges). type (an id or short name: every stock module's source is readable, a good start for your own) or name (a library module's file, also when it does not compile).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown_type`: no such type Hint: module.list
- `module.unknown`: no such library module Hint: module.write
- `module.host`: a host module has no source Hint: module.describe

### Examples

**The stock VCA** (`empty`)

```json
{
  "type": "vca"
}

```

Result:

```json
{
  "type": "com.suppleaudio.rack.vca",
  "origin": "stock",
  "source": "<any>"
}

```

## `module.learn.list@1`

List the MIDI controllers of module knobs.

Every module's, or one module's. control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Every binding** (`song_with_rack_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "module": "mo_3",
      "param": "level",
      "midi": {
        "kind": "cc",
        "channel": 1,
        "number": 21
      },
      "control": "CC 21 · ch 1"
    }
  ]
}

```

## `module.learn.remove@1`

Remove a module knob's MIDI controller.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `param` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Free the level** (`song_with_rack_learn`)

```json
{
  "module": "mo_3",
  "param": "level"
}

```

Result:

```json
{}

```

## `module.learn.set@1`

Let a MIDI controller turn a module's knob.

One binding per knob (a new one replaces it). midi.device: a substring of the MIDI input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = +1), relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle. soft_takeover: an absolute control acts once it reaches the knob. A knob turns along its travel (a frequency's is logarithmic); each movement is one undo step. A plugin module's knobs are its plug-in's: fx.learn.set.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `param` | string |  |  |  |  |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**The VCA's level on CC 21** (`song_with_rack`)

```json
{
  "module": "mo_3",
  "param": "level",
  "midi": {
    "kind": "cc",
    "channel": 1,
    "number": 21
  }
}

```

Result:

```json
{
  "param": "level",
  "control": "CC 21 · ch 1"
}

```

## `module.list@1`

List module types: id, name, tags, what they do.

Host modules (Out, In, Notes, Transport) first, then stock, the user's library and the project's. query matches names, ids, tags and descriptions; tag one tag.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `query` | string |  |  |  |  |
| `tag` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Find the VCA** (`empty`)

```json
{
  "query": "vca"
}

```

Result:

```json
{
  "types": [
    {
      "type": "com.suppleaudio.rack.vca",
      "name": "VCA",
      "tags": ["amplifier", "utility"],
      "origin": "stock",
      "description": "<any>"
    }
  ]
}

```

## `module.randomize@1`

Move a module's knobs at random, by an amount.

amount 0..1 (default 0.25): how far each knob may move, as a share of its range (choices change with that probability); switches stay. seed makes it repeatable. Returns the knobs it set.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `amount` | number |  | 0.0..1.0 |  |  |
| `seed` | integer |  |  |  |  |

### Result

```json
{}

```

### Examples

**Nudge the VCA** (`song_with_rack`)

```json
{
  "module": "mo_3",
  "seed": 7
}

```

Result:

```json
{
  "params": "<any>"
}

```

## `module.remove@1`

Remove a module with its cables.

Also its knob envelopes; an Out module's channels lose it as input; an In module's sends go; a plugin module's plug-in goes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Remove the VCA** (`song_with_rack`)

```json
{
  "module": "mo_3"
}

```

Result:

```json
{}

```

## `module.set@1`

Set a module's knobs, label, place or bypass.

params merge into its knobs (a number in the knob's unit, or text in it, "80 %" or "1.2 kHz"; a percent knob such as a Macro's runs 0 to 100; a number outside the knob's range is refused with params.out_of_range, never clamped; a choice by key; a default value is dropped from the file). A knob edit is heard at once, without a recompile. Notes source/device accept null to clear them (source returns to the home track; device to none). source: none disables the track feed while keeping routes and device. source_sends: false stops it hearing MIDI sends into its source track (its own sends and device still play). bypass passes each input to the output of its kind. label names it (an Out module's label is what channels' input menus list). preset: a factory preset by name (module.describe lists them): every knob to it, then params on top.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `params` | value |  |  |  |  |
| `label` | string |  |  |  |  |
| `preset` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |
| `bypass` | boolean |  |  |  |  |
| `source` | value or null |  |  |  |  |
| `device` | string or null |  |  |  |  |
| `source_sends` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**Turn the VCA down** (`song_with_rack`)

```json
{
  "module": "mo_3",
  "params": {
    "level": -12
  }
}

```

Result:

```json
{}

```

**Name the Out** (`song_with_rack`)

```json
{
  "module": "mo_1",
  "label": "Bells"
}

```

Result:

```json
{}

```

## `module.write@1`

Write a module to the library and compile it; racks use it at once.

name: its library name (lower case, digits, '_' or '-'). source: the whole module, a struct deriving from Module (the module guide; module.get shows any type's source). The file is saved even when it does not compile, so module.edit can fix it; ok and diagnostics (line, column, hint) say how it went. Once it compiles, type describes it as module.describe does and module.add takes its id; modules already playing it are rebuilt with the new code (reloaded counts them), their knobs kept. A version that does not compile leaves racks on the last that did.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.name`: not a library name Hint: lower case, digits, '_' or '-'
- `module.size`: more than 1 MiB Hint: split it up
- `module.id_taken`: a built-in or another library module has its id Hint: give ModuleInfo.id your own id

### Examples

**A tremolo module** (`song_with_rack`)

```json
{
  "name": "wobble",
  "source": "#include <supple/script.h>\n\n// Tremolo: a sine LFO moves the input's level.\nstruct Wobble : Module {\n    static constexpr ModuleInfo info = {.name = \"Wobble\",\n                                        .id = \"com.example.wobble\",\n                                        .vendor = \"Example\",\n                                        .description = \"A sine LFO moves the input's level.\",\n                                        .tags = {\"amplifier\", \"modulation\"}};\n\n    Input in = audioIn(\"In\").help(\"What it moves.\");\n    Param rate = frequency(\"Rate\", 0.1, 20, 4).help(\"The LFO's speed.\");\n    Param depth = percent(\"Depth\", 50).help(\"How far the level dips.\");\n    Output out = audioOut(\"Out\").help(\"The input, wobbling.\");\n\n    double phase = 0;\n\n    void process(int frames) {\n        for (int i = 0; i < frames; ++i) {\n            const double gain = 1.0 - depth.at(i) / 100.0 * (0.5 + 0.5 * sin(2 * pi * phase));\n            for (int c = 0; c < channels(); ++c) out[c][i] = in[c][i] * gain;\n            phase += rate.at(i) / sampleRate();\n            phase -= floor(phase);\n        }\n    }\n};\n"
}

```

Result:

```json
{
  "name": "wobble",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```

## `plugin.list@1`

List scanned plugins; filter matches id, name, or vendor.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `filter` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "plugin_list",
  "properties": {
    "plugins": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "vendor": {
            "type": "string"
          },
          "format": {
            "type": "string",
            "description": "clap or vst3"
          },
          "instrument": {
            "type": "boolean"
          },
          "semantic": {
            "type": "boolean"
          },
          "features": {
            "type": "array",
            "items": {
              "type": "string"
            },
            "description": "CLAP features (a VST3's subcategories): the main category first, then the kind of plugin, such as reverb"
          },
          "path": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean",
            "description": "Another location earlier on the plugin path has the same id; fx.add by id uses that one"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic", "features", "path"],
        "additionalProperties": false
      }
    },
    "scan": {
      "type": "object",
      "title": "scan_report",
      "properties": {
        "failures": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_failure",
            "properties": {
              "module": {
                "type": "string"
              },
              "reason": {
                "type": "object",
                "title": "scan_reason",
                "properties": {
                  "code": {
                    "type": "string"
                  },
                  "message": {
                    "type": "string"
                  },
                  "path": {
                    "type": "string"
                  },
                  "hint": {
                    "type": "string"
                  },
                  "details": {
                    "type": "array",
                    "items": {}
                  }
                },
                "required": ["code", "message", "path", "hint"],
                "additionalProperties": false
              },
              "cached": {
                "type": "boolean"
              }
            },
            "required": ["module", "reason", "cached"],
            "additionalProperties": false
          }
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_reason",
            "properties": {
              "code": {
                "type": "string"
              },
              "message": {
                "type": "string"
              },
              "path": {
                "type": "string"
              },
              "hint": {
                "type": "string"
              },
              "details": {
                "type": "array",
                "items": {}
              }
            },
            "required": ["code", "message", "path", "hint"],
            "additionalProperties": false
          }
        },
        "cached_modules": {
          "type": "integer"
        }
      },
      "required": ["failures", "warnings", "cached_modules"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugins", "scan"],
  "additionalProperties": false
}

```

### Examples

**Find a test synth** (`empty`)

```json
{
  "filter": "test.sine"
}

```

Result:

```json
{
  "plugins": "<any>",
  "scan": "<any>"
}

```

## `plugin.override@1`

Load a plug-in normally, on a bridge, or not at all for this run.

A safety choice for a plug-in suspected of crashing: mode bridge runs it in a dedicated bridge process (a crash then stays there; a live bridge can drop blocks under load), disable does not load it (its strips pass audio through and a rack module of it is silent), normal loads it as the project says. It holds until the app quits, is not in the project or its history, and is not saved: a project saved while it holds is byte-for-byte the one saved without. Instances of the plug-in restart, so state that was never saved is lost and held notes end. Stock plug-ins cannot be bridged, and bridge needs the bridge executable; both are refused with plugin.override_unavailable.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  | id or name, as plugin.list shows it |
| `mode` | string |  |  |  | One of: normal, bridge, disable. |

### Result

```json
{
  "type": "object",
  "title": "plugin_override_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "format": {
      "type": "string"
    },
    "mode": {
      "type": "string",
      "enum": ["normal", "bridge", "disable"]
    },
    "changed": {
      "type": "boolean",
      "description": "false: it was set that way already"
    },
    "fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "the fx of this plug-in in the project now"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "name", "format", "mode", "changed", "fx"],
  "additionalProperties": false
}

```

### Examples

**Load normally** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "mode": "normal"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "format": "clap",
  "mode": "normal",
  "changed": false,
  "fx": []
}

```

## `plugin.overrides@1`

List the plug-ins this run bridges or has disabled.

What plugin.override set, with the fx of each plug-in in the project now. bridge_available: a bridge executable was found, so mode bridge can be set.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "plugin_override_list",
  "properties": {
    "overrides": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_override_entry",
        "properties": {
          "plugin": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "format": {
            "type": "string"
          },
          "mode": {
            "type": "string",
            "enum": ["normal", "bridge", "disable"]
          },
          "fx": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["plugin", "name", "format", "mode", "fx"],
        "additionalProperties": false
      }
    },
    "bridge_available": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["overrides", "bridge_available"],
  "additionalProperties": false
}

```

### Examples

**None set** (`empty`)

```json
{}

```

Result:

```json
{
  "overrides": [],
  "bridge_available": "<any>"
}

```

## `plugin.scan@1`

Rescan configured plugin paths and return the available plugins.

wait: false scans in the background (every module through the supervised scanner) and returns at once with the current list and the scan's job; the catalogue changes when the job is done. A background scan already running is returned instead of a second one.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `wait` | boolean |  |  |  | false: scan in the background (default true) |

### Result

```json
{
  "type": "object",
  "title": "plugin_list",
  "properties": {
    "plugins": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "vendor": {
            "type": "string"
          },
          "format": {
            "type": "string",
            "description": "clap or vst3"
          },
          "instrument": {
            "type": "boolean"
          },
          "semantic": {
            "type": "boolean"
          },
          "features": {
            "type": "array",
            "items": {
              "type": "string"
            },
            "description": "CLAP features (a VST3's subcategories): the main category first, then the kind of plugin, such as reverb"
          },
          "path": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic", "features", "path"],
        "additionalProperties": false
      }
    },
    "scan": {
      "type": "object",
      "title": "scan_report",
      "properties": {
        "failures": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_failure",
            "properties": {
              "module": {
                "type": "string"
              },
              "reason": {
                "type": "object",
                "title": "scan_reason",
                "properties": {
                  "code": {
                    "type": "string"
                  },
                  "message": {
                    "type": "string"
                  },
                  "path": {
                    "type": "string"
                  },
                  "hint": {
                    "type": "string"
                  },
                  "details": {
                    "type": "array",
                    "items": {}
                  }
                },
                "required": ["code", "message", "path", "hint"],
                "additionalProperties": false
              },
              "cached": {
                "type": "boolean"
              }
            },
            "required": ["module", "reason", "cached"],
            "additionalProperties": false
          }
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_reason",
            "properties": {
              "code": {
                "type": "string"
              },
              "message": {
                "type": "string"
              },
              "path": {
                "type": "string"
              },
              "hint": {
                "type": "string"
              },
              "details": {
                "type": "array",
                "items": {}
              }
            },
            "required": ["code", "message", "path", "hint"],
            "additionalProperties": false
          }
        },
        "cached_modules": {
          "type": "integer"
        }
      },
      "required": ["failures", "warnings", "cached_modules"],
      "additionalProperties": false
    },
    "job": {
      "type": "string",
      "description": "the background scan (wait: false)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugins", "scan"],
  "additionalProperties": false
}

```

### Examples

**Scan** (`empty`)

```json
{}

```

Result:

```json
{
  "plugins": "<any>",
  "scan": "<any>"
}

```

## `project.get@1`

Show the project: name, rate, tempo, meter, file, version and branch.

recoverable: recordings a crash left out of the project (project.recoverable). autosave_available: a newer autosave exists next to the file; open it as a project if you want it (nothing is loaded silently).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Examples

**A new project** (`empty`)

```json
{}

```

Result:

```json
{
  "name": "",
  "path": "",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": 48000.0,
  "tempo": 120.0,
  "meter": "4/4",
  "timecode": {
    "rate": "30",
    "start": "00:00:00:00"
  },
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.new@1`

Start a new, empty project (replaces the current one and its history).

The new project has one master and no file until project.save {path}. Unsaved changes to the current project are lost: save first.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `sample_rate` | number |  | 8000.0..384000.0 |  | Hz; default 48000. |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Examples

**New** (`song`)

```json
{
  "name": "Sketch"
}

```

Result:

```json
{
  "name": "Sketch",
  "path": "",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": 48000.0,
  "tempo": "<any>",
  "meter": "<any>",
  "timecode": "<any>",
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.open@1`

Open a project file and its branches (replaces the current project).

recoverable > 0: recordings a crash left out of the project; project.recoverable lists them and project.recover puts them back. autosave_available: a newer autosave exists next to the file (it is not loaded silently). branch: the branch current when saved. warnings: project.history when the saved branches could not be restored (main opens alone; the first save over the file keeps a copy of it); also project module copies that could not be read or compiled, and name types using a library version instead or left unavailable; project.suspect_plugin when a crash report points at a plug-in the project holds (it loads normally: the engine never prompts or changes how a plug-in loads by itself; plugin.override first to protect the run).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `expect_hash` | string |  |  |  | The hash project.plugins returned: the file is opened only if its bytes are those it checked (project.changed otherwise) |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Errors

- `file.open`: the file cannot be read Hint: check the path
- `schema.invalid`: the file has problems Hint: details list each with its path

### Examples

**Open** (`saved`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "name": "<any>",
  "path": "<any>",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": "<any>",
  "tempo": "<any>",
  "meter": "<any>",
  "timecode": "<any>",
  "version": "<any>",
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.plugins@1`

Plug-ins a project or recovery names, and any a crash report suspects.

Reads the file as project.open would and loads nothing: no plug-in, no probe. Every branch's head is read, not the history steps behind them. plugins lists each (format, id) once with the name, vendor and version the file saves, uses (its fx in the saved branch), branches, mode (this run's plugin.override), stock and bridgeable, and resolved: the installed plug-in (id, name, vendor, version, path) from the catalogue as it stands, or null (not installed, or the catalogue is empty: catalogue_empty says it is, and no scan was started; plugin.scan first). suspect is null, or says that a crash report points at it and the installed file is that build (build same), that version when the report names no build (version), or could not be checked against it (unreadable): attribution (fault_in_image or active_token) and the incidents, newest first. A plug-in is never loaded to find out. hash is the SHA-256 of the file's bytes: pass it to project.open as expect_hash so the check and the load read the same file. template: true reads a project template (a folder or file) instead. Recovery is checked the same way, before it opens anything: session reads the autosave project.recover {session} would open, autosave: true the one project.recover {autosave: true} would (autosave_file names one, as there). The result's source says which, and autosave names the file read: pass it to project.recover as autosave_file so the check and the load read one file. Give exactly one of path, session and autosave. project.open and project.recover also warn (project.suspect_plugin) of a plug-in still on the normal load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `template` | boolean |  |  |  |  |
| `session` | string |  |  |  | A session project.recoverable lists |
| `autosave` | boolean |  |  |  | The open project's newer autosave |
| `autosave_file` | string |  |  |  | With autosave: which one, as offered |

### Result

```json
{}

```

### Errors

- `file.open`: the file cannot be read Hint: check the path
- `schema.invalid`: the file has problems Hint: details list each with its path
- `project.plugins_args`: the source is not exactly one of path, session, autosave Hint: give one
- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.no_autosave`: no autosave is newer than the saved file Hint: open the project as saved

### Examples

**Which plug-ins** (`saved`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "source": "file",
  "path": "<any>",
  "hash": "<any>",
  "catalogue_empty": "<any>",
  "plugins": "<any>"
}

```

## `project.recover@1`

Put back what a crash left behind: recordings, or an unsaved project.

files: as project.recoverable lists them (omit for all). Each becomes an item on the track it was recorded on (or track) at the position it was recorded, in one undo step. session: instead opens that unsaved project from its newest readable autosave in place of the current one (unsaved changes are lost: save first), still unsaved, and puts back its leftover recordings as one more undo step. autosave: instead opens the saved project's autosave newer than its file (project.get autosave_available) in place of what is open, keeping its path: it is unsaved until saved over the file. autosave_file names the autosave to open (as project.recover returned or a client offered it); it must be newer than the file as this project was opened. A plug-in an earlier crash report points at raises project.suspect_plugin among the result's warnings (project.plugins session or autosave says so before the call), and loads normally.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `files` | array |  |  |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `session` | string |  |  |  | A session project.recoverable lists |
| `autosave` | boolean |  |  | false | The saved project's newer autosave |
| `autosave_file` | string |  |  |  | With autosave: which one, e.g. autosave-3.json, as offered |

### Result

```json
{
  "type": "object",
  "title": "recover_result",
  "properties": {
    "recovered": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "autosave": {
      "type": "string",
      "description": "The autosave it opened"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["recovered"],
  "additionalProperties": false
}

```

### Errors

- `recover.nothing`: there is nothing to recover Hint: project.recoverable
- `recover.unknown_file`: a file is not a recording to recover Hint: project.recoverable lists them
- `recover.duplicate_file`: a file is listed twice Hint: list each once
- `recover.no_track`: the track it was recorded on is gone Hint: give track
- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.busy`: the session belongs to an app that is running Hint: recover it in that app
- `recover.damaged`: nothing in the session can be opened Hint: project.recoverable.delete
- `recover.session_args`: session cannot be combined with files or track Hint: recover the session, then its recordings
- `recover.no_autosave`: no autosave is newer than the saved file Hint: open the project as saved
- `recover.keep`: a copy recovery must keep could not be made; nothing changed Hint: make the folder writable and retry
- `recover.autosave_args`: autosave cannot be combined with session, files or track Hint: recover the autosave, then the recordings

### Examples

**Recover everything** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recovered": [
    {
      "track": "tr_1",
      "item": "it_1",
      "take": "tk_1",
      "file": "<any>",
      "seconds": 1.0,
      "lost_frames": 0,
      "takes": ["tk_1"]
    }
  ]
}

```

**Recover an unsaved project** (`session_left_behind`)

```json
{
  "session": "20261114-090000-4242"
}

```

Result:

```json
{
  "recovered": [
    {
      "track": "tr_1",
      "item": "<any>",
      "take": "<any>",
      "file": "<any>",
      "seconds": 1.0,
      "lost_frames": 0,
      "takes": "<any>"
    }
  ],
  "autosave": "autosave-1.json"
}

```

## `project.recoverable@1`

List recordings and unsaved projects a crash left behind.

recordings: takes (audio WAVs, and MIDI takes' journals with midi_events) recorded but never committed (the program stopped mid-recording) or never saved, found in the media folder's rec/ (file: relative to the media folder, with / on every platform). start_seconds is where each was recorded; seconds includes audio written after the file's last header update. sessions: unsaved projects of earlier runs that ended without saving or discarding them (a crash), each with its newest autosave's project name, tracks and branches, its leftover recordings and its size on disk; project.recover {session} opens one, project.recoverable.delete deletes one.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "recoverable_list",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    },
    "recordings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recoverable_recording",
        "properties": {
          "file": {
            "type": "string"
          },
          "track": {
            "type": "string"
          },
          "start_seconds": {
            "type": "number"
          },
          "seconds": {
            "type": "number"
          },
          "channels": {
            "type": "integer"
          },
          "midi_events": {
            "type": "integer",
            "description": "A MIDI take's journal: the events it holds"
          }
        },
        "required": ["file", "track", "start_seconds", "seconds", "channels"],
        "additionalProperties": false
      }
    },
    "sessions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recoverable_session",
        "properties": {
          "session": {
            "type": "string"
          },
          "started_ms": {
            "type": "integer",
            "description": "When that run started (Unix ms)"
          },
          "autosaved_ms": {
            "type": "integer",
            "description": "When its newest readable autosave was written (Unix ms); absent: none, only recordings"
          },
          "name": {
            "type": "string"
          },
          "tracks": {
            "type": "integer",
            "description": "Channels and busses, not masters"
          },
          "branches": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "recordings": {
            "type": "integer",
            "description": "Takes no autosaved version uses"
          },
          "bytes": {
            "type": "integer",
            "description": "Its folder's size on disk"
          }
        },
        "required": ["session", "name", "tracks", "branches", "recordings", "bytes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["recordings", "sessions"],
  "additionalProperties": false
}

```

### Examples

**After a crash while recording** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recordings": [
    {
      "file": "rec/Vocal-1.wav",
      "track": "tr_1",
      "start_seconds": 1.0,
      "seconds": 1.0,
      "channels": 2
    }
  ],
  "sessions": []
}

```

**An unsaved project a crash left behind** (`session_left_behind`)

```json
{}

```

Result:

```json
{
  "recordings": [],
  "sessions": [
    {
      "session": "20261114-090000-4242",
      "started_ms": "<any>",
      "autosaved_ms": "<any>",
      "name": "Sketch",
      "tracks": 1,
      "branches": ["main"],
      "recordings": 1,
      "bytes": "<any>"
    }
  ]
}

```

**Nothing to recover** (`empty`)

```json
{}

```

Result:

```json
{
  "recordings": [],
  "sessions": []
}

```

## `project.recoverable.delete@1`

Delete an unsaved project a crash left behind, with its recordings.

session: as project.recoverable lists it. Its folder and everything in it are deleted for good (this is not an undo step).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `session` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "session_deleted",
  "properties": {
    "session": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["session"],
  "additionalProperties": false
}

```

### Errors

- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.busy`: the session belongs to an app that is running Hint: quit that app
- `recover.delete`: the folder could not be deleted Hint: make it writable

### Examples

**Delete one** (`session_left_behind`)

```json
{
  "session": "20261114-090000-4242"
}

```

Result:

```json
{
  "session": "20261114-090000-4242"
}

```

## `project.save@1`

Save the project (canonical JSON) to its file or a new path.

path: where to save (becomes the project's file); omit to save in place. The file keeps every branch with its unmerged steps (undoable after opening) and which branch is current; history: true also keeps undo history. warnings: running plug-ins whose current state could not be read; the file keeps the state each had before (fx.restart, then save again, or replace the plug-in); project.history_backup names the copy kept of a file whose branches could not be restored when opened. project.recording: a first save, or a save into another folder, while recording; stop first. project.autosave_folder: the autosave folder beside the file (<file>-data) could not be read; nothing was written.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `history` | boolean |  |  | false | Also keep undo and redo history (ADR-0076) |

### Result

```json
{
  "type": "object",
  "title": "save_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Examples

**Save next to the work directory** (`empty`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `project.set@1`

Set project name, notes, Console 9, pan law, timebase or timecode.

timebase: what items follow when the tempo changes, for items and tracks that inherit it — beats (default), time or beats_position; they keep their place now. timecode: the frame rate (23.976, 24, 25, 29.97df, 29.97 or 30; default 30) and start, the timecode at project time 0 (default 00:00:00:00), which MTC and LTC sync and output use; either member alone keeps the other.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `title` | string |  |  |  |  |
| `author` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `console9` | boolean |  |  |  |  |
| `pan_law` | object |  |  |  | Channels' and busses' pan law unless a track sets its own: db is the centre's cut (0, -2.5, -3, -4.5 or -6; default 0); boost keeps the centre at 0 dB and raises the sides instead |
| `pan_law.db` | number or string | dB |  | 0.0 | Quantity strings are range-checked by the server. One of: 0.0, -2.5, -3.0, -4.5, -6.0. |
| `pan_law.boost` | boolean |  |  | false |  |
| `history_budget_bytes` | integer |  | 1.0.. |  | Estimated retained history byte budget; default 1 GiB. Leases and branch boundaries may exceed it. |
| `render` | object |  |  |  | Render settings (session-model §1.10); every member optional |
| `render.source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `render.masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `render.bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `render.start_beat` | number |  | 0.0..1e+09 |  |  |
| `render.end_beat` | number |  | ..1e+09 |  |  |
| `render.tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `render.pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `render.sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `render.channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `render.outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `render.normalize` | object |  |  |  |  |
| `render.limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `render.trim` | object |  |  |  |  |
| `render.seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `render.handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `render.stems` | object |  |  |  |  |
| `render.matrix` | object |  |  |  |  |
| `render.if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `render.metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `render.report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `render.add_to_project` | boolean |  |  |  |  |
| `timebase` | string |  |  |  | One of: time, beats, beats_position. |
| `timecode` | object |  |  |  |  |
| `timecode.rate` | string |  |  |  | One of: 23.976, 24, 25, 29.97df, 29.97, 30. |
| `timecode.start` | string |  |  |  | HH:MM:SS:FF, ";" before the frames in drop-frame |

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

**Name it** (`empty`)

```json
{
  "name": "Demo"
}

```

Result:

```json
{}

```

**EBU timecode starting at one hour** (`empty`)

```json
{
  "timecode": {
    "rate": "25",
    "start": "01:00:00:00"
  }
}

```

Result:

```json
{}

```

**Pan with the -3 dB law, boosting the sides** (`empty`)

```json
{
  "pan_law": {
    "db": -3,
    "boost": true
  }
}

```

Result:

```json
{}

```

## `project.template.delete@1`

Delete a saved project template by name.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_delete_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `template.unknown`: no template has that name Hint: use a name from project.template.list

### Examples

**Delete the band setup** (`song_with_project_template`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `project.template.list@1`

List saved project templates by name and path.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "project_template_list_result",
  "properties": {
    "templates": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "project_template_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["name", "path"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["templates"],
  "additionalProperties": false
}

```

### Examples

**None yet** (`empty`)

```json
{}

```

Result:

```json
{
  "templates": []
}

```

## `project.template.load@1`

Start a new, unsaved project from a project template.

Replaces the current project and its history, like project.new: unsaved changes are lost, so save first. The new project has no file until project.save {path} and gets its own copy of the template's audio; the template itself is never changed.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_load_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Examples

**Start from the band setup** (`song_with_project_template`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "name": "<any>"
}

```

## `project.template.save@1`

Save the whole project as a named project template.

Writes the project as it is now (tracks, FX and their states, routing, items, settings; not the history) to a folder of its own in the user's templates directory, with a copy of every audio file it uses. The project keeps its own file. replace: true replaces a template of that name. warnings: running plug-ins whose current state could not be read; the template has the state each had before.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_save_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `template.exists`: the name already exists Hint: choose another name or set replace: true
- `media.missing`: an audio file the project uses is gone Hint: restore it, or remove the items that use it

### Examples

**Save the band setup** (`song`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `rack.add@1`

Add a modular rack on a channel, its Out the input.

A rack is a patching space homed on a channel (track, or a new channel named after name). It starts with an Out module, replacing the channel's previous audio, MIDI or rack input: what the patch sends to Out plays on the channel, before its inserts and Console 9 stage. The channel's MIDI items play into Notes modules. Add modules with module.add and connect them with cable.add.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.home`: a rack lives on a channel Hint: give a channel's id
- `rack.limit`: too many racks Hint: remove a rack first

### Examples

**A rack on the bass channel** (`song`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{
  "rack": "rk_1",
  "track": "tr_1",
  "out": "mo_1"
}

```

**A rack on a new channel** (`song`)

```json
{
  "name": "Drones"
}

```

Result:

```json
{
  "rack": "rk_1",
  "track": "tr_2",
  "out": "mo_1"
}

```

## `rack.arrange@1`

Lay out a rack's modules by signal flow.

Moves the modules (default all the rack's) as rack.patch lays out new ones: left to right by signal flow along their cables, ordered within columns so cables cross less, wrapped to about a window's shape. From x, y (default the modules' top-left corner). One undo step; returns the new places.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `modules` | array |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |

### Result

```json
{}

```

### Errors

- `rack.module`: a module that is not in the rack Hint: name modules of this rack

### Examples

**Tidy up a rack** (`song_with_rack`)

```json
{
  "rack": "rk_1"
}

```

Result:

```json
{
  "modules": {
    "mo_1": {
      "x": 328,
      "y": 138
    },
    "mo_2": {
      "x": 40,
      "y": 40
    },
    "mo_3": {
      "x": 184,
      "y": 115
    }
  }
}

```

## `rack.capture@1`

Record a rack's pitch and gate as notes in a MIDI item on a track.

Renders the project offline from from_beat (default 0) for beats (default 16, at most 1024) and writes the notes that the pitch and gate outputs ("mo_N.<output>", poly: a note per voice) play as a MIDI item on track at at (default from_beat): a gate's rise starts a note at the pitch then (round(60 + 12 x pitch)), its fall ends it. velocity: an output (0..1) read at each rise; default 96. quantize: a grid in beats (0.25 = sixteenths) for starts and lengths. notes: notation the rack's Notes modules play, as rack.probe takes it. The rack is not changed; edit the notes with midi.edit, or play them through any instrument. async:true returns {job}; rendering uses a snapshot, cancellation leaves the project unchanged, and completion adds one undo step. A changed rack graph or automation, destination type/frozen state or overlapping items, timing, or project fails with rack.capture_stale; submit again. Destination fader moves and unrelated item edits are allowed. Notes MIDI and incoming In routes are snapshotted.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `async` | boolean |  |  | false |  |
| `rack` | string |  |  |  | Rack id |
| `pitch` | string |  |  |  |  |
| `gate` | string |  |  |  |  |
| `velocity` | string |  |  |  |  |
| `track` | string |  |  |  | Track id |
| `beats` | number |  | 0.25..1024.0 |  |  |
| `from_beat` | number |  | 0.0.. |  |  |
| `at` | number |  | 0.0.. |  |  |
| `quantize` | number |  | 0.015625..4.0 |  |  |
| `notes` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown`: no such rack Hint: rack.list
- `rack.port`: not an output of a module in the rack Hint: rack.get lists them
- `rack.capture`: the render is too long Hint: capture at most 600 seconds
- `rack.capture_stale`: capture target changed during rendering Hint: submit rack.capture again for the current rack, track and position

### Examples

**Print a chord the rack plays to a MIDI item on the second track** (`song_with_rack_preset`)

```json
{
  "rack": "rk_1",
  "pitch": "mo_2.pitch",
  "gate": "mo_2.gate",
  "track": "tr_2",
  "beats": 4,
  "notes": "voice v; 1: [C4 E4 G4]:h r:h"
}

```

Result:

```json
{
  "item": "<any>",
  "take": "<any>",
  "source": "<any>",
  "position": 0.0,
  "length": 4.0,
  "notes": 3,
  "voices": 3
}

```

## `rack.capture.commit@1`

Attach a rack capture's rendered MIDI notes.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `source` | object |  |  |  |  |
| `source.sequence` | object |  |  |  | MIDI item: ppq, length (ticks), notes [[start, length, channel, pitch, velocity, off_velocity]], controls [[tick, kind, channel, number, value]], sysex [[tick, "F0 .. F7"]] |
| `source.pooled` | boolean |  |  | false |  |
| `name` | string |  |  |  |  |
| `at` | number |  | 0.0.. |  |  |
| `beats` | number |  | 0.25..1024.0 |  |  |
| `voices` | integer |  | 0.0..16.0 |  |  |
| `warnings` | array |  |  |  |  |

### Result

```json
{}

```

### Examples

## `rack.get@1`

A rack's modules with their changed knobs, cables, Outs and problems.

Reads the modules placed in a rack, including each one's knobs (module.get reads a module type's source instead). Modules with their type, label, place and size and the knobs that differ from their defaults (detail full: also their input, output and knob keys); cables with feedback (true: it closes a loop and carries the previous frame); Out modules and the channels they feed (sends: their rack sends to busses and masters, ADR-0075, {route, to}); latency_samples for each Out and the rack node, and note_outs with their latencies (ADR-0064); problems (missing types, cables that do not fit). Brief/default latency reads prepared values without creating plug-ins; unprepared latency is null. detail full may probe unprepared plug-ins.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `detail` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**What the rack holds** (`song_with_rack`)

```json
{
  "rack": "rk_1"
}

```

Result:

```json
{
  "rack": "rk_1",
  "name": "Bass",
  "track": "tr_1",
  "modules": "<any>",
  "cables": [
    {
      "cable": "cb_1",
      "from": "mo_3.out",
      "to": "mo_1.l",
      "feedback": false
    }
  ],
  "outs": "<any>",
  "latency_samples": 0,
  "note_outs": [],
  "problems": []
}

```

## `rack.levels@1`

Read every module output's linear peak, without subscribing to telemetry.

Returns {modules: {mo_N: {port: level}}}. Each level is the maximum absolute sample across all channels in the latest processed rack block (at most 32 frames), including unconnected outputs. Levels may exceed 1. Stopped, silent or uninstantiated modules read zero; unavailable types have no known ports. Anticipative levels can lead the heard audio.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown`: no such rack Hint: use a rack.list id

### Examples

**Read output peaks while stopped** (`song_with_rack`)

```json
{
  "rack": "rk_1"
}

```

Result:

```json
{
  "modules": {
    "mo_1": {},
    "mo_2": {
      "pitch": 0.0,
      "gate": 0.0,
      "velocity": 0.0,
      "pressure": 0.0,
      "retrigger": 0.0,
      "modwheel": 0.0,
      "bend": 0.0
    },
    "mo_3": {
      "out": 0.0
    }
  }
}

```

## `rack.list@1`

List racks: home channels, module counts, Outs.

Each Out lists the channels it is the input of (channels) and, when it has any, its rack sends to busses and masters (sends: {route, to}, ADR-0075).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**One rack** (`song_with_rack`)

```json
{}

```

Result:

```json
{
  "racks": [
    {
      "rack": "rk_1",
      "name": "Bass",
      "track": "tr_1",
      "modules": 3,
      "cables": 1,
      "outs": [
        {
          "module": "mo_1",
          "label": "Out 1",
          "channels": ["tr_1"]
        }
      ]
    }
  ]
}

```

## `rack.paste@1`

Paste the module clipboard with its plug-in settings as one undo step.

Place the clip's top-left at x/y; omitted coordinates use free space right of existing modules. No routes or automation are copied.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown`: no such destination rack Hint: use rack.list
- `rack.clip_empty`: no modules have been copied Hint: call module.copy first
- `rack.unknown_type`: a copied module type is unavailable Hint: restore the module type
- `chain.plugin_missing`: a copied plug-in is unavailable Hint: install the copied plug-in
- `track.unknown`: a copied Notes source was removed Hint: restore the source track or copy again
- `rack.limit`: the destination would exceed its limits Hint: remove modules or paste into another rack
- `rack.position`: the clip extends beyond the coordinate range Hint: choose a position closer to the origin

### Examples

**Paste the copied VCA and Out** (`song_with_rack_clip`)

```json
{
  "rack": "rk_1",
  "x": 500,
  "y": 40
}

```

Result:

```json
{
  "modules": ["mo_4", "mo_5"],
  "cables": ["cb_2"]
}

```

## `rack.patch@1`

Add named modules and the cables among them in one step.

modules: {name: {type | plugin, params?, preset?, label?, x?, y?, source?, device?}} as module.add takes them; a name is lower-case letters, digits and '_'. cables: [{from, to, amount?, color?}] or [from, to, amount?], an end "name.port" or "name.knob" (a plugin module's knob is its plug-in's: "synth.cutoff"), or an existing module's "mo_N.port" (the rack's Out from rack.add). Modules without x, y are laid out left to right by signal flow from x, y (default right of the rack's modules), as rack.arrange does. A name labels its module unless label is given: "kick" is Kick, "vco2" is VCO 2, a type's own key ("vco") none. Everything is checked as module.add and cable.add check it; one error changes nothing. Returns the ids by name.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `modules` | value |  |  |  |  |
| `cables` | array |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |

### Result

```json
{}

```

### Errors

- `rack.patch`: a name, an end or a field the patch cannot use Hint: names: lower-case letters, digits and '_'; ends: name.port

### Examples

**A tremolo into the Out** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "modules": {
    "osc": {
      "type": "vco"
    },
    "lfo": {
      "type": "lfo",
      "params": {
        "rate": 5
      }
    },
    "amp": {
      "type": "vca"
    }
  },
  "cables": [
    ["osc.saw", "amp.in"],
    ["lfo.out", "amp.level", 6],
    {
      "from": "amp.out",
      "to": "mo_1.l"
    }
  ]
}

```

Result:

```json
{
  "modules": {
    "osc": "mo_4",
    "lfo": "mo_5",
    "amp": "mo_6"
  },
  "cables": ["cb_2", "cb_3", "cb_4"]
}

```

## `rack.preset.delete@1`

Delete a saved rack preset.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.preset`: no saved preset of that name Hint: rack.preset.list
- `rack.preset_factory`: a factory patch cannot be deleted Hint: save your own version under its name instead

### Examples

**Delete a preset** (`song_with_rack_preset`)

```json
{
  "name": "Gated VCA"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `rack.preset.list@1`

List rack presets, saved and factory: names and contents.

query: a part of the name or description. factory: true lists only the factory patches that ship with the app, false only the saved ones (default both). Each preset: name, path (a saved one's) or factory: true, description, its module and cable counts and its module types. A saved preset hides a factory patch of its name. Files that are not rack presets are skipped with a warning (warnings).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `query` | string |  |  |  |  |
| `factory` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**The saved presets** (`song_with_rack_preset`)

```json
{
  "factory": false
}

```

Result:

```json
{
  "presets": [
    {
      "name": "Gated VCA",
      "path": "<any>",
      "modules": 3,
      "cables": 1,
      "types": [
        "com.suppleaudio.rack.notes",
        "com.suppleaudio.rack.out",
        "com.suppleaudio.rack.vca"
      ]
    }
  ]
}

```

**A factory patch** (`song_with_rack`)

```json
{
  "query": "Init Voice",
  "factory": true
}

```

Result:

```json
{
  "presets": [
    {
      "name": "Init Voice",
      "factory": true,
      "description": "<any>",
      "modules": 6,
      "cables": 9,
      "types": [
        "com.suppleaudio.rack.adsr",
        "com.suppleaudio.rack.filter",
        "com.suppleaudio.rack.notes",
        "com.suppleaudio.rack.out",
        "com.suppleaudio.rack.vca",
        "com.suppleaudio.rack.vco"
      ]
    }
  ]
}

```

## `rack.preset.load@1`

Add a saved preset's modules and cables to a rack.

name (rack.preset.list: a saved preset, or else a factory patch) or path (a preset file). x, y: the group's top-left (default right of the rack's modules). io: join (default) gives the preset's Out and In modules' cables to the rack's own Outs and Ins in order, so a whole patch sounds at once (extra ones are added); add makes new ones. Notes modules play the rack's channel. Your own modules the preset carries are written into the library when it lacks them (installed lists them). Every embedded source must compile and declare its sources key as ModuleInfo.id before any module is installed. Knobs and cables the module types no longer have are left out with a warning (warnings); a module type or plug-in that is not installed is an error, and the rack does not change.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `path` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |
| `io` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.preset_source`: name and path both or neither Hint: give one
- `rack.unknown_type`: the preset needs a module type that is not installed Hint: module.write it, or load another preset
- `chain.plugin_missing`: the preset needs a plug-in that is not installed Hint: install it
- `rack.preset_module`: an embedded source does not compile or has another id Hint: fix its source and ModuleInfo.id to match the sources entry
- `rack.preset_invalid`: not a rack preset Hint: save it again

### Examples

**Load a saved patch into a new rack** (`song_with_rack_preset`)

```json
{
  "rack": "rk_2",
  "name": "Gated VCA"
}

```

Result:

```json
{
  "modules": ["mo_5", "mo_6"],
  "cables": ["cb_2"]
}

```

## `rack.preset.save@1`

Save a rack's modules and cables as a preset.

modules: which (default the whole rack); the cables among them come too. Plugin modules keep their plug-in's settings, and the source of your own (library) modules goes with them. A Notes module's source track, MIDI learn, envelopes, sends and the channels an Out feeds stay with the project. Saved by name in the user's rack presets; replace: true overwrites one.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `modules` | array |  |  |  |  |
| `description` | string |  |  |  |  |
| `replace` | boolean |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.preset_name`: not a usable name Hint: a name without path characters
- `rack.preset_exists`: a preset has the name Hint: replace: true or a new name

### Examples

**Save the whole rack** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "name": "Gated VCA"
}

```

Result:

```json
{
  "path": "<any>",
  "modules": 3,
  "cables": 1
}

```

## `rack.probe@1`

Listen to a rack's ports: level, pitch, gate rate, notes, a CV's range.

Renders the project offline from from_beat (default 0) for seconds (default 2, at most 10) with a listener on each port ("mo_N.<output key>", at most 8) and describes what each carried: audio's peak and rms (dB) and its pitch when periodic (a poly cable's first 4 voices summed); gates counted over the voices, per beat, and how long any is high; the notes the voices' pitches visit; a CV's range and speed (voice 1). notes: notation (as midi.write takes it) the rack's Notes modules play from from_beat, besides what their tracks hold. Nothing is changed; playback is not disturbed.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  | Rack id |
| `ports` | array |  |  |  |  |
| `seconds` | number |  | 0.05..10.0 |  |  |
| `from_beat` | number |  | 0.0.. |  |  |
| `notes` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown`: no such rack Hint: rack.list
- `rack.port`: not an output of a module in the rack Hint: rack.get lists them

### Examples

**Listen to Notes' gate and the VCA while a chord plays** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "ports": ["mo_2.gate", "mo_3.out"],
  "seconds": 1,
  "notes": "voice v; 1: [C4 E4 G4]:h r:h"
}

```

Result:

```json
{
  "seconds": 1.0,
  "from_beat": 0.0,
  "ports": "<any>"
}

```

## `rack.remove@1`

Remove a rack with its modules and cables.

Channels that took one of its Out modules as input lose it; sends into its In modules go; plugin modules' plug-ins go with them.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Remove the rack** (`song_with_rack`)

```json
{
  "rack": "rk_1"
}

```

Result:

```json
{}

```

## `rack.set@1`

Rename a rack or move it to another channel.

name "" shows the home channel's name. track moves its home (its Out modules keep feeding the channels they feed; Notes modules without a source follow the new home's MIDI; its rack sends to busses and masters follow the new home's mute and solo, ADR-0075). A frozen home's rack cannot move (track.frozen).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `track` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Name it** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "name": "Bells"
}

```

Result:

```json
{}

```

## `record.commit@1`

Record a recording's commit (or a recovery's) as a replayable edit.

The takes as the device delivered them and the punch range then; placing them again builds the same items, takes and sources. Audio files must still be there.

Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `takes` | array |  |  |  |  |
| `takes[].track` | string |  |  |  | Track id, e.g. "tr_1". |
| `takes[].file` | string |  |  |  | Relative to the media folder when inside it |
| `takes[].hash` | string |  |  |  |  |
| `takes[].start_sample` | integer |  | -281474976710656.0..281474976710656.0 |  |  |
| `takes[].frames` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].segments` | array |  |  |  |  |
| `takes[].loop_start` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].loop_end` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].overflow_frames` | integer |  | 0.0..281474976710656.0 |  |  |
| `takes[].sample_rate` | number |  |  |  |  |
| `takes[].midi` | boolean |  |  |  |  |
| `takes[].events` | array |  |  |  |  |
| `punch` | object |  |  |  |  |
| `punch.enabled` | boolean |  |  |  |  |
| `punch.start` | number |  |  |  |  |
| `punch.end` | number |  |  |  |  |

### Result

```json
{}

```

### Examples

## `region.add@1`

Add a named region in beats.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `section` | boolean |  |  |  | Whether the region is a section of the song, which context.at and session.describe report [true] |

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

**Add** (`empty`)

```json
{
  "start": 0,
  "end": 4,
  "name": "Verse"
}

```

Result:

```json
{
  "id": "rg_1"
}

```

## `region.list@1`

List rulers in this project.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "ruler_list",
  "properties": {
    "regions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "ruler_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "timebase": {
            "type": "string",
            "enum": ["time", "beats", "beats_position"]
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "pos": {
            "type": "number"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          },
          "section": {
            "type": "boolean"
          }
        },
        "required": ["id", "timebase", "name", "color"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["regions"],
  "additionalProperties": false
}

```

### Examples

**List empty** (`empty`)

```json
{}

```

Result:

```json
{
  "regions": []
}

```

## `region.remove@1`

Remove a ruler.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Region id, e.g. "rg_1". |

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

**Remove** (`song_with_region`)

```json
{
  "id": "rg_1"
}

```

Result:

```json
{}

```

## `region.set@1`

Edit a ruler's position, name, or color.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Region id, e.g. "rg_1". |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `section` | boolean |  |  |  | Whether the region is a section of the song, which context.at and session.describe report [true] |

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

**Rename** (`song_with_region`)

```json
{
  "id": "rg_1",
  "name": "Bridge"
}

```

Result:

```json
{}

```

## `registry.describe@1`

Describe a command.

Describe a command.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `command` | string |  |  |  |  |

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

**Describe registry.list** (`empty`)

```json
{
  "command": "registry.list"
}

```

Result:

```json
{
  "name": "registry.list",
  "version": 1,
  "kind": "query",
  "exposure": "mcp-exec",
  "summary": "List available commands.",
  "description": "List available commands.",
  "tags": [],
  "args": {
    "type": "object",
    "properties": {
      "tag": {
        "type": "string"
      },
      "kind": {
        "type": "string"
      },
      "text": {
        "type": "string"
      }
    },
    "required": [],
    "additionalProperties": false
  },
  "result": {
    "type": "object",
    "properties": {
      "warnings": {
        "type": "array",
        "items": {
          "type": "object"
        }
      }
    }
  },
  "errors": [],
  "examples": [
    {
      "title": "List commands",
      "args": {
        "text": "registry.list"
      },
      "result": {
        "commands": [
          {
            "name": "registry.list",
            "version": 1,
            "kind": "query",
            "summary": "List available commands.",
            "exposure": "mcp-exec"
          }
        ]
      },
      "fixture": "empty"
    }
  ]
}

```

## `registry.list@1`

List available commands.

List available commands.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `tag` | string |  |  |  |  |
| `kind` | string |  |  |  |  |
| `text` | string |  |  |  |  |

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

**List commands** (`empty`)

```json
{
  "text": "registry.list"
}

```

Result:

```json
{
  "commands": [
    {
      "name": "registry.list",
      "version": 1,
      "kind": "query",
      "summary": "List available commands.",
      "exposure": "mcp-exec"
    }
  ]
}

```

## `render.dry-run@1`

List the files a render would write, with lengths, without rendering.

Each file: path (where if_exists sends it), exists (its destination is already there), skip (if_exists skip leaves it out), seconds and outputs. operation: mix, stems or regions (default: tracks or regions given, else the settings' source).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `render.mix@1`

Render the mix offline with the project's settings (default: whole song).

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary. The mix answers with its file's entry at the top level (path, secondary_path, outputs, peak_dbfs, …); analyze: summary or full adds the report inline. masters renders several masters, a file each (the first on top, all in files); a pattern without {track} then gets -{track}, the master's name.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render** (`song_with_fx`)

```json
{
  "name": "mix-v1"
}

```

Result:

```json
{
  "render": "rd_1",
  "path": "<any>",
  "outputs": "<any>",
  "branch": "main",
  "version": "<any>",
  "start_beat": 0,
  "seconds": "<any>",
  "peak_dbfs": "<any>",
  "output_latency": 0
}

```

## `render.preset.delete@1`

Delete one of the user's render presets, or the user's render default.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.preset_factory`: factory presets cannot be deleted Hint: delete one of your own

### Examples

**Delete a preset** (`render_preset`)

```json
{
  "name": "Archive"
}

```

Result:

```json
{
  "deleted": true
}

```

## `render.preset.list@1`

Render presets, factory then user's, with settings.

default: the user's render default, or null. Presets live in the user's data folder (render-presets/), shared by every project.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**List** (`empty`)

```json
{}

```

Result:

```json
{
  "presets": "<any>",
  "default": null
}

```

## `render.preset.load@1`

Replace the project's render settings with a preset (one undo step).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.preset_unknown`: no preset with that name Hint: render.preset.list shows them

### Examples

**A factory preset** (`empty`)

```json
{
  "name": "WAV 24-bit"
}

```

Result:

```json
{
  "render": "<any>"
}

```

## `render.preset.save@1`

Save render settings as a named preset or the user's default.

settings: what to save (default: the effective settings now). Never saved: stems.tracks, matrix and custom bounds, which belong to one project. A file per preset in the user's data folder; factory presets cannot be overwritten.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |
| `settings` | object |  |  |  | Render settings (session-model §1.10); every member optional |
| `settings.source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `settings.masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `settings.bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `settings.start_beat` | number |  | 0.0..1e+09 |  |  |
| `settings.end_beat` | number |  | ..1e+09 |  |  |
| `settings.tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `settings.pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `settings.sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `settings.channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `settings.outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `settings.normalize` | object |  |  |  |  |
| `settings.limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `settings.trim` | object |  |  |  |  |
| `settings.seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `settings.handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `settings.stems` | object |  |  |  |  |
| `settings.matrix` | object |  |  |  |  |
| `settings.if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `settings.metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `settings.report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `settings.add_to_project` | boolean |  |  |  |  |

### Result

```json
{}

```

### Errors

- `render.preset_factory`: a factory preset has that name Hint: save under another name

### Examples

**A preset of FLAC files** (`empty`)

```json
{
  "name": "Archive",
  "settings": {
    "outputs": [
      {
        "format": "flac"
      }
    ]
  }
}

```

Result:

```json
{
  "name": "Archive"
}

```

## `render.queue.add@1`

Queue a render (mix, stems, regions) for render.queue.run.

args: the render command's arguments (as for render.mix/stems/regions). The queued render is pinned to the session and settings as they are now (its version, a branch's head when branch is given, and the effective render settings, a loop range as beats), so later edits do not change it; a dry run lists its files now. The queue is transport-like state: not an undo step, not saved with the project.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `command` | string |  |  |  |  |
| `args` | value |  |  |  |  |
| `label` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `render.empty`: the render would write nothing Hint: add items or a range

### Examples

**Queue the mix as it is now** (`song`)

```json
{
  "command": "render.mix",
  "args": {
    "end_beat": 1,
    "name": "queued"
  }
}

```

Result:

```json
{
  "id": "q1",
  "command": "render.mix",
  "label": "<any>",
  "version": "<any>",
  "files": "<any>",
  "args": "<any>"
}

```

## `render.queue.list@1`

The queued renders, in the order render.queue.run runs them.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Empty** (`empty`)

```json
{}

```

Result:

```json
{
  "queue": []
}

```

## `render.queue.remove@1`

Take a render off the queue (id), or clear it (all: true).



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `all` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.queue_unknown`: no queued render with that id Hint: render.queue.list shows them

### Examples

**Clear** (`empty`)

```json
{
  "all": true
}

```

Result:

```json
{
  "removed": 0
}

```

## `render.queue.run@1`

Run every queued render in order, removing each when done.

Each render renders the version it was queued at. job: true runs them as one job (job.get for progress). The result lists each render's files under its id.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.queue_empty`: nothing is queued Hint: render.queue.add first

### Examples

**Render the queue** (`render_queued`)

```json
{}

```

Result:

```json
{
  "renders": "<any>"
}

```

## `render.regions@1`

Render each region (or the render matrix's tracks per region) to files.

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song_with_region`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `render.settings@1`

The render settings that apply, and the layer each one comes from.

effective: what a render with these arguments would use (every member filled in); project: project.render as stored; default: the user's render default (null if none); from: per member, args, project, default or built-in (ADR-0055). Change the project's settings with project.set {render} or render.preset.load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**What File → Mix would do** (`song`)

```json
{}

```

Result:

```json
{
  "effective": "<any>",
  "project": "<any>",
  "default": null,
  "from": "<any>"
}

```

## `render.stems@1`

Render each selected track to its own file (stems).

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `route.add@1`

Send audio between tracks, or MIDI between channels and rack modules.

The destination must not feed the source. Master accepts routes. With fx instead of to, the send feeds that plug-in's sidechain (key) input — e.g. a kick keying a compressor on the bass; it is delay-compensated, and soloing the keyed track does not stop the key. MIDI sends use channel or Note Out sources and channel or Notes destinations, midi: {} (all channels unchanged); from_channel filters and to_channel remaps, 0..16 with 0 meaning all/original. to_midi selects a hardware MIDI output by exact name instead of to. Missing outputs warn while audio keeps playing. A rack's Out module (from: mo_N) sends audio to a buss or master: a rack send, added after the destination's Console 9 decode, never encoded, with level and mute only; it follows the mute and solo of the rack's home channel (ADR-0075).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string or object |  |  |  |  |
| `from.module` | string |  |  |  |  |
| `from.track` | string |  |  |  |  |
| `to` | string or object |  |  |  |  |
| `to.module` | string |  |  |  |  |
| `to.track` | string |  |  |  |  |
| `to_midi` | string |  |  |  | Exact hardware MIDI output name; requires midi: {} and replaces to/fx. |
| `midi` | object |  |  |  | MIDI send routing; {} means every channel unchanged. Channel values 0..16: from 0 = all, to 0 = original. |
| `midi.from_channel` | integer |  | 0.0..16.0 | 0 |  |
| `midi.to_channel` | integer |  | 0.0..16.0 | 0 |  |
| `fx` | string |  |  |  | Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out |
| `tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `volume_db` | number or string | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `mono` | boolean |  |  |  | Sum the source to mono before the pan |
| `phase_invert` | boolean |  |  |  |  |
| `channels` | object |  |  |  | Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity |
| `channels.from` | array |  |  |  |  |
| `channels.to` | array |  |  |  |  |

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

### Errors

- `route.cycle`: the send would feed back Hint: choose another destination
- `route.no_sidechain`: the plug-in has no key input Hint: pick another plug-in
- `route.endpoint`: MIDI sends need channel or note-module endpoints Hint: use from: {module: mo_N}, to: {track: tr_N}, midi: {}
- `route.midi`: MIDI routing or audio controls do not apply to these endpoints Hint: use channel/Note Out to channel/Notes for MIDI; omit audio controls
- `route.rack_send`: a rack send has level and mute only Hint: pan, polarity and channel routing belong in the rack or on the buss

### Examples

**Send a track to the master** (`song`)

```json
{
  "from": "tr_1",
  "to": "tr_0",
  "volume_db": "-6 dB"
}

```

Result:

```json
{
  "id": "sd_1"
}

```

**Key a plug-in from a track, before its fader** (`song_with_keymix`)

```json
{
  "from": "tr_1",
  "fx": "fx_1",
  "tap": "pre_fader"
}

```

Result:

```json
{
  "id": "sd_1"
}

```

**Play a channel's instrument from Note Out** (`song_with_note_out`)

```json
{
  "from": {
    "module": "mo_4"
  },
  "to": {
    "track": "tr_2"
  },
  "midi": {}
}

```

Result:

```json
{
  "id": "sd_2"
}

```

**Send a rack's Out to a buss** (`song_with_rack_buss`)

```json
{
  "from": {
    "module": "mo_1"
  },
  "to": "tr_2",
  "volume_db": "-6 dB"
}

```

Result:

```json
{
  "id": "sd_2"
}

```

## `route.learn.list@1`

List the MIDI controller bindings of send controls.

Every send's, or one's (route). control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "route_learn_list",
  "properties": {
    "learn": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["learn"],
  "additionalProperties": false
}

```

### Examples

**Every binding** (`song_with_mix_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "route": "sd_1",
      "param": "volume",
      "midi": {
        "kind": "cc",
        "channel": 1,
        "number": 12
      },
      "control": "CC 12 · ch 1"
    }
  ]
}

```

## `route.learn.remove@1`

Remove a send control's MIDI controller binding.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |
| `param` | string |  |  |  |  |

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

**Free the level** (`song_with_mix_learn`)

```json
{
  "route": "sd_1",
  "param": "volume"
}

```

Result:

```json
{}

```

## `route.learn.set@1`

Let a MIDI controller move a send's level, pan, mute or tap.

One binding per control (a new one replaces it); saved with the project. param: volume, pan, mute, phase_invert, mono, tap:post_fader, tap:pre_fader, tap:pre_fx. midi.device: a substring of the MIDI input's name (empty: any). A level or pan follows a knob (mode absolute, soft_takeover: once it reaches the value) or an encoder (relative1 127 = -1, relative2 63 = -1, relative3 65 = -1). A button: a note or program change flips it per press, as a CC in toggle mode does; an absolute CC is on above 63. A tap:… control sets that tap on each press.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, pan, mute, phase_invert, mono, tap:post_fader, tap:pre_fader, tap:pre_fx |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "route_learn_set_result",
  "properties": {
    "param": {
      "type": "string"
    },
    "control": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["param", "control"],
  "additionalProperties": false
}

```

### Errors

- `route.learn_param`: no such control Hint: use one of: volume, pan, mute, phase_invert, mono, tap:post_fader, tap:pre_fader, tap:pre_fx
- `learn.mode`: toggle on a level or pan Hint: use absolute or a relative mode

### Examples

**Mute the send from a pad** (`song_with_route`)

```json
{
  "route": "sd_1",
  "param": "mute",
  "midi": {
    "kind": "note",
    "channel": 10,
    "number": 36
  }
}

```

Result:

```json
{
  "param": "mute",
  "control": "Note C2 · ch 10"
}

```

## `route.list@1`

List sends with endpoints and mixer settings.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "route_list",
  "properties": {
    "routes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "route_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "from": {
            "type": "string"
          },
          "to": {
            "type": "string"
          },
          "fx": {
            "type": "string"
          },
          "tap": {
            "type": "string"
          },
          "volume_db": {
            "type": "number"
          },
          "pan": {
            "oneOf": [
              {
                "anyOf": [
                  {
                    "type": "number",
                    "minimum": -100.0,
                    "maximum": 100.0
                  },
                  {
                    "type": "string",
                    "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*%)?\\s*$"
                  }
                ],
                "x-unit": "%",
                "x-unit-strings": true,
                "description": "Quantity strings are range-checked by the server."
              },
              {
                "const": "follow"
              }
            ]
          },
          "mute": {
            "type": "boolean"
          },
          "mono": {
            "type": "boolean"
          },
          "phase_invert": {
            "type": "boolean"
          },
          "channels": {
            "type": "object",
            "title": "channel_map",
            "properties": {
              "from": {
                "type": "array",
                "items": {
                  "type": "integer",
                  "minimum": 1.0,
                  "maximum": 128.0
                }
              },
              "to": {
                "type": "array",
                "items": {
                  "type": "integer",
                  "minimum": 1.0,
                  "maximum": 128.0
                }
              }
            },
            "required": ["from", "to"],
            "additionalProperties": false
          },
          "midi": {
            "type": "object",
            "title": "midi_routing",
            "properties": {
              "from_channel": {
                "type": "integer",
                "minimum": 0.0,
                "maximum": 16.0,
                "default": 0
              },
              "to_channel": {
                "type": "integer",
                "minimum": 0.0,
                "maximum": 16.0,
                "default": 0
              }
            },
            "additionalProperties": false
          },
          "from_midi": {
            "type": "string"
          },
          "to_midi": {
            "type": "string"
          }
        },
        "required": [
          "id",
          "from",
          "to",
          "fx",
          "tap",
          "volume_db",
          "pan",
          "mute",
          "mono",
          "phase_invert",
          "channels"
        ],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["routes"],
  "additionalProperties": false
}

```

### Examples

**No sends yet** (`empty`)

```json
{}

```

Result:

```json
{
  "routes": []
}

```

## `route.matrix@1`

Read audio or MIDI endpoints, existing sends and blocked pairs.

mode: audio (default), midi or hardware_midi (channels/Note Out and hardware inputs to channels/hardware outputs). MIDI sources are channels and Note Out; destinations are channels and Notes, with no audio controls. Audio sources: tracks in tree order (depth, parent, parent_send). destinations: tracks (kind track), plug-in keys (kind key: fx with a sidechain input) and racks' In modules (kind module: mo_N, named "<rack> » <In>", with rack and home track), each track's keys and In modules after it. Each channel's rack Out modules follow it among sources (type rack_out, named "<rack> » <Out>", with rack and home track): they send only to busses and masters (their rule), added after the decode, never encoded (ADR-0075). A pair is blocked when the source's rule applies (masters feed only keys), the destination's rule applies (channels take only hardware audio), it is the same track, or it is listed in cycles (the destination already feeds the source; for an In module, its rack's Outs reach the source; path names its ids, names the same by name). sends: existing routes per pair (to: a track, an fx or an In module). Otherwise route.add accepts the pair (an In module: route.add {from, to: mo_N}). A source's channels past 2 are auxiliary (ADR-0020); channel_names names those a multi-out plug-in writes ({"3": "Kick"}), and route.add {channels: {from: [3, 4], to: [1, 2]}} sends that pair.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `mode` | string |  |  | "audio" |  |

### Result

```json
{}

```

### Examples

**A song's routing** (`song`)

```json
{}

```

Result:

```json
{
  "sources": "<any>",
  "destinations": "<any>",
  "sends": "<any>",
  "cycles": "<any>"
}

```

## `route.remove@1`

Remove a send.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  | Route id, e.g. "sd_1". |

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

**Remove** (`song_with_route`)

```json
{
  "route": "sd_1"
}

```

Result:

```json
{}

```

## `route.set@1`

Change a route's endpoints, tap, level, pan, mute, or channel map.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  | Route id, e.g. "sd_1". |
| `from` | string or object |  |  |  |  |
| `from.module` | string |  |  |  |  |
| `from.track` | string |  |  |  |  |
| `to` | string or object |  |  |  |  |
| `to.module` | string |  |  |  |  |
| `to.track` | string |  |  |  |  |
| `to_midi` | string |  |  |  | Exact hardware MIDI output name; requires midi: {} and replaces to/fx. |
| `midi` | object |  |  |  | MIDI send routing; {} means every channel unchanged. Channel values 0..16: from 0 = all, to 0 = original. |
| `midi.from_channel` | integer |  | 0.0..16.0 | 0 |  |
| `midi.to_channel` | integer |  | 0.0..16.0 | 0 |  |
| `fx` | string |  |  |  | Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out |
| `tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `volume_db` | number or string | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `mono` | boolean |  |  |  | Sum the source to mono before the pan |
| `phase_invert` | boolean |  |  |  |  |
| `channels` | object |  |  |  | Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity |
| `channels.from` | array |  |  |  |  |
| `channels.to` | array |  |  |  |  |

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

### Errors

- `route.channels`: the map pairs channels the tracks do not have Hint: pair 1 and 2: {from: [1, 2], to: [2, 1]}
- `route.endpoint`: MIDI sends need channel or note-module endpoints Hint: send channel/Note Out MIDI to a channel or Notes module
- `route.midi`: audio controls do not apply to MIDI routes Hint: use mute and MIDI channel routing
- `route.rack_send`: a rack send has level and mute only Hint: pan, polarity and channel routing belong in the rack or on the buss
- `route.in_use`: sd_1 is tr_1's input, not a send Hint: keep sd_1 as tr_1's input; add a rack send with route.add

### Examples

**Mute a route** (`song_with_route`)

```json
{
  "route": "sd_1",
  "mute": true
}

```

Result:

```json
{}

```

**Swap left and right** (`song_with_route`)

```json
{
  "route": "sd_1",
  "channels": {
    "from": [1, 2],
    "to": [2, 1]
  }
}

```

Result:

```json
{}

```

## `scene.add@1`

Add a scene (launcher column) at the end or an index.

at is the column (0-based, default: after the last); the scenes from there on move one column right. A project holds at most 1024 scenes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `at` | integer |  | 0.0..1024.0 |  | Column, 0-based. |

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

### Errors

- `scene.limit`: the project has 1024 scenes Hint: remove a scene first
- `scene.index`: at is past the last column Hint: use 0 to the scene count

### Examples

**A verse** (`song`)

```json
{
  "name": "Verse"
}

```

Result:

```json
{
  "id": "sc_1"
}

```

## `scene.capture@1`

Make a scene from a section's items, one clip per track.

For each channel the items overlapping the region must be one item lying wholly inside it: several are scene.capture.items, one that starts before or ends after it scene.capture.edge, one that starts after its start scene.capture.offset (a clip begins at its own start); a region holding no item is scene.capture.empty. Items on a master are skipped with a warning. Each item becomes a clip as clip.add {item} makes it (MIDI copied unless pooled; a warped take is clip.unsupported): loop mode when it fills the region, once when it is shorter, named as the item is (its active take's name). The scene is added after the last, named after the region unless name is given, with its colour. The region and the items are unchanged. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `region` | string |  |  |  | Region id, e.g. "rg_1". |
| `name` | string |  |  |  |  |
| `pooled` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "scene_capture",
  "properties": {
    "scene": {
      "type": "string"
    },
    "clips": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "laid_clip",
        "properties": {
          "clip": {
            "type": "string"
          },
          "source_item": {
            "type": "string"
          },
          "track": {
            "type": "string"
          }
        },
        "required": ["clip", "source_item", "track"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["scene", "clips"],
  "additionalProperties": false
}

```

### Errors

- `scene.capture.empty`: the region holds no item Hint: choose a section with items
- `scene.capture.items`: several items of one track inside the region Hint: consolidate them (item.consolidate) or capture a smaller region
- `scene.capture.edge`: an item starts before or ends after the region Hint: split it at the region's edge (item.split) or widen the region
- `scene.capture.offset`: an item starts after the region's start Hint: move it to the start of the region (item.move)
- `clip.unsupported`: a warped take Hint: reset its warp first
- `scene.limit`: the project has 1024 scenes Hint: remove a scene first
- `clip.limit`: the project has 16384 clips Hint: remove clips first

### Examples

**Capture the verse** (`song_with_section`)

```json
{
  "region": "rg_1"
}

```

Result:

```json
{
  "scene": "<any>",
  "clips": "<any>"
}

```

## `scene.insert@1`

Lay a scene's clips on the timeline as items and a section.

The span starts at bar (1-based) or at (beats, on a bar line) and lasts bars whole bars. Each clip becomes an item on its track at the start, as item.add {clip} makes it: a once clip lasts its length or the span's if shorter; a loop clip lasts the span, as one looping item when it repeats within it, which needs its period to be its content: loop off with its length (as clip.add makes it), or loop on over the whole content with a length of whole repeats (scene.insert.period otherwise). Takes are copied, MIDI with them unless pooled; an item takes its clip's name (its active take's) when the clip has one. Notes are copied as they are, and context.mismatch warns once for each item whose clip's key is not the project's under it. An item already in the span on a track that receives a clip is refused (scene.insert.occupied), and an empty scene (scene.empty). A section named after the scene (name overrides) is added over the span. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `at` | number |  | 0.0.. |  | Start in beats, on a bar line. |
| `bar` | integer |  | 1.0..1e+05 |  | Start bar, 1-based. |
| `bars` | integer |  | 1.0.. |  |  |
| `name` | string |  |  |  |  |
| `pooled` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "scene_insert",
  "properties": {
    "items": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "laid_clip",
        "properties": {
          "clip": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "track": {
            "type": "string"
          }
        },
        "required": ["clip", "item", "track"],
        "additionalProperties": false
      }
    },
    "region": {
      "type": "string"
    },
    "start": {
      "type": "number",
      "description": "The span's start, in beats."
    },
    "end": {
      "type": "number",
      "description": "The span's end, in beats."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["items", "region", "start", "end"],
  "additionalProperties": false
}

```

### Errors

- `scene.at`: at is not on a bar line, or neither or both of at and bar Hint: give bar, or an at on a bar line
- `scene.bars`: bars is out of range Hint: give a whole number of bars from 1
- `scene.empty`: the scene has no clips Hint: add clips with clip.add
- `scene.insert.occupied`: an item is already in the span on a track that gets a clip Hint: move or remove it, or choose another bar
- `scene.insert.period`: a repeating clip no single looping item reproduces Hint: loop the clip over its whole content, or ask for no more bars than it is long

### Examples

**A verse at bar 5** (`song_with_clip`)

```json
{
  "scene": "sc_1",
  "bar": 5,
  "bars": 4
}

```

Result:

```json
{
  "items": "<any>",
  "region": "<any>",
  "start": 16.0,
  "end": 32.0
}

```

## `scene.list@1`

List scenes in column order with clip counts.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "scene_list",
  "properties": {
    "scenes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "scene_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "order": {
            "type": "integer"
          },
          "clips": {
            "type": "integer"
          }
        },
        "required": ["id", "name", "order", "clips"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["scenes"],
  "additionalProperties": false
}

```

### Examples

**Two scenes** (`song_with_clip`)

```json
{}

```

Result:

```json
{
  "scenes": [
    {
      "id": "sc_1",
      "name": "Verse",
      "order": 0,
      "clips": 1
    },
    {
      "id": "sc_2",
      "name": "Chorus",
      "order": 1,
      "clips": 0
    }
  ]
}

```

## `scene.move@1`

Move a scene to another column.

to is the column it ends up in (0-based); the others close up around it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `to` | integer |  | 0.0..1023.0 |  | Column. |

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

### Errors

- `scene.index`: to is past the last column Hint: use 0 to the count minus 1

### Examples

**Chorus first** (`song_with_clip`)

```json
{
  "scene": "sc_2",
  "to": 0
}

```

Result:

```json
{}

```

## `scene.remove@1`

Remove a scene with its clips and their takes.

The scenes after it move one column left. A clip.removed warning names the clips removed with it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |

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

**An empty scene** (`song_with_clip`)

```json
{
  "scene": "sc_2"
}

```

Result:

```json
{}

```

## `scene.set@1`

Rename or recolour a scene.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Rename** (`song_with_clip`)

```json
{
  "scene": "sc_1",
  "name": "Verse 1"
}

```

Result:

```json
{}

```

## `script.run@1`

Run a Lua script on the command bus.

Every call the script makes goes through the command registry, so it is validated, transactional and in the undo history like a call from the GUI or an agent. Use it for loops, conditionals and arithmetic that the command set cannot express; use batch for a fixed list of steps. Not transactional itself: undo the edits it made with history.undo.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `code` | string |  |  |  | The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays). |
| `name` | string |  |  |  | The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name. |
| `budget_ms` | number |  | 1.0..10000.0 |  | Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count. |
| `args` | value |  |  |  | Converted to the global table `args`. |

### Result

```json
{
  "type": "object",
  "title": "script_run_result",
  "properties": {
    "result": {
      "description": "What the script returned (null if it returned nothing)."
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "What the script printed."
    },
    "log_dropped": {
      "type": "integer",
      "description": "Cumulative lines evicted from this script's retained log."
    },
    "script": {
      "type": "string",
      "description": "The name it ran under."
    },
    "instructions": {
      "type": "integer",
      "description": "Lua instructions the run executed."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["result", "log", "log_dropped", "script", "instructions"],
  "additionalProperties": false
}

```

### Errors

- `script.syntax`: the source does not compile Hint: fix the Lua syntax at the reported line
- `script.budget`: the run spent its instruction budget Hint: raise budget_ms or make the script do less between waits
- `script.memory`: the run hit the script's memory cap Hint: raise memory_limit_bytes or hold less data
- `script.error`: the script raised a Lua error Hint: read the message and the traceback
- `script.arguments`: a command was called with something other than a table Hint: call it with a table of arguments, e.g. q.track.add{name = "Bass"}
- `script.unavailable`: no command dispatcher is attached to the script Hint: run the script through a host that provides a Runtime

### Examples

**A script that computes, with no commands** (`empty`)

```json
{
  "code": "local total = 0\nfor i = 1, 4 do total = total + i end\nreturn {total = total, steps = {1, 2, 3, 4}}"
}

```

Result:

```json
{
  "result": {
    "total": 10,
    "steps": [1, 2, 3, 4]
  },
  "log": [],
  "log_dropped": 0,
  "script": "<any>",
  "instructions": "<any>"
}

```

## `session.describe@1`

Describe the session in compact text: tracks, items, fx, routes, key.

The quickest way to see where things are; use the IDs it shows with other commands.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "text",
  "properties": {
    "text": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["text"],
  "additionalProperties": false
}

```

### Examples

**Describe** (`song`)

```json
{}

```

Result:

```json
{
  "text": "<any>"
}

```

## `sync.get@1`

Show external sync settings and what the engine hears from the master.

source: off, midi_clock, mtc or ltc. midi_input: the MIDI input port the master's clock or MTC arrives on (its name, or a part of it). ltc_input: the device input channel carrying LTC (1-based). follow_tempo (midi_clock): chase the master's tempo with the live playrate (pitch preserved); off follows Start, Continue, Stop and Song Position only. offset_ms: positive plays Steppe that much behind the master. on_loss: stop, or freewheel for freewheel_frames timecode frames, when the signal stops. ltc_output: write LTC of the transport (the project's timecode) to ltc_output_channel, replacing whatever a master plays there, at ltc_level_db. status: present (the signal arrives), running, locked, state (idle, acquiring, relocating, chasing, locked), the master's bpm (midi_clock) or timecode and rate (mtc, ltc), chase_rate (the live playrate the chase applies on top of the saved one) and error_ms.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "sync_info",
  "properties": {
    "settings": {},
    "status": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings", "status"],
  "additionalProperties": false
}

```

### Examples

**Off** (`empty`)

```json
{}

```

Result:

```json
{
  "settings": {
    "source": "off",
    "midi_input": "",
    "ltc_input": 1,
    "follow_tempo": true,
    "offset_ms": 0.0,
    "on_loss": "stop",
    "freewheel_frames": 25,
    "ltc_output": false,
    "ltc_output_channel": 3,
    "ltc_level_db": -18.0
  },
  "status": "<any>"
}

```

## `sync.set@1`

Set external sync (MIDI clock, MTC, LTC) and LTC output; returns them.

source: off, midi_clock, mtc or ltc. midi_input: the MIDI input port the master's clock or MTC arrives on (its name, or a part of it). ltc_input: the device input channel carrying LTC (1-based). follow_tempo (midi_clock): chase the master's tempo with the live playrate (pitch preserved); off follows Start, Continue, Stop and Song Position only. offset_ms: positive plays Steppe that much behind the master. on_loss: stop, or freewheel for freewheel_frames timecode frames, when the signal stops. ltc_output: write LTC of the transport (the project's timecode) to ltc_output_channel, replacing whatever a master plays there, at ltc_level_db. Omitted members keep their values. Machine settings: not an undo step, not saved in the project (the app keeps them with its preferences).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | One of: off, midi_clock, mtc, ltc. |
| `midi_input` | string |  |  |  |  |
| `ltc_input` | integer |  | 1..64 |  |  |
| `follow_tempo` | boolean |  |  |  |  |
| `offset_ms` | number |  | -10000..10000 |  |  |
| `on_loss` | string |  |  |  | One of: stop, freewheel. |
| `freewheel_frames` | integer |  | 1..300 |  |  |
| `ltc_output` | boolean |  |  |  |  |
| `ltc_output_channel` | integer |  | 1..64 |  |  |
| `ltc_level_db` | number |  | -60..0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "sync_info",
  "properties": {
    "settings": {},
    "status": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings", "status"],
  "additionalProperties": false
}

```

### Examples

**Chase MTC from a MIDI interface** (`empty`)

```json
{
  "source": "mtc",
  "midi_input": "Interface"
}

```

Result:

```json
{
  "settings": {
    "source": "mtc",
    "midi_input": "Interface",
    "ltc_input": 1,
    "follow_tempo": true,
    "offset_ms": 0.0,
    "on_loss": "stop",
    "freewheel_frames": 25,
    "ltc_output": false,
    "ltc_output_channel": 3,
    "ltc_level_db": -18.0
  },
  "status": "<any>"
}

```

## `take.activate@1`

Choose the take heard from an item or clip.

take (its id) or index (0-based in the item's take order — the same position after item.split, which copies the takes in order: comping splits an item and picks a take by index on the piece).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `index` | integer |  | 0.0.. |  |  |

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

**Activate** (`song_with_item`)

```json
{
  "item": "it_1",
  "take": "tk_1"
}

```

Result:

```json
{}

```

## `take.add@1`

Add a take to an item or clip, optionally copied from one of its takes.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `from` | string |  |  |  | Take id, e.g. "tk_1". |

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

**Copy a take** (`song_with_item`)

```json
{
  "item": "it_1",
  "from": "tk_1"
}

```

Result:

```json
{
  "id": "tk_2"
}

```

## `take.extract@1`

Take a take out of its item or clip into an item of its own on any channel.

The new item copies the old one's length, fades, gain and timebase and starts at position (beats; default: where the item starts). The take keeps its FX and envelopes. The only take of an item moves the item itself. From a clip, position is required (a clip has no place in time), track defaults to the clip's, the item is as long as the clip, and its loop and take envelopes convert at the tempo where it lands (ADR-0097); the clip goes with its last take.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `track` | string |  |  |  | Default: the item's or clip's track. |
| `position` | number |  | 0.0.. |  | Beats. |

### Result

```json
{
  "type": "object",
  "title": "take_extract_result",
  "properties": {
    "item": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["item"],
  "additionalProperties": false
}

```

### Examples

**Move take 2 to beat 8** (`song_with_takes`)

```json
{
  "take": "tk_2",
  "track": "tr_1",
  "position": 8
}

```

Result:

```json
{
  "item": "it_2"
}

```

## `take.hits.detect@1`

Detect transient candidates in audio takes, then commit one undoable edit.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `takes` | array |  |  |  |  |
| `sensitivity` | number |  | 0.0..1.0 | 0.5 |  |

### Result

```json
{
  "type": "object",
  "title": "audio_job",
  "properties": {
    "job": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["job"],
  "additionalProperties": false
}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Detect attacks** (`song_with_audio`)

```json
{
  "takes": ["tk_1"]
}

```

Result:

```json
{
  "job": "<any>"
}

```

## `take.hits.set@1`

Replace a take's transient candidates in original source seconds.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `positions` | array |  |  |  |  |
| `source_hash` | string |  |  | "" |  |
| `algorithm` | string |  |  | "manual" |  |
| `sensitivity` | number |  | 0.0..1.0 | 0.5 |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Mark attacks** (`song_with_audio`)

```json
{
  "take": "tk_1",
  "positions": [0.1, 0.3]
}

```

Result:

```json
"<any>"

```

## `take.list@1`

List the takes of an item or clip and identify its active take.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |

### Result

```json
{
  "type": "object",
  "title": "take_list",
  "properties": {
    "takes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "take_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "source": {
            "type": "string"
          },
          "active": {
            "type": "boolean"
          },
          "gain_db": {
            "type": "number"
          },
          "pan": {
            "type": "number"
          },
          "start_offset": {
            "type": "number"
          },
          "playrate": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:x|×))?\\s*$"
              }
            ],
            "x-unit": "x",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "preserve_pitch": {
            "type": "boolean"
          },
          "pitch": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:st|semi|semitones))?\\s*$"
              }
            ],
            "x-unit": "st",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "pitch_mode": {
            "type": "string"
          },
          "stretch_markers": {},
          "transients": {},
          "warp_peers": {},
          "hits_stale": {
            "type": "boolean"
          }
        },
        "required": [
          "id",
          "name",
          "source",
          "active",
          "gain_db",
          "pan",
          "start_offset",
          "playrate",
          "preserve_pitch",
          "pitch",
          "pitch_mode",
          "stretch_markers",
          "transients",
          "warp_peers",
          "hits_stale"
        ],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["takes"],
  "additionalProperties": false
}

```

### Examples

**List** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "takes": "<any>"
}

```

## `take.remove@1`

Delete a take from its item or clip (which goes with its last take).

The take's FX, envelopes and unshared MIDI go with it. When it was the heard take, the next take is heard (else the previous one). item: the item or clip that held it; item_removed: it was the only take, so that item or clip was removed too.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |

### Result

```json
{
  "type": "object",
  "title": "take_remove_result",
  "properties": {
    "item": {
      "type": "string"
    },
    "item_removed": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["item", "item_removed"],
  "additionalProperties": false
}

```

### Examples

**Delete the second take** (`song_with_takes`)

```json
{
  "take": "tk_2"
}

```

Result:

```json
{
  "item": "it_1",
  "item_removed": false
}

```

## `take.set@1`

Set a take's gain, pan, channel mode, name, offset, playrate or pitch.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `gain_db` | number or string | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `pan` | number or string | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `channel_mode` | string |  |  |  | One of: normal, reverse_stereo, mono_mix, left, right. |
| `name` | string |  |  |  |  |
| `rebase_seconds` | number |  |  |  | Rebase an audio take after trimming its item start; preserves the warp map. |
| `start_offset` | number |  |  |  | Source position at the item start; negative for a late first pass. |
| `playrate` | number or string | x | 0.01..100.0 |  | Playback speed: 2 plays the source twice as fast. Its length in seconds is the item's, so the take runs out sooner or loops. Quantity strings are range-checked by the server. |
| `preserve_pitch` | boolean |  |  |  | Keep the source's pitch when the playrate changes (the default). With pitch_mode "resample" the pitch follows the speed. |
| `pitch` | number or string | st | -48.0..48.0 |  | Pitch offset in semitones. Stretched when the pitch is preserved, otherwise a further speed factor of 2^(pitch/12). Quantity strings are range-checked by the server. |
| `pitch_mode` | string |  |  |  | Time-stretch algorithm: "signalsmith" (pitch-preserving, the default) or "resample" (varispeed). One of: resample, signalsmith. |

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

**Lower a take** (`song_with_item`)

```json
{
  "take": "tk_1",
  "gain_db": "-3 dB"
}

```

Result:

```json
{}

```

**Twice as fast, pitch and all** (`song_with_item`)

```json
{
  "take": "tk_1",
  "playrate": 2,
  "preserve_pitch": false
}

```

Result:

```json
{}

```

**Half speed, pitch kept** (`song_with_item`)

```json
{
  "take": "tk_1",
  "playrate": 0.5
}

```

Result:

```json
{}

```

**An octave up, same tempo** (`song_with_item`)

```json
{
  "take": "tk_1",
  "pitch": 12
}

```

Result:

```json
{}

```

## `take.stretch.add@1`

Add a pitch-preserving audio stretch marker.

Omit source_pos to insert a point on the current source map at item_pos without changing its timing. An identical existing point is not duplicated. Warp peers receive the same source-relative point; ordinary edit groups stay local, as with other take edits. Requires nonlooped pitch-preserving audio.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `source_pos` | number |  |  |  |  |
| `item_pos` | number |  | 0.0.. |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Edit an anchor** (`song_with_audio`)

```json
{
  "take": "tk_1",
  "source_pos": 0.3,
  "item_pos": 0.25
}

```

Result:

```json
"<any>"

```

**Insert an anchor without moving audio** (`song_with_audio`)

```json
{
  "take": "tk_1",
  "item_pos": 0.25
}

```

Result:

```json
"<any>"

```

## `take.stretch.move@1`

Move an audio stretch marker's item position.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `index` | integer |  |  |  |  |
| `item_pos` | number |  | 0.0.. |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Edit an anchor** (`song_with_warp`)

```json
{
  "take": "tk_1",
  "index": 1,
  "item_pos": 0.26
}

```

Result:

```json
"<any>"

```

## `take.stretch.remove@1`

Remove an interior audio stretch marker.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `index` | integer |  |  |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Edit an anchor** (`song_with_warp`)

```json
{
  "take": "tk_1",
  "index": 1
}

```

Result:

```json
"<any>"

```

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

## `timesig.remove@1`

Remove the meter change at a bar: the meter before it continues.

Bar 1's meter cannot be removed (change it with timesig.set).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bar` | integer |  | 2.0.. |  |  |

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

**Back to 4/4 at bar 9** (`meter_change`)

```json
{
  "bar": 9
}

```

Result:

```json
{}

```

## `timesig.set@1`

Set the meter from a bar on, e.g. {"bar": 9, "meter": "3/4"}.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bar` | integer |  | 1.0.. |  |  |
| `meter` | string |  |  |  |  |

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

**Waltz from bar 9** (`empty`)

```json
{
  "bar": 9,
  "meter": "3/4"
}

```

Result:

```json
{}

```

## `track.add@1`

Add a track (a channel, buss or master) and return its id.

parent: a buss/master id (default tr_0); masters have no parent; index: position among its siblings; name: default the first free ch_N, buss_N or mstr_N. A new channel gets the user's default monitoring (on unless set otherwise; monitoring needs the track armed). color: a track colour's name or "#rrggbb", "" for none; default for a channel or buss the palette colour the fewest tracks carry (red first), for a master none.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  | "channel" | One of: channel, buss, master. |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `parent` | string |  |  |  | Parent track id, e.g. "tr_1". |
| `index` | integer |  | 0.0.. |  |  |
| `channels` | integer |  | 1.0..128.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_ref",
  "properties": {
    "track": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["track"],
  "additionalProperties": false
}

```

### Examples

**Add a bass track** (`empty`)

```json
{
  "name": "Bass"
}

```

Result:

```json
{
  "track": "tr_1"
}

```

**Add a green buss for the drums** (`empty`)

```json
{
  "name": "Drums",
  "type": "buss",
  "color": "green"
}

```

Result:

```json
{
  "track": "tr_1"
}

```

## `track.describe@1`

Describe track groups and requested/applied effective gain at a time.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `at` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_description",
  "properties": {
    "track": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "type": {
      "type": "string",
      "enum": ["channel", "buss", "master"]
    },
    "groups": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "vcas": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "volume_db": {
      "type": "number"
    },
    "requested_effective_volume_db": {
      "type": "number"
    },
    "effective_volume_db": {
      "type": "number"
    },
    "at": {
      "type": "number"
    },
    "effective_mute": {
      "type": "boolean"
    },
    "solo_silenced": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "track",
    "name",
    "type",
    "groups",
    "vcas",
    "volume_db",
    "requested_effective_volume_db",
    "effective_volume_db",
    "at",
    "effective_mute",
    "solo_silenced"
  ],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Inspect effective gain** (`song`)

```json
{
  "track": "tr_1",
  "at": 0
}

```

Result:

```json
{
  "track": "tr_1",
  "name": "Bass",
  "type": "channel",
  "groups": [],
  "vcas": [],
  "volume_db": 0.0,
  "requested_effective_volume_db": 0.0,
  "effective_volume_db": 0.0,
  "at": 0.0,
  "effective_mute": false,
  "solo_silenced": false
}

```

## `track.duplicate@1`

Duplicate channels or busses with their FX, items, sends and automation.

Each copy goes right after its original, under the same parent, with the same settings and name; its main and input chains (each plugin's settings as it sounds now, as fx.copy), items and takes (unpooled MIDI copied, audio media shared), sends and record input, and the envelopes on the track, its plugins, sends and takes. Not copied: a folder's children, sends into the track, and item groups. Masters cannot be duplicated. One undo step; the copies' ids in the order given.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `tracks` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_duplicate_result",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks"],
  "additionalProperties": false
}

```

### Errors

- `track.unknown`: no such track Hint: use a live id from track.list
- `track.strip_type`: a master cannot be duplicated Hint: duplicate channels and busses

### Examples

**Duplicate a track with its plugins, item and automation** (`song_with_env`)

```json
{
  "tracks": ["tr_1"]
}

```

Result:

```json
{
  "tracks": ["tr_2"]
}

```

## `track.freeze@1`

Freeze a channel's items and FX to pre-fader audio and release its plugins.

One undo step after rendering; job:true reports progress through job.get. Fader, pan and outgoing sends remain live. Unfreeze to edit the captured sound.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `freeze.unsupported`: the channel has unsupported live dependencies Hint: disarm it and remove external inputs; use unity master playrate
- `freeze.stale_target`: sound dependencies changed during rendering Hint: retry track.freeze with the current sound
- `freeze.tail_limit`: the tail is infinite or did not settle Hint: disable continuous sound or infinite feedback before freezing

### Examples

**Freeze the channel** (`song_with_fx`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{
  "track": "tr_1",
  "frozen": true,
  "source": "<any>",
  "path": "<any>",
  "frames": "<any>",
  "tail_frames": "<any>"
}

```

## `track.freeze.commit@1`

Attach a verified freeze artifact as one replayable edit.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `expected` | string |  |  |  |  |
| `media` | object |  |  |  |  |
| `media.path` | string |  |  |  |  |
| `media.hash` | string |  |  |  |  |
| `media.format` | string |  |  |  |  |
| `media.sample_rate` | number |  | 0.0.. |  |  |
| `media.channels` | integer |  | 0.0.. |  |  |
| `media.frames` | integer |  | 0.0.. |  |  |
| `freeze` | object |  |  |  |  |
| `freeze.source` | string |  |  |  |  |
| `freeze.content_frames` | integer |  | 1.0..268435456.0 |  |  |
| `freeze.tail_frames` | integer |  | 0.0..268435456.0 |  |  |
| `freeze.latency_frames` | integer |  | 0.0..268435456.0 |  |  |
| `freeze.dependency_hash` | string |  |  |  |  |
| `states` | array |  |  |  |  |
| `states[].fx` | string |  |  |  |  |
| `states[].value` | object |  |  |  |  |
| `states[].blob` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

## `track.learn.list@1`

List the MIDI controller bindings of track controls.

Every track's, or one's (track). control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_learn_list",
  "properties": {
    "learn": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["learn"],
  "additionalProperties": false
}

```

### Examples

**Every binding** (`song_with_mix_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "track": "tr_1",
      "param": "volume",
      "midi": {
        "kind": "cc",
        "channel": 1,
        "number": 7
      },
      "control": "CC 7 · ch 1"
    }
  ]
}

```

## `track.learn.remove@1`

Remove a track control's MIDI controller binding.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `param` | string |  |  |  |  |

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

**Free the level** (`song_with_mix_learn`)

```json
{
  "track": "tr_1",
  "param": "volume"
}

```

Result:

```json
{}

```

## `track.learn.set@1`

Let a MIDI controller move a track's fader, pan, mute, solo or arm.

One binding per control (a new one replaces it); saved with the project. param: volume, pan, width, pan_left, pan_right, pan_boost, mute, solo, phase_invert, mono, armed, monitor, lane_mute. midi.device: a substring of the MIDI input's name (empty: any). A level or pan follows a knob (mode absolute, soft_takeover: once it reaches the value) or an encoder (relative1 127 = -1, relative2 63 = -1, relative3 65 = -1). A button: a note or program change flips it per press, as a CC in toggle mode does; an absolute CC is on above 63. monitor goes off, on, auto in turn on each press.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, pan, width, pan_left, pan_right, pan_boost, mute, solo, phase_invert, mono, armed, monitor, lane_mute |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_learn_set_result",
  "properties": {
    "param": {
      "type": "string"
    },
    "control": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["param", "control"],
  "additionalProperties": false
}

```

### Errors

- `track.learn_param`: no such control Hint: use one of: volume, pan, width, pan_left, pan_right, pan_boost, mute, solo, phase_invert, mono, armed, monitor, lane_mute
- `learn.mode`: toggle on a level or pan Hint: use absolute or a relative mode

### Examples

**The fader on CC 7** (`song`)

```json
{
  "track": "tr_1",
  "param": "volume",
  "midi": {
    "kind": "cc",
    "channel": 1,
    "number": 7
  }
}

```

Result:

```json
{
  "param": "volume",
  "control": "CC 7 · ch 1"
}

```

## `track.list@1`

List tracks in tree order with their level, mute/solo, fx and items.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_list",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "track_entry",
        "properties": {
          "frozen": {
            "type": "boolean"
          },
          "freeze": {
            "type": "object",
            "title": "track_freeze",
            "properties": {
              "source": {
                "type": "string",
                "pattern": "^[a-z]{2}_[0-9]+$"
              },
              "content_frames": {
                "type": "integer",
                "minimum": 1.0,
                "maximum": 268435456.0
              },
              "tail_frames": {
                "type": "integer",
                "minimum": 0.0,
                "maximum": 268435456.0
              },
              "latency_frames": {
                "type": "integer",
                "minimum": 0.0,
                "maximum": 268435456.0
              },
              "dependency_hash": {
                "type": "string",
                "pattern": "^sha256:[0-9a-f]{64}$"
              }
            },
            "required": [
              "source",
              "content_frames",
              "tail_frames",
              "latency_frames",
              "dependency_hash"
            ],
            "additionalProperties": false
          },
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "type": {
            "type": "string",
            "enum": ["channel", "buss", "master"]
          },
          "parent": {
            "type": "string"
          },
          "depth": {
            "type": "integer"
          },
          "volume_db": {
            "type": "number"
          },
          "mute": {
            "type": "boolean"
          },
          "solo": {
            "type": "string"
          },
          "fx": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "items": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "groups": {
            "type": "array",
            "items": {
              "type": "string",
              "pattern": "^[a-z]{2}_[0-9]+$"
            }
          },
          "vcas": {
            "type": "array",
            "items": {
              "type": "string",
              "pattern": "^[a-z]{2}_[0-9]+$"
            }
          },
          "hw_output": {
            "type": "array",
            "items": {
              "type": "integer"
            },
            "description": "Masters: the device outputs they play to"
          },
          "armed": {
            "type": "boolean",
            "description": "Masters: recording takes the output"
          },
          "lane_mute": {
            "type": "boolean",
            "description": "Masters: their own items are silent"
          },
          "requested_effective_volume_db": {
            "type": "number"
          },
          "effective_volume_db": {
            "type": "number"
          },
          "at": {
            "type": "number"
          },
          "effective_mute": {
            "type": "boolean"
          },
          "solo_silenced": {
            "type": "boolean"
          }
        },
        "required": [
          "frozen",
          "id",
          "name",
          "type",
          "parent",
          "depth",
          "volume_db",
          "mute",
          "solo",
          "fx",
          "items",
          "groups",
          "vcas",
          "requested_effective_volume_db",
          "effective_volume_db",
          "at",
          "effective_mute",
          "solo_silenced"
        ],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks"],
  "additionalProperties": false
}

```

### Examples

**List** (`song`)

```json
{}

```

Result:

```json
{
  "tracks": "<any>"
}

```

## `track.move@1`

Move a track under another track or reorder it among its siblings.

parent defaults to the track's current parent (a pure reorder); index defaults to the end of the destination's children. A track cannot move under itself or one of its own descendants.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `parent` | string |  |  |  | Track id, e.g. "tr_1". |
| `index` | integer |  | 0.0.. |  |  |

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

### Errors

- `tr.cycle`: the destination is inside the track Hint: choose another parent

### Examples

**Move a track to the master** (`song`)

```json
{
  "track": "tr_1",
  "parent": "tr_0"
}

```

Result:

```json
{}

```

## `track.remove@1`

Remove a track with its items, clips, fx, routes and envelopes (not a folder).

Removes the track's racks too, except a rack another channel hears (its Out as the channel's input, or its Note Out's MIDI), which moves there. A warning names each rack removed (track.rack_removed) or moved (track.rack_moved) and its rack sends: removed with it, or following the new home's mute and solo. Its clips go with their takes, named by a clip.removed warning.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

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

**Remove** (`song`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{}

```

## `track.set@1`

Set track fields; choosing audio, MIDI or rack input replaces the others.

Only the given fields change. solo: off | on | in_place | defeat. armed + record_input (device input channels, 1-based) prepare a track for transport.record; record_midi records MIDI instead: hardware audio and MIDI or a rack Out (record_rack) replace each other: a track has one input. Choosing one clears the other kinds. null clears only the named kind; set all three to null for no input. Feed a rack's Notes through MIDI sends from another channel. A device name is a substring of the MIDI port ("*" = every device), with midi_channel 1-16 (0 = all). monitor: off | on | auto (auto: while armed) — a monitored track hears its input live (an instrument plays the keyboard), also while stopped. record_mode: input (new items), midi_overdub (merge into the MIDI item under the start, as a new take), midi_replace. color: red | orange | yellow | green | teal | blue | violet | grey | "#rrggbb" ("" clears it), shown as a stripe on the track. A master takes armed alone: transport.record then records its output (after its fader, before its own items) as an item on the master; lane_mute silences a master's items. chain_midi: bus (every plug-in with a note input hears the chain's MIDI, a note-output plug-in feeds those after it and the post-FX sends) or instrument (only the first such plug-in hears the track's MIDI, as before 2.10).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `type` | string |  |  |  | One of: channel, buss, master. |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `volume_db` | number or string | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | number or string | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `pan_mode` | string |  |  |  | Channels and busses: pan (balance a stereo image, pan a mono one), dual (pan_left and pan_right place each input on its own) or width (width narrows, widens or swaps the image, pan places it) One of: pan, dual, width. |
| `width` | number or string | % | -100.0..100.0 |  | Width mode: 100 as recorded, 0 mono, -100 sides swapped Quantity strings are range-checked by the server. |
| `pan_left` | number or string | % | -100.0..100.0 |  | Dual mode: where the left input goes (default -100) Quantity strings are range-checked by the server. |
| `pan_right` | number or string | % | -100.0..100.0 |  | Dual mode: where the right input goes (default 100) Quantity strings are range-checked by the server. |
| `pan_law` | object or null |  |  |  |  |
| `pan_law.db` | number or string | dB |  | 0.0 | Quantity strings are range-checked by the server. One of: 0.0, -2.5, -3.0, -4.5, -6.0. |
| `pan_law.boost` | boolean |  |  | false |  |
| `mute` | boolean |  |  |  |  |
| `phase_invert` | boolean |  |  |  | Flip the polarity of the main pair (after the FX chain, before the fader) |
| `mono` | boolean |  |  |  | Sum the main pair to mono (after the FX chain, before the fader and pan): a mono check on the master |
| `solo` | string |  |  |  |  |
| `armed` | boolean |  |  |  |  |
| `record_input` | array or null |  |  |  | Hardware audio input; replaces MIDI or rack input. null clears hardware audio only |
| `record_midi` | string or null |  |  |  | MIDI device; replaces hardware audio or rack input. null clears MIDI only |
| `record_rack` | string or null |  |  |  |  |
| `midi_channel` | integer |  | 0.0..16.0 |  |  |
| `record_mode` | string |  |  |  | One of: input, midi_overdub, midi_replace. |
| `monitor` | string |  |  |  | One of: off, on, auto. |
| `parent_send` | boolean |  |  |  | The implicit send to the parent buss/master (off: only explicit sends leave the track) |
| `timebase` | string |  |  |  | What this channel's items follow when the tempo changes (those that inherit it): project, beats, time or beats_position; they keep their place now One of: project, time, beats, beats_position. |
| `channels` | integer |  | 1.0..128.0 |  | 1-2: the track's main pair; up to 128 adds auxiliary channels 3-128 for routes and plug-in pins (a detector on 3/4, a multi-out instrument's outputs), outside the fader, pan and parent send |
| `hw_output` | array |  |  |  | Masters: the device outputs the main pair plays to: [l, r], [mono] or [] for none (engine.inputs lists them). Default: tr_0 [1, 2], other masters [] |
| `lane_mute` | boolean |  |  |  | Masters: silence the master's own items (its prints) and nothing else |
| `chain_midi` | string |  |  |  | What the FX chain does with MIDI: bus (default; plug-ins hear and feed it in order) or instrument (only the first plug-in with a note input hears the track's MIDI; plug-ins' note output goes nowhere) One of: bus, instrument. |

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

### Errors

- `track.record_input`: choose one input: audio, MIDI or rack Hint: provide one non-null input field; it replaces the previous input

### Examples

**Turn the bass down 4 dB and a little left** (`song`)

```json
{
  "track": "tr_1",
  "volume_db": "-4 dB",
  "pan": -15
}

```

Result:

```json
{}

```

**Arm for recording from inputs 1 and 2** (`song`)

```json
{
  "track": "tr_1",
  "armed": true,
  "record_input": [1, 2]
}

```

Result:

```json
{}

```

**Feed the parent only through explicit sends** (`song`)

```json
{
  "track": "tr_1",
  "parent_send": false
}

```

Result:

```json
{}

```

**Play and record a MIDI keyboard (any device, channel 1)** (`song`)

```json
{
  "track": "tr_1",
  "armed": true,
  "record_midi": "*",
  "midi_channel": 1,
  "monitor": "auto"
}

```

Result:

```json
{}

```

**Narrow a stereo track to half width, a little right** (`song`)

```json
{
  "track": "tr_1",
  "pan_mode": "width",
  "width": 50,
  "pan": 20
}

```

Result:

```json
{}

```

**Give a track the -3 dB pan law** (`song`)

```json
{
  "track": "tr_1",
  "pan_law": {
    "db": -3
  }
}

```

Result:

```json
{}

```

**Check the mix in mono** (`song`)

```json
{
  "track": "tr_0",
  "mono": true
}

```

Result:

```json
{}

```

**Four channels: a key signal can arrive on 3/4** (`song`)

```json
{
  "track": "tr_1",
  "channels": 4
}

```

Result:

```json
{}

```

**Play the main mix to outputs 3 and 4** (`song`)

```json
{
  "track": "tr_0",
  "hw_output": [3, 4]
}

```

Result:

```json
{}

```

**Arm the main master to print the mix** (`song`)

```json
{
  "track": "tr_0",
  "armed": true
}

```

Result:

```json
{}

```

**Hear the live mix without the print** (`song`)

```json
{
  "track": "tr_0",
  "lane_mute": true
}

```

Result:

```json
{}

```

**Only the instrument hears the track's MIDI, as before 2.10** (`song`)

```json
{
  "track": "tr_1",
  "chain_midi": "instrument"
}

```

Result:

```json
{}

```

## `track.template.delete@1`

Delete a saved track template by name.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_template_delete_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `template.unknown`: no template has that name Hint: use a name from track.template.list

### Examples

**Delete the bass track** (`song_with_saved_template`)

```json
{
  "name": "Bass"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `track.template.list@1`

List saved track templates by name, path and effect count.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "track_template_list_result",
  "properties": {
    "templates": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "track_template_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          },
          "effects": {
            "type": "integer",
            "minimum": 0.0
          }
        },
        "required": ["name", "path", "effects"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["templates"],
  "additionalProperties": false
}

```

### Examples

**None yet** (`empty`)

```json
{}

```

Result:

```json
{
  "templates": []
}

```

## `track.template.load@1`

Add a track from a track template (settings and FX) in one undo step.

Give exactly one of name (in the user's track-templates directory) or path. parent defaults to the master. Every plugin and state is checked before the model changes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `path` | string |  |  |  |  |
| `parent` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_template_load_result",
  "properties": {
    "track": {
      "type": "string"
    },
    "effects": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["track", "effects"],
  "additionalProperties": false
}

```

### Examples

**Add the saved bass track** (`song_with_saved_template`)

```json
{
  "name": "Bass"
}

```

Result:

```json
{
  "track": "<any>",
  "effects": ["<any>", "<any>"]
}

```

## `track.template.save@1`

Save a track (settings and FX chains) as a named track template.

Writes a supple.track-template/1.0 file to the user's track-templates directory: the track's type, name, colour, notes, level, pan and monitoring, and its main and input chains as fx.chain.save stores them (semantic patches or base64 state). Not routing or items. replace: true overwrites an existing name.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "track_template_save_result",
  "properties": {
    "path": {
      "type": "string"
    },
    "effects": {
      "type": "integer",
      "minimum": 0.0
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "effects"],
  "additionalProperties": false
}

```

### Examples

**Save the bass track** (`song_with_chain`)

```json
{
  "track": "tr_1",
  "name": "Bass"
}

```

Result:

```json
{
  "path": "<any>",
  "effects": 2
}

```

## `track.unfreeze@1`

Restore a frozen channel's original items and plugin state.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `freeze.restore`: saved plugins could not be restored Hint: install or repair the saved plugins before unfreezing

### Examples

**Unfreeze the channel** (`song_frozen`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{
  "track": "tr_1",
  "frozen": false
}

```

## `transport.get@1`

The play position and whether it is playing.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**Stopped at the start** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": 0.0,
  "beat": 0.0,
  "sample": 0,
  "loop": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  },
  "punch": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  },
  "counting_in": false
}

```

## `transport.loop@1`

Set or toggle the loop range: playback reaching its end repeats it.

start and end as seconds, beat or bar (start_bar 5, end_bar 9 = bars 5-8); a range turns the loop on unless enabled says otherwise; enabled alone toggles the current range; clear removes the range and turns the loop off. Recording while looping makes one take per pass on one item. Transport state: not an undo step, not saved in the project.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `clear` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0..2147483647.0 |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "loop",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, end not after start, or clear with a range Hint: e.g. start_bar 5, end_bar 9

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

**Remove the range** (`empty`)

```json
{
  "clear": true
}

```

Result:

```json
{
  "enabled": false,
  "start_seconds": 0,
  "end_seconds": 0
}

```

## `transport.metronome@1`

Turn the metronome on or off and set its level; returns the settings.

A click on every beat of the meter while the transport plays (the bar's first accented), in the live output only: renders and recordings never contain it. count_in_bars: that many bars of clicks (at the meter and tempo where playback starts) before transport.record begins — the position holds and nothing is recorded until the count ends; count_in_playback counts in before transport.play too. The count clicks even with the metronome off. Transport state: not an undo step, not saved in the project. No arguments: the current settings.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `volume_db` | number or string | dB | -60.0..6.0 |  | Quantity strings are range-checked by the server. |
| `count_in_bars` | integer |  | 0.0..8.0 |  |  |
| `count_in_playback` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "metronome",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "volume_db": {
      "type": "number"
    },
    "count_in_bars": {
      "type": "integer"
    },
    "count_in_playback": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "volume_db", "count_in_bars", "count_in_playback"],
  "additionalProperties": false
}

```

### Examples

**On** (`empty`)

```json
{
  "enabled": true
}

```

Result:

```json
{
  "enabled": true,
  "volume_db": -6.0,
  "count_in_bars": 0,
  "count_in_playback": false
}

```

**Two bars of count-in before recording** (`empty`)

```json
{
  "count_in_bars": 2
}

```

Result:

```json
{
  "enabled": false,
  "volume_db": -6.0,
  "count_in_bars": 2,
  "count_in_playback": false
}

```

## `transport.pause@1`

Pause playback where it is (ends a recording).



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.play@1`

Start playback from the current position.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.playrate.apply-to-tempo@1`

Scale every tempo BPM by the master playrate, then reset it to 1.

One version: multiply every BPM by the saved scalar rate, preserving beat positions, ramp flags and meter changes, and reset rate to 1. Require stopped transport, no recording and no active nonempty master playrate envelope. Reject resulting BPM outside 1..1000 before mutation. Existing item timebases still apply; time-locked audio is not made sound-equivalent by this tempo edit.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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

### Errors

- `playrate.playing`: the transport is playing Hint: transport.stop before applying playrate to tempo
- `record.playrate`: a recording is active Hint: transport.stop first
- `playrate.envelope`: an active nonempty master playrate envelope exists Hint: disable it with env.set {active: false} before applying the scalar rate
- `tempo.range`: a scaled BPM is outside 1..1000 Hint: reduce the master rate or adjust the tempo map first

### Examples

**Apply the factory unity rate** (`empty`)

```json
{}

```

Result:

```json
{}

```

## `transport.playrate.get@1`

Get requested/effective master playrate, pitch mode and active envelope.

requested_rate is the saved project ratio; envelope names the active nonempty tr_0/playrate envelope, including automation items. No engine is required. effective_rate follows consumed output, including compensated source transitions and graph/backend output latency; it is 1.0 without a running engine. GUI clients can use EngineApp::playrateState() without executing/logging a query each sync.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport_playrate",
  "properties": {
    "requested_rate": {
      "type": "number"
    },
    "effective_rate": {
      "type": "number"
    },
    "preserve_pitch": {
      "type": "boolean"
    },
    "envelope": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["requested_rate", "effective_rate", "preserve_pitch"],
  "additionalProperties": false
}

```

### Examples

**Factory settings** (`empty`)

```json
{}

```

Result:

```json
{
  "requested_rate": 1.0,
  "effective_rate": 1.0,
  "preserve_pitch": false
}

```

## `transport.playrate.set@1`

Set the project's master playrate ratio and preserve-pitch flag.

rate: finite 0.25..4.0 (default 1); preserve_pitch defaults false in a new project. Omitted fields keep their values. Updates during a drag use Origin.gesture like track faders, and write/touch/latch capture tr_0/playrate coarsely: unlike track levels (ADR-0061), the curve keeps playing while it is written (no hold or touch return). Recording requires unity. Settings are saved/undoable. File sources convert before take FX and live input mixing; instruments retain their pitch. Scalar edits ramp and converter changes crossfade for 10 ms after priming. Edits include the render horizon and compensated source lookahead; newer requests coalesce while a transition finishes. Long-latency take chains can exceed the usual 200 ms response. effective_rate follows audible output, independently of requested_rate.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rate` | number |  | 0.25..4.0 |  |  |
| `preserve_pitch` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "transport_playrate",
  "properties": {
    "requested_rate": {
      "type": "number"
    },
    "effective_rate": {
      "type": "number"
    },
    "preserve_pitch": {
      "type": "boolean"
    },
    "envelope": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["requested_rate", "effective_rate", "preserve_pitch"],
  "additionalProperties": false
}

```

### Errors

- `record.playrate`: recording requires unity master playrate Hint: stop recording before changing rate or enabling a non-unity rate envelope

### Examples

**Store double speed with pitch preservation** (`empty`)

```json
{
  "rate": 2,
  "preserve_pitch": true
}

```

Result:

```json
{
  "requested_rate": 2.0,
  "effective_rate": 1.0,
  "preserve_pitch": true
}

```

## `transport.punch@1`

Set or toggle the punch range: recordings keep only that range.

start and end as seconds, beat or bar; a range turns punch on unless enabled says otherwise; clear removes the range and turns punch off. The recording keeps running; the committed items cover only the punch range (the file keeps everything). Not an undo step.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `clear` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0..2147483647.0 |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "punch",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, end not after start, or clear with a range Hint: e.g. start_bar 5, end_bar 9
- `record.punch`: the active recording cannot represent this punch Hint: clear the punch or use a range within 2^48 recording frames

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

**Remove the range** (`empty`)

```json
{
  "clear": true
}

```

Result:

```json
{
  "enabled": false,
  "start_seconds": 0,
  "end_seconds": 0
}

```

## `transport.record@1`

Play and record every armed track's input or master's output.

Recording requires saved master rate 1 and no active master rate envelope that leaves 1 anywhere, including automation items. Non-unity recording and an armed track with unsupported output record mode are refused before opening files. An armed master records its output (after its fader, before its own items) as an item on the master; while it records, its items are silent in the punch range, or everywhere without punch. Armed channels in midi_overdub or midi_replace also record every incoming Note Out route, alongside an optional MIDI device input; no instrument is required. Rack Out audio inputs record stereo after input FX, before items and track inserts.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)
- `record.rack_input`: the loaded plan has no armed rack input for the target Hint: select an Out with track.set {record_rack, armed: true} and load it before recording
- `record.midi_route`: a Note Out recording target is not armed in the loaded plan Hint: arm the destination in a MIDI record mode and load it before recording
- `record.playrate`: master playrate can leave unity Hint: set rate to 1 and disable non-unity master playrate automation
- `record.output_unsupported`: output recording is not supported yet Hint: use track.set {record_mode: input} on the reported track

### Examples

**While the engine runs** (`engine_armed`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": true,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.seek@1`

Move the play position (one of seconds, beat or bar).

With master playrate enabled, prepare file-source converters off-thread before publishing the audible seek. Existing playback and live monitoring continue until ready; preparation errors leave the old seek epoch intact.

Kind: `action`. Exposure: `mcp-tool`.

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
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `transport.position`: not exactly one position Hint: give seconds, beat or bar

### Examples

**To bar 2 (120 BPM, 4/4)** (`engine_running`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": 2.0,
  "beat": 4.0,
  "sample": 96000,
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.stop@1`

Stop playback and return to where it started (ends a recording).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
              "properties": {
                "code": {
                  "type": "string"
                },
                "message": {
                  "type": "string"
                },
                "path": {
                  "type": "string"
                },
                "hint": {
                  "type": "string"
                },
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `vca.add@1`

Add an audio-free VCA fader to a track group.

VCA gain adds to member automation before the fader, capped at the Console 9 native top. Group suspension only suspends linked editing. Use vca.members.set to nest VCAs.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `volume_db` | number or string | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `solo` | boolean |  |  |  |  |
| `followers` | array |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Lower the group** (`song_with_group`)

```json
{
  "group": "gr_1",
  "volume_db": -3
}

```

Result:

```json
{}

```

## `vca.learn.list@1`

List the MIDI controller bindings of VCA controls.

Every VCA's, or one's (group). control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "vca_learn_list",
  "properties": {
    "learn": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["learn"],
  "additionalProperties": false
}

```

### Examples

**Every binding** (`song_with_mix_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "group": "gr_1",
      "param": "volume",
      "midi": {
        "kind": "cc",
        "channel": 1,
        "number": 9
      },
      "control": "CC 9 · ch 1"
    }
  ]
}

```

## `vca.learn.remove@1`

Remove a VCA control's MIDI controller binding.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `param` | string |  |  |  |  |

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

**Free the level** (`song_with_mix_learn`)

```json
{
  "group": "gr_1",
  "param": "volume"
}

```

Result:

```json
{}

```

## `vca.learn.set@1`

Let a MIDI controller move a VCA's fader, mute or solo.

One binding per control (a new one replaces it); saved with the project. param: volume, mute, solo. midi.device: a substring of the MIDI input's name (empty: any). A level or pan follows a knob (mode absolute, soft_takeover: once it reaches the value) or an encoder (relative1 127 = -1, relative2 63 = -1, relative3 65 = -1). A button: a note or program change flips it per press, as a CC in toggle mode does; an absolute CC is on above 63. The bindings go with the VCA.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, mute, solo |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "vca_learn_set_result",
  "properties": {
    "param": {
      "type": "string"
    },
    "control": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["param", "control"],
  "additionalProperties": false
}

```

### Errors

- `vca.learn_param`: no such control Hint: use one of: volume, mute, solo
- `learn.mode`: toggle on a level or pan Hint: use absolute or a relative mode

### Examples

**The VCA's fader on CC 9** (`song_with_vca`)

```json
{
  "group": "gr_1",
  "param": "volume",
  "midi": {
    "kind": "cc",
    "channel": 1,
    "number": 9
  }
}

```

Result:

```json
{
  "param": "volume",
  "control": "CC 9 · ch 1"
}

```

## `vca.members.set@1`

Replace the tracks or nested VCAs controlled by a VCA.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `tracks` | array |  |  |  |  |
| `vcas` | array |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Assign tracks** (`song_with_vca`)

```json
{
  "group": "gr_1",
  "tracks": ["tr_1"],
  "vcas": []
}

```

Result:

```json
{}

```

## `vca.remove@1`

Remove a VCA and its automation, keeping the track group.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Remove** (`song_with_vca`)

```json
{
  "group": "gr_1"
}

```

Result:

```json
{}

```

## `vca.set@1`

Set a VCA's level, mute or solo without moving member faders.

VCA gain adds to member automation before the fader, capped at the Console 9 native top. Group suspension only suspends linked editing. Use vca.members.set to nest VCAs.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `volume_db` | number or string | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `solo` | boolean |  |  |  |  |
| `followers` | array |  |  |  |  |

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

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Lower the group** (`song_with_vca`)

```json
{
  "group": "gr_1",
  "volume_db": -3
}

```

Result:

```json
{}

```
