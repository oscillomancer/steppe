# Command reference

Generated from the command registry.

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



Kind: `query`. Exposure: `mcp-tool`.

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

Analyze a render id or audio path: loudness, spectrum, envelope and stereo.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `render` | string |  |  |  |  |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. |

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
| `tap` | string |  |  | "post_fader" |  |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. |

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
| `mode` | string |  |  | "warp" |  |
| `reference` | string |  |  | "" |  |
| `hits` | string |  |  | "reference" |  |

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

## `autoitem.add@1`

Put an automation item on an envelope: a block that replaces it there.

position (beat) or bar, length in beats. points (beats from the item's start, values in the envelope's units) over source_length beats (default: length), repeated while loop (default true) and the item is longer; without points it captures what the envelope plays over that range. Duplicate it pooled (autoitem.duplicate) to edit several at once. Track, route and plugin envelopes.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `length` | number |  |  | 4.0 |  |
| `points` | array |  |  |  |  |
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

Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Automation item id, e.g. "ai_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

### Result

```json
{
  "type": "object"
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

Start a branch from a branch's head or an earlier version, and switch to it.

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



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

What differs between two branch heads: objects added, removed, changed (fields).



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



Kind: `query`. Exposure: `mcp-tool`.

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
    "remapped": {}
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



Kind: `action`. Exposure: `mcp-tool`.

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

from: "mo_N.<output>"; to: "mo_N.<input>" or "mo_N.<knob>" (a modulation cable: amount is the knob's movement at a signal of 1, in octaves for a frequency, the knob's unit otherwise; default an octave or half the range). Several cables into one input sum; a mono cable reaches every voice of a poly module. feedback: it closes a loop, so it carries the previous frame (a one-sample delay).

Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

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

## `dsp.check@1`

Compile a DSP script without saving it; return its diagnostics.

Give source (a whole script) or name (one in the library). ok is true when it compiles; plugin then lists its id, name and parameters as the patch shows them.

Kind: `query`. Exposure: `mcp-tool`.

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

Change part of a library script by replacing text; compile it, as dsp.write.

For a change to part of a script, instead of sending all of it to dsp.write. Each edit replaces old, which must be in the script exactly once, with new (empty deletes it). The edits apply in order, each to the script as the ones before left it, and all or none: when one fails, nothing is saved and the error says where old and the script part. Then as dsp.write: it saves the script, compiles it and returns the diagnostics; instances playing it follow within half a second.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `edits` | array |  |  |  |  |

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

Export a DSP script as a plugin for other DAWs: a CLAP file, or C++ source.

format clap (the default): copies the platform's script player and embeds the script in its trailer (Linux/Windows) or bundle resource (macOS, re-signed ad hoc with the system codesign tool). The copy is a plugin of its own, under the script's id and name, that plays this version of the script in any CLAP host (it reads no library), written to <folder>/<plugin name>.clap (default: the script's folder, under export/) with the licenses to ship beside it (notices). It runs on this platform, as fast as the player it copies (a debug build's is slow). format source: a C++ project of the plugin template that builds the same plugin (same id, parameters and state) as CLAP and VST3, plus AU on macOS, with the script compiled as C++, written to the empty <folder> (default: the script's folder, under export/<script>-source); its README says how to build it, and license (MIT unless GPL-3.0-or-later) is the license of its own files. Export again after changing the script; an export does not follow the library.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `script` | string |  |  |  | a name from dsp.list |
| `format` | string |  |  |  | clap (default): a plugin file; source: a C++ project |
| `folder` | string |  |  |  | an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new |
| `license` | string |  |  |  | source: the license of the project's own files (default MIT) |

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signal` | string |  |  |  |  |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `level_db` | value | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | value | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.0..10.0 | 1.0 |  |
| `scale` | number |  | 0.5..3.0 | 1.0 |  |
| `pointer` | array |  |  |  |  |

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

