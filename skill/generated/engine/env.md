# `env` commands

Envelopes: automate a mixer or plug-in parameter with points.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `env.add@1`

Automate a parameter: a track's or route's mixer control, or a plugin's.

Target one of track (param volume|pan|mute; width|pan_left|pan_right|pan_boost on channels/busses; playrate only on tr_0), route (volume|pan|mute), group (VCA volume|mute), or fx (a semantic key such as "gain" or "filter1.cutoff", or for other plugins a CLAP id or parameter name; a semantic plugin's parameter named by id or name is stored by its key, its plain points converted to the key's units: converted), or module (a rack module's knob by key, values in its unit, a choice by its key; module.describe). points: [{beat|bar|seconds, value, shape?, tension?}]; values in the parameter's unit (dB, %, Hz… — text like "800 Hz" works for plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values move along the parameter's curve (a log cutoff sweeps evenly in pitch). take (param volume|pan|mute): a take envelope, its points in beats from the item's start (it moves with the item), or on a clip's take from the clip's start in beats (MIDI) or seconds (audio); it shapes the take after its take FX (a MIDI take needs an instrument in its take FX). Master playrate points are finite numeric ratios 0.25..4.0; an active nonempty master curve replaces the saved scalar during playback and rendering. Audio sources convert before take FX/live mixing; MIDI and automation follow the same timeline clock. Width and dual pans use -100..100 percent; pan_boost uses 0..1 (on at 0.5), overriding the effective law's boost while the envelope is active and nonempty.

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
    "param": {
      "type": "string",
      "description": "A plug-in parameter's canonical reference: its semantic key or decimal id."
    },
    "param_name": {
      "type": "string",
      "description": "A plug-in parameter's display name."
    },
    "converted": {
      "type": "boolean",
      "description": "True when a semantic plugin's parameter was named by id or name: the points were plain values, stored converted to the key's units (env.list shows those)."
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
  "points": 2,
  "param": "gain",
  "param_name": "<any>"
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
          "param_name": {
            "type": "string",
            "description": "A plug-in parameter's display name."
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
      "mode": "read",
      "param_name": "<any>"
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
