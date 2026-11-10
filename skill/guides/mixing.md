# Corrective mixing

Mixing in Steppe is measurement-driven: render the mix, analyze it, change one
thing, render again, `analyze.compare`. Use the stock EQ and dynamics plugins for
corrective processing; their guides describe the controls. This guide covers
levels, panning, muting/soloing, routing, rendering, and measuring loudness and
masking.

To measure a point in an FX chain (loudness, peaks, tonal balance, stereo,
key), put Lichen there and read it with `fx.meters`; see [the analyzer
guide](analyzer.md). `fx.meters` reads any plugin's readouts the same way
(a reverb's RT60, a gate's suggested threshold): `from: "render"` after a
`render.mix`, live while playing.

## The loop

1. `session.describe` — tracks, their chains, levels and routing in one read.
2. `render.mix {analyze: "summary"}` (optionally `start_beat`/`end_beat`) writes
   the mix to a WAV and returns its analysis inline. Use
   `analyze.render {render: <render id>}` to inspect it later. Read loudness
   first (`integrated_lufs`,
   `true_peak_dbtp`), then the band levels and stereo width.
3. Run `analyze.masking {masker_track, maskee_track}` on parts that fight
   (kick and bass, bass and keys). It renders each track's post-fader output
   and reports per Bark band how much of the maskee is masked and the three
   worst bands.
4. Fix with the smallest move: level (`track.set {track, volume_db}`), pan
   (`track.set {track, pan}`), a different octave or voicing (edit the MIDI),
   or a cut with Yarrow in the masked band on the less important part.
5. Render again and `analyze.compare {reference, test}` with the two render ids.

Group a change and its render in one `batch`: one undo step, atomic.

## Sends and buses

Use `route.add {"from":"tr_1","to":"tr_2","tap":"post_fader","volume_db":"-6 dB"}` to feed a bus track; place bus processing in that track's FX chain and use `route.list` to inspect sends. `route.set` changes a send's level, pan, tap, endpoints, or mute; `route.remove` deletes it. The destination must not feed the source, including through folder parent sends. The `channels` argument maps one-based source channels to destination channels, for example `{"from":[3,4],"to":[1,2]}`. Hardware endpoints are not available through these commands yet.

To sidechain, route into the plugin's key: `route.add {"from":"tr_1","fx":"fx_3","tap":"post_fader"}` (tr_1 is the kick track and fx_3 is the compressor on the bass track; `fx.describe` shows `sidechain_channels` when a plugin has a key input, otherwise the route fails with `route.no_sidechain`). The key is delay-compensated, follows the source's mute at a post-fader tap, and keeps sounding when another track is soloed.

Use `plugin.list` to find a plugin id for `fx.add`; the returned `fx` (also shown by `session.describe`) identifies the loaded instance. Use `plugin.scan` to refresh the catalogue, `fx.move` to reorder a chain, `fx.patch.get/set` for a semantic patch, and `fx.param.get` for one value. For arrangement edits, `item.set/split/duplicate` and `take.list/add/set/activate` change clips and takes; `marker.*` and `region.*` label the timeline, while `time.convert` maps seconds, beats, and bars.

## Exporting and comparing versions

`render.stems {mode: "isolated", tap: "post_fader"}` writes each top-level
track before master processing. Use `mode: "through_master"` to hear one stem
at a time through the master chain. `render.regions` writes named regions; a
project's `render.matrix` can select tracks or master ids for each region.
`render.dry-run {operation: "regions"}` lists the files and lengths first.
Patterns such as `{project}-{region_number:02}-{region|replace(" ","_")}`
name the files; names that escape the renders directory are rejected.

Use `branch.render-ab {a, b, end_beat}` to render and analyze two branch heads
without switching the active branch. For a long export, pass `job: true` to
`render.mix`, `render.stems`, or `render.regions`, then use `job.get {job}` or
`job.wait {job}` for progress and `job.cancel {job}` to stop it. The project's
render settings (`render.settings` shows them and where each comes from) apply
to every render; arguments override them for one call. Set
`normalize: {mode: "lufs_i", target: -14}` with `limit_true_peak_db: -1` to
deliver a streaming master: each output is measured as written and its
`checks` say whether it met the target and the ceiling.