Play test signals or notes through a plugin and measure it: levels, THD, pitch.

Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays each signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at level_db; and file, audio_file played as it is (resampled to the render's rate, mono on both channels, at most 60 s), the only signal when signals is not given. An instrument, at velocity 0.8: notes (a note, then a chord), note (A4; pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from silence, no output, a note out of tune, sound that never dies away, non-finite samples the framework silenced, a script stopped by its loop budget or indexing outside an array). cpu_percent is measured, so it varies between runs. save: true keeps each output as a WAV (path) for analyze and compare. automation is a list of {param, at_s, value}: semantic patch keys and physical values, as discrete sample-accurate changes applied to every signal (no implicit ramps). automation.max_step is the largest adjacent-sample change in full-scale amplitude; baseline_max_step uses the same render without automation; excess_step is their positive difference. A step above both four times baseline and 0.05 suggests clicks or zipper noise: a screening heuristic, not proof of an artifact or of inaudibility. sidechain supplies a timed key; its middle half is measured against a connected silent key with identical automation: gain_change_db is negative for ducking, null if either output is below -120 dBFS. Slow attack may not settle in that window. Each pass has a fresh instance and initial patch. THD and impulse response are omitted during automation or a non-silent key; gain_db then describes the changing render, not static gain. notes supplies an instrument's sequence ({key, velocity, start_s, length_s}), replacing the notes signal; defaults become notes and silence. Do not combine it with the A4 note signal or select only silence. release_ms starts at its last note-off. Times round to samples at the render rate; spans must fit within seconds. sample_rate renders at that rate instead of the session's (sandbox) or the project's (DAW); latency is in samples at the rate used, which sample_rate in the result reports. meters gives a semantic plugin's read-only parameters (its meters and readouts) by patch key in physical units, as they stood when the signal ended (the tail after it does not move them). traces gives what a script's trace(name, value) calls recorded over the signal, in trace_points intervals of trace_interval_s: for each name, the last value in each interval and the lowest and highest, null where none was given.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signals` | array |  |  |  | default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `automation` | array |  |  |  | discrete physical-value changes on every signal |
| `sidechain` | object |  |  |  | timed generated key input |
| `notes` | array |  |  |  | instrument sequence replacing the notes stimulus |
| `level_db` | value | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | value | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.1..10.0 | 1.0 |  |
| `sample_rate` | value | Hz | 8000.0..384000.0 |  | default: the session's or project's rate Quantity strings are range-checked by the server. |
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

Kind: `action`. Exposure: `mcp-tool`.

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

List native audio device names, host APIs, rates and channel capacities.

Enumerates PortAudio devices on macOS/Windows without opening a stream. backend defaults to portaudio; dummy returns an empty list without initializing hardware. Use exact host_api and name values with engine.start. Linux uses JACK ports and returns backend.unsupported; engine.inputs lists running JACK inputs.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  |  |

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

The device's audio inputs, MIDI devices and the outputs masters play to.

audio: one entry per input channel (channel, the 1-based number track.set record_input takes, and the device port it is connected to). midi: the MIDI devices (names track.set record_midi matches; JACK ports or native CoreMIDI/WinMM inputs, fixed until restart). outputs: one entry per output channel (channel, the number track.set hw_output takes for a master, and the port it is connected to). While the engine is stopped: running false and empty lists (engine.start opens the device).

Kind: `query`. Exposure: `mcp-tool`.

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
  "outputs": []
}

```

## `engine.start@1`

Start playback through JACK, native PortAudio, or a device-free dummy.

backend: jack (Linux default), portaudio (macOS/Windows native device), or dummy (default elsewhere, no device). For portaudio, host_api, input_device and output_device select exact names from engine.devices. inputs defaults to 0 (playback only); outputs (the device channels masters can play to, track.set hw_output) defaults to one per physical playback port with JACK (2 to 64) and 2 otherwise. rate defaults to the project rate; a rate given is asked of the device (CoreAudio switches the device's rate when it can, WASAPI converts) or, with JACK on PipeWire, suggested to the graph (node.rate: applied only while the devices are idle and the rate is allowed). force_graph (JACK on PipeWire, default false) instead forces block as the graph's quantum, and rate if given, while the engine runs. PortAudio opens up to 16 native MIDI inputs; engine.inputs lists their names. Dummy is paced like a device. block: the device block in frames (default 256); the device's period may differ and is reported. anticipative (default true): render ahead of the playhead on worker threads (edits are heard up to 200 ms later); false renders everything in the audio callback. The engine runs at the device's rate (media is resampled; renders stay at the project's rate). Background and live thread capacities follows the app's Audio processing threads preference (Automatic by default); live helpers join each callback with no added latency. memory_lock reports the default-off host-process memory policy: requested, active, and a structured OS error on failure. cpu_idle_latency reports the default-off 20 microsecond PM QoS request, released on engine stop. A protection failure does not prevent audio startup. MIDI inputs that refuse to open are skipped: midi_warnings reports each device, numeric os_error, message and corrective hint; audio continues.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  |  |
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
    "midi_dropped": {
      "type": "integer"
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
        "type": "string"
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
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "failed_errors": {}
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
    "midi_dropped",
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
    "meter_drops",
    "failed_fx",
    "dropped",
    "voice_dropped",
    "failed_errors"
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
  "midi_dropped": 0,
  "midi_warnings": [],
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
    "active": false
  },
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "dropped": 0,
  "voice_dropped": 0,
  "failed_errors": []
}

```

## `engine.status@1`

Real-time engine state: backend, rate, block, device period, latency, xruns.

preparing_audio: source preparation is pending; the previous ready plan stays audible. prepared_version is the last compiled version, preparation_version the pending target (zero when idle); preparation_error explains a failed preparation. plan_latency: the session's plugin delay in frames; device_latency: frames from the engine's outputs to the device. restart_needed: the device changed rate or went away; engine.stop and engine.start again. midi_devices_changed is advisory: MIDI topology changed but audio keeps running. midi_devices_message asks for an explicit engine restart to pick up the new set; existing port names stay fixed. midi_dropped: native MIDI messages/packets rejected or lost to bounded queues (zero for other backends). midi_warnings lists inputs skipped at start, with device, numeric os_error, message and corrective hint. meter_drops: entries a full ring dropped (the renderer's, the audio callback's) — a strip's peak/RMS missed a block, never audio. dropped/voice_dropped count lost plug-in output/voice records. memory_lock reports requested/active and an optional OS error with errno and a corrective hint, including while stopped. Bridges are excluded. cpu_idle_latency reports the owned 20 microsecond request with the same status/error fields; it affects system idle while held and leaves CPU frequency policy unchanged. live_helpers is the enabled live helper capacity. scheduling: each thread class's policy (none, normal, fifo, rr, high) and priority as applied at start: the device callback, live helpers, renderer and background helpers (real-time below a real-time callback when permitted). workgroup: whether the live helpers joined the device's audio workgroup (macOS CoreAudio; state none, ready, joined, partial or failed, with the helper counts and the last refusal's OS error, message and hint). warnings includes scheduling fallbacks and plan warnings with corrective actions. failed_errors adds structured code, message, path and hint to each failure. failed_fx: isolated plugins whose process crashed or hung (silent now; fx.restart re-creates them).

Kind: `query`. Exposure: `mcp-tool`.

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
    "midi_dropped": {
      "type": "integer"
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
        "type": "string"
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
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "failed_errors": {}
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
    "midi_dropped",
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
    "meter_drops",
    "failed_fx",
    "dropped",
    "voice_dropped",
    "failed_errors"
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
  "midi_dropped": 0,
  "midi_warnings": [],
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
    "active": false
  },
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "dropped": 0,
  "voice_dropped": 0,
  "failed_errors": []
}

```

## `engine.stop@1`

Stop real-time playback and release the audio device.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Target one of track (param volume|pan|mute; playrate only on tr_0), route (volume|pan|mute), group (VCA volume|mute), or fx (a semantic key such as "gain" or "filter1.cutoff", or for other plugins a CLAP id or parameter name). points: [{beat|bar|seconds, value, shape?, tension?}]; values in the parameter's unit (dB, %, Hz… — text like "800 Hz" works for plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values move along the parameter's curve (a log cutoff sweeps evenly in pitch). take (param volume|pan|mute): a take envelope, its points in beats from the item's start (it moves with the item); it shapes the take after its take FX (a MIDI take needs an instrument in its take FX). Master playrate points are finite numeric ratios 0.25..4.0; an active nonempty master curve replaces the saved scalar during playback and rendering. Audio sources convert before take FX/live mixing; MIDI and automation follow the same timeline clock.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `group` | string |  |  |  | VCA group id, e.g. "gr_1". |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `route` | string |  |  |  | Route id, e.g. "sd_1". |
| `param` | string |  |  |  |  |
| `points` | array |  |  |  |  |

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



Kind: `query`. Exposure: `mcp-tool`.

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

from_beat/to_beat: replace only points with from_beat <= beat < to_beat.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `points` | array |  |  |  |  |
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Set an envelope's options: active (heard), mode (read, touch, latch, write).

visible shows or hides the envelope's lane in the arrange view (a hidden envelope still plays while active).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `active` | boolean |  |  |  |  |
| `visible` | boolean |  |  |  |  |
| `mode` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

## `fx.add@1`

Insert a plugin into a track's (or a take's) FX chain and return its id.

plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic plugins. chain: main (default; the track's effects) or input (the recorded-input chain). take instead of track: the take's own chain (take FX) — the item plays through it before the track's input, cut to the item; an instrument there plays a MIDI take on its own. An instrument with outputs past its main pair (a multi-out sampler's Kick, Snare...) gets that many more channels on the track, and its pins.out sends each output there; route.matrix lists them and route.add {channels: {from: [3, 4]}} sends one to a buss.

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

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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
    "params": {},
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
    "params_total": {
      "type": "integer",
      "description": "Plain parameters matching; at most 512 are listed."
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
  "status_available": false
}

```

## `fx.learn.list@1`

List the MIDI controller bindings of plugin parameters.

Every effect's, or one effect's (fx). control names the message in words.

Kind: `query`. Exposure: `mcp-tool`.

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
    "learn": {}
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  | A semantic key, or a plain parameter's id or name (fx.describe) |
| `midi` | object |  |  |  |  |
| `mode` | string |  |  |  |  |
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

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `from` | string |  |  | "live" | live: the playing instance; render: the last render.mix |

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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `track` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_param_result",
  "properties": {
    "value": {}
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

Kind: `edit`. Exposure: `mcp-tool`.

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
| `fx` | string |  |  |  |  |
| `changed_only` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {}
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



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_list_result",
  "properties": {
    "patches": {}
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

Semantic plugins only. Values in physical units: {"params": {"gain": "-6 dB"}}.

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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {}
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

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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
  "properties": {},
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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Set instance options: bypass, offline, wet (%), label, isolation, pins.

isolation: in_process, dedicated (the plugin runs in a process of its own: a crash silences only it), shared_bridge (one process for all such plugins), inherit (the project default: in_process). pins: which of the track's channels (1-based; track.set channels adds 3-128) the plugin reads ({in: [l, r]}), writes ({out: [l, r]}) and keys from ({key: [3, 4]}: a detector on 3/4, for a plugin with a sidechain input); {} restores 1/2 in and out. out past its first two entries: a track channel for each of the plugin's auxiliary output channels in order (0: unconnected); fx.add sets them for a multi-out instrument.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `bypass` | boolean |  |  |  |  |
| `offline` | boolean |  |  |  |  |
| `wet` | value | % | 0.0..100.0 |  | Quantity strings are range-checked by the server. |
| `label` | string |  |  |  |  |
| `isolation` | string |  |  |  |  |
| `pins` | object |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

## `group.add@1`

Create a named track group with optional links and members.



Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `item` | string |  |  |  |  |
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



Kind: `query`. Exposure: `mcp-tool`.

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

Add an item on a track: MIDI in notation, a MIDI file or an audio file.

position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h" — every bar must add up; the item is as long as the written bars unless length (beats) is given. audio_file: a path at the project's sample rate; with the app's copy-imports setting on, the project keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, every track merged, channels kept); its notes keep their beats and follow the project's tempo (the file's tempo and meter are not applied; a warning says when they differ), and the item is as long as the file unless length is given. A master holds audio items only (prints, played after its fader); busses hold none.

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
        "type": "string"
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
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

Kind: `action`. Exposure: `mcp-tool`.

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

### Result

```json
{}

```

### Examples

## `item.duplicate@1`

Copy an item and its takes to a new beat position (default: immediately after).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  |  |
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

Spread an item's takes onto new tracks, or one after another on its track.

In recording order. mode tracks: the first take stays on the item's track and each other take gets a new channel right below it (named after the take) at the same time. mode serial: the takes follow each other on the item's own track, each as long as the item. Every take becomes a one-take item with its FX and envelopes.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
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

Exports the active take over the part the item covers (trims and splits respected). Audio: the source's own samples, rate and channels, from the source position at the item's start to the one at its end (playrate, unpreserved pitch and stretch markers decide that span but are not applied; a looping item ignores markers and repeats its source as heard; outside the source is silence). No gain, fades, pan, channel mode, FX or track processing. 16/24-bit PCM WAV sources keep their format; others are 32-bit float. MIDI: what the item plays (loops unrolled, notes ending by its end) from 0, at the source PPQ, with the tempo and meter at the item's start. Relative paths resolve against the work directory; a missing extension is added; existing files are replaced, the project's media never.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
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

Kind: `edit`. Exposure: `mcp-tool`.

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

Mix the selected items through their strips onto a new muted channel.

Background job. Only the given items sound; each plays through its channel's full strip (take FX, item gain/fades, track FX, automation, fader, pan, Console 9 encode), and the sum is decoded by a neutral Console 9 Buss, as a fresh buss fed by those channels hears it. Sends, parent busses, mute switches and solo are ignored. Span: earliest item start to latest end, no tail. Result: a project-rate stereo float WAV in the media folder on a new muted channel after the last source (under their common parent, else the main master), one item at the span's start. Source items stay. One undo step; job.get returns track, item and file.

Kind: `action`. Exposure: `mcp-tool`.

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

### Result

```json
{}

```

### Examples

## `item.move@1`

Move an item in time (position in beats or bar) and/or to another track.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  | Item id, e.g. "it_1". |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Copy items as they were in a version (a clipboard) to tracks and beats.

version: where the items are read (default: now) — a GUI copy or cut remembers the version, so a paste reproduces the items as copied even after they were removed or edited. items: [{item, track, position}] (beats; channels only). Takes, fades, take FX and MIDI are copied; an audio source the project no longer has is restored.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `version` | integer |  | 1.0.. |  |  |
| `items` | array |  |  |  |  |

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
| `item` | string |  |  |  | Item id, e.g. "it_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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
| `item` | string |  |  |  |  |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |
| `length` | number |  | 0.0.. |  |  |
| `mute` | boolean |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `locked` | boolean |  |  |  | The arrange view marks locked items. |
| `gain_db` | value | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `fade_in` | number |  | 0.0.. |  |  |
| `fade_in_shape` | string |  |  |  |  |
| `fade_out_shape` | string |  |  |  |  |
| `fade_out` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `timebase` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `item` | string |  |  |  |  |
| `at` | number |  | 0.0.. |  |  |
| `snap` | string |  |  | "none" |  |

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
  "type": "object"
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
  "type": "object"
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
  "type": "object"
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
  "type": "object"
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
  "type": "object"
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

## `log.list@1`

Recent commands with origin, arguments and outcome (the command log).

Newest last. Who did what: origin is gui, agent:<client>/<model>, mcp:<client>, lua:<script>, rpc. queries: include read-only commands (left out by default). since: only entries after this seq (poll with the last seq seen). origin: entries whose origin starts with this. The log keeps the last 2000 commands.

Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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
          }
        },
        "required": ["id", "timebase", "name", "color"],
        "additionalProperties": false
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `pos` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Edit a MIDI item's notes and CCs: add, remove, move, resize, velocity, quantize.

ops run in order and are one undo step. notes: indices into midi.notes' list as it was before this command (every op refers to that list, so later ops are not affected by earlier removals). Beats are from the item's start. add {pitch, start, length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid), strength? 0-1, ends?}; cc.set {number, points [{beat, value}], channel?}; cc.clear {number, from?, to?}. Returns the new indices of the notes the ops added or changed (to keep a selection).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `ops` | array |  |  |  |  |

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

## `midi.get@1`

Read an item's MIDI as notation.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |

### Result

```json
{
  "type": "object",
  "title": "text",
  "properties": {
    "text": {
      "type": "string"
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

## `midi.notes@1`

List a MIDI item's notes and CC points (beats from the item's start).

Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their position in this list. start/length/beat are in beats from the item's start; channel is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them too. For writing whole phrases, midi.write with notation is usually better.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |

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

## `midi.write@1`

Replace an item's MIDI with notation (the item keeps its position and length).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |
| `notation` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

## `module.add@1`

Add a module to a rack: a type with knob values.

type: an id or a short name ("vca", "notes"; module.list). x, y: its top-left corner in the rack (default: right of the others). params: knob values by key in the knob's units (a choice by its key, a switch as a bool). Notes: source (the track whose MIDI it plays; default the rack's channel) and device (a live MIDI input it always plays; "" any). plugin: a plug-in id (fx.scan) for a plugin module, which shows the plug-in's name and opens its editor; its ports are in_l, in_r, out_l, out_r. Returns its size and keys.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `type` | string |  |  |  |  |
| `label` | string |  |  |  |  |
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

## `module.describe@1`

A module type's knobs, inputs and outputs, with units and help.

type (an id or a short name, "vca") or module (a module's type). Knobs: key, name, kind (number, integer, choice, toggle), unit, min, max, default, choices (keys), group, help. Ports: key, name, signal (audio, pitch in octaves from C4, gate, cv -1..1, unipolar 0..1, phase 0..1), help, and an input's normal (its value with nothing connected). A cable into a knob moves it by amount per unit of signal: octaves for a frequency, the knob's own unit otherwise.

Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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

## `module.list@1`

List module types: id, name, tags, what they do.

Host modules (Out, In, Notes, Transport) first, then stock, the user's library and the project's. query matches names, ids, tags and descriptions; tag one tag.

Kind: `query`. Exposure: `mcp-tool`.

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

**Amplifiers** (`empty`)

```json
{
  "tag": "amplifier"
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

Kind: `edit`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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

params merge into its knobs (values in the knob's units; a choice by key; a default value is dropped from the file). A knob edit is heard at once, without a recompile. bypass passes each input to the output of its kind. label names it (an Out module's label is what channels' input menus list).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `params` | value |  |  |  |  |
| `label` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |
| `bypass` | boolean |  |  |  |  |
| `source` | string |  |  |  |  |
| `device` | string |  |  |  |  |

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

## `plugin.scan@1`

Rescan configured plugin paths and return the available plugins.

wait: false scans in the background (every module through the supervised scanner) and returns at once with the current list and the scan's job; the catalogue changes when the job is done. A background scan already running is returned instead of a second one.

Kind: `action`. Exposure: `mcp-tool`.

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
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
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
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "load_warning",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "sample_rate",
    "tempo",
    "meter",
    "version",
    "generation",
    "recoverable",
    "autosave_available"
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
  "sample_rate": 48000.0,
  "tempo": 120.0,
  "meter": "4/4",
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false
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
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
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
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "load_warning",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "sample_rate",
    "tempo",
    "meter",
    "version",
    "generation",
    "recoverable",
    "autosave_available"
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
  "sample_rate": 48000.0,
  "tempo": "<any>",
  "meter": "<any>",
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false
}

```

## `project.open@1`

Open a project file (replaces the current project and its history).

recoverable > 0: recordings a crash left out of the project; project.recoverable lists them and project.recover puts them back. autosave_available: a newer autosave exists next to the file (it is not loaded silently).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

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
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
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
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "load_warning",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "sample_rate",
    "tempo",
    "meter",
    "version",
    "generation",
    "recoverable",
    "autosave_available"
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
  "sample_rate": "<any>",
  "tempo": "<any>",
  "meter": "<any>",
  "version": "<any>",
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false
}

```

## `project.recover@1`

Put recordings a crash left out back into the project (one undo step).

files: as project.recoverable lists them (omit for all). Each becomes an item on the track it was recorded on (or track) at the position it was recorded.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `files` | array |  |  |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

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
  "required": ["recovered"],
  "additionalProperties": false
}

```

### Errors

- `recover.nothing`: there is nothing to recover Hint: project.recoverable
- `recover.unknown_file`: a file is not a recording to recover Hint: project.recoverable lists them
- `recover.duplicate_file`: a file is listed twice Hint: list each once
- `recover.no_track`: the track it was recorded on is gone Hint: give track

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

## `project.recoverable@1`

List recordings a crash left out of the project.

Takes recorded but never committed (the program stopped mid-recording) or never saved, found in the media folder's rec/. start_seconds is where each was recorded; seconds includes audio written after the file's last header update.

Kind: `query`. Exposure: `mcp-tool`.

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
          }
        },
        "required": ["file", "track", "start_seconds", "seconds", "channels"],
        "additionalProperties": false
      }
    }
  },
  "required": ["recordings"],
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
  "recordings": []
}

