# Spec: session model, time model, versions and the project file

**Status:** Implemented; amended with each change (dated notes and ADRs).
**Decision records:** ADR-0007 (bus), ADR-0008 (model, IDs, undo, branches),
ADR-0009 (formats), ADR-0010 (value domains).
**Libraries:** `libs/timeline` (`supple::time`: tempo/meter maps and time
conversion — pure, reusable by notation and the sandbox), `host/engine/model`
(`supple::model`: session types, edits, versions, branches, diff, project
file), commands in `host/engine/src/*commands.cpp`.

This is the data the whole DAW is made of. Every object here is reachable by a
stable ID, editable only through commands, versioned, diffable and savable as
canonical JSON. The REAPER research (`docs/research/reaper-ux.md`) is the
workflow reference; where REAPER encodes structure implicitly (folder depth
by track order, receives by track index), Steppe makes it explicit.

---

## 1. Object model

```
Session
├── project        settings, notes, render settings, next-id counters
├── tempo          tempo points + meter changes (the time map)
├── rulers         markers mk_*, regions rg_*, ruler lanes
├── tracks         tr_*  typed strips: channel | buss | master (ADR-0019);
│                        a forest rooted at the masters, tr_0 the main master
│   ├── chain      fx_* (ordered; containers nest), input chain
│   └── lanes      comp lanes (fixed lanes)
├── items          it_*  (on a track + lane), each with takes tk_*
├── sources        md_* audio media, mi_* MIDI sources (pooled or unique)
├── routes         sd_*  edges: track→track, track→hw out, hw in→track
├── envelopes      en_*  automation (targets any param), automation items ai_*
├── groups         gr_*  track edit groups and optional VCA controls (ADR-0052)
├── racks          rk_*  modular racks on channels, with modules mo_* and
│                        cables cb_* (ADR-0062, rack.md §2)
└── blobs          content-addressed opaque data (plugin states, large arrays)
```

All collections are ID-keyed maps (ADR-0009). Ordered relations are explicit
arrays of IDs (`children`, `takes`, `chain`). Each object carries a `rev`
(revision stamp: the version number that last changed it) used for merge
conflict detection (§6.4).

### 1.1 IDs

`<kind>_<n>`, `n` from a per-kind counter in `project.next_ids`, never
reused within a lineage: `tr` track, `it` item, `tk` take, `md` media
source, `mi` MIDI source, `fx` plugin instance, `sd` route, `en` envelope,
`ai` automation item, `mk` marker, `rg` region, `tp` tempo point, `ts` meter
change, `gr` group, `ln` lane, `rj` render job, `rk` rack, `mo` module, `cb`
cable. `tr_0` is the main master. The rack kinds' counters are written once
they pass 1 (a project without racks keeps its 2.6 `next_ids`).
Events inside containers (MIDI notes, envelope points, stretch markers) are
not objects (ADR-0008).

### 1.2 Project

`name`, `title`, `author`, `notes`; `sample_rate` (the engine rate; media at
other rates is resampled, never converted on import); `ppq` default for new
MIDI (960); `timebase` default for new items (`time` | `beats` |
`beats_position`); `media_dir` (relative); `render` default settings (§1.10);
`next_ids`; `history_budget_bytes` (positive estimated bytes, default 1 GiB; §6.2);
`seed` (session randomness, e.g. dither); `console9` (bool,
default true: Console 9 summing on every strip; false keeps its gains and pan
law but runs the stages linear, ADR-0019 §6); `masters` (ordered master
ids, tr_0 first). *(`pan_law` was removed with ADR-0019.)*

Master playrate (ADR-0045) is `playrate: {rate, preserve_pitch}`: a finite
ratio 0.25..4.0 (default 1.0) and bool (default false). Missing settings in
older projects decode to these defaults; the 1.x migration sets them explicitly.
Schema 2.1 and later persist settings and automation. Playback and mix/stem
renders apply the master rate before file sources reach take FX or live mixing;
MIDI and automation follow the same fractional clock (engine.md, ADR-0045).

### 1.3 Time map (`tempo`) — the contract lives in `libs/timeline`

- **Positions** are stored in one of two domains: **seconds** (double) or
  **beats** (double, quarter-note beats from project start). Every
  positioned object stores the domain its timebase implies. Conversion to
  samples happens only when compiling a render plan: `sample = round_half_even(seconds × sample_rate)`.
- **Tempo points** `tp_*`: `{beat, bpm, shape}` with `shape` `step` (tempo
  holds until the next point) or `ramp` (BPM changes linearly **in beats**
  to the next point). Seconds for a ramp segment from beat b0 (bpm v0) to b1
  (v1): `t(b) = t0 + (60 · (b1 − b0) / (v1 − v0)) · ln(v(b) / v0)` with
  `v(b) = v0 + (v1 − v0)(b − b0)/(b1 − b0)`; step segments are linear. The
  inverse is closed-form. The first point is at beat 0; there is always one.
  Points are at least `1e-6` beats apart (`TimeMap::kMinTempoSpacing`), so
  every ramp has a finite slope.
- **Meter changes** `ts_*`: `{bar, numerator, denominator}` at bar starts
  (bar 1 is the first bar; the first change is at bar 1). Bars are derived:
  `beatOfBar(n)` sums bar lengths (`4·num/den` beats each).
- `libs/timeline` API: `TimeMap` (immutable, built from points/changes),
  `secondsAt(beat)`, `beatAt(seconds)`, `barBeat(beat) → {bar, beat,
  fraction}`, `beatOf(bar, beat)`, `bpmAt(beat)`, `meterAt(bar)`,
  `samplesAt(seconds, rate)`. Round-trip `beatAt(secondsAt(b))` within 1e-9
  beats; property tests over random maps; ramps tested against numeric
  integration.

### 1.4 Tracks (typed strips)

*(Rewritten 2026-09-30 for ADR-0019; the universal REAPER track is gone.)*
A track is a mixer **strip** of one of three types. Signal order and the
built-in Console 9 stages are implicit in the type (ADR-0019 §2); `chain`
holds only user inserts.

| type | summing input | items/record | children | built-in stages | fader/pan drive |
|---|---|---|---|---|---|
| `channel` | — (record/monitor inputs only) | yes | never | C9 Channel last | C9 Channel |
| `buss` | encoded strips | no | yes (folder) | C9 Buss first, C9 Channel last | C9 Channel |
| `master` | encoded strips | audio items after the plain stage; arm records its output (ADR-0057) | yes (folder) | C9 Buss first, plain gain/pan, then its items | plain stage |

Fields:

