# `fx` commands

Plug-ins on tracks and takes: add, set, parameters, patches, presets.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `fx.add@1`

Insert a plugin into a track's (or a take's) FX chain and return its id.

plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic plugins. chain: main (default; the track's effects) or input (the recorded-input chain). take instead of track: the take's own chain (take FX) — the item plays through it before the track's input, cut to the item; an instrument there plays a MIDI take on its own. An instrument with outputs past its main pair (a multi-out sampler's Kick, Snare...) gets that many more channels on the track, and its pins.out sends each output there; route.matrix lists them and route.add {channels: {from: [3, 4]}} sends one to a buss. Without index, a note effect (an arpeggiator) goes to the front of the track's chain, ahead of what it plays, and an instrument after the note effects there.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `take` | string |  |  |  | Take id, e.g. "tk_1". |
| `plugin` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |
| `patch` | value |  |  |  |  |
| `chain` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_add",
  "properties": {
    "fx": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "semantic": {
      "type": "boolean"
    },
    "chain": {
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
  "required": ["fx", "name", "semantic", "chain"],
  "additionalProperties": false
}

```

### Errors

- `plugin.unknown`: no such plugin Hint: closest ids in the hint; see plugin.list

### Examples

**An instrument on the bass track** (`song`)

```json
{
  "track": "tr_1",
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "Test Sine",
  "semantic": false,
  "chain": ["fx_1"]
}

```

**An effect in the track's input chain** (`song`)

```json
{
  "track": "tr_1",
  "plugin": "com.suppleaudio.test.gain",
  "chain": "input"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["fx_1"]
}

```

**Take FX: a gain on the take of an item** (`song_with_item`)

```json
{
  "take": "tk_1",
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["fx_1"]
}

```

## `fx.chain.delete@1`

Delete a saved FX chain preset by name.

Removes the preset file from the user's chains directory. Tracks that loaded it keep their effects.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_delete_result",
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

- `chain.unknown`: no preset has that name Hint: use a name from fx.chain.list

### Examples

**Delete the bass chain** (`song_with_saved_chain`)

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

## `fx.chain.list@1`

List saved FX chain presets by name, path and effect count.

Lists valid supple.fx-chain/1.0 files, sorted by name and path. Invalid documents are skipped with warnings. See fx.chain.save and fx.chain.load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_list_result",
  "properties": {
    "chains": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "fx_chain_entry",
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
  "required": ["chains"],
  "additionalProperties": false
}

```

### Examples

**List presets** (`empty`)

```json
{}

```

Result:

```json
{
  "chains": []
}

```

## `fx.chain.load@1`

Append or replace a track's FX chain from a preset in one undo step.

Give exactly one of name (in the user's chains directory) or path (relative to the work directory). mode defaults to append; replace removes the target chain, its envelopes and key routes. Validates every plugin and state before changing the model.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `path` | string |  |  |  |  |
| `chain` | string |  |  | "main" |  |
| `mode` | string |  |  | "append" |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_load_result",
  "properties": {
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
  "required": ["effects"],
  "additionalProperties": false
}

```

### Errors

- `chain.source`: name/path missing or both given Hint: give exactly one of name or path
- `chain.plugin_missing`: a saved plugin is not installed Hint: install the named plugin or choose another preset

### Examples

**Load a saved bass chain** (`song_with_saved_chain`)

```json
{
  "track": "tr_1",
  "name": "Bass",
  "mode": "replace"
}

```

Result:

```json
{
  "effects": ["<any>", "<any>"]
}

```

## `fx.chain.save@1`

Save an ordered FX chain as a named preset.

Save main (default) or input FX to the user's chains directory. Preserves semantic patches or opaque base64 state. Refuses empty chains; replace: true explicitly overwrites an existing name.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `chain` | string |  |  | "main" |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_chain_save_result",
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

### Errors

- `chain.name`: invalid preset name Hint: use a name without path separators
- `chain.empty`: the chain has no effects Hint: add effects before saving
- `chain.exists`: the name already exists Hint: choose another name or set replace: true

### Examples

**Save the bass chain** (`song_with_chain`)

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

## `fx.copy@1`

Copy a plugin and its current settings into a track's FX chain.

Adds a new instance of fx's plugin to track's main (default) or input chain, at index (default: the end), with the source's options (label, bypass, offline, wet, isolation, oversampling, parallel, pdc) and its settings as they are now: the running plugin's own state when the engine has one (editor edits included), else the model's patch or state. Not copied: envelopes, key routes into the source, and, when the track differs, channel pins. The source stays where it is; use fx.move to move it. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `track` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |
| `chain` | string |  |  | "main" |  |

### Result

```json
{
  "type": "object",
  "title": "fx_copy_result",
  "properties": {
    "fx": {
      "type": "string"
    },
    "chain": {
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
  "required": ["fx", "chain"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown`: no such plugin instance Hint: use an fx id returned by fx.add, fx.copy or fx.chain.load
- `fx.index`: index is past the end of the chain Hint: use 0 to the chain's size

### Examples

**Duplicate a plugin at the end of its own chain** (`song_with_fx`)

```json
{
  "fx": "fx_1",
  "track": "tr_1"
}

```

Result:

```json
{
  "fx": "<any>",
  "chain": ["fx_1", "fx_2", "<any>"]
}

```

## `fx.describe@1`

Describe an instance and its live problems; full adds parameters and patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `full` | boolean |  |  | false |  |
| `filter` | string |  |  |  | Plain parameters only: list those whose name contains this text (case-insensitive). |

### Result

```json
{
  "type": "object",
  "title": "fx_description",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "description": {
      "type": "string"
    },
    "params": {
      "description": "With full: each parameter; modulatable_by_envelope: true when a route reaches it through its envelope (ADR-0096 M4); modulatable: true when a modulation route can take it (modulation.add)"
    },
    "patch": {},
    "status_available": {
      "type": "boolean",
      "description": "A live instance supplied its current problems"
    },
    "problems": {
      "description": "Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable"
    },
    "sidechain_channels": {
      "type": "integer",
      "description": "The plug-in's key input: route.add {fx} feeds it"
    },
    "latency_samples": {
      "type": "integer",
      "description": "The instance's reported latency, in samples"
    },
    "isolation_effective": {
      "type": "string",
      "description": "Where the instance runs: in_process, shared_bridge or dedicated; inherit resolves to the plug-in isolation preference (stock plug-ins in process), as the instance is made"
    },
    "override": {
      "type": "string",
      "description": "This run's plugin.override for the plug-in, when it is not normal: bridge (it runs on a dedicated bridge) or disable (it is not loaded: the strip passes audio through, a rack module is silent, and there is no isolation_effective or parameter section)"
    },
    "outputs_notes": {
      "type": "boolean",
      "description": "The loaded plug-in sends notes or MIDI: fx.set midi_output chooses replace or merge"
    },
    "params_total": {
      "type": "integer",
      "description": "Plain parameters matching; at most 512 are listed."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "name", "description", "status_available"],
  "additionalProperties": false
}

```

### Examples

**Describe** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "description": "<any>",
  "latency_samples": "<any>",
  "isolation_effective": "in_process",
  "status_available": false
}

```

## `fx.learn.list@1`

List the MIDI controller bindings of plugin parameters.

Every effect's, or one effect's (fx). control names the message in words.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_learn_list",
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

**Every binding** (`song_with_learn`)

```json
{}

```

Result:

```json
{
  "learn": [
    {
      "fx": "fx_2",
      "param": "gain",
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

## `fx.learn.remove@1`

Remove a plugin parameter's MIDI controller binding.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
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

**Free the gain** (`song_with_learn`)

```json
{
  "fx": "fx_2",
  "param": "gain"
}

```

Result:

```json
{}

```

## `fx.learn.set@1`

Let a MIDI controller (CC, note, pitch bend) move a plugin parameter.

One binding per parameter (a new one replaces it). midi.device: a substring of the MIDI input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = +1), relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle (above 0 flips it). soft_takeover: an absolute control acts once it reaches the value. when_selected / when_focused / when_visible: only while the effect's track is selected, or its window or editor has the focus or shows (the GUI).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  | A parameter's semantic key, id or name (fx.describe); a semantic plugin's is stored as its key |
| `midi` | object |  |  |  |  |
| `midi.kind` | string |  |  |  | One of: cc, note, pitch_bend, program. |
| `midi.channel` | integer |  | 1.0..16.0 |  |  |
| `midi.number` | integer |  | 0.0..127.0 | 0 |  |
| `midi.device` | string |  |  | "" |  |
| `mode` | string |  |  |  | One of: absolute, relative1, relative2, relative3, toggle. |
| `soft_takeover` | boolean |  |  |  |  |
| `when_selected` | boolean |  |  |  |  |
| `when_focused` | boolean |  |  |  |  |
| `when_visible` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_learn_set_result",
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

### Examples

**Gain on CC 21** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain",
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
  "param": "gain",
  "control": "CC 21 · ch 1"
}

```

## `fx.meters@1`

Read a plugin's meters and readouts, playing now or as a render ended.

The read-only parameters of a plugin: a semantic plugin's by patch key in physical units (an analyzer's loudness, a reverb's RT60, a gate's suggested threshold), any other plugin's by name as CLAP values. from live (the default): the playing instance's, which move while it processes (playback, monitoring); source none while no instance runs (start the engine). from render: as the last render.mix ended, which renders through instances of its own; source none until a render.mix has included this plugin.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `from` | string |  |  | "live" | live: the playing instance; render: the last render.mix One of: live, render. |

### Result

```json
{
  "type": "object",
  "title": "fx_meters",
  "properties": {
    "source": {
      "type": "string",
      "description": "live, render, or none (nothing to read yet)"
    },
    "meters": {
      "description": "Read-only values by patch key (physical) or name"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["source", "meters"],
  "additionalProperties": false
}

```

### Examples

**Read meters** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "source": "<any>",
  "meters": "<any>"
}

```

## `fx.move@1`

Move a plugin within a track chain or to another track.

Works on the track's chain, on its input chain (an input-chain effect stays on its track) and on a take's chain (take FX stay on their take: index only).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
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

- `modulation.cycle`: the move puts a modulated take or FX where its rack hears it Hint: remove the route, or choose another track

### Examples

**Move a gain first** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "index": 0
}

```

Result:

```json
{}

```

## `fx.param.get@1`

Read one saved plugin parameter by semantic key or numeric CLAP id.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `param` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_param_result",
  "properties": {
    "value": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["value"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain"
}

```

Result:

```json
{
  "value": "<any>"
}

```

## `fx.param.set@1`

Set semantic parameters in units, or plain plugin parameters by name/id.

Plain CLAP/VST3 values use the declared range or plugin text; read-only parameters refuse edits.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `values` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Set the gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "values": {
    "gain": -3
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.patch.get@1`

Read a semantic plugin's saved patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `changed_only` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `fx.patch.list@1`

List an instance's read-only factory patches, by name.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_list_result",
  "properties": {
    "patches": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patches"],
  "additionalProperties": false
}

```

### Examples

**List factory patches** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "patches": []
}

```

## `fx.patch.merge@1`

Merge a partial patch into a plugin instance; returns the effective change.

Semantic plugins only. Values in physical units: {"params": {"gain": "-6 dB"}}. changed holds only the members the merge changed: against the saved patch, or for an instance never saved against the plug-in's defaults (a value set to what it already is changes nothing).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Turn the gain down** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "params": {
      "gain": "-6 dB"
    }
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.patch.set@1`

Replace a semantic patch, or apply a factory patch by name.

Give exactly one of patch or name. Factory names come from fx.patch.list; omitted parameters take defaults.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `patch` | value |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {},
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Replace gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "schema": "supple.test-gain-patch/1.0",
    "params": {
      "gain": -6
    }
  }
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `fx.preset.delete@1`

Delete one of the user's presets for a plugin; factory presets stay.

Removes the user preset's file. Factory presets belong to the plugin and cannot be deleted. Instances that loaded the preset keep their settings.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_delete_result",
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

- `fx.unknown_preset`: no user preset has that name Hint: choose a user preset from fx.preset.list
- `fx.preset_factory`: factory presets cannot be deleted Hint: delete only presets whose source is user

### Examples

**Delete a saved preset** (`song_with_preset`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `fx.preset.list@1`

List a plugin's presets: its factory ones, then the user's.

source factory: the plugin's own patches in its order (read-only; the same as fx.patch.list's names). source user: the user's saved presets for this plugin, sorted by name, with their file path. See fx.preset.load and fx.preset.save.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_list_result",
  "properties": {
    "presets": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "fx_preset_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "source": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["name", "source"],
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
  "required": ["presets"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown`: no such plugin instance Hint: use an fx id from fx.add

### Examples

**List presets** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "presets": []
}

```

## `fx.preset.load@1`

Apply a factory or user preset to a plugin instance in one undo step.

name from fx.preset.list. source (factory or user) is needed only when both have the name. A factory preset or a semantic plugin's user preset replaces the whole patch (omitted parameters take defaults); a plain plugin's user preset is its saved state. Options such as wet and bypass are not part of a preset.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_load_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "source"],
  "additionalProperties": false
}

```

### Errors

- `fx.unknown_preset`: no preset has that name Hint: choose a name from fx.preset.list
- `fx.preset_ambiguous`: a factory and a user preset share the name Hint: add source: factory or source: user
- `fx.preset_invalid`: the preset file is not valid for this plugin Hint: save the preset again

### Examples

**Load a saved preset** (`song_with_preset`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "name": "Quiet",
  "source": "user"
}

```

## `fx.preset.save@1`

Save a plugin instance's current settings as a named user preset.

Saves what the running plugin has now (editor edits included): the patch of a semantic plugin, else its state. The preset is the user's, for every instance of this plugin in any project. A factory preset's name cannot be taken; replace: true overwrites a user preset of that name.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_preset_save_result",
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

- `fx.preset_name`: invalid preset name Hint: use a name without path separators
- `fx.preset_factory`: a factory preset has the name Hint: choose another name; factory presets are read-only
- `fx.preset_exists`: a user preset has the name Hint: choose another name or set replace: true

### Examples

**Save the gain's settings** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `fx.remove@1`

Remove a plugin instance (and its envelopes).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

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

**Remove** (`song_with_fx`)

```json
{
  "fx": "fx_1"
}

```

Result:

```json
{}

```

## `fx.restart@1`

Re-create a plugin instance from its saved patch/state (after a crash).

engine.status lists failed_fx: isolated plugins whose process crashed or hung. The new instance starts from the model's patch or state.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

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

**Restart** (`song_with_fx`)

```json
{
  "fx": "fx_1"
}

```

Result:

```json
{}

```

## `fx.set@1`

Set instance options: bypass, offline, wet, label, isolation, pins, MIDI.

isolation: in_process, dedicated (the plugin runs in a process of its own: a crash silences only it), shared_bridge (one process for all such plugins), inherit (the project default: in_process). pins: which of the track's channels (1-based; track.set channels adds 3-128) the plugin reads ({in: [l, r]}), writes ({out: [l, r]}) and keys from ({key: [3, 4]}: a detector on 3/4, for a plugin with a sidechain input); {} restores 1/2 in and out. out past its first two entries: a track channel for each of the plugin's auxiliary output channels in order (0: unconnected); fx.add sets them for a multi-out instrument. midi_output, for a plug-in with note output (fx.describe outputs_notes): replace (default) gives the plug-ins after it, and the track's post-FX MIDI sends, only its output; merge adds its output to the MIDI that reached it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `bypass` | boolean |  |  |  |  |
| `offline` | boolean |  |  |  |  |
| `wet` | number or string | % | 0.0..100.0 |  | Quantity strings are range-checked by the server. |
| `label` | string |  |  |  |  |
| `isolation` | string |  |  |  | One of: inherit, in_process, shared_bridge, dedicated. |
| `pins` | object |  |  |  |  |
| `pins.in` | array |  |  | [] |  |
| `pins.out` | array |  |  | [] |  |
| `pins.key` | array |  |  | [] |  |
| `midi_output` | string |  |  |  | One of: replace, merge. |

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

**Half wet** (`song_with_fx`)

```json
{
  "fx": "fx_1",
  "wet": "50%"
}

```

Result:

```json
{}

```

**Key the plugin from channels 3/4** (`song_with_keymix_4ch`)

```json
{
  "fx": "fx_1",
  "pins": {
    "key": [3, 4]
  }
}

```

Result:

```json
{}

```

## `fx.state.capture@1`

Record a plug-in's own state change as a replayable edit.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plug-in id, e.g. "fx_1". |
| `state` | string |  |  |  | The state's blob hash, as the plug-in's state field holds it |

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