```

## `project.save@1`

Save the project (canonical JSON) to its file or a new path.

path: where to save (becomes the project's file); omit to save in place. warnings: running plug-ins whose current state could not be read; the file keeps the state each had before (fx.restart, then save again, or replace the plug-in).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

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
        "type": "object",
        "title": "save_warning",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
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

Change project metadata (name, title, author, notes, Console 9, timebase).

timebase: what items follow when the tempo changes, for items and tracks that inherit it — beats (default), time or beats_position; they keep their place now.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `title` | string |  |  |  |  |
| `author` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `console9` | boolean |  |  |  |  |
| `history_budget_bytes` | integer |  | 1.0.. |  | Estimated retained history byte budget; default 1 GiB. Leases and branch boundaries may exceed it. |
| `render` | object |  |  |  | Render settings (session-model §1.10); every member optional |
| `timebase` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

## `project.template.delete@1`

Delete a saved project template by name.



Kind: `action`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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
        "type": "object",
        "title": "save_warning",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
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

A rack is a patching space homed on a channel (track, or a new channel named after name). It starts with an Out module, made the channel's input: what the patch sends to Out plays on the channel, before its inserts and Console 9 stage. The channel's MIDI items play into Notes modules. Add modules with module.add and connect them with cable.add.

Kind: `edit`. Exposure: `mcp-tool`.

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

## `rack.get@1`

A rack's modules, cables, Out modules and problems.

Modules with their type, label, place and size and the knobs that differ from their defaults (detail full: also their input, output and knob keys); cables with feedback (true: it closes a loop and carries the previous frame); Out modules and the channels they feed; problems (missing types, cables that do not fit).

Kind: `query`. Exposure: `mcp-tool`.

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
  "problems": []
}

```

