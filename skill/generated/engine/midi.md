# `midi` commands

MIDI items and ports: edit notes and CCs, inputs and outputs.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `midi.edit@1`

Edit a MIDI item's notes and CCs: move, resize, quantize, fit to key or chord.

ops run in order and are one undo step. notes: indices into midi.notes' list as it was before this command (every op refers to that list, so later ops are not affected by earlier removals). Beats are from the item's start. add {pitch, start, length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid, at the note's place in the item's roll), strength? 0-1, ends?}; cc.set {number, points [{beat, value}], channel?}; cc.clear {number, from?, to?}. Scale ops read the context track (key.set, chord.set) at the project beat each note starts, in the order of the ops: transpose_degrees {notes, degrees, key?} moves notes by scale degrees (a pitch outside the scale first goes down to the scale); snap_to_scale {notes? (all), mode? nearest|down|up, key?} moves notes outside the scale to it (nearest: a tie goes down); fit_to_chord {notes? (all), chord?} moves each note to the nearest tone of the chord (a tie goes down; a slash bass counts). key is "Eb major", chord is "Cm7"; given, they replace the context. A note that is looped or shared by another item in a different key or chord is refused with midi.context_varies; none holding is midi.no_key or midi.no_chord. On a clip they read the clip's own context (the key and chords it was written against, clip.set {context}) at the clip beat each note sounds, never the project's. On an item whose take follows (take.set {follow}) they read the take's own context at each note's context beat, and act on the notes as written: the answer says acts_on: "written" when some heard note sounds otherwise (take.bake writes what sounds). Returns the new indices of the notes the ops added or changed (to keep a selection).

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
| `sounding` | boolean |  |  | false | Answer what sounds (a following take's notes as they play), in item time, not what is written. A clip is refused: midi.sounding_clip. |
| `from` | number |  |  |  | With sounding: the first song beat; the heard notes of from..to answer, passes as they fall. |
| `to` | number |  |  |  | With sounding: the end song beat. |

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
    "take_context": {
      "type": "string"
    },
    "sounding_differs": {
      "type": "integer"
    },
    "differs_by_pass": {
      "type": "boolean"
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

### Errors

- `midi.sounding_clip`: a clip has no place in time, so what it sounds as is not defined Hint: ask launch.state how it follows, or lay it on the timeline first
- `midi.range_args`: from and to go with sounding Hint: give sounding: true, or leave from and to out

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

**Read what a following take sounds as** (`song_following`)

```json
{
  "item": "it_1",
  "sounding": true
}

```

Result:

```json
{
  "text": "<any>",
  "context": "<any>",
  "follow": "key",
  "take_context": "1: key C major ~:w\n",
  "sounding_differs": 2
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

Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their position in this list. start/length/beat are in beats from the item's start; channel is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them too. For writing whole phrases, midi.write with notation is usually better. Errors: midi.sounding_clip (sounding on a clip: what it sounds as depends on where it is launched).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item or clip id, e.g. "it_1" or "cl_1". |
| `sounding` | boolean |  |  | false | Add each note's sounding pitch as `sounds` (a following take's notes as they play; the first pass of a looped item, and differs_by_pass when later passes differ). `pitch` stays the written one. A clip is refused: midi.sounding_clip. |

### Result

```json
{}

```

### Errors

- `midi.sounding_clip`: sounding was asked of a clip Hint: launch.state says how a launched clip follows; lay the clip on the timeline (item.add {clip}) to read what sounds

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

**A following take's notes with the pitches they sound as** (`song_following`)

```json
{
  "item": "it_1",
  "sounding": true
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

The item or clip keeps its length: notation longer than it is stored but does not play past its end, and midi.beyond_length warns. Lengthen it first (item.set length, clip.set length), or write the notation to fit. The notes are stored as written: on a take that follows (take.set {follow}) its context and follow stay, so the new notes sound as the context says, and the answer's follows says key or chords. Chord symbols in the text do not change a take's context (take.set does).

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
  "title": "midi_write_result",
  "properties": {
    "follows": {
      "type": "string"
    },
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
