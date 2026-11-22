# `module` commands

Rack modules: add, edit, copy, fork, and their knobs.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `module.add@1`

Add a module to a rack: a type with knob values.

type: an id or a short name ("vca", "notes"; module.list). x, y: its top-left corner in the rack (default: right of the others). params: knob values by key in the knob's units (a choice by its key, a switch as a bool). Notes: source (the track whose MIDI it plays; default the rack's channel) and device (a live MIDI input it always plays; "" any). plugin (instead of type): a plug-in id (plugin.list) for a plugin module, which shows the plug-in's name and opens its editor; its ports are in_l, in_r, out_l, out_r and, for instruments, pitch, gate and velocity. preset: a factory preset (module.describe), params on top. Returns its size and keys.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rack` | string |  |  |  |  |
| `type` | string |  |  |  |  |
| `label` | string |  |  |  |  |
| `preset` | string |  |  |  |  |
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

**An instrument plug-in, played by cables from Notes** (`song_with_rack`)

```json
{
  "rack": "rk_1",
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "module": "mo_4",
  "type": "com.suppleaudio.rack.plugin",
  "x": "<any>",
  "y": "<any>",
  "width": "<any>",
  "height": "<any>",
  "inputs": ["in_l", "in_r", "pitch", "gate", "velocity"],
  "outputs": ["out_l", "out_r"],
  "knobs": [],
  "fx": "fx_1",
  "plugin": "Test Sine"
}

```

## `module.check@1`

Compile a module without saving; return diagnostics and description.

source (a whole module) or name (one in the library). ok is true when it compiles; type then describes it as module.describe would.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.unknown`: no such library module Hint: module.list

### Examples

**A mistake, with its hint** (`empty`)

```json
{
  "source": "struct X : Module { void process(int frames) {} };"
}

```

Result:

```json
{
  "ok": false,
  "diagnostics": "<any>"
}

```

**A library module** (`song_with_module`)

```json
{
  "name": "wobble"
}

```

Result:

```json
{
  "ok": true,
  "diagnostics": [],
  "type": "<any>"
}

```

## `module.copy@1`

Copy modules, their current plug-in settings and internal cables.

Replace this engine's clipboard, independent of the project and undo. Module order is retained; cables to unselected modules are omitted.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modules` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.unknown`: a selected module no longer exists Hint: use rack.get ids
- `module.copy`: a module occurs twice Hint: select each module once

### Examples

**Copy a VCA and its Out, with their cable** (`song_with_rack`)

```json
{
  "modules": ["mo_3", "mo_1"]
}

```

Result:

```json
{
  "modules": 2,
  "cables": 1
}

```

## `module.describe@1`

A module type's knobs, inputs and outputs, with units and help.

type (an id or a short name, "vca") or module (a module's type). Knobs: key, name, kind (number, integer, choice, toggle), unit, min, max, default, choices (keys), group, help. Ports: key, name, signal (audio, pitch in octaves from C4, gate, cv -1..1, unipolar 0..1, phase 0..1), help, and an input's normal (its value with nothing connected). A cable into a knob moves it by amount per unit of signal: octaves for a frequency, the knob's own unit otherwise.

Kind: `query`. Exposure: `mcp-exec`.

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

Kind: `edit`. Exposure: `mcp-exec`.

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

## `module.edit@1`

Edit a library module in place: replace text, then save and compile it.

name (or type, a library or project module's id). A project type is edited into the library's entry of the same id, or a free name when the id is absent. Each edit replaces old, which must be in the source exactly once, with new (empty deletes it), in order and all or none. Then as module.write: saved, compiled, and racks playing it rebuilt.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `type` | string |  |  |  |  |
| `edits` | array |  |  |  |  |
| `edits[].old` | string |  |  |  | text the source has in one place |
| `edits[].new` | string |  |  |  | what replaces it |

### Result

```json
{}

```

### Errors

- `module.unknown`: no such library module Hint: module.list
- `module.edit`: an edit's old text is not in the source, or in several Hint: copy old exactly from module.get
- `module.built_in`: built-in modules are not edited Hint: module.fork

### Examples

**Slow the tremolo down** (`song_with_module`)

```json
{
  "name": "wobble",
  "edits": [
    {
      "old": "0.1, 20, 4",
      "new": "0.1, 20, 2"
    }
  ]
}

```

Result:

```json
{
  "name": "wobble",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```

## `module.fork@1`

Copy a module type's source into the library under a new id.

type: the one to copy (stock or library). id: the copy's ("com.<you>.<name>"). name: its library name (default: the id's last part). The copy is saved and compiled as module.write does; then module.edit changes it.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  |  |  |
| `id` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown_type`: no such type Hint: module.list
- `module.host`: a host module has no source Hint: module.describe
- `module.exists`: the library has a module of that name Hint: give name

### Examples

**Your own VCA to change** (`empty`)

```json
{
  "type": "vca",
  "id": "com.example.vca2"
}

```

Result:

```json
{
  "name": "vca2",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```

## `module.get@1`

A module type's source, not a placed module's knobs (rack.get reads those).

Reads a module type's source, for writing your own; the knobs of a module placed in a rack are in rack.get (detail full lists every knob key; module.describe lists a type's knobs and ranges). type (an id or short name: every stock module's source is readable, a good start for your own) or name (a library module's file, also when it does not compile).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `type` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.unknown_type`: no such type Hint: module.list
- `module.unknown`: no such library module Hint: module.write
- `module.host`: a host module has no source Hint: module.describe

### Examples

**The stock VCA** (`empty`)

```json
{
  "type": "vca"
}

