# `item` commands

Items on tracks: add, set, move, split, paste, consolidate, export.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `item.add@1`

Add an item on a track: notation, a MIDI or audio file, or a clip's copy.

position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h" — every bar must add up; the item is as long as the written bars unless length (beats) is given. audio_file: a path at the project's sample rate; with the app's copy-imports setting on, the project keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, every track merged, channels kept); its notes keep their beats and follow the project's tempo (the file's tempo and meter are not applied; a warning says when they differ), and the item is as long as the file unless length is given. clip: a copy of that clip's takes, as long as the clip, its loop and take envelopes converted at the tempo where the item lands (ADR-0097); the item is named as the clip is (its active take's name) when the clip has a name. follow (none, key or chords; default: the clip's own) is set on the laid MIDI takes with the clip's context, so the notes sound as the key or chords under the item say, mapped from those they were written against (ADR-0098, following is playback): the notes stay as written, a context edit moves them, take.bake writes the sounding pitches, and notes on channel 10 and clips that cannot follow sound as written. The answer's followed says how they sound, by bar. With none context.mismatch warns when the clip's key, or in the same key its chord, is not the project's under the item. A master holds audio items only (prints, played after its fader); busses hold none.

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
| `follow` | string |  |  |  | How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow. One of: none, key, chords. |
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
    },
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
    }
  },
  "required": ["item", "take", "position", "length"],
  "additionalProperties": false
}

```

### Errors

- `notation.*`: the notation does not compile Hint: line:col and a hint in the error
- `item.follow`: follow with a source other than clip Hint: follow applies to a clip's copy: give clip, or leave follow out
- `tr.unknown`: no such track Hint: see track.list
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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

- `item.consolidate_stale`: the selection, or what its render read (a route's source, a track it hears), changed Hint: consolidate again once the edits are done
- `modulation.limit`: the consolidated takes' routes would pass the route limit Hint: remove routes first (modulation.list), for instance the copied chain's
- `item.consolidate_*`: invalid material, stale inputs or failed capture Hint: repair the selected sources/layout and retry consolidation
- `item.follow_merge`: merging following MIDI takes would change how a note sounds Hint: take.bake the following takes first, then consolidate
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

### Errors

- `item.consolidate_stale`: the selection, or what its render read, changed since the job began Hint: consolidate again once the edits are done
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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

### Errors

- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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

### Errors

- `modulation.cycle`: the move puts a modulated take where its rack hears it Hint: remove the route, or choose another track

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

- `item.unknown`: no such item Hint: use an item id from session.describe (items are listed under their tracks)
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

- `item.heal_fx`: the pieces' take FX, their states or their routes differ Hint: make the chains and routes match (modulation.list {fx}), or consolidate
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
- `item.mix_stale`: the selection, or what its mix read (a route's source, a track it hears), changed Hint: mix again once the edits are done
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

### Errors

- `item.mix_stale`: the selection, or what its mix read, changed since the job began Hint: mix again once the edits are done

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

### Errors

- `modulation.cycle`: the move puts a modulated take where its rack hears it Hint: remove the route, or choose another track

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

### Errors

- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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
