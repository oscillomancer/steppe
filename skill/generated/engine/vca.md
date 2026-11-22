# `vca` commands

VCA faders: add, members, learn.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

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
