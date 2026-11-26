# `track` commands

Tracks, channels, busses: add, set level/pan/arm, move, freeze.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

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

Each copy goes right after its original, under the same parent, with the same settings and name; its main and input chains (each plugin's settings as it sounds now, as fx.copy), items and takes (unpooled MIDI copied, audio media shared), sends and record input, the envelopes on the track, its plugins, sends and takes, and every modulation route whose target is on the track (its fader, pan, width, sends' levels and plugins, take FX included). Not copied: a folder's children, sends into the track, and item groups. Masters cannot be duplicated. One undo step; the copies' ids in the order given.

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
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

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
- `modulation.cycle`: the track's parent send would feed a rack what it modulates Hint: remove the modulation route first (modulation.remove), or choose another parent

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
- `modulation.cycle`: the parent send would feed a rack what it modulates Hint: remove the modulation route first (modulation.remove), or leave the parent send off

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
