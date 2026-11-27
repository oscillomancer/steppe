# `modulation` commands

Rack modulation routes: Mod Out to plug-in parameters.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `modulation.add@1`

Route a rack's Mod Out to a plug-in parameter or mixer level.

The plug-in sits in a track's chain or input chain, or in a take's chain (an item's or a clip's: copies of the take keep the route). depth is the offset at a signal of 1: for a plug-in parameter a fraction of its CLAP range (-1..1). A parameter fx.describe marks modulatable takes the offset directly; one marked modulatable_by_envelope (automatable, continuous, not modulatable, or any VST3's) takes it through its active envelope, which must exist (modulation.envelope; modulate adds one), and for a VST3 depth is a fraction of its normalized range; for a track's or send's volume in dB (-48..48); for a track's pan or width in percent (-200..200). The offset adds to the value automation or the fader gives, and the sum stays in the target's range. Another enabled route onto the same target from the same kind of source (the module cabled into the Mod Out input) is allowed with the warning modulation.stacked, which names the routes already there: their offsets add.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | object |  |  |  |  |
| `source.module` | string |  |  |  | Mod Out module id, e.g. "mo_1". |
| `source.input` | string |  |  |  | One of the Mod Out's inputs, in_1 .. in_16. |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `depth` | number |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `lane` | boolean |  |  |  | Show the target's automation lane in the timeline while the route exists (default true). It never changes the sound. |

### Result

```json
{
  "type": "object",
  "title": "modulation_added",
  "properties": {
    "modulation": {
      "type": "string"
    },
    "param": {
      "type": "string",
      "description": "A plug-in target's canonical reference: its semantic key or decimal id."
    },
    "param_name": {
      "type": "string",
      "description": "A plug-in target's display name."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["modulation"],
  "additionalProperties": false
}

```

### Errors

- `modulation.source`: the source is not a Mod Out module Hint: use rack.get to find the rack's Mod Out, or add one with module.add
- `modulation.target`: the target is missing, not one object, or not modulatable Hint: check fx.describe or the track's params, and name one fx, track or route
- `modulation.envelope`: the parameter takes routes through an envelope it lacks Hint: add one with env.add, or use modulate, which adds it
- `modulation.depth`: the depth is outside the target's range Hint: -1..1 for a plug-in parameter, -48..48 dB for volume, -200..200 % for pan or width
- `modulation.duplicate`: the same source already reaches that parameter Hint: change that route's depth with modulation.set instead
- `modulation.cycle`: the route would let its rack hear what it modulates Hint: modulate another track, or remove the send into the rack's In module
- `modulation.limit`: the project holds the most routes it can Hint: remove routes you no longer need with modulation.remove

### Examples

**Add** (`song_with_mod_out`)

```json
{
  "source": {
    "module": "mo_2",
    "input": "in_1"
  },
  "target": {
    "fx": "fx_1",
    "param": "gain"
  },
  "depth": 0.25
}

```

Result:

```json
{
  "modulation": "mr_1",
  "param": "gain",
  "param_name": "<any>"
}

```

## `modulation.list@1`

List modulation routes by target or source rack.

Each route with its source signal's range (bipolar -1..1, or unipolar 0..1 when every cable into the Mod Out input is unipolar, a gate or a phase), fed_by (each cable into that input: its module, output and module type) and, when the plan cannot apply it, inert: source (nothing is cabled into the input), target (the parameter cannot take it, or the target's track is frozen and its level is outside the print), envelope (a parameter reached through its envelope, which is gone or off), latency (its plug-in changed latency while modulated; editing a route into it turns them back on) or take (its plug-in is in the chain of a take that does not play: not its item's or clip's active take, or its item is muted; in a comp, each piece's copy of a take answers for that piece). owner: for a plug-in in a take's chain, the item or clip and the take that hold it; track lists those too.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Target track id, e.g. "tr_1". |
| `fx` | string |  |  |  | Target plug-in instance id, e.g. "fx_1". |
| `rack` | string |  |  |  | Source rack id, e.g. "rk_1". |

### Result

```json
{
  "type": "object",
  "title": "modulation_list",
  "properties": {
    "modulations": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "modulation_entry",
        "properties": {
          "modulation": {
            "type": "string"
          },
          "source": {
            "type": "object",
            "title": "modulation_source",
            "properties": {
              "module": {
                "type": "string",
                "pattern": "^mo_[0-9]+$",
                "description": "Mod Out module id, e.g. \"mo_1\"."
              },
              "input": {
                "type": "string",
                "pattern": "^in_([1-9]|1[0-6])$",
                "description": "One of the Mod Out's inputs, in_1 .. in_16."
              }
            },
            "required": ["module", "input"],
            "additionalProperties": false
          },
          "target": {
            "type": "object",
            "title": "modulation_target",
            "properties": {
              "fx": {
                "type": "string",
                "pattern": "^fx_[0-9]+$",
                "description": "Plugin instance id, e.g. \"fx_1\"."
              },
              "track": {
                "type": "string",
                "pattern": "^tr_[0-9]+$",
                "description": "A track's volume, pan or width."
              },
              "route": {
                "type": "string",
                "pattern": "^sd_[0-9]+$",
                "description": "A send's volume."
              },
              "param": {
                "type": "string",
                "minLength": 1,
                "description": "A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route."
              }
            },
            "required": ["param"],
            "additionalProperties": false
          },
          "depth": {
            "type": "number"
          },
          "enabled": {
            "type": "boolean"
          },
          "lane": {
            "type": "boolean",
            "description": "The route asks for its target's lane to be shown (a display flag; the sound is the same either way)"
          },
          "range": {
            "type": "string"
          },
          "fed_by": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "modulation_fed_by",
              "properties": {
                "module": {
                  "type": "string"
                },
                "output": {
                  "type": "string"
                },
                "type": {
                  "type": "string"
                }
              },
              "required": ["module", "output", "type"],
              "additionalProperties": false
            }
          },
          "inert": {
            "type": "string"
          },
          "param_name": {
            "type": "string",
            "description": "A plug-in target's display name."
          },
          "owner": {
            "type": "object",
            "title": "modulation_owner",
            "properties": {
              "item": {
                "type": "string"
              },
              "clip": {
                "type": "string"
              },
              "take": {
                "type": "string"
              }
            },
            "required": ["take"],
            "additionalProperties": false,
            "description": "A take FX target's take, and the item or clip that holds it"
          }
        },
        "required": [
          "modulation",
          "source",
          "target",
          "depth",
          "enabled",
          "lane",
          "range",
          "fed_by"
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
  "required": ["modulations"],
  "additionalProperties": false
}

```

### Examples

**List** (`song_with_modulation`)

```json
{}

```

Result:

```json
{
  "modulations": [
    {
      "modulation": "mr_1",
      "source": {
        "module": "mo_2",
        "input": "in_1"
      },
      "target": {
        "fx": "fx_1",
        "param": "gain"
      },
      "depth": 0.25,
      "enabled": true,
      "lane": true,
      "range": "bipolar",
      "fed_by": [],
      "inert": "source",
      "param_name": "<any>"
    }
  ]
}

```

## `modulation.remove@1`

Remove a modulation route.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modulation` | string |  |  |  | Modulation route id, e.g. "mr_1". |

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

**Remove** (`song_with_modulation`)

```json
{
  "modulation": "mr_1"
}

```

Result:

```json
{}

```

## `modulation.set@1`

Change a modulation route's source, target, depth, enabled or lane.

Omitted fields keep their values. A depth drag sends Origin.gesture, so the drag is one undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `modulation` | string |  |  |  | Modulation route id, e.g. "mr_1". |
| `source` | object |  |  |  |  |
| `source.module` | string |  |  |  | Mod Out module id, e.g. "mo_1". |
| `source.input` | string |  |  |  | One of the Mod Out's inputs, in_1 .. in_16. |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `depth` | number |  |  |  |  |
| `enabled` | boolean |  |  |  |  |
| `lane` | boolean |  |  |  | Show or hide the target's automation lane for this route. It never changes the sound. |

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

- `modulation.source`: the source is not a Mod Out module Hint: use rack.get to find the rack's Mod Out, or add one with module.add
- `modulation.target`: the target is missing, not one object, or not modulatable Hint: check fx.describe or the track's params, and name one fx, track or route
- `modulation.envelope`: the parameter takes routes through an envelope it lacks Hint: add one with env.add, or use modulate, which adds it
- `modulation.depth`: the depth is outside the target's range Hint: -1..1 for a plug-in parameter, -48..48 dB for volume, -200..200 % for pan or width
- `modulation.duplicate`: the same source already reaches that parameter Hint: change that route's depth with modulation.set instead
- `modulation.cycle`: the route would let its rack hear what it modulates Hint: modulate another track, or remove the send into the rack's In module

### Examples

**Deepen** (`song_with_modulation`)

```json
{
  "modulation": "mr_1",
  "depth": 0.5
}

```

Result:

```json
{}

```

## `modulation.telemetry@1`

Read each modulation route's live offset and span.

offset: the route's target's current offset, the sum of every route on it, as the engine last applied it (a plug-in parameter's as a fraction of its range, a volume's in dB, a pan's or width's in percent); absent while no running plan applies the route. low and high: what this route alone can add, depth times its signal range (bipolar -1..1, unipolar 0..1), in the same unit; absent while nothing is cabled into its Mod Out input.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Target track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "modulation_telemetry",
  "properties": {
    "modulations": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "modulation_reading",
        "properties": {
          "modulation": {
            "type": "string"
          },
          "offset": {
            "type": "number"
          },
          "low": {
            "type": "number"
          },
          "high": {
            "type": "number"
          }
        },
        "required": ["modulation"],
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
  "required": ["modulations"],
  "additionalProperties": false
}

```

### Examples

**Stopped, nothing cabled into the route's input yet** (`song_with_modulation`)

```json
{}

```

Result:

```json
{
  "modulations": [
    {
      "modulation": "mr_1"
    }
  ]
}

```
