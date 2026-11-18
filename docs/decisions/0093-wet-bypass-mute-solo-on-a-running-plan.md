# ADR-0093: FX wet and bypass, strip mute and solo reach a running plan

**Status:** Accepted, 2026-11-18. Widens ADR-0060's mix-value edits (route E
stage 4).

Contracts: `specs/engine.md` §1.6 and "Groups".

## Context

After route E stages 1 to 3, a level, pan or patch edit no longer compiles a
plan. Wet, bypass, mute and solo still compile, although a plan's executors
already read each of them every block. A stage's target wet level, 0 when
it is bypassed, is ramped over `Plan::smoothFrames` each block. Mute and
solo close fader and send gates that ramp the same way. Track mute already
skips the compile in a session with hardware MIDI routes (`933d5071`) through
ADR-0060's mix slots.

Solo was decided twice: by the plan compile and by `model::
effectiveTrackLevel`, the readout behind `track.get` and `group.get`. The
two copies agreed only because they had been written alike.

## Decision

### One solo function

`model::soloSilencedTracks(session)` returns the tracks a solo silences. The
plan compile and `effectiveTrackLevel` both call it; neither keeps its own
copy. The rules are unchanged. A test compiles sessions covering every solo
rule and checks, for each node, that the plan, the readout and the function
agree. A mutant that changes one rule, and one that makes the plan diverge,
both fail it.

### What a running plan takes without compiling

An edit that changes only the following fields is published into the newest
plan, by ADR-0060's detection (a whole-object comparison with only these
fields copied from the new version), slots and seqlock:

- **FX `wet` and `bypass`**, on every stage that runs the FX: a track's
  chain, its input chain, its take chains and a rack's stages. Each stage
  has a slot. The executor copies the slot into the stage at the start of
  a block, and the stage's own wet ramp then crossfades as it does today.
  `offline` is not one of these fields: it decides whether an instance
  exists.
- **Track `mute`**, in every session, not only with hardware MIDI routes.
- **Track `solo`** (Off, On, InPlace, Defeat) and **VCA `solo` and
  `mute`**. The main thread calls `model::soloSilencedTracks` on the new
  session and publishes one silenced flag per node. A rack send follows its
  home channel's flag.
- **Route `mute`**, only when the send is encoded either way: a tap before
  the fader, a level other than 0 dB, its own pan, mono or polarity. A route
  mute decides whether a send needs an encoder (ADR-0060's rule for a 0 dB
  level), so a mute that changes the encoding compiles. MIDI routes and rack
  sends are never encoded, so their mute is always a value.

Item mute still compiles, because it decides which events a plan holds.

### An edit that changes both executors' values compiles

In the anticipative executor, the callback takes a value edit at its next
block and the renderer at the next block it renders, up to the render-ahead
later in the stream. An edit that changes these fields both on a node, edge
or stage the callback reads and on one the renderer reads would therefore
play, in between, a mix that neither session has. Moving solo from a
monitored track to a rendered one would silence both until the renderer took
it (about 200 ms with a full renderer); the reverse would play both. Such an
edit compiles, so both executors switch at the handover
(`detail::splitsExecutors`, on the main thread, before anything is
published). It compares each value the new session would publish with the
one its slot holds, and tells the readers apart as `syncMix` does: a node's
executor, an edge's owner, a hardware MIDI route's source.

Every edit that still takes the value path changes one executor's values,
so its output is the old session's up to one point and the new session's
after it. The in-callback executor has one executor and is unaffected.
ADR-0060's level and pan edits are not gated this way: they blend rather
than silence, as that ADR describes.

The alternative was to let live nodes take a value edit at the stream
position where the renderer takes it. That keeps these edits off the
compile, but the callback would have to switch values inside a block at the
renderer's handover position, as plan adoption does. It remains open if
these compiles turn out to matter.

### Why nothing at compile time depends on wet or bypass

The compile reads `fx.bypass` and `fx.wet` only to set the stage's
`bypass` and `wet`.

- **Latency and PDC.** A stage's latency comes from its instance (or its
  oversampler), whether it is bypassed or not. Its dry delay is always
  sized to that latency (`plan.cpp`, `prepareStage`), so a bypassed stage
  delays its signal exactly as a running one does. The latency solve, chain
  layout and edge delays never read bypass.
- **The MIDI bus.** A stage's place on the bus comes from its ports, at
  compile. Whether it takes part in a block (`busLive`) is decided at the
  start of each block from `bypass` (`midi_bus.cpp`).
- **Auto-bypass on silence** is its own field. Its sleeping state is
  decided per block.

A test compiles a session with every FX's bypass and wet changed and checks
that every latency, delay, live flag and bus slot equals the original
compile's; only the stages' `bypass` and `wet` differ.

## Consequences

- Wet, bypass, mute and solo edits cost a model commit and a few atomic
  stores, whatever the session's size. This includes a solo, which floods
  the signal graph once on the main thread. In a split plan the main thread
  also compares the new values with the slots once, and floods for solo a
  second time.
- In the in-callback executor, each of these edits while playing sounds
  sample for sample like a compiled reload.
- In the anticipative executor, a node the renderer runs takes a value
  edit at the start of the next block the renderer renders. A compiled
  reload hands over at the renderer's position too. A live node takes a
  value edit at its next callback block.
  - **Exact, when the renderer has rendered its whole render-ahead and is
    waiting for the callback as the edit is made.** An edit heard only on
    rendered nodes sounds sample for sample like the compiled reload: both
    start at the block where the renderer resumes. An edit heard on a live
    node plays there from its next block. The compiled reload reaches it at
    the renderer's position, the render-ahead (200 ms by default) rounded
    up to a render block later: 213 ms in the tests. The two are
    identical once that reload's ramp is done.
  - **Otherwise,** the renderer may still be filling while the compile
    runs. It renders those blocks with the old plan, so the compiled reload
    hands over later than it would have, never earlier. A value edit is
    therefore never heard later than a compiled reload made at the same
    moment.
  - An edit that changes both executors' values compiles (above), so it is
    a compiled reload.
  - The tests fill the renderer before the edit, so every run starts from
    the same renderer position, and check the exact case. Comparing runs
    without that compares different renderer positions, by whole render
    blocks (native CI 44247107355).
- Offline renders still compile in full and are the reference.
- Native macOS and Windows CI must pass before merging.
