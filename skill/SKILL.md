---
name: steppe
description: Operate the Steppe DAW and its stock plugins (synth, EQ, dynamics, effects) through their MCP tools — write MIDI in Steppe notation, program synth patches, render and analyze audio, mix, route, render stems. Use whenever a task involves making or changing music, sounds or mixes in Steppe or its plugin sandbox.
---

# Steppe

Steppe is a DAW driven through typed commands. You drive it through
MCP tools (in the sandbox: `plugin_*`, `patch_*`, `render`, `analyze`, …; in
the DAW: dedicated command tools plus `describe` / `exec` / `query` for the rest).
Humans use the same commands through the GUI. Session edits enter the undo
history; queries and rendering alone do not.

## Tools: core set + describe/exec/query

The engine lists 38 common command tools plus `describe`, `exec` and `query` by
default. A proxy to the GUI also lists its existing `gui_selection` tool. Dedicated
tools need no lookup. To find any other command, walk `describe` in three steps:
`describe {}` lists the command groups, one line each (`fx`, `track`, `render`, ...);
`describe {"group": "fx"}` lists that group's commands (name, kind, summary);
`describe {"command": "fx.patch.list"}` gives its arguments, result schema, errors
and examples. `describe {"text": "patch"}` searches names and summaries across groups
(a long answer says `truncated`: add `group` or a longer text). Call the command with
`exec {"command": "fx.patch.list", "args": {"fx": "fx_2"}}`, or use `query`
with the same arguments for read-only commands. Dedicated tools omit result
schemas; use `describe {"command": ...}` when you need one. Clients that defer tool loading can
start `steppe-mcp --tools all` or `steppe-engine --mcp --tools all` to list every
non-Internal command as a dedicated tool. The sandbox's dedicated set is unchanged.

Guides use dotted registry names (`render.mix`) and dedicated MCP tool names
(`render_mix`). For any registered command, use
`exec {"command": "render.mix", "args": {"analyze": "summary"}}`. Only names
listed by `tools/list` are directly callable tools.

| Task | Sandbox tool | DAW tool |
|---|---|---|
| Render and analyze | `render` with `analyze: "summary"` | `render_mix` with `analyze: "summary"` |
| Inspect a render | `analyze {source: <render id>}` | `analyze_render {render: <render id>}` |
| Compare | `compare {reference, test}` | `analyze_compare {reference, test}` |
| Merge a patch | `patch_merge {instance, patch}` | `fx_patch_merge {fx, patch}` |
| Read changed values | `patch_get {instance, detail: "changed"}` | `fx_patch_get {fx, changed_only: true}` |

Braced calls with bare names or angle brackets are argument sketches: replace
them with actual values. Fenced JSON examples are JSON; pass them to the named
tool (or as `args` through `exec`).

## Rules that matter more than anything below

1. **Measure, don't guess.** You cannot hear. After every change that
   affects sound, render and read the analysis report, then compare against a
   reference using the tools for your host in the table above. Numbers beat
   intuition.
2. **Use real units.** Values are physical: `"cutoff": "1.2 kHz"`,
   `"attack": "5 ms"`, `"gain": "-3 dB"`, `"fine": "+7 ct"`. Plain numbers
   are in the parameter's canonical unit (Hz, ms, dB, st, ct, %).
3. **Change small things with merge patches.** Use your host's patch-merge
   tool with only the members you want to change;
   `null` deletes a member. Read the current patch first; the changed-values
   forms above are short.
4. **Group related steps into one `batch`** — atomic (all or nothing), one
   undo step, and later steps can reference earlier results with
   `"${name.field}"`.
5. **Read errors; they tell you the fix.** Every error has a `path` and a
   `hint` (valid range, closest valid name, an example). Fix exactly what it
   says and retry once; don't guess around it.
6. **Taste is the human's.** Descriptors say whether you hit a target, not
   whether it sounds good. When a result matters, say what you aimed for,
   what the numbers show, and ask the human to listen. In the DAW, propose
   changes on a branch so they can A/B and accept or reject.
7. **Let the optimizer tune numbers.** You choose the structure (which
   modules, which routes, which parameters matter); the sandbox's
   `optimize_start` tunes up to 32 active, writable, modulatable float parameters against a reference
   or a target profile.
   Don't hand-tweak ten knobs one at a time.

## Where to read next (load only what the task needs)

