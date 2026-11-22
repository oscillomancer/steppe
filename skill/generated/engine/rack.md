# `rack` commands

Modular racks on channels: add, arrange, patch, levels, presets.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

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