## Targets that are safe defaults

| goal | measure | target |
|---|---|---|
| streaming-ready level | `loudness.integrated_lufs` | −14 (±1) |
| no inter-sample clipping | `loudness.true_peak_dbtp` | ≤ −1.0 |
| mono-safe low end | `spectral.bands.low.width`, `spectral.bands.sub.width` | < 0.1 |
| no mud | `spectral.bands.low_mid.level_db` | not the loudest band unless intended |
| not harsh | `spectral.bands.high_mid.level_db`, `spectral.tilt_db_per_oct` | tilt −3..−6 for full mixes |
| mono compatibility | `stereo.mono_loss_lu` | < 1.5 LU |

Request `detail: "standard"` with `analyze.render` for band level objects and
widths; summary detail provides scalar band levels and omits widths.
These are starting points, not rules; genre and intent win. When a target
conflicts with the sound the human wants, say so and ask.

## Habits

- Change levels by small amounts (1–2 dB) and re-measure; loudness is not
  linear in perception.
- Keep the master under 0 dBTP while mixing; loudness comes at the end.
- Work on a branch (`branch.create`) so the human can A/B your mix against
  theirs and merge or discard it.

## Automation

Anything that should change over time — a fade, a filter sweep, a send that
opens for a chorus — is an envelope, not a series of edits:

```
env.add {"fx": "fx_3", "param": "filter1.cutoff",
         "points": [{"bar": 9, "value": "400 Hz"},
                    {"bar": 13, "value": "4 kHz", "shape": "slow"}]}
env.add {"track": "tr_2", "param": "volume",
         "points": [{"bar": 16, "value": 0}, {"bar": 17, "value": "-60 dB"}]}
```

Mixer targets: track `volume` (dB), `pan` (%), `mute`;
route `volume`, `pan`, `mute`. Plugin targets: the semantic keys
`fx.describe` lists (values in their unit; a cutoff sweep moves evenly in
pitch), or a CLAP id/name for other plugins. `env.points.set` replaces
points (or a beat range of them); `env.set {env, active: false}` bypasses an
envelope; render and `analyze.compare` to hear what it did.

Use [Hickory, the stereo utility](utility.md) for gain staging, channel checks, bass mono and lining up multi-miked tracks; its `correlation` and `side_level` readouts say what the stereo image is doing. Render and compare after width or polarity changes.
For a level change at one point in an FX chain, to drive a plugin harder or trim after one, see [Gain](gain.md): one knob, ±32 dB.
For track and bus compression, external-key ducking, upward compression and peak catching, see [Sundew, the compressor](comp.md).
For a true-peak ceiling on the master and loudness against a LUFS target, see [Treeline, the limiter](limiter.md).
Use [Yarrow, the stock EQ](eq.md) for measured rumble, masking and harshness cuts, de-essing and dynamic or mid/side bands.
Use [Sorrel, the gate](gate.md) to reduce between-hit spill or noise between phrases; its `suggested`, `noise_floor` and `openings` readouts say where to set it and whether it chatters.
Use [Teasel, the saturation](drive.md) for colour and distortion; set it by its `thd` and `even` readouts, and drive only a band, the mid or the side when the rest should stay clean.
For onset punch, softened attacks and shorter or longer decays, see [Gorse, the transient shaper](transient.md).
For echoes, in time or free, ping-pong and ducked throws, see [Bracken, the delay](delay.md); its `echo_time` and `decay` readouts say when the echoes come and how long they last.
Use [Ghyll, the reverb](reverb.md) for a room or hall built from its size, materials and the places of the source and listener; its `rt60` and `clarity` readouts say how long and how clear it is. Use 100% mix on a send return.
For chorus, doubling, width or vibrato, see [Withy, the chorus](chorus.md); its `detune` and `mono_loss` readouts say how far the voices swing and what folding to mono costs.
For phasing, a swirl, a resonant or tempo-locked sweep, a photocell throb or an endless barber pole, see [Pollia, the phaser](phaser.md); its `centre_now` readout says where the stages sit, and `keep_below` keeps a bass's or a mix's lows whole.
For tape, from a mix's peaks gently rounded to a worn cassette, see [Umber, the tape machine](tape.md); set its drive by the `vu` readout (0 VU is −18 dBFS RMS), and its `headroom` and `saturation` readouts say how close the peaks came to the tape's limit and what they lost.

