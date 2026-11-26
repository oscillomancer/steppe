# `modulate` commands

Modulate one parameter or mixer level with an LFO or follower.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `modulate@1`

Modulate a parameter or mixer level: LFO, follower, random, macro.

The whole gesture in one undo step. It takes the first rack homed on the target's track that holds a Mod Out, else the first rack named Modulators, else adds one on a new channel named Modulators (rack: choose one instead). It adds the source module (lfo: an LFO; random: an LFO's Random Steps preset; macro: a Macro, its first knob; follower: a Follower fed by an In module and a send from the track from), cables it into the first free Mod Out input (adding a Mod Out when none is free) and routes that input to the target. {module, input} routes a Mod Out input already patched. For a macro, knob names the knob to move. depth as modulation.add; default a quarter of a plug-in parameter's range, 6 dB, or 50 percent. A parameter reached through its envelope (fx.describe: modulatable_by_envelope) gets a one-point envelope at its current value first when it has none. A rack that would close a loop (it already drives the track a follower hears, or hears the target) is passed over for the next, or a new Modulators rack. Only a real cycle is refused (modulation.cycle): a follower on a track the target feeds, itself included. target: {fx, param} (a plug-in parameter by key, name or CLAP id; fx.describe full marks the modulatable ones), {track, param: volume|pan|width} or {route, param: volume}. The source module is left set to its defaults: change it with module.set {module, params} on the module this returns (an LFO's rate, shape or sync; a follower's gain and threshold; a macro's macro_1, which sits at 0 until moved, so a macro does nothing at first; module.describe {type} lists the knobs) and the route's depth with modulation.set. A second route from the same kind of source onto a target that already has one warns modulation.stacked (their offsets add).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `target` | object |  |  |  |  |
| `target.fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `target.track` | string |  |  |  | A track's volume, pan or width. |
| `target.route` | string |  |  |  | A send's volume. |
| `target.param` | string |  |  |  | A plug-in parameter's key, name or decimal CLAP id (fx.describe marks the modulatable ones); volume, pan or width for a track; volume for a route. |
| `source` | string or object |  |  |  |  |
| `source.module` | string |  |  |  |  |
| `source.input` | string |  |  |  |  |
| `depth` | number |  |  |  |  |
| `from` | string |  |  |  | The track a follower listens to. |
| `rack` | string |  |  |  | Rack id, e.g. "rk_1". |

### Result

```json
{
  "type": "object",
  "title": "modulated",
  "properties": {
    "modulation": {
      "type": "string"
    },
    "param": {
      "type": "string",
      "description": "A plug-in target's canonical reference."
    },
    "param_name": {
      "type": "string",
      "description": "A plug-in target's display name."
    },
    "rack": {
      "type": "string"
    },
    "module": {
      "type": "string"
    },
    "mod_out": {
      "type": "string"
    },
    "input": {
      "type": "string"
    },
    "send": {
      "type": "string"
    },
    "channel": {
      "type": "string",
      "description": "The channel made for a new rack."
    },
    "knob": {
      "type": "string",
      "description": "A macro's knob to move, by key."
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
- `modulation.depth`: the depth is outside the target's range Hint: -1..1 for a plug-in parameter, -48..48 dB for volume, -200..200 % for pan or width
- `modulation.duplicate`: the same source already reaches that parameter Hint: change that route's depth with modulation.set instead
- `modulation.cycle`: the route, or a follower on a track the target feeds, would let the rack hear what it modulates Hint: listen to another track, or modulate a buss it feeds
- `modulation.limit`: the project holds the most routes it can Hint: remove routes you no longer need with modulation.remove

### Examples

**An LFO on a gain** (`song_with_mod_out`)

```json
{
  "target": {
    "fx": "fx_1",
    "param": "gain"
  },
  "source": "lfo"
}

```

Result:

```json
{
  "modulation": "mr_1",
  "param": "gain",
  "param_name": "<any>",
  "rack": "rk_1",
  "module": "mo_3",
  "mod_out": "mo_2",
  "input": "in_1"
}

```