| field | type | notes |
|---|---|---|
| `type` | `channel` \| `buss` \| `master` | default `channel`; tr_0 is always `master` |
| `name`, `color`, `notes` | string | |
| `parent` | track id or null | a buss or master; null exactly for masters (the roots) |
| `children` | [track id] | order among siblings; empty on channels |
| `channels` | int | 1-128 (busses and masters ≥ 2): 1/2 are the main pair (fader, pan, Console 9, meters, parent send); 3-128 are auxiliary channels that routes and plug-in pins address, never faded, panned or sent to the parent (ADR-0020); a multi-out instrument's outputs land there (ADR-0021). A mono channel is placed by its pan |
| `volume` | dB (−inf..+12) | the fader: C9 Channel Fader on channels/busses (0 dB = native unity; +12.04 is the native top, clamped to +12), plain gain on masters |
| `pan` | % −100..100 | C9 Channel Pan (sin/cos, −3 dB centre) on channels/busses; balance on masters (0 dB centre) |
| `mute`, `solo` | bool / enum (`off`, `on`, `in_place`, `defeat`) | mute ramps the fader to −inf before the encode |
| `phase_invert`, `mono` | bool | the main pair's polarity flip and mono sum, after the inserts and before the fader (ADR-0022), ramped like mute |
| `parent_send` | `{enabled}` | the encoded main pair into the parent (no level or channel map of its own) |
| `hw_output` | [int] or absent | masters only (ADR-0056): the device output channels the main pair plays to, 1-based and at most 64: `[l, r]`, `[mono]` ((L+R)/2) or `[]` (none). Absent: tr_0 plays to `[1, 2]`, other masters to none. Masters sharing a channel add |
| `lane_mute` | bool | masters only (ADR-0057): silences the master's own items and nothing else; default false |
| `record` | `{armed, input: route id or null, monitor: off|on|auto, mode: input|output|midi_overdub|midi_replace}` | channels; on masters only `armed` (it records the master's output, ADR-0057). `output` still loads from project files, but an armed output-mode channel prevents recording with `record.output_unsupported` before any file is created; use input mode instead |
| `timebase` | enum or `project` | default for its items (channels) |
| `chain`, `input_chain` | [fx id] | §1.7; `input_chain` on channels only |
| `lanes` | `{mode: none|fixed|comp, lanes: [ln id], comp: …}` | channels only |
| `locked`, `hidden_tcp`, `hidden_mixer`, `folder_compact` | bool / enum | UI state kept in the model because it is user intent, but tagged `ui: true` so diffs/merges can ignore it |
| `learn` | [binding] | MIDI learn on the strip's own controls (ADR-0058), one per control, sorted by `param`, omitted when empty: `{param, midi, mode, soft_takeover}` with `midi` and `mode` as an effect's (§1.7). `param`: `volume`, `pan`, `mute`, `solo`, `phase_invert`, `mono`, `armed` (channels, masters), `monitor` (channels) or `lane_mute` (masters). Copied with `track.duplicate`, not kept by track templates; ignored by the freeze hash |

`volume`, `pan` and `mute` are automatable (§1.9). Width and trim are not
strip fields: insert a utility (Hickory) for them.

Track templates (`supple.track-template/1.0`) persist the strip's `channels`
in their `track` object and pass it to `track.add` when loaded. The optional
field defaults to stereo (2) for older templates. Its strip-type bounds match
`track.add`; FX pins retain their saved channel assignments.

Presets and templates live in the user's configuration folder: FX chain
presets (`supple.fx-chain/1.0`) in `<config>/<slug>/chains`, a plugin's own
presets in `presets/<plugin folder>/<name>.json` (`supple.preset/1.0`, the
sandbox's preset document, sandbox.md; the folder is the plugin ID made
safe for file names, then `-` and the first 8 hex digits of the ID's
SHA-256, so no two IDs share one; presets in the bare safe-name folder
earlier versions used still list and load when the file is that plugin's,
and a save or delete of the same name removes that copy), track templates in
`track-templates`, and project templates in `templates`. A project
template is a folder, `templates/<name>/`, holding `project.json` (an
ordinary project file, §5, of the head without history) and `media/` with a
copy of every audio file the head uses, named relative to it; a missing file
refuses the save (`media.missing`). project.template.load opens it as a new
project with no file and copies the template's media into that project's
own media folder, so neither depends on the other afterwards. An older
single-file template (`templates/<name>.json`, absolute media paths) still
loads. fx.chain.delete, track.template.delete and project.template.delete
remove a preset or template by name; none of these file actions changes the
history.

### 1.5 Items and takes

**Item** `it_*`: `track`, `lane` (null = main lane), `position` (in the
item's timebase domain), `length`, `timebase` (`track` = inherit), `loop`
(bool: loop the source), `mute`, `locked`, `gain` dB, `fade_in` /
`fade_out` `{length (s), shape: linear|slow|fast|slow_fast|fast_slow|cos|exp, curve %}`,
`crossfade` implicit when items overlap on a lane (auto-crossfade length from
the overlap), `takes` [tk id], `active_take`, `snap_offset`, `group`,
`color`, `notes`. A `locked` item refuses every command that would change or
remove it, its takes or its MIDI (`item.set`, `item.move`, `item.split`,
`item.remove`, `take.add`, `take.set`, `take.activate`, `take.remove`,
`take.extract`, `item.explode-takes`, `item.apply-fx`, `midi.edit`,
`midi.write`) with `item.locked`, until `item.set {locked: false}`; copying
it is allowed.

**Take** `tk_*`: `name`, `source` (md or mi id, or null = empty take),
`start_offset` (seconds into the source, or beats for MIDI),
`playrate` (0.01..100), `preserve_pitch` (bool), `pitch` (st), `pitch_mode`
(algorithm id: `signalsmith`, a Signalsmith Stretch mode (MIT), or
`resample`), `gain` dB, `pan` %, `phase_invert`, `channel_mode`
(`normal|reverse_stereo|mono_mix|left|right|...`), `stretch_markers`
(array of `{source_pos, item_pos, slope?}` objects, seconds), `take_markers` (array of
`{pos, name?, color?}` objects), `chain` (take FX), envelopes via §1.9.

### 1.6 Sources

- **Audio media** `md_*`: `{path (relative to media_dir when inside it),
  hash (SHA-256 of file bytes), format, sample_rate, channels, frames}`.
  Decoding and resampling to the engine rate are cached by the engine keyed
  by `(hash, rate)`; peak pyramids are cached outside the project (never in
  diffs). Recording sources may initially have an empty hash. A background
  completion replaces matching media metadata in stored sessions without a
  version/undo step or revision change; existing reader leases remain immutable
  and retain their versions. An undone/deleted take drops its completion.
  Missing files are reported, never fatal.
- **MIDI** `mi_*`: `{ppq, length (ticks), notes, controls, sysex, pooled:
  bool}` — a `midi::Sequence` (notation spec Part A). Several takes may
  reference one pooled source (edits affect all); unpooled sources belong to
  one take. The notation is a *view* of this data (`midi.get
  {format: "notation"}` / `midi.write`), never the storage.

### 1.7 FX chains and instances

**Instance** `fx_*`: `plugin` `{format: clap|vst3|lv2, id, name, vendor,
version}`, `label` (instance name), `bypass` (processing skipped, latency
kept), `offline` (unloaded; audio passes through), `wet` % (dry/wet),
`isolation` (`inherit|in_process|shared_bridge|dedicated`), `oversampling`
(integer 1, 2, 4 or 8 only), `pins` `{in: [l, r], out: [l, r], key: [l, r]}` — the track
channels (1-based) a track-chain plug-in reads, writes and keys from (a
detector on 3/4); one entry feeds both pins (in, key) or takes the left output
(out); empty: 1/2 in and out, the key from key routes only (ADR-0020); `out`
past its first two entries names a track channel per auxiliary output channel
of a multi-out plug-in, in order (0: unconnected; `fx.add` sets them for an
instrument, growing the track, ADR-0021),
`parallel` (bool: run in
parallel with the previous instance in the chain, outputs summed), `pdc`
(`compensate|ignore`), `auto_bypass_on_silence` (bool), `container` (for
`plugin.format == "container"`: a nested `chain` array plus its own pins),
**state**: for plugins with the semantic extension, `patch` (canonical patch
JSON, diffable); otherwise `state` = blob ref (content hash) plus `params`
(last known CLAP values by param id, for display/diff only — the blob is
authoritative). Parameter-map keys must be complete unsigned 32-bit decimal
integers; signs, whitespace, trailing bytes and overflow report `schema.key`
consistently on every platform. `learn` (ADR-0027, omitted when empty): MIDI learn, one
binding per parameter, sorted by `param` — `{param (semantic key, or a
plain parameter's id as text), midi: {kind: cc|note|pitch_bend|program,
channel 1..16, number 0..127 (0 for pitch_bend), device (a substring of the
MIDI input's name; empty: any)}, mode: absolute|relative1|relative2|
relative3|toggle, soft_takeover, when_selected, when_focused,
when_visible}` (defaults omitted). It is copied with the effect (fx.copy,
chain presets, templates).

The FX schema exports oversampling as the integer enum `[1, 2, 4, 8]`.
Project loading rejects other integer factors with `fx.oversampling` at
`/fx/<id>/oversampling`; its message and hint list the allowed values. Chain
files use the same check and retain `chain.invalid` at the effect's
`oversampling` field. The project schema version is unchanged: supported
factors still round-trip identically; unsupported factors were never written
by Steppe.

Chains are arrays of fx ids on tracks (`chain`, `input_chain`), takes
(`chain`) and the master's monitoring chain (`project.monitor_chain`, not
rendered).

### 1.8 Routes

`sd_*` edges, stored at top level (not inside tracks):

| field | notes |
|---|---|
| `from` | `{track: id}` or `{hw_input: {channels: [..]}}` or `{midi_input: device}` or `{module: id}` (a rack's Out module: a channel's record input, or Note Out with `midi`: a channel's instrument, rack.md §5–6) |
| `to` | `{track: id}` or `{fx: id}` (a plug-in's key, ADR-0018) or `{module: id}` (a send into a rack's In module); hardware outputs are a master's `hw_output` (§1.4, ADR-0056), and a route to `hw_channels` is refused with `route.hw_output` |
| `tap` | `post_fader` (default), `pre_fader` (post-FX), `pre_fx` |
| `volume` dB, `pan` % or `follow` (default: the strip's pan), `mute`, `phase_invert`, `mono` | into a buss/master: realized by the send's own C9 Channel (ADR-0019 §3) |
| `channels` | `{from: [..], to: [..]}` channel mapping, 1-based in 1..128 and up to each strip's channels (e.g. [1,2] → [3,4] for a detector on 3/4). Main pairs map the encoded send; pairs touching auxiliary channels are linear links at the route's level, mute and polarity (ADR-0020) |
| `midi` | `{from_channel: 0..16, to_channel: 0..16}` (0 = all/original) or null (audio only) |
| `learn` | MIDI learn on the send's controls (ADR-0058), as a track's (§1.4): `param` `volume`, `pan`, `mute`, `phase_invert`, `mono` or `tap:post_fader`, `tap:pre_fader`, `tap:pre_fx` (a press selects that tap); omitted when empty |

Folder/master sends are the tracks' `parent_send`, not routes. Record inputs
are routes from `hw_input`/`midi_input`, or from a rack's Out module,
referenced by `track.record.input`. A Note Out module instead sends MIDI to
any channel through an ordinary route with `midi` set; it is not a record
input and several such routes may reach a channel. It cannot send audio or
be selected as `record.input`; an armed `midi_overdub` or `midi_replace`
channel records all incoming Note Out routes alongside its optional device
input (engine.md §4), through the existing MIDI take/loop/punch rules. It cannot
feed a key or In module. Note Out channel routing uses 0 for all/original,
or 1..16, as on MIDI input routes. `route.add`/`route.set` accept track/module
ids or explicit `{track: id}`/`{module: id}` endpoints; omitted fields inside
`midi: {}` default to 0. The serialized `MidiRouting` already has both integer
fields; no file schema change is required and old projects keep loading.
Cycles are rejected at edit time (`route.cycle`, naming the cycle).
ADR-0019 §4 rules, rejected at edit time with `route.strip_type` (and a hint
naming the allowed alternative): audio into a channel only from `hw_input`;
audio into a buss/master only from strips; key
routes from any tap of any strip.

### 1.9 Envelopes and automation items

`en_*`: `target` — one of `{track: id, param: volume|pan|mute}`,
`{track: "tr_0", param: playrate}` (one global master-rate envelope only),
`{route: id, param: volume|pan|mute}`, `{fx: id, param: <key or CLAP id>}`,
`{take: id, param: volume|pan|pitch|mute}`, `{tempo: true}` (the tempo map is
edited through `tempo.*`, not envelopes); `mode`
(`trim|read|touch|latch|latch_preview|write`); `active`, `armed`,
`visible`; `points`: array of `[pos, value, shape, tension]` in the
envelope's time domain (`time`: seconds; beat timebases: beats), sorted, with `shape` `linear|square|slow|fast_start|fast_end|bezier`;
`items`: [ai id], derived at serialization from the authoritative
`automation_items` map in numeric ID order. It is not a model field. On load,
stale or mismatched lists are ignored with a `project.derived_items` warning.

Playrate points, including automation-item source points and their scaled
outputs, must be finite ratios 0.25..4.0 even on inactive envelopes. The
master target is unique on edits and load; other strips, routes and takes
cannot target playrate. A plugin parameter named playrate remains independent.

Values are in the target's physical unit (dB, %, Hz for stock plugin
params, CLAP value for third-party params); interpolation happens in the
target's normalized domain (ADR-0010). Automation items `ai_*` (top level,
`automation_items`, written only when there are any): `{envelope, position,
length, source_length (beats), loop, pool (0: none; items of one pool share
points and source_length), points (beats from the item's start), baseline,
amplitude, name}`. An item replaces the envelope over its span: its points
repeat every `source_length` while `loop`, values play as `baseline +
amplitude × (value − baseline)`, a later item cuts an earlier one it
overlaps, and the envelope steps at each edge (gap log #28). Item positions,
lengths and source points are beats even when the parent is a time envelope.
Expansion places those beat points through the tempo map into the parent's
seconds domain, retaining the underlying envelope's native interpolation.
Expanded points interpolate in the parent envelope's domain. Take envelopes
(`{take: id, param}`) use item-relative beats and move with the item; they
take no automation items. Expansion is bounded to 100,000 repetitions/points
per envelope; over-budget edits and files refuse with `automation.budget`.
Copied takes clone their FX, envelopes and automation items with new IDs;
automation items retain their pool. Splits insert the curve value at the cut
and retain the segment shape on both sides. MIDI overdub uses the take's play rate.

### 1.10 Rulers and render settings

Markers `mk_*` `{pos, timebase, name, color, lane}`; regions `rg_*` `{start, end,
timebase, name, color, lane}`; ruler lanes `{id, name}`.

Render settings `render` (ADR-0055): the project's one set of settings for
`render.*`, versioned like every other field. Every member is optional. A
missing member comes from the user's default, then the built-in default
(shown in brackets).

- `source`: `mix` | `stems` | `regions` [`mix`]: the dialog's source
  (`render.mix` always renders the mix).
- `masters`: [master ids], 1–256 [the main master alone]: the masters the
  mix renders, one file each (ADR-0056). With more than one, a pattern
  without `{track}` gets `-{track}` (the master's name). Presets and the
  user default never hold it.
- `bounds`: `project` (beat 0 to the last item's end) | `loop` (the
  transport loop range, the GUI's time selection, whether or not looping is
  on; `render.empty` when none is set) | `custom` (the members
  `start_beat` [0] and `end_beat` [the last item's end]) [`project`]. A
  command that gives `start_beat` or `end_beat` without `bounds` renders
  custom bounds.
- `tail_s` 0–600 [0]: seconds rendered past the end.
- `pattern`: a string (used for every source), or `{mix, stems, regions}`,
  each optional [`{project}-v{version}` for the mix,
  `{project}-{track}-{region}` otherwise].
- `sample_rate` 8000–384000 [the project rate]: the rate the chain runs at
  after the offline render.
- `channels`: `1` | `2` | `"track"` (each file as wide as its source: a
  1-channel track's stem is mono, everything else stereo) [2]. Mono is
  (L+R)/2.
- `outputs`: 1–4 objects [`[{format: "wav", bit_depth: 32}]`], each
  `{format: wav|aiff|flac|mp3|vorbis, bit_depth?, dither?, sample_rate?,
  mp3?, vorbis_quality?, suffix?}`:
  - `bit_depth`: WAV 16/24/32 (32 is float) [32]; AIFF and FLAC 16/24 [24];
    absent for MP3 and Vorbis.
  - `dither`: `none` | `tpdf` | `shaped` [`tpdf`]. Applies only to 16- and
    24-bit output.
  - `sample_rate`: overrides the chain's rate for this output only. MP3
    accepts the MPEG rates 8000–48000 and otherwise defaults to 48000, or
    44100 when the chain rate is a multiple of 44100 above 48000 (below
    48000, the next MPEG rate up). Vorbis accepts up to 192000 and
    otherwise defaults to half the chain rate.
  - `mp3`: `{mode: cbr|vbr, bitrate_kbps (cbr: 32, 40, 48, 56, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320), vbr_quality 0–9}` [cbr 320].
  - `vorbis_quality`: −1 to 10 [6].
  - `suffix`: a file-name-safe string (letters, digits, space, `_ . + -`,
    up to 32) inserted before the extension of a second or later output.
    Without one, a later output whose extension repeats an earlier one's
    gets `.<bit depth>`, `.<kbps>k`, `.V<quality>` or `.q<quality>`.
- `normalize`: `{mode: none|lufs_i|lufs_s|lufs_m|peak|true_peak|rms,
  target, only_reduce?, scope?}` [`none`; target −14, −100 to 0]. `only_reduce` never
  raises the level. `scope`: `file` [default] normalizes each file on its
  own; `master` applies to every stem the gain that would normalize the
  master mix over the same range (other sources normalize each file).
- `limit_true_peak_db` −60 to 0, or absent [absent]: a true-peak ceiling,
  applied with or without normalization. Stems with `normalize.scope:
  master` refuse it.
- `trim`: `{start, end, threshold_db}` [off; −60 dBFS]: removes silence
  below the threshold at either end of each file.
- `seamless_loop` [false]: sums the tail into the start, so the file is
  exactly the range long and loops without a gap. It needs `tail_s` > 0 and
  cannot be combined with `trim` or `handles_s`.
- `handles_s` 0–60 [0]: regions only; extends each region earlier and later
  (never before 0).
- `stems`: `{mode: isolated|through_master, tap: post_fader|pre_fader|pre_fx,
  tracks?: [track ids], skip_silent?}` [isolated, post_fader, every
  top-level track, false]. `skip_silent` leaves out files whose sample peak
  is below −90 dBFS.
- `matrix`: `{rg id or "project": [track/master ids, e.g. "tr_0"]}`.
- `if_exists`: `overwrite` | `number` (adds `-2`, `-3`… to the base name of
  all the file's outputs) | `skip` | `error` [`overwrite`].
- `metadata`: `{title, artist, album, track_number, date, genre, comment,
  isrc, cover, bwf, markers}`, all optional [none]:
  - Text members accept the §7.3 tokens.
  - `date` is `YYYY` or `YYYY-MM-DD`.
  - `cover` is a PNG or JPEG of at most 10 MiB, as a path relative to the
    project folder (refused for an unsaved project or a path that leaves the
    folder).
  - `bwf: true` writes a BWF `bext` chunk, version 2, to WAV files: time
    reference and loudness fields.
  - `markers: true` writes the markers and regions inside the file's range
    as WAV cue points or AIFF markers.
- `report` [false]: writes a plain-text loudness report beside each file.
- `add_to_project` [false].

Files before schema 2.3 used `format` (a string or `{container, bit_depth,
dither}`), `bit_depth`, `secondary`, `tail_ms` and
`normalize.limit_true_peak_db`. The loader migrates them into `outputs`,
`tail_s` and `limit_true_peak_db`, and saving writes only the new members.
Both the project schema and `project.set` argument schema mark matrix row keys
as region IDs (with `project` as a literal alternative in `propertyNames`) and
array cells as track IDs, and `stems.tracks` entries as track IDs. Merge replay
and conflict checks remap these references; the `project` key and free text such
as `pattern` remain unchanged.

### 1.11 Blobs

`blobs`: map from `sha256:<hex>` to `{size, encoding: "base64", data}` in the
project file (or sidecar files beyond 1 MiB, §5.3). Immutable,
deduplicated, reference-counted across versions.

---

## 2. In-memory representation

- Every object type is an immutable C++ value struct; collections are
  `ImmutableMap<Id, std::shared_ptr<const T>>` (a sorted vector for ≤ 256
  entries, a persistent HAMT beyond — behind one interface so it can start
  as a sorted vector and upgrade without touching callers; ADR-0008 allows
  `immer` if needed). Large arrays (MIDI events, envelope points) are chunked
  (`ChunkedArray<T>`, 4096 elements per chunk) so editing one note copies one
  chunk.
- `Session` holds the root pointers; edits produce a new `Session` sharing
  everything untouched. Readers on any thread hold a `std::shared_ptr<const
  Session>`; nothing is ever mutated in place.
- `EditTxn` is the only way to build a new version: typed setters that
  copy-on-write the path, assign IDs from counters, stamp `rev`, and record a
  per-object change list for events and diffs.

## 3. Validation invariants (checked after every edit, in debug builds after every command; always before save)

The track tree is a forest rooted at the masters (`project.masters`, tr_0
first, all of type `master`, parent null); every other track's parent is a
buss or master; channels have no children; items only on channels and
masters (a master's takes are audio or empty, ADR-0057); record settings and
`input_chain` only on channels, except `record.armed` on masters; `lane_mute`
only on masters; routes obey ADR-0019 §4; every referenced ID exists; items'
tracks/lanes exist; `active_take` ∈ `takes`; sources referenced by takes
exist (or are marked missing); no route cycles (including parent sends);
envelope targets exist (a module's knob among them); points sorted; every fx
belongs to exactly one chain or one plugin module; racks obey rack.md §2.5
(homes are channels, cables within one rack, Out modules feed channels as
their record inputs, no cycle through a rack, the limits); counters exceed
every used ID; tempo map has a point at
beat 0 and meters at bar starts. A violation is a bug (`model.invariant`),
never a user error — user errors are caught by command validation first.

## 4. Diff and describe

- `diff(Session a, Session b) → [{op: add|remove|update, id, kind, fields:
  [{field, before, after}]}]` — object-level, deterministic order (kind
  order as in §1, then ID). Large arrays diff as summaries (`notes: 3 added,
  1 changed`) with an optional detailed form. Used by `session.changed`
  events, branch reviews and the undo history display.
- `describe(session, scope, detail)` → compact text for LLMs, e.g.

```
Project "Demo" · 48 kHz · 120 BPM 4/4 · 16 bars (0:32.0)
tr_1 Drums [buss] → master
  tr_2 Kick   vol −3.0 dB · fx: fx_2 EQ (4 bands), fx_3 Comp (−4 dB GR@1:1…)
              it_4 bars 1–17 "kick loop" (audio md_1, 1.00×)
  tr_3 Snare  it_5 bars 1–17 (MIDI mi_2, 64 notes)
tr_6 Bass → master · fx: fx_7 Marram "Round Sub", fx_8 EQ
routes: sd_1 tr_2 → tr_6 ch 1/2→3/4 pre-fader −6 dB (sidechain)
markers: mk_1 "Verse" bar 1 · regions: rg_1 "Chorus" bars 9–17
```

## 5. Project file (`supple.project/2.7`)

### 5.1 Shape

*(Schema `supple.project/2.0` since ADR-0019: strip `type`,
`project.console9`, `project.masters`; strip `width`/`trim` removed, and
`phase_invert` kept since ADR-0022. 1.x files migrate as ADR-0019 §11 says,
with warnings.)*

Schema 2.1 adds master playrate and its envelope target (ADR-0045).
Schema 2.3 reshapes the render settings (§1.10, ADR-0055).
Schema 2.4 adds masters' `hw_output` and the render setting `masters`
(ADR-0056). Schema 2.5 adds masters' `lane_mute`, and allows `record.armed`
and audio items on masters (ADR-0057). Before 2.4, a route from a master to `hw_channels` (which never
played) becomes that master's `hw_output` (its first such route) and the
route is dropped, with a `project.hw_output_route` warning.

The loader accepts 1.x and 2.x documents and saves 2.5. Migration allocates
child channel IDs in track-ID order, inserts each child first, and moves the
source items and record/input FX there while keeping user inserts on the
buss. Hardware/MIDI record routes follow the new child. Legacy numeric send
pan remains numeric (including the old default centre); new sends default to
`follow`. `project.open` includes nonempty load `warnings` as structured
`{code, message, path}` entries, including the change to Console 9 staging.
Masters are ordered by `project.masters`, independently of processing order;
`track.list` and `session.describe` visit every root in that order.

Canonical JSON (ADR-0009) with members in this order: `schema`, `project`,
`tempo` (`points`, `meters`), `rulers` (`markers`, `regions`), `tracks`,
`items`, `takes`, `fx`, `sources` (`media`, `midi`), `routes`, `envelopes`,
`automation_items`, `groups`, `racks`, `modules` and `cables` (when present), `blobs`, then preserved unknown top-level
sections and `extensions`. `groups` / `gr_*` are typed track groups since 2.2 (ADR-0052);
older opaque groups are preserved in extensions with a warning. `ui` belongs to the sidecar. *(Amended 2026-09-26: `fx` — plugin instances are
ID-keyed objects like the rest; chains reference them by id.)* Members equal
to their defaults are omitted (the schema fills them in on load). Objects keyed by ID in ID order
(numeric by kind). Compact array forms for bulk data:

- MIDI notes: `[[start, length, channel, pitch, velocity, off_velocity], …]`
  (ticks; channel 1-based); controls `[[tick, kind, channel, number, value], …]`
  with `kind` `cc|program|pressure|poly|bend`; sysex `[[tick, "F0 … F7"], …]`.
- Envelope points `[[pos, value, shape, tension], …]`.

These arrays are the one place canonical form prints one inner array per
line (scalar arrays of arrays: each inner array on one line, outer array one
element per line) — a writer option `compactRows` in core-json. *(Not needed so far: the canonical writer already prints these rows one
per line.)*

### 5.2 Compatibility policy

Project exception to ADR-0009: known sections/fields reject wrong types or
shapes with a field path for every minor. Unknown fields within known objects
are dropped with warnings listing their paths. Unknown top-level sections and
an explicit top-level `extensions` object are preserved verbatim on save.
A newer minor loads with a version warning; older majors migrate and newer
majors refuse. A project `ui` member is ignored with a warning pointing to
`<name>.ui.json`. Legacy opaque `groups` is preserved in extensions with a migration warning. The parser is
fuzzed (`project.fuzz`). Unknown plugin formats or missing plugins load as
placeholder instances that keep their state untouched and pass audio
through (the session survives; `project.load` reports them).

### 5.3 Files on disk

The file is named by the caller; the GUI saves `<name>.json` (gui.md §4.11).
Beside it is an optional sidecar directory, the file name plus `-data`
(`demo.json-data/`), for blobs over 1 MiB (`<hash>.bin`) and autosaves.
UI state lives separately in `<name>.ui.json` (gui.md §1.2), never in the
project document. Saves are atomic (write temp, fsync, rename). Autosave every 60 s to
`<file>-data/autosave-<n>.json` (keep 5). History is persisted only
on request (ADR-0008 clarification).

Media live in the project's `media_dir` (default `media/`) beside the file
and are named relative to it. A project saved for the first time takes its
unsaved recordings and imports from the scratch folder; one saved as into
another folder gets a copy of its media folder there (the old project keeps
its own; a different file of the same name already there refuses the save,
`media.conflict`). With the app's copy-imports setting on (the GUI's
default), item.add copies an imported audio file into the media folder (a
same-named file with other bytes gets `-2`, `-3`…; the same bytes are
reused) instead of naming it where it is. The GUI gives each project a
folder of its own (gui.md §4.11).
Copy failures retain the underlying filesystem error even when temporary-file
cleanup succeeds, so `media.copy` identifies the cause of a failed import or save.
Only if the resolved destination is a strict descendant of the resolved source
media folder does the recursive copy skip that destination subtree, including
symlink aliases. Existing destination files stay there; copied files are never
read back as new source media. An ancestor destination is copied to normally,
so saving back to the original project folder includes newly recorded media.

The engine app writes autosaves from main-thread service (engine.md §6,
ADR-0030), only when a named project's head changed since the last one and
60 s of monotonic service time have passed. Unchanged/unnamed projects have no
autosave deadline; an overdue edit arms immediate service. A failed write
waits 60 s before retrying. History timestamps still use wall time, and
`<n>` is a positive unsigned 64-bit decimal index and counts up from the highest
present. Discovery and rotation ignore names with nonnumeric or out-of-range
suffixes (such as `autosave-1-backup.json`). An exhausted index reports
`autosave.index` without writing or pruning, and retries after the same interval.
`project.open` of a file that has
a newer autosave next to it reports `autosave_available` and loads nothing
from it; opening the autosave file itself is how it comes back.

## 6. Versions, undo, branches

### 6.1 Version store

`VersionStore`: `Version {id (monotonic u64), parents [1–2], session
(shared_ptr), txn {label, origin, invocations (resolved, for replay),
created_ids}, time}`. Branch refs: `main`, `agent/<agent>/<topic>`, user
branches. Each branch has a head and an undo cursor.

### 6.2 Undo / redo

Per branch, linear. `history.undo` moves the head to its parent (the
undone version stays as the branch's redo target until a new edit);
`history.redo` moves forward. `project.set {history_budget_bytes}` changes the
persisted estimated history byte budget (default 1 GiB, positive integer).
Above it, oldest eligible linear steps on each branch combine into one
`earlier history` step; intermediate snapshots are released and undo/redo crosses
the combined step as one operation. Shared MIDI/blob/object/collection payloads
are counted once. Leases, branch bases/heads, redo targets and fork/merge
boundaries survive; required data/replay logs may exceed the budget.
`history.list` reports `truncated`, `retained_bytes`, `budget_bytes`, and
`squashed_steps` per entry. This does not persist undo history into project files.
Coalescing: consecutive edits with the same `coalesce_key` (e.g. one fader
gesture, `fx.param.set` on the same param within one gesture) replace the
head instead of stacking.

### 6.3 Branch operations

`branch.create {from: "main", name}`, `branch.list`, `branch.switch
{name, mode: live|offline}` (live: engine reconciles to that head),
`branch.diff {a, b}`, `branch.render-ab {a, b, range, ...}` (offline renders
of both + analysis comparison, returning two render ids for sample-aligned
A/B playback), `branch.merge {from, into: "main"}`, `branch.delete`.

### 6.4 Merge by replay

If `into` has not moved since the fork: fast-forward. Otherwise replay the
source branch's transactions (in order) onto `into`'s head:

1. For each logged invocation, rewrite IDs through the remap table (IDs the
   branch created that collide with IDs created on `into` since the fork get
   fresh IDs; schemas identify ID-typed fields: only strings, and object
   keys, whose argument-schema pattern is an ID pattern such as `^tr_[0-9]+$`
   are rewritten — a track *named* "tr_1" keeps its name). Coalesced edits
   never erase a version that another branch's head or base still refers to.
2. Re-validate and apply. A failure is a **conflict** of kind `invalid`.
3. Before applying, compare the `rev` of every object the invocation touches
   against the fork point: if `into` changed that object since the fork,
   it is a **conflict** of kind `concurrent_edit`.
   A branch merged before reaches `into` only as replayed steps, so its
   objects may have other IDs there. Each replayed step records which branch
   version it replays and the remap reached so far, including objects removed
   since. A later merge continues after the newest step of the branch that
   `into` already holds: directly, through merges of merges (whose remaps
   compose), or because a held replayed step descends from it along first
   parents (budget squashing may have combined its own step). It starts from
   that remap. Conflicts compare the branch's state there, its ID fields
   rewritten through the remap by the object schemas (names and other text
   stay as written), with `into`'s head; a removal conflicts only if `into`'s
   copy differs. If the fork reached `into` only through history that is no
   longer kept and some merge on the way remapped IDs, the merge stops with an
   `invalid` conflict asking for a new branch. Replayed steps never coalesce
   with later edits.
4. On conflicts, the merge stops without changing `into` and returns the list
   `[{step, invocation, kind, object, into_value, branch_value}]`. The human
   (or agent, on request) resolves by editing either branch and merging
   again, or by `branch.merge {..., strategy: "take_branch" | "keep_main"}`
   per conflict.

### 6.5 Engine reconciliation

`RenderPlan` compilation (engine spec) diffs the previous compiled version
against the new one: unchanged plugin instances are reused as-is; instances
whose `patch`/`state` changed get it applied (`set_patch`/`state.load` on
the main thread); new instances are created and activated off the RT path;
removed instances are deactivated and destroyed after the audio thread has
released the old plan.

---

## 7. Command catalogue

Schemas are written per command in code (ADR-0007); this is the inventory
and naming. All `edit` unless marked (q) query or (a) action.

- **project**: `project.new` (a), `project.open` (a), `project.save` (a),
  `project.get` (q), `project.set`, `project.describe` (q); templates
  `project.template.save` (a), `project.template.load {name}` (a, a new
  unsaved project), `project.template.list` (q), `project.template.delete`
  (a).
- **session**: `session.describe {scope, detail}` (q), `session.query
  {kind, filter, fields}` (q), `session.get {id, fields}` (q).
- **tempo/meter**: `tempo.set {points}`, `tempo.add`, `tempo.remove`,
  `timesig.set {bar, numerator, denominator}`, `timesig.remove`,
  `time.convert {from, to, value}` (q).
- **tracks**: `track.add {name, type: channel|buss|master, parent, index, channels, template?}`,
  `track.remove`, `track.move {parent, index}`, `track.set {fields}`,
  `track.duplicate {tracks}` (each copy right after its original: settings,
  both chains with live settings, items and takes — unpooled MIDI copied —,
  sends, record input, and the envelopes on the track, its plug-ins, sends and
  takes; not a folder's children, sends into it or item groups; masters are
  refused), `track.list` (q); templates `track.template.save {track, name}`
  (a), `track.template.load {name}` (a new strip under the master),
  `track.template.list` (q), `track.template.delete` (a).
- **items/takes**: `item.add {track, position, length, source|midi|notation}`,
  `item.consolidate {items}` (background action, one atomic result edit;
  active audio with take FX baked, MIDI kept separate per track; engine.md),
  `item.heal {items}` (pure edit joining compatible audio split/slice chains;
  one result per track, no rendering; exact eligibility in engine.md),
  `item.remove`, `item.move`, `item.set`, `item.split {item, at, snap?: none|zero_crossing}`
  (default none; audio zero-crossing search and errors in engine.md),
  `item.duplicate`, `item.glue` (a+edit), `take.add`, `take.remove`,
  `item.paste {version?, items: [{item, track, position}]}` copies the retained
  version's takes and their FX, including referenced opaque state blobs when
  the current session no longer contains them (for example after undo),
  `take.set`, `take.activate`, `take.remove` (the item goes with its last
  take), `take.extract {take, track, position}` (into an item of its own),
  `item.explode-takes {item, mode: tracks|serial}` (recording order: the
  first take stays, the others on new channels below or one after another),
  `item.apply-fx {item, mode: take|replace}` (the heard take rendered through
  its take FX and the track's main chain, pre-fader, over the item's span,
  into an audio file in the media folder: a new heard take or in its place),
  `item.export {item, path}` (a: the heard take's raw source span as WAV, or
  what a MIDI item plays as a Standard MIDI File; engine.md),
  `item.mix-to-track {items}` (background action, one undo step: the items
  through their channels' strips, decoded as a fresh buss hears them, onto a
  new muted channel; engine.md).
- **MIDI**: `midi.write {item | {track, at}, notation, replace: bool}`,
  `midi.get {item, format: notation|events}` (q), `midi.edit {item, ops:
  [{select: {pitch, range, channel}, set|shift|transpose|quantize|velocity}]}`,
  `midi.quantize`, `midi.humanize {seed}`; Standard MIDI Files are read by
  `item.add {midi_file}` (notes keep their beats and follow the project's
  tempo) and written by `item.export`. `midi.edit` note indices are a set per operation: repeated
  indices edit each original note once. Tick conversions and all resulting
  starts, ends, lengths and sequence lengths are checked before publication;
  out-of-range arithmetic returns structured `midi.range` with a repair hint,
  leaving the session unchanged. MIDI items are merged by
  `item.consolidate` (engine.md): those on one track into one item from the
  first start to the last end (what each heard take plays: looped passes
  unrolled, notes ending by their item's end; gaps empty, overlaps layered),
  or a lone item's MIDI takes layered into one take; the earliest item, its
  settings and take FX are kept.
- **FX**: `fx.add {track?|take?, plugin, index?, patch?, chain?}`
  (exactly one of track or take), `fx.remove`,
  `fx.move`, `fx.copy {fx, track, index?, chain?}` (live settings),
  `fx.set {fx, bypass?, offline?, wet?, label?, isolation?, pins?}`
  (oversampling, parallel and PDC are model fields, not `fx.set` arguments),
  `fx.param.set {fx, values: {key|id: value}}`,
  `fx.param.get` (q), `fx.meters {fx, from: live|render}` (q: the read-only
  values of the playing instance, or as the last `render.mix` ended),
  `fx.patch.get` (q), `fx.patch.set`, `fx.patch.merge`,
  `fx.describe` (q), a plugin's presets `fx.preset.list` (q),
  `fx.preset.load`, `fx.preset.save` (a), `fx.preset.delete` (a), chain presets
  `fx.chain.save` (a), `fx.chain.load`, `fx.chain.list` (q),
  `fx.chain.delete` (a), MIDI learn `fx.learn.set {fx, param, midi, mode?,
  soft_takeover?, when_selected?, when_focused?, when_visible?}` (one per
  parameter; a plain parameter by id or name), `fx.learn.remove {fx, param}`,
  `fx.learn.list {fx?}` (q),
  `fx.state.capture` (internal, from plugin notifications), `plugin.list`
  (q), `plugin.scan` (a).
- **routes**: `route.add {from, to, tap, channels, …}`, `route.remove`,
  `route.set`, `route.list` (q). MIDI learn on a strip's or a send's own
  controls (ADR-0058): `track.learn.set {track, param, midi, mode?,
  soft_takeover?}` / `route.learn.set {route, …}` (a), `track.learn.remove
  {track, param}` / `route.learn.remove {route, param}` (a),
  `track.learn.list {track?}` / `route.learn.list {route?}` (q); a VCA's
  are `vca.learn.*` (Track groups and VCA controls, below).
- **automation**: `env.add {target}`, `env.remove`, `env.set {mode, active,
  …}`, `env.points.set {env, points, range?}`, `env.points.add`,
  `env.points.remove {range}`, `env.add {take, param}` (take envelopes);
  automation items `autoitem.add {env, position|bar, length, points?,
  source_length, loop}` (no points: captures what plays there),
  `autoitem.set` (points and source_length reach the pool),
  `autoitem.duplicate {pooled}`, `autoitem.unpool`, `autoitem.remove`,
  `autoitem.list` (q).
- **rulers**: `marker.add/remove/set`, `region.add/remove/set`.
- **transport** (a): `transport.play`, `transport.stop`, `transport.pause`,
  `transport.record`, `transport.seek {pos}`, `transport.loop {range | clear}`,
  `transport.punch {start, end | clear}` (`clear` removes the range and turns
  it off), `transport.metronome {count_in_bars,
  count_in_playback, …}` (a count-in before recording), `transport.get` (q).
- **render** (a): `render.mix {master}` (one master for the call; default the `masters` setting, else tr_0), `render.stems`, `render.regions` (matrix),
  `render.items`, `render.dry-run` (q: resolved file list), `render.job.*`;
  `format` (wav|flac), `secondary` (a second file in another format/bit depth
  from the same render); `render.queue.add/list/remove/run` (renders queued
  with their arguments, run in order).
- **analysis** (q): `analyze.render {render}`, `analyze.track {track, range}`
  (renders the track's output offline, then analyzes), `analyze.compare`,
  `analyze.masking {masker_track, maskee_track, range}`.
- **history/branches**: `history.undo`, `history.redo`, `history.list` (q),
  `branch.*` (§6.3).
- **scripting**: `script.run {code, name?, budget_ms?, args?}` (a) — Lua on the
  model thread in a sandbox (ADR-0006, `libs/lua-bus`), its calls recorded
  under `lua:<name>`; `q.batch` inside is one transaction and `q.on` subscribes
  to the events below. Not transactional itself: undo its edits with
  `history.undo`.
- **meta**: `batch`, `registry.*`.

Master playrate commands (ADR-0045) are `transport.playrate.get`,
`transport.playrate.set {rate?, preserve_pitch?}` and
`transport.playrate.apply-to-tempo`; their exact contracts and recording limit
are in engine.md's Master playrate section. The
model-thread `activePlayrateEnvelope(Session)` identifies an active nonempty
master curve; `validateRecordingPlayrate(Session)` returns `record.playrate`
when the saved scalar or effective curve prevents unity recording.

### 7.3 Render file-name patterns

A closed set of `{variable}` tokens with explicit formatting args instead of
REAPER's `$`-mini-language: `{project}`, `{track}`, `{track_number:02}`,
`{folder_path}`, `{region}`, `{region_number:02}`, `{marker}`,
`{item}`, `{take}`, `{start:beats|seconds|timecode}`, `{format}`,
`{sample_rate}`, `{bit_depth}`, `{date:YYYY-MM-DD}`, `{time:HH-mm-ss}`,
`{counter:03}`, `{version}` (the rendered session's version number) and
`{branch}` (its branch name). A pattern may contain `/` to make subfolders
inside the render folder; they are created as needed. With several outputs,
`{format}`, `{sample_rate}` and `{bit_depth}` take the first output's values. Filters: `|lower`, `|upper`, `|replace(" ","_")`,
`|max(8)`. `render.dry-run` returns the resolved list. Empty tokens drop an
adjacent separator (`-`, `_`, `.`, space), as REAPER does. Replacement arguments
are double-quoted literals: pipes, braces and commas inside them are not
pattern delimiters (for example `{track|replace("|","}")|lower}`). Quote
escapes are not supported. Collision checks across primary and secondary
outputs compare the resolved parent directory and final filename; directory
symlink aliases cannot produce duplicate destinations.

---

## 8. Tests required (P4)

timeline property tests (§1.3); model edit tests per command; invariant
checker; diff determinism; describe goldens; project file round trip
(byte-identical) and fuzzing; version store: undo/redo round trips under
random command sequences (fuzzed: random valid command streams, then undo
all → initial state `identical`, redo all → final state); branch merge
fast-forward, replay with ID remapping, both conflict kinds; memory-bound
eviction; blob dedup.

### Follow-up contracts (2026-10-04)

- Undo/redo reloads the active engine plan and emits `session.changed` as
  `{version, branch}`, like an edit. Per-lineage ID high-water counters survive
  undo; adding after undo cannot reuse an ID. Independent branches may allocate
  overlapping IDs; merge remaps each creation before replaying the next command,
  including commands in the same batch, and a later merge of the same branch
  keeps the earlier merge's remap (§6.4).
- Project fields are represented in changes/diffs by the singleton `project`.
  Concurrent changes to the same project field conflict only when their new
  values differ; disjoint fields replay normally.
- Each invocation inside a batch has a savepoint. A caught failure restores
  that invocation's model/history/event changes while successful siblings remain.
- `project.new/open` stops and finishes recording, cancels and joins project jobs,
  then increments `project.get.generation` and resets session caches/scripts.
  Device and render-ahead settings persist. Deferred completions check generation;
  stale completions emit an event and never mutate the replacement project.
- First save stages and hashes scratch-media copies, publishes the media, then
  atomically writes the project. A failure removes files created by that attempt;
  scratch originals and the unsaved project identity remain intact.
- Render completion/import is bound to the originating project generation and
  current branch. A stale completion preserves files and reports
  `render.stale_target` in the job result; no items are imported.
  A job cancelled during project replacement keeps its cancelled status and
  receives the stale result when its queued completion is serviced.
- `track.add` and `track.set` accept 1..128 channels; busses/masters require at least 2.
- `track.set {hw_output}` (ADR-0056) accepts 0–2 distinct channels 1..64, on masters only
  (`track.strip_type` otherwise, `track.hw_output` for three channels or a repeated one).
  Retyping a master clears it. `track.list` reports every master's resolved `hw_output`.
- `track.set {armed}` accepts masters (ADR-0057; `record_input`, `record_midi`,
  `midi_channel`, `record_mode` and `monitor` stay channel-only, `track.strip_type`), and
  `track.set {lane_mute}` is masters only. Retyping a master clears `lane_mute`; a buss is
  never armed. Group-linked arm spreads to channels and masters, monitoring to channels.
  `track.list` reports masters' `armed` and `lane_mute`. `item.add {audio_file}`,
  `item.move`, `item.paste`, `take.extract` and `project.recover` accept masters for audio;
  MIDI there, and any item on a buss, is `track.strip_type`. `transport.record` records
  armed masters' outputs with the armed channels' inputs.
  `fx.param.set` keeps semantic parameter units and also supports plain CLAP/VST3
  parameter names/IDs, declared ranges, writable flags and plugin text conversion.
  Plain parameter edits save processor state after applying queued values on a
  private probe. Latency probes restore patch/state/params before measuring.
- Ruler lists include `timebase`; descriptions map seconds through the time map.
  MIDI edits reject non-finite/oversized tick conversions and arithmetic before
  rounding, including extreme deltas and impractically small quantization grids.

### Numeric positions and expansion budgets (2026-10-04)

Every edit and project load validates original numbers and derived ends, seconds,
source offsets and sample products. Positions must fit signed-int bar addressing
and signed-64 samples at the project/device/render rate, with space for block, render-ahead and
PDC additions. Take playback/pitch transforms must have finite positive speeds
and representable source/variant frame lengths. MIDI ticks and note-end sums
are checked before use; materialized MIDI expansion is limited to 1,000,000
events/passes per track to bound planning memory and keep note IDs in int32.
Automation retains its 100,000 expanded-point budget and validates finite scaled
curves. In-memory command renders use the engine’s existing 2^28-frame capacity
at the project and requested output rates, with block headroom; this is a work
budget, not a new duration policy.

## Track groups and VCA controls (schema 2.2, ADR-0052)

`groups` is an ID-keyed map of `gr_*` objects: `{name, color, active, members,
links, vca?}`. `members` is a unique set of track IDs; commands sort it
numerically. Groups may overlap. Defaults: active, no members, links
`volume`, `mute`, `solo`. Other links are `pan`, `armed`, `monitor`,
`selection`, `edit`. The group's color uses the track palette or `#rrggbb`.
Names and colors never propagate. `Item.group` remains an independent opaque
item-group token, not a reference to this collection.

For each linked attribute a manual command expands the active connected
component, takes a snapshot, and updates each eligible member once. Volume
uses a relative dB delta, pan a percentage-point delta; results clamp to
existing field bounds. Discrete controls copy the requested value. Armed and
monitor affect channels only; record input/mode, polarity and mono are local.
Finite relative moves leave silent followers silent. Setting the source to
silence silences followers; raising a source from silence changes only that
source (there is no finite relative delta). Prefer mute for reversible group
silencing. `bypass_groups: true` on track.set or linked item commands edits
only the named member. Each command is one undo step; a batch remains
sequential, so agents should issue one linked edit rather than repeat it for
every member. GUI selection edits expand once and dispatch bypassed targets.
Automation playback never propagates edits; a deliberate linked control edit
records each affected writing-mode envelope using the existing capture rules.

Edit links match item start/end after timebase resolution and project-rate
sample rounding, with the same lane token (main matches main; ordered lane
objects are not yet implemented). No overlap/nearest/transient matching.
Selection, item.move on the same track, item.set position/length/mute/fades,
split, remove and duplicate link. Take contents, source offsets, MIDI edits,
warp, gain, name, color, lock and timebase remain local. A matched locked item
refuses an edit atomically; copies retain the existing permission to copy
locked items. Cross-track moves with matching peers require bypass. Split and
copy preserve whole takes, including fields not interpreted by grouping.
Split results add `linked_left`/`linked_right`; duplicate adds `linked` when
there are linked copies. Empty added result arrays are omitted.

An optional `vca` is `{volume_db, mute, solo, followers, learn}`: defaults 0 dB,
false, false, empty, empty. `learn` is MIDI learn on the VCA's own controls
(ADR-0058), as a track's (§1.4): `param` `volume`, `mute` or `solo`; it goes
with the VCA. `followers` are group IDs with their own VCA, not tracks.
The graph must be acyclic and a track may have at most 32 distinct controlling
VCAs. Unique ancestors contribute once in numeric ID order. Direct membership
plus ancestry determines control; audio routing is unchanged. Assigning a
channel and its parent buss applies control at both faders. `active: false`
suspends linked edits/selection only: VCA gain and mute still apply.

Envelopes may target `{object: gr_id, param: volume|mute}` in the saved target
shape; `env.add` accepts `{group, param}`. VCA automation uses ordinary curves,
timebases, automation items and writing modes. Volume stays in -150..+12 dB,
mute in 0..1; source and scaled automation-item values obey these bounds.
VCA solo is a discrete edit, not an envelope target. Removing a VCA removes
its envelopes/items and incoming follower references but preserves the group;
removing a group removes its VCA, never tracks. Track removal prunes membership.
Track duplication/single-track templates do not inherit groups; project
templates retain them. Group objects participate in versions, diffs, history
budgets and replay ID remapping.

Commands: group.add `{name,color?,members?,links?,active?}` returns `{group}`;
group.set `{group,name?,color?,active?,links?}`; group.members.set
`{group,members}`; group.remove `{group}`; group.list; group.describe `{group}`;
group.targets `{track?|item?,attribute}` resolves linked targets. vca.add and
vca.set accept `{group,volume_db?,mute?,solo?,followers?}`;
vca.members.set `{group,tracks?,vcas?}` replaces supplied sets (tracks writes
the same group membership, vcas writes followers); vca.remove `{group}`.
`vca.learn.set {group, param, midi, mode?, soft_takeover?}`, `vca.learn.remove
{group, param}` and `vca.learn.list {group?}` (q) edit and read the VCA's MIDI
learn as track.learn.* do a track's (§7); a group without a VCA is
`vca.missing`.
track.describe `{track,at?}` and track.list `{at?}` expose groups, VCAs,
`requested_effective_volume_db`, `effective_volume_db`, effective mute and
solo suppression at project seconds `at` (default transport time, or zero).
These are model/curve target values, not instantaneous smoothing telemetry.

The writer now emits `supple.project/2.2`. Typed groups follow automation_items
and precede blobs. Earlier opaque groups are moved verbatim into
`extensions.legacy_track_groups`, with an unused numeric suffix on collision,
and `project.groups_legacy` warns. They never become active automatically.

The model-thread helpers in `supple/model/groups.h` resolve sorted, unique
`linkedTracks`, `linkedItems`, `trackGroups` and `vcaControllers` IDs.
`validateGroups` checks links, graph bounds and group-envelope values;
`effectiveTrackLevel` and `groupParameterAt` evaluate the project-time control
values without querying an audio executor.

### Track freeze (schema 2.2, ADR-0054)

`Track.freeze`, absent when unfrozen, contains `source` (media ID),
`content_frames` (>0), `tail_frames` (>=0), `latency_frames` (>=0) and
`dependency_hash` (SHA-256). Content starts at project time zero; media frames
are content plus tail, at the project rate, float32 WAV, with max(2, track
channels) channels. Items, takes, FX, opaque blobs and automation retain their
IDs and state. The descriptor replaces their execution, not their model data.

`track.freeze {track, job?: false}` renders and attaches one history edit;
`job: true` returns a job whose completion includes the committed edit.
`track.unfreeze {track}` restores execution in one edit. `track.list` reports
`frozen` and the descriptor; `session.describe` marks frozen channels.
`track.freeze.commit` is the internal typed replay record: attaching existing
verified media and captured states must not start another render during merges.

Freeze sound dependencies include track content and processing, referenced MIDI
and media, take/FX envelopes and their automation items, and global timing.
Mixer state, outgoing routing and track display fields are excluded. Pending
renders validate dependencies and latency scheduling at completion, not the head
version. A changed project/branch/media directory also refuses completion.

Frozen channels reject captured-content edits, including indirect pooled edits,
new incoming dependencies and arm/monitor. Fader/pan/mute/solo, their envelopes,
outgoing sends and parent routing remain editable. Tempo/meter/master-rate and
sample-rate edits require unfreezing all frozen tracks; those unfreezes may precede
the timing edit in the same batch. Deletion remains undoable.
Published cache files stay on disk across unfreeze and history reclamation,
because saved projects, autosaves and replay logs may still need them. First save,
Save As and project templates carry frozen media like other media sources.

Writers now emit `supple.project/2.2`; missing freeze fields in older projects
mean unfrozen. Newer minor warnings begin above 2.2. Old applications can discard
freeze metadata and restore original processing; do not use them to preserve a
frozen state.

### Audio warp data (schema 2.2, ADR-0053)

Takes optionally persist `transients: {algorithm, source_hash, sensitivity,
positions}` and `warp_peers: [tk_id...]`. Hit positions are sorted original-source
seconds, sensitivity is 0..1, and both hits and stretch markers are limited to
65,536 entries. Absent transient state means not analyzed; an empty positions array
means analyzed silence. Older documents default to no hits and independent takes.
`warp_peers` is a sorted reciprocal set including self (at most 128 takes), with
aligned item spans, scalar rates, pitch and source-relative maps. These typed IDs
participate in branch remapping; they are independent of editing groups.

Stretch anchors map item-local seconds to original-file seconds. Between anchors
the map is linear; outside it extrapolates at scalar take playrate. Empty or malformed
legacy maps retain scalar playback; malformed rows are preserved with load warnings.
Nonzero legacy slope is retained with a warning and interpreted linearly. New warp
commands only author slope zero and strictly increasing coordinates with segment
rates 0.01..100; anchor coordinate magnitudes are at most 1e9 seconds. Source positions outside the file represent silence. Master playrate
acts after this map. Splitting/rebasing subtracts the cut from marker item times;
slipping translates absolute source positions; rate-stretch scales marker item times.

### Render settings (schema 2.3, ADR-0055)

Writers now emit `supple.project/2.3`. The 2.3 change is the shape of `project.render`
(§1.10); every other member is as in 2.2. On load, a 2.2 or older `render` migrates:
`format` (string or `{container, bit_depth, dither}`) and `bit_depth` become
`outputs[0]` (a boolean `dither` maps to `tpdf` or `none`); `secondary` becomes
`outputs[1]` (`"none"` or null leaves one output); `tail_ms` becomes `tail_s`;
`normalize` as a string becomes `{mode}`, and its `limit_true_peak_db` moves to the
top level. A save and reopen gives the same bytes. An older application opening a 2.3
file ignores the members it does not know, with warnings, and renders with its own
defaults for them.

### Master hardware outputs (schema 2.4, ADR-0056)

Writers now emit `supple.project/2.4`. The 2.4 changes are the masters' `hw_output`
(§1.4) and the render setting `masters` (§1.10); a 2.3 file loads unchanged apart from
its routes to `hw_channels`, which become `hw_output` as §5.1 says. An older
application opening a 2.4 file ignores both members with warnings: every master but
tr_0 is then silent, and its mix renders tr_0 alone.

### Master recording and items (schema 2.5, ADR-0057)

Writers now emit `supple.project/2.5`. The 2.5 change is the masters' `lane_mute` (§1.4);
2.5 also lets a master be armed (`record.armed`, nothing else in `record`) and hold audio
items, which play after its plain gain/pan stage. A 2.4 file loads unchanged. An older
application opening a 2.5 file ignores `lane_mute` with a warning, but refuses a project
whose masters are armed or hold items: its strip rules allow neither (`track.strip_type`).

### Strip and send MIDI learn (schema 2.6, ADR-0058)

Writers now emit `supple.project/2.6`. The 2.6 change is `learn` on tracks (§1.4),
routes (§1.8) and VCAs. A 2.5 file loads unchanged; an older application opening a 2.6
file ignores `learn` with a warning, and its controllers move nothing on strips, sends or
VCAs.

### Modular racks (schema 2.7, ADR-0062)

Writers now emit `supple.project/2.7`. The 2.7 change is the `racks`, `modules` and
`cables` collections, written only when present, and a route end `module` (rack.md §2).
A 2.6 file loads unchanged; an older application opening a 2.7 file keeps the
collections as unknown sections and refuses routes with a module end.