| Task | Read |
|---|---|
| Writing melodies, chords, drums, bass lines, controller moves | `guides/notation.md` |
| The project's key, chords and sections: reading and setting them, how notes are spelled | `guides/harmony.md` |
| Moving a parameter or a level with an LFO, a random source, a macro or another track's level (ducking): `modulate`, `modulation.*`, and how to measure it | `guides/modulation.md` |
| Trying ideas as scenes of clips, launching them, and turning scenes into a song with sections: `scene.*`, `clip.*`, `launch.*`, `arrangement.build` | `guides/launcher.md` |
| The sandbox loop: load a plugin, program it, render, analyze, iterate | `guides/sandbox-loop.md` |
| EQ: rumble, masking, harshness, de-essing, dynamic and mid/side bands (Yarrow) | `guides/eq.md` |
| A real room or hall: its size, materials, where the sound and the listener stand, decay, clarity, held ambience (Ghyll) | `guides/reverb.md` |
| Designing a synth sound: oscillators, filters, envelopes, LFOs, routes, factory starting points (Marram) | `guides/sound-design.md`, then `plugin_describe` / `fx_describe` for exact fields |
| Playing a sample: a file or a factory sample pitched with a loop, as a one-shot, or sliced onto keys (Coppice) | `guides/sampler.md` |
| A drum kit from samples: sixteen pads on the GM keys, factory kits, choke groups, variation per hit, extra outputs (Pollard) | `guides/drum-sampler.md` |
| A drum machine: realistic drums synthesized, each pad on its own MIDI note (GM's drum map by default), tune, decay, choke groups, extra outputs (Drumlin) | `guides/drum-machine.md` |
| Corrective mixing (levels, rumble, masking, loudness targets) and automation | `guides/mixing.md` |
| Compressing (six characters, upward too), external-key ducking, catching peaks (Sundew/compressor) | `guides/comp.md` |
| Limiting the master to a true-peak ceiling, loudness targets (LUFS), level-matched comparison (Treeline/limiter) | `guides/limiter.md` |
| Gain staging, polarity, channel checks, width, balance, bass mono, lining tracks up (Hickory/utility) | `guides/utility.md` |
| A level change at one point in an FX chain: driving a plugin harder, trimming after one (Gain) | `guides/gain.md` |
| Cleaning spill or noise with a gate or expander, finding its threshold (Sorrel/gate) | `guides/gate.md` |
| Saturation and distortion, from subtle colour to folding, crushing and tuned feedback (Teasel) | `guides/drive.md` |
| Onset punch, softer attacks, shorter or longer decays (Gorse, the transient shaper) | `guides/transient.md` |
| Writing a Lua script (loops, conditionals, subscriptions, one transaction) | `guides/lua.md`, `generated/engine.lua` |
| Writing a plugin of your own: an effect or an instrument as a DSP script (`exec {"command": "dsp.write"}`, then `fx_add`) | `guides/dsp-workflow.md`, `guides/dsp-language.md`, `generated/dsp-examples.md` |
| Modular racks: modules and cables on a channel, generative patches, a plug-in in a rack, a rack as an effect, rack presets | `guides/rack.md`, `generated/rack-modules.md` |
| Writing a rack module of your own (`exec {"command": "module.write"}`, then `exec {"command": "rack.patch"}`) | `guides/modules.md` |
| Exact command arguments and results | `generated/sandbox/index.md` (sandbox) or `generated/engine/index.md` (DAW): read the index, then one group's file (`generated/engine/fx.md`); or the `describe` tool |
| A stock plugin's parameters and patch format | its guide, and `plugin_describe` (sandbox) or `fx_describe` (DAW) for every field, unit and range |
| Echoes: in time or free, ping-pong, groove, tape and analog, pitch-shifted, reversed or smeared, ducked or frozen (Bracken) | `guides/delay.md` |
| Chorus, stereo widening, string ensemble, multi-voice doubling and vibrato, with lows kept dry (Withy) | `guides/chorus.md` |
| Phasing: swirls, resonant sweeps, a photocell vibe's throb, envelope and tempo sweeps, an endless barber pole (Pollia) | `guides/phaser.md` |
| Tape: a mix's peaks gently rounded, drums pushed into tape, head bump, wow and flutter, hiss, a worn machine or a cassette (Umber) | `guides/tape.md` |
| Guitar or bass DI into an amp and a miked cabinet: cleans to high gain, the tone stack, mic placement, the cabinet or the amp alone (Hornbeam) | `guides/amp.md` |
| Measuring loudness, peaks, tonal balance, stereo and key at any point in a chain, and reading any plugin's meters (Lichen, `fx.meters`) | `guides/analyzer.md` |
| What the analysis numbers mean | `guides/analysis.md` |

Files under `generated/` are produced from the code for this skill version.
If a guide and a generated reference disagree, prefer the generated reference.
If the running host is a different version, use its live `describe` and
`tools/list` schemas.
