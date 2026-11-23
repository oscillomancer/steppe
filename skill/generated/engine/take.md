# `take` commands

Takes of an item: add, activate, bake, extract, hits, stretch.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

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

## `take.bake@1`

Write a following take's sounding pitches into its notes, and stop following.

A take that follows the project's key or chords stores its notes as written and maps them as they sound. take.bake writes what sounds into the notes and sets follow to none and replaces the take's context with the song's under the item (what the notes are written in now; none when the song has no key there), so turning follow on again sounds as before and moves with a later song edit; a render after equals the render before. A pooled source is first made the take's own copy. When every pass of a looped item sounds alike the loop stays; otherwise the item is unrolled (loop off, its passes written out, each note at its place, its other takes repeated alike) and the answer says unrolled: true. followed is what was baked, as item.add's. Errors: take.not_following (follow is none), take.not_active (the take is not its item's active take: what sounds is the active take's). One undo step. pitches is recorded by history, not given.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `pitches` | array |  |  |  | Recorded by history, not given: each heard note's written pitch, where it is heard and the pitch it sounded as, so a replay writes what was heard. A replay that finds other notes is take.bake_replay. |
| `pitches[].was` | integer |  | 0.0..127.0 |  |  |
| `pitches[].at` | integer |  |  |  |  |
| `pitches[].pitch` | integer |  | 0.0..127.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "take_bake_result",
  "properties": {
    "followed": {
      "type": "object",
      "title": "followed",
      "properties": {
        "mode": {
          "type": "string",
          "enum": ["none", "key", "chords"]
        },
        "regions": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "followed_region",
            "properties": {
              "from_bar": {
                "type": "integer",
                "description": "First project bar of the run."
              },
              "to_bar": {
                "type": "integer",
                "description": "Last project bar of the run (inclusive)."
              },
              "from_key": {
                "type": "string"
              },
              "to_key": {
                "type": "string"
              },
              "by": {
                "type": "string",
                "enum": ["transpose", "degree", "chords", "copy"]
              },
              "interval": {
                "type": "integer",
                "description": "Semitones, for transpose."
              }
            },
            "required": ["from_bar", "to_bar", "by"],
            "additionalProperties": false
          }
        },
        "folded": {
          "type": "integer"
        },
        "copied": {
          "type": "string",
          "enum": ["drums", "audio", "no_key", "no_notes"]
        },
        "mismatch": {
          "type": "object",
          "title": "followed_mismatch",
          "properties": {
            "kind": {
              "type": "string",
              "enum": ["key", "chord"],
              "description": "What differs: the key, or (in the same key) the chord."
            },
            "clip_key": {
              "type": "string",
              "description": "kind key: the clip's key."
            },
            "key": {
              "type": "string",
              "description": "kind key: the project's key there, which differs."
            },
            "clip_chord": {
              "type": "string",
              "description": "kind chord: the chord the clip was written over."
            },
            "chord": {
              "type": "string",
              "description": "kind chord: the project's chord there, which differs."
            },
            "bar": {
              "type": "integer",
              "description": "The first project bar where they differ."
            }
          },
          "required": ["kind", "bar"],
          "additionalProperties": false,
          "description": "Set with the context.mismatch warning: the notes play as written under a key, or in the same key over a chord, that is not the clip's."
        }
      },
      "required": ["mode"],
      "additionalProperties": false
    },
    "unrolled": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["followed", "unrolled"],
  "additionalProperties": false
}

```

### Errors

- `take.not_following`: the take's follow is none: its notes sound as written Hint: set follow first (take.set {follow: "key"}), or there is nothing to bake
- `take.not_active`: the take is not its item's active take Hint: take.activate it first: what sounds is the active take's
- `take.bake_replay`: the recorded pitches do not match the heard notes Hint: run take.bake without pitches
- `take.source`: the take's MIDI source is missing Hint: undo to a version that has it

### Examples

**Write what sounds into the notes** (`song_following`)

```json
{
  "take": "tk_1"
}

```

Result:

```json
{
  "followed": "<any>",
  "unrolled": false
}

```

## `take.comp@1`

Comp an item's takes: each span plays one take, crossfaded at the seams.

item (any piece, or an item with several takes to start one) or items (the pieces, in any order). Spans {from, to, take|index} (beats; index is the take's 0-based place in the item's take order) choose which take plays; what no span covers keeps its take. The item is split where the take changes into pieces that each keep all the takes, and a crossfade (seconds, default 0.01; shape cos is equal power, linear for phase-coherent material) is centred on every seam between different takes: the pieces overlap and fade, so nothing new reaches the audio path. Call it again on any piece (or items: the pieces) to move seams: the crossfades are rebuilt for the whole run, and the same call twice changes nothing. Audio only: a seam is a hard cut, with the warning comp.crossfade_skipped, when a take is MIDI or empty, warped or has envelopes. Pieces of one take beside each other are joined (comp.midi_boundary warns of MIDI ones that cannot be, where a held note sounds again). take.activate {index} on a piece changes its take and keeps the seams. One undo step. Returns the pieces in time order.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `items` | array |  |  |  |  |
| `spans` | array |  |  |  |  |
| `spans[].from` | number or string | beats | 0.0.. |  | Quantity strings are range-checked by the server. |
| `spans[].to` | number or string | beats | 0.0.. |  | Quantity strings are range-checked by the server. |
| `spans[].take` | string |  |  |  | Take id, e.g. "tk_1". |
| `spans[].index` | integer |  | 0.0.. |  |  |
| `crossfade` | number |  | 0.0.. | 0.01 | Seconds. |
| `crossfade_shape` | string |  |  | "cos" | One of: cos, linear. |
| `bypass_groups` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "comp_result",
  "properties": {
    "pieces": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "comp_piece",
        "properties": {
          "item": {
            "type": "string"
          },
          "from": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:beats|qn))?\\s*$"
              }
            ],
            "x-unit": "beats",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "to": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:beats|qn))?\\s*$"
              }
            ],
            "x-unit": "beats",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "take": {
            "type": "string"
          },
          "index": {
            "type": "integer"
          },
          "name": {
            "type": "string"
          },
          "fade_in": {
            "type": "number",
            "description": "Seconds."
          },
          "fade_out": {
            "type": "number",
            "description": "Seconds."
          }
        },
        "required": ["item", "from", "to", "take", "index", "name", "fade_in", "fade_out"],
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
  "required": ["pieces"],
  "additionalProperties": false
}

```