## `rack.list@1`

List racks: home channels, module counts, Outs.



Kind: `query`. Exposure: `mcp-tool`.

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

## `rack.remove@1`

Remove a rack with its modules and cables.

Channels that took one of its Out modules as input lose it; sends into its In modules go; plugin modules' plug-ins go with them.

Kind: `edit`. Exposure: `mcp-tool`.

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

name "" shows the home channel's name. track moves its home (its Out modules keep feeding the channels they feed; Notes modules without a source follow the new home's MIDI).

Kind: `edit`. Exposure: `mcp-tool`.

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

## `region.add@1`

Add a named region in beats.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
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



Kind: `query`. Exposure: `mcp-tool`.

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
          }
        },
        "required": ["id", "timebase", "name", "color"],
        "additionalProperties": false
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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
  "type": "object"
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
    "type": "object"
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
            "summary": "List available commands."
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
  "type": "object"
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
      "summary": "List available commands."
    }
  ]
}

```

## `render.dry-run@1`

List the files a render would write, with lengths, without rendering.

Each file: path (where if_exists sends it), exists (its destination is already there), skip (if_exists skip leaves it out), seconds and outputs. operation: mix, stems or regions (default: tracks or regions given, else the settings' source).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | value |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | value |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `normalize` | object |  |  |  |  |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  |  |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call |
| `tap` | string |  |  |  | Sets stems.tap for this call |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline |
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

Render the mix offline with the project's render settings (default: whole song).

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary. The mix answers with its file's entry at the top level (path, secondary_path, outputs, peak_dbfs, …); analyze: summary or full adds the report inline. masters renders several masters, a file each (the first on top, all in files); a pattern without {track} then gets -{track}, the master's name.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | value |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | value |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `normalize` | object |  |  |  |  |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  |  |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call |
| `tap` | string |  |  |  | Sets stems.tap for this call |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline |
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



Kind: `action`. Exposure: `mcp-tool`.

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

Render presets: the factory ones, then the user's, with their settings.

default: the user's render default, or null. Presets live in the user's data folder (render-presets/), shared by every project.

Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `edit`. Exposure: `mcp-tool`.

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

Save render settings as a named preset, or as the user's render default.

settings: what to save (default: the effective settings now). Never saved: stems.tracks, matrix and custom bounds, which belong to one project. A file per preset in the user's data folder; factory presets cannot be overwritten.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |
| `settings` | object |  |  |  | Render settings (session-model §1.10); every member optional |

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

Queue a render (mix, stems or regions) to run later with render.queue.run.

args: the render command's arguments (as for render.mix/stems/regions). The queued render is pinned to the session and settings as they are now (its version, a branch's head when branch is given, and the effective render settings, a loop range as beats), so later edits do not change it; a dry run lists its files now. The queue is transport-like state: not an undo step, not saved with the project.

Kind: `action`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `action`. Exposure: `mcp-tool`.

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

Run every queued render in order; each one leaves the queue when it is done.

Each render renders the version it was queued at. job: true runs them as one job (job.get for progress). The result lists each render's files under its id.

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | value |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | value |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `normalize` | object |  |  |  |  |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  |  |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call |
| `tap` | string |  |  |  | Sets stems.tap for this call |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline |
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

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | value |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | value |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `normalize` | object |  |  |  |  |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  |  |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call |
| `tap` | string |  |  |  | Sets stems.tap for this call |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline |
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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | value |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | value |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `normalize` | object |  |  |  |  |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  |  |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call |
| `tap` | string |  |  |  | Sets stems.tap for this call |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline |
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

Send one track to another track, bus, the master, or a plug-in's key.

The destination must not feed the source. Master accepts routes. With fx instead of to, the send feeds that plug-in's sidechain (key) input — e.g. a kick keying a compressor on the bass; it is delay-compensated, and soloing the keyed track does not stop the key. Hardware routing is not supported yet.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  | A track, or a rack's In module (mo_*): the send then plays into the rack, linear (no Console 9 stage) |
| `fx` | string |  |  |  | Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out |
| `tap` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `mono` | boolean |  |  |  | Sum the source to mono before the pan |
| `phase_invert` | boolean |  |  |  |  |
| `channels` | object |  |  |  | Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Errors

- `route.cycle`: the send would feed back Hint: choose another destination
- `route.no_sidechain`: the plug-in has no key input Hint: pick another plug-in

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

## `route.learn.list@1`

List the MIDI controller bindings of send controls.

Every send's, or one's (route). control names the message in words.

Kind: `query`. Exposure: `mcp-tool`.

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
    "learn": {}
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, pan, mute, phase_invert, mono, tap:post_fader, tap:pre_fader, tap:pre_fx |
| `midi` | object |  |  |  |  |
| `mode` | string |  |  |  |  |
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



Kind: `query`. Exposure: `mcp-tool`.

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

What can be routed where: strips, plug-in keys, sends and blocked pairs.

sources: strips in tree order (depth, parent, parent_send). destinations: strips and plug-in keys (fx with a sidechain input). A pair is blocked when the source's rule applies (masters feed only keys), the destination's rule applies (channels take only hardware audio), it is the same strip, or it is listed in cycles (the destination already feeds the source). sends: existing routes per pair. Otherwise route.add accepts the pair. A source's channels past 2 are auxiliary (ADR-0020); channel_names names those a multi-out plug-in writes ({"3": "Kick"}), and route.add {channels: {from: [3, 4], to: [1, 2]}} sends that pair.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  | A track, or a rack's In module (mo_*): the send then plays into the rack, linear (no Console 9 stage) |
| `fx` | string |  |  |  | Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out |
| `tap` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `mono` | boolean |  |  |  | Sum the source to mono before the pan |
| `phase_invert` | boolean |  |  |  |  |
| `channels` | object |  |  |  | Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Errors

- `route.channels`: the map pairs channels the strips do not have Hint: pair 1 and 2: {from: [1, 2], to: [2, 1]}

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

Describe the whole session in compact text (tracks, items, fx, routes).

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

## `take.activate@1`

Choose the take heard from an item.

take (its id) or index (0-based in the item's take order — the same position after item.split, which copies the takes in order: comping splits an item and picks a take by index on the piece).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `take` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Add a take to an item, optionally copied from another take of that item.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `from` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
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

Take a take out of its item into an item of its own on any channel.

The new item copies the old one's length, fades, gain and timebase and starts at position (beats; default: where the item starts). The take keeps its FX and envelopes. The only take of an item moves the item itself.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `track` | string |  |  |  |  |
| `position` | number |  | 0.0.. |  | Beats. |

### Result

```json
{
  "type": "object",
  "title": "take_extract_result",
  "properties": {
    "item": {
      "type": "string"
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

List the takes of an item and identify its active take.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |

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

Delete a take from its item (the item goes with its last take).

The take's FX, envelopes and unshared MIDI go with it. When it was the heard take, the next take is heard (else the previous one). item_removed: it was the item's only take, so the item was removed too.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |

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

Set take gain, pan, channel mode, name, source offset, or playrate and pitch.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `gain_db` | value | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `channel_mode` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `rebase_seconds` | number |  |  |  | Rebase an audio take after trimming its item start; preserves the warp map. |
| `start_offset` | number |  |  |  | Source position at the item start; negative for a late first pass. |
| `playrate` | value | x | 0.01..100.0 |  | Playback speed: 2 plays the source twice as fast. Its length in seconds is the item's, so the take runs out sooner or loops. Quantity strings are range-checked by the server. |
| `preserve_pitch` | boolean |  |  |  | Keep the source's pitch when the playrate changes (the default). With pitch_mode "resample" the pitch follows the speed. |
| `pitch` | value | st | -48.0..48.0 |  | Pitch offset in semitones. Stretched when the pitch is preserved, otherwise a further speed factor of 2^(pitch/12). Quantity strings are range-checked by the server. |
| `pitch_mode` | string |  |  |  | Time-stretch algorithm: "signalsmith" (pitch-preserving, the default) or "resample" (varispeed). |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.

ramp: true changes the tempo linearly (in beats) to the next point; otherwise it steps.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `points` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
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



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bar` | integer |  | 2.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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

Add a channel, buss or master strip and return its id.

parent: a buss/master id (default tr_0); masters have no parent; index: position among its siblings; name: default the first free ch_N, buss_N or mstr_N. A new channel gets the user's default monitoring (on unless set otherwise; monitoring needs the track armed).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  | "channel" |  |
| `name` | string |  |  |  |  |
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

## `track.describe@1`

Describe track groups and requested/applied effective gain at a time.



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `edit`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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
| `freeze` | object |  |  |  |  |
| `states` | array |  |  |  |  |

### Result

```json
{}

```

### Examples

## `track.learn.list@1`

List the MIDI controller bindings of track controls.

Every track's, or one's (track). control names the message in words.

Kind: `query`. Exposure: `mcp-tool`.

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
    "learn": {}
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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

One binding per control (a new one replaces it); saved with the project. param: volume, pan, mute, solo, phase_invert, mono, armed, monitor, lane_mute. midi.device: a substring of the MIDI input's name (empty: any). A level or pan follows a knob (mode absolute, soft_takeover: once it reaches the value) or an encoder (relative1 127 = -1, relative2 63 = -1, relative3 65 = -1). A button: a note or program change flips it per press, as a CC in toggle mode does; an absolute CC is on above 63. monitor goes off, on, auto in turn on each press.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, pan, mute, solo, phase_invert, mono, armed, monitor, lane_mute |
| `midi` | object |  |  |  |  |
| `mode` | string |  |  |  |  |
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
    }
  },
  "required": ["param", "control"],
  "additionalProperties": false
}

```

### Errors

- `track.learn_param`: no such control Hint: use one of: volume, pan, mute, solo, phase_invert, mono, armed, monitor, lane_mute
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `parent` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Remove a track with its items, fx, routes and envelopes (not a folder).



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
  "properties": {},
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

Set track fields: name, type, volume, pan, mute, solo, polarity, mono, arming.

Only the given fields change. solo: off | on | in_place | defeat. armed + record_input (device input channels, 1-based) prepare a track for transport.record; record_midi records MIDI instead: a device name (substring of the MIDI port, "*" = every device) with midi_channel 1-16 (0 = all). monitor: off | on | auto (auto: while armed) — a monitored track hears its input live (an instrument plays the keyboard), also while stopped. record_mode: input (new items), midi_overdub (merge into the MIDI item under the start, as a new take), midi_replace. color: red | orange | yellow | green | teal | blue | violet | grey | "#rrggbb" ("" clears it), shown as a stripe on the track. A master takes armed alone: transport.record then records its output (after its fader, before its own items) as an item on the master; lane_mute silences a master's items.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bypass_groups` | boolean |  |  | false |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `type` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `phase_invert` | boolean |  |  |  | Flip the polarity of the main pair (after the FX chain, before the fader) |
| `mono` | boolean |  |  |  | Sum the main pair to mono (after the FX chain, before the fader and pan): a mono check on the master |
| `solo` | string |  |  |  |  |
| `armed` | boolean |  |  |  |  |
| `record_input` | array |  |  |  |  |
| `record_midi` | string |  |  |  |  |
| `record_rack` | string |  |  |  | A rack's Out module as the channel's input: the rack plays on the channel, before its inserts (rack.list lists them) |
| `midi_channel` | integer |  | 0.0..16.0 |  |  |
| `record_mode` | string |  |  |  |  |
| `monitor` | string |  |  |  |  |
| `parent_send` | boolean |  |  |  | The implicit send to the parent buss/master (off: only explicit sends leave the strip) |
| `timebase` | string |  |  |  | What this channel's items follow when the tempo changes (those that inherit it): project, beats, time or beats_position; they keep their place now |
| `channels` | integer |  | 1.0..128.0 |  | 1-2: the strip's main pair; up to 128 adds auxiliary channels 3-128 for routes and plug-in pins (a detector on 3/4, a multi-out instrument's outputs), outside the fader, pan and parent send |
| `hw_output` | array |  |  |  | Masters: the device outputs the main pair plays to: [l, r], [mono] or [] for none (engine.inputs lists them). Default: tr_0 [1, 2], other masters [] |
| `lane_mute` | boolean |  |  |  | Masters: silence the master's own items (its prints) and nothing else |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

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

## `track.template.delete@1`

Delete a saved track template by name.



Kind: `action`. Exposure: `mcp-tool`.

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
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `template.unknown`: no template has that name Hint: use a name from track.template.list

### Examples

**Delete the bass strip** (`song_with_saved_template`)

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



Kind: `query`. Exposure: `mcp-tool`.

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

Add a strip from a track template (settings and FX) in one undo step.

Give exactly one of name (in the user's track-templates directory) or path. parent defaults to the master. Every plugin and state is checked before the model changes.

Kind: `edit`. Exposure: `mcp-tool`.

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
    }
  },
  "required": ["track", "effects"],
  "additionalProperties": false
}