```

Result:

```json
{
  "type": "com.suppleaudio.rack.vca",
  "origin": "stock",
  "source": "<any>"
}

```

## `module.learn.list@1`

List the MIDI controllers of module knobs.

Every module's, or one module's. control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Every binding** (`song_with_rack_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "module": "mo_3",
      "param": "level",
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

## `module.learn.remove@1`

Remove a module knob's MIDI controller.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `param` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Free the level** (`song_with_rack_learn`)

```json
{
  "module": "mo_3",
  "param": "level"
}

```

Result:

```json
{}

```

## `module.learn.set@1`

Let a MIDI controller turn a module's knob.

One binding per knob (a new one replaces it). midi.device: a substring of the MIDI input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = +1), relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle. soft_takeover: an absolute control acts once it reaches the knob. A knob turns along its travel (a frequency's is logarithmic); each movement is one undo step. A plugin module's knobs are its plug-in's: fx.learn.set.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `param` | string |  |  |  |  |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |

### Result

```json
{}

```

### Examples

**The VCA's level on CC 21** (`song_with_rack`)

```json
{
  "module": "mo_3",
  "param": "level",
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
  "param": "level",
  "control": "CC 21 · ch 1"
}

```

## `module.list@1`

List module types: id, name, tags, what they do.

Host modules (Out, In, Notes, Transport) first, then stock, the user's library and the project's. query matches names, ids, tags and descriptions; tag one tag.

Kind: `query`. Exposure: `mcp-exec`.

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

**Find the VCA** (`empty`)

```json
{
  "query": "vca"
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

Kind: `edit`. Exposure: `mcp-exec`.

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

Kind: `edit`. Exposure: `mcp-exec`.

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

params merge into its knobs (a number in the knob's unit, or text in it, "80 %" or "1.2 kHz"; a percent knob such as a Macro's runs 0 to 100; a number outside the knob's range is refused with params.out_of_range, never clamped; a choice by key; a default value is dropped from the file). A knob edit is heard at once, without a recompile. Notes source/device accept null to clear them (source returns to the home track; device to none). source: none disables the track feed while keeping routes and device. source_sends: false stops it hearing MIDI sends into its source track (its own sends and device still play). bypass passes each input to the output of its kind. label names it (an Out module's label is what channels' input menus list). preset: a factory preset by name (module.describe lists them): every knob to it, then params on top.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `module` | string |  |  |  |  |
| `params` | value |  |  |  |  |
| `label` | string |  |  |  |  |
| `preset` | string |  |  |  |  |
| `x` | number |  | -1e+06..1e+06 |  |  |
| `y` | number |  | -1e+06..1e+06 |  |  |
| `bypass` | boolean |  |  |  |  |
| `source` | value or null |  |  |  |  |
| `device` | string or null |  |  |  |  |
| `source_sends` | boolean |  |  |  |  |

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

## `module.write@1`

Write a module to the library and compile it; racks use it at once.

name: its library name (lower case, digits, '_' or '-'). source: the whole module, a struct deriving from Module (the module guide; module.get shows any type's source). The file is saved even when it does not compile, so module.edit can fix it; ok and diagnostics (line, column, hint) say how it went. Once it compiles, type describes it as module.describe does and module.add takes its id; modules already playing it are rebuilt with the new code (reloaded counts them), their knobs kept. A version that does not compile leaves racks on the last that did.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `module.name`: not a library name Hint: lower case, digits, '_' or '-'
- `module.size`: more than 1 MiB Hint: split it up
- `module.id_taken`: a built-in or another library module has its id Hint: give ModuleInfo.id your own id

### Examples

**A tremolo module** (`song_with_rack`)

```json
{
  "name": "wobble",
  "source": "#include <supple/script.h>\n\n// Tremolo: a sine LFO moves the input's level.\nstruct Wobble : Module {\n    static constexpr ModuleInfo info = {.name = \"Wobble\",\n                                        .id = \"com.example.wobble\",\n                                        .vendor = \"Example\",\n                                        .description = \"A sine LFO moves the input's level.\",\n                                        .tags = {\"amplifier\", \"modulation\"}};\n\n    Input in = audioIn(\"In\").help(\"What it moves.\");\n    Param rate = frequency(\"Rate\", 0.1, 20, 4).help(\"The LFO's speed.\");\n    Param depth = percent(\"Depth\", 50).help(\"How far the level dips.\");\n    Output out = audioOut(\"Out\").help(\"The input, wobbling.\");\n\n    double phase = 0;\n\n    void process(int frames) {\n        for (int i = 0; i < frames; ++i) {\n            const double gain = 1.0 - depth.at(i) / 100.0 * (0.5 + 0.5 * sin(2 * pi * phase));\n            for (int c = 0; c < channels(); ++c) out[c][i] = in[c][i] * gain;\n            phase += rate.at(i) / sampleRate();\n            phase -= floor(phase);\n        }\n    }\n};\n"
}

```

Result:

```json
{
  "name": "wobble",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "type": "<any>",
  "reloaded": 0
}

```
