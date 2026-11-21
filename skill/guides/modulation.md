# Modulation: an LFO, a random source, a macro or a follower on a parameter

A modulation moves a parameter or a mixer level around the value it already
has, by a signal from a rack. You cannot hear it: **render and measure** (see
the end). The sandbox has its own routes (`routes` in a patch); this guide is
the DAW's `modulate` and `modulation.*`.

## One call

```
exec {"command": "modulate", "args": {"target": {"fx": "fx_4", "param": "gain"},
                                      "source": "lfo", "depth": 0.25}}
```

`modulate` is one undo step. `source` is one of:

| source | what moves the target |
|---|---|
| `lfo` | an LFO (rate, shape, sync are its knobs) |
| `random` | the same LFO set to Random Steps |
| `macro` | a Macro's first knob; it sits at 0 until you move it, so it does nothing at first |
| `follower` + `from: "tr_1"` | the level of another track, so a bass can duck from the drums |

`target` is a plug-in parameter `{fx, param}` (key, id, or name in any case), a
track's `{track, param: volume | pan | width}` or a send's `{route, param: volume}`.
A name that two parameters share is refused with their ids; the route stores the
key or id, and answers give `param_name` beside it.

It finds or makes the rack that does the work (the first rack on the target's
track with a Mod Out, else one named Modulators on a new channel), adds the
source module, cables it into a Mod Out input and routes that input to the
target. The result names the `modulation` route (`mr_1`), the `rack`, the `module`,
the `mod_out` and, for a macro, the `knob` to move. A follower that would close a loop through a rack is moved to
another rack; only a real cycle (a follower on a track the target feeds, itself
included) is refused with `modulation.cycle`.

## Depth, and the knobs

`depth` is the offset at a signal of 1: a fraction of a plug-in parameter's range
(`0.25` is a quarter of it), dB for a volume (`-24` ducks by up to 24 dB), percent
for a pan or width. The default is a quarter of the range, 6 dB or 50 %. Change it
with `modulation.set {modulation, depth}`; turn a route off and on with
`enabled`. The source module is left at its defaults: set its knobs with
`module.set {module, params}` (`module.describe {type}` lists them), for example
an LFO's rate or a follower's threshold.

Which parameters take it: `fx.describe` with full detail marks a parameter
`modulatable` (taken directly) or `modulatable_by_envelope` (continuous and
automatable but not modulatable by itself, as Drumlin's pads' tune is: `modulate`
adds a one-point envelope first and the route rides on it, so keep that envelope
on). A parameter that is neither is refused with `modulation.target`, and its
hint says what the plug-in does take.

## Reading it back

- `modulation.list {track? | fx? | rack?}`: each route with its signal range, the
  cables that feed it (`fed_by`) and, when it does nothing, `inert` and why:
  `source` (nothing is cabled into that Mod Out input), `target` (the parameter
  cannot take it, or the target's track is frozen), `envelope` (the envelope it
  rides on is gone or off) or `latency`.
- `modulation.telemetry {track?}`: the offset each route is adding now, and the
  most it can add, in the target's unit. The offset needs a running plan
  (`engine.start`); it is absent while none applies the route.
- An unfed route is not an error: it is added and reported `inert: "source"`.

## Measuring that it works

1. Render the same bars with the route on and off (`modulation.set {enabled}`),
   or compare `render.stems` of the target's track.
2. Read the numbers (`analyze_render` with `detail: full`, `guides/analysis.md`):
   an LFO on a level makes the momentary loudness swing at its rate; a follower
   makes the target dip where the followed track plays; a random source steps.
3. Renders are deterministic, and so is a modulated render: twice the same, and the
   same after a save and reopen. If a modulated track gives a different result
   each time, that is a bug to report.

## Rules of thumb

- Depth first, then rate: a modulation you cannot measure at a depth of 0.25 will not
  appear at 0.01.
- A macro is for a person's hand or a later `module.set`; to hear movement, use an LFO.
- Delete a route with `modulation.remove`; removing its Mod Out or the plug-in it
  targets removes it too, with a `modulation.removed` warning.
- Do not stack many routes on one target to fake an envelope: automate it (`env.add`).