### Errors

- `comp.item`: neither or both of item and items, or an empty items Hint: give item (any piece) or items (the pieces)
- `comp.takes`: an item has fewer than two takes Hint: record several passes or take.add, or use take.activate
- `comp.mismatch`: the pieces cannot be followed as one comp, or two could continue it Hint: name the pieces in items (take.comp.get lists a comp's pieces)
- `comp.span`: a span is empty, out of order, overlapping or outside the comp Hint: give from before to, inside the comp, in time order without overlap
- `comp.take`: a span has no or both of take and index, or one not in the item Hint: give take (an id of the item) or index (0..takes-1); take.list lists them
- `comp.loop`: a looped item Hint: comp an item that does not loop
- `comp.linked`: the item has linked peers Hint: set bypass_groups true to comp it alone, or take it out of its track group
- `item.locked`: a piece is locked Hint: item.set {locked: false}
- `item.kind`: a clip Hint: lay the clip on the timeline first (item.add {clip})

### Examples

**Take the second take for bars 2 to 3** (`song_with_takes`)

```json
{
  "item": "it_1",
  "crossfade": 0,
  "spans": [
    {
      "from": 1,
      "to": 3,
      "index": 1
    }
  ]
}

```

Result:

```json
{
  "pieces": "<any>"
}

```

## `take.comp.get@1`

List the pieces of a comp: spans, takes and fades.

The pieces of the comp that item belongs to (or items), in time order: from and to are the seams in beats (crossfades taken out), take and index the take each plays, fade_in and fade_out the crossfades in seconds. An item with several takes and no seams is a comp of one piece.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `items` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "comp_result",
  "properties": {
    "pieces": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "comp_piece",
        "properties": {
          "item": {
            "type": "string"
          },
          "from": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:beats|qn))?\\s*$"
              }
            ],
            "x-unit": "beats",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "to": {
            "anyOf": [
              {
                "type": "number"
              },
              {
                "type": "string",
                "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:beats|qn))?\\s*$"
              }
            ],
            "x-unit": "beats",
            "x-unit-strings": true,
            "description": "Quantity strings are range-checked by the server."
          },
          "take": {
            "type": "string"
          },
          "index": {
            "type": "integer"
          },
          "name": {
            "type": "string"
          },
          "fade_in": {
            "type": "number",
            "description": "Seconds."
          },
          "fade_out": {
            "type": "number",
            "description": "Seconds."
          }
        },
        "required": ["item", "from", "to", "take", "index", "name", "fade_in", "fade_out"],
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
  "required": ["pieces"],
  "additionalProperties": false
}

```

### Examples

**Read a comp** (`song_with_takes`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "pieces": "<any>"
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

Set a take's gain, pan, name, rate, pitch, context, follow and more.



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
| `context` | string |  |  |  | A MIDI take's lead sheet, as context.write's text: bars of the meter at its item's position, bar 1 at context beat 0, in context beats (the take's playrate does not scale them); "" clears it. A point at or past the context's period is take.context_range. |
| `follow` | string |  |  |  | How the take's notes sound against the project's context (ADR-0098, following is playback): none plays them as written, key or chords maps them as they play. Needs a MIDI take with a readable key in its context. The notes stay as written; take.bake writes them. One of: none, key, chords. |

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

- `take.no_key`: follow needs a key in the take's context Hint: give the take a context with a key: take.set {context: "1: key D minor ~:w"}
- `take.follow_audio`: a take that is not MIDI holds no context or follow Hint: set context and follow on a MIDI take (take.list shows each take's source), or leave them out
- `take.in_clip`: a clip's take holds no context or follow Hint: the clip's context and follow are the clip's: clip.set {context, follow}
- `take.context_syntax`: the lead sheet text could not be read (syntax, bar order, bar length or length) Hint: write bars as context.write does, e.g. "1: key A minor {Am}:w"
- `take.context_range`: a point lies at or past the context's period Hint: move it earlier, inside the period

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

**Follow the project's key, the take written in A minor** (`song_with_item`)

```json
{
  "take": "tk_1",
  "context": "1: key A minor ~:w",
  "follow": "key"
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
