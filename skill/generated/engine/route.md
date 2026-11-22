# `route` commands

Sends, sidechain keys and MIDI routes between tracks.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

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