## Strip types and Console 9 gain staging

Every strip has a type. Channels play items, instruments and monitored inputs;
create a buss for groups and returns (`track.add {type: "buss"}`), and put
its processing in its insert chain. Masters are terminal roots; they feed
hardware outputs and renders. `render.mix {master: "tr_0"}` selects the main
master; other master ids select independent mixes. Channels cannot receive
other strips or have children. Audio sends target busses or masters; keys may
target a plug-in on any strip.

A master can print its mix in real time: `track.set {track: "tr_0", armed:
true}`, then `transport.record`. The take is the master's output after its
fader, before its own items, and lands as an item on the master. A master's
items (audio only) play after its fader and FX, beside the live mix, so a
fresh print doubles it: `track.set {track, lane_mute: true}` silences the
master's items alone. While a master records, its items are silent in the
punch range (everywhere without punch), so an old print never reaches the new
one. `render.mix` includes them; stems of channels and busses do not.

The implicit summing stages use **Airwindows Console 9, by Chris Johnson**.
Channels encode after inserts; busses decode their input sum before inserts
and re-encode after their fader; masters decode before their insert chain and
finish with a plain fader and balance. A channel's or buss's pan and fader act
before its encode, and an encode → decode hop is unity for small signals, so
under the default 0 dB pan law a centred channel at 0 dB reaches the master at
its own level through any number of busses.

**Pan laws.** `project.set {pan_law: {db, boost}}` sets the law of every
channel and buss: `db` is the centre's cut for a mono source, one of 0
(default), −2.5, −3 (constant power), −4.5 or −6 (constant amplitude). A cut
law applies at every strip a centred signal crosses: under −3 dB a channel →
buss → master path is 6 dB down. `boost: true` (gain compensation) keeps the
centre at 0 dB and raises a hard-panned side by the law instead, which can
clip a hot source. `track.set {track, pan_law: {db: -3}}` gives one strip its
own law; `pan_law: null` returns it to the project's.

**Pan modes** (`track.set {pan_mode}` on channels and busses; masters only
balance): `pan` (default) balances a stereo image and pans a mono one;
`dual` places each input on its own (`pan_left`, `pan_right`, defaults −100
and 100), so you can narrow, widen, move or swap an image without dropping
either side; `width` sets the image's width (`width`: 100 as recorded, 0 mono,
−100 swapped) and places its centre with `pan`. Two inputs at one position
sum, so dual pan's centred inputs follow the law (6 dB hotter for correlated
content at 0 dB). A send that follows its strip's pan copies its mode and law.
Only `pan` is automatable.

`project.set {console9: false}` keeps these gains and pan laws and makes the
stages linear, omitting the curves, denormal noise and dither. Small signals
remain level-matched. Switching takes 10 ms. With Console 9 on, correlated
signals summed before decoding can increase peaks relative to linear summing
before the decode saturates; leave headroom and measure true peaks.

A unity post-fader send with `pan: "follow"` shares the strip's encoded output.
Other audio sends have their own encoder: pre-fader/pre-FX sends use the send
level, while post-fader sends combine strip and send levels (clamped to
+12.04 dB) and follow strip mute. Send polarity and mono conversion happen
before encoding. Mute and solo ramp the fader before encoding over 10 ms;
pre-fader keys still hear the signal, while post-fader keys hear the encoded
fader output. Noise from silent Console 9 stages is near −150 dBFS.

Isolated post-fader channel and buss stems decode through a neutral Buss.
They describe each strip's contribution and do **not** sum exactly to a mix
with nonlinear Console 9 enabled. Use a master render for the final mix.