```

### Examples

**Add the saved bass strip** (`song_with_saved_template`)

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

Save a strip (settings and FX chains) as a named track template.

Writes a supple.track-template/1.0 file to the user's track-templates directory: the strip's type, name, colour, notes, level, pan and monitoring, and its main and input chains as fx.chain.save stores them (semantic patches or base64 state). Not routing or items. replace: true overwrites an existing name.

Kind: `action`. Exposure: `mcp-tool`.

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
    }
  },
  "required": ["path", "effects"],
  "additionalProperties": false
}

```

### Examples

**Save the bass strip** (`song_with_chain`)

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



Kind: `edit`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `volume_db` | value | dB | -60.0..6.0 |  | Quantity strings are range-checked by the server. |
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

Scale every tempo-point BPM by master playrate, then reset the rate to 1.

One version: multiply every BPM by the saved scalar rate, preserving beat positions, ramp flags and meter changes, and reset rate to 1. Require stopped transport, no recording and no active nonempty master playrate envelope. Reject resulting BPM outside 1..1000 before mutation. Existing item timebases still apply; time-locked audio is not made sound-equivalent by this tempo edit.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Kind: `query`. Exposure: `mcp-tool`.

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

rate: finite 0.25..4.0 (default 1); preserve_pitch defaults false in a new project. Omitted fields keep their values. Updates during a drag use Origin.gesture like strip faders, and write/touch/latch capture tr_0/playrate coarsely: unlike strip levels (ADR-0061), the curve keeps playing while it is written (no hold or touch return). Recording requires unity. Settings are saved/undoable. File sources convert before take FX and live input mixing; instruments retain their pitch. Scalar edits ramp and converter changes crossfade for 10 ms after priming. Edits include the render horizon and compensated source lookahead; newer requests coalesce while a transition finishes. Long-latency take chains can exceed the usual 200 ms response. effective_rate follows audible output, independently of requested_rate.

Kind: `edit`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

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

## `transport.record@1`

Play and record every armed track's input or master's output.

Recording requires saved master rate 1 and no active master rate envelope that leaves 1 anywhere, including automation items. Non-unity recording and an armed track with unsupported output record mode are refused before opening files. An armed master records its output (after its fader, before its own items) as an item on the master; while it records, its items are silent in the punch range, or everywhere without punch.

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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `solo` | boolean |  |  |  |  |
| `followers` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Kind: `query`. Exposure: `mcp-tool`.

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
    "learn": {}
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `param` | string |  |  |  | The control: volume, mute, solo |
| `midi` | object |  |  |  |  |
| `mode` | string |  |  |  |  |
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



Kind: `edit`. Exposure: `mcp-tool`.

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
  "properties": {},
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



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `solo` | boolean |  |  |  |  |
| `followers` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
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
