# `clip` commands

Launcher clips in a scene's cells: add, set, move, duplicate.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `clip.add@1`

Add a clip to a scene's cell: from notation, a file or an item.

One source at most: notation (as item.add's), midi_file, audio_file, or item (a copy of that item's takes; pooled shares its MIDI instead). Without one, an empty MIDI clip of one bar. A MIDI clip is measured in beats, as long as its notes rounded up to whole bars; an audio clip in seconds of take time, as long as the file. From an item, the length, loop and take envelopes convert at the tempo where the item sits (ADR-0097); the clip is named as the item is (its active take's name) unless name is given. Clips live on channels, one per (track, scene). A clip keeps the key and chords it was written against (context): as a lead sheet in clip bars ("1: key D minor {Dm}:w"), read from the project at a place (context_at), under the item it copies, or from the {SYMBOL} chords of its notation (a stated context holding chords wins over them; clip.chords_ignored warns when they differ from it). follow (none, key or chords) needs a MIDI clip with a key at its start (clip.no_key, clip.follow_audio); laying the clip (item.add {clip}, scene.insert, arrangement.build) lays its notes as written, the take carrying the clip's context and the follow (their follow argument), so they sound as the key or chords under them say; a launched clip follows them live (ADR-0098).

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
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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
- `modulation.cycle`: the move puts a modulated take or FX where its rack hears it Hint: remove the route, or choose another track

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

Set a clip's name, colour, mute, gain, fades, length, loop, mode or quantize.

length and loop are in the clip's unit: beats for MIDI, seconds of take time for audio. loop: {on, start?, length?}, the window that repeats inside the clip from the take's start. mode loop repeats every length; once plays it once. quantize (bar, beat, 1/2 … 1/32, none) overrides the project's launch_quantize; null returns to it. context (a lead sheet in clip bars; "" clears it) or context_at (read from the project at a bar or beat) sets the key and chords the clip was written against, and follow (none, key, chords) how it is mapped when laid; shortening a clip drops the points past its end (clip.context_dropped), and a context left with no key at the start resets follow to none (clip.follow_reset). fade_in and fade_out (audio clips) shape each pass of the period, as an item's fades shape the item: in from the pass's start, out to its end.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `mute` | boolean |  |  |  |  |
| `gain_db` | number |  | -150.0..24.0 |  |  |
| `fade_in` | number |  | 0.0.. |  | Seconds of take time, from each pass's start. |
| `fade_in_shape` | string |  |  |  | One of: linear, slow, fast, slow_fast, fast_slow, cos, exp. |
| `fade_out` | number |  | 0.0.. |  | Seconds of take time, to each pass's end. |
| `fade_out_shape` | string |  |  |  | One of: linear, slow, fast, slow_fast, fast_slow, cos, exp. |
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
- `clip.unsupported`: a fade on a MIDI clip Hint: MIDI clips play no fades, as MIDI items play none

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
