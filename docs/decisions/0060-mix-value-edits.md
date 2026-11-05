# ADR-0060: Mix-value edits update the running plan

**Status:** Accepted, 2026-11-05.

Contracts: `specs/engine.md` §1.6, `specs/gui.md` §2.

## Context

A fader or pan drag sends one `track.set` per pointer motion event, and every
edit committed a version that `Engine::load` compiled into a new plan: the
graph, every source and, once master playrate scheduling has started
(ADR-0045, ADR-0046), a prepared rate transition with source priming. The
main thread did that work for each motion event. A 40-track session without
plug-ins measured 0.27 ms per load before the playrate was touched and 7 ms
plus 4.8 ms of rate preparation after it, which stays until the engine stops
because rate-enabled plans glide through 1.0. With 125–1000 motion events a
second, the GUI thread could not keep up: faders and pan controls lagged
behind the hand in busy sessions, and badly after any playrate change.

ADR-0046 already gave the master playrate scalar a control path that leaves
the plan alone. Strip levels and pans had none.

## Decision

An edit that changes only strip `volume_db`/`pan`, send `volume_db`/`pan` and
VCA `volume_db` is applied to the newest published plan instead of compiling.

- Detection compares persistent collections by storage, then each changed
  object whole with the mix fields copied from the new version, so a field
  added later can never pass as a mix edit. A send level moving to or from
  exactly 0 dB, or a send pan starting or stopping to follow its source,
  changes the send's encoding and compiles. An unchanged session compiles (a
  maintenance reload).
- Each plan has mix slots of atomics (per node, per route edge, per VCA),
  initialised from what was compiled. The main thread publishes the new
  values between an odd and an even sequence number (a seqlock).
- Each executor copies the slots for the nodes and edges it reads at the
  start of a block, using the functions compile uses for static gains, so the
  plan then equals a compiled one. The existing fader ramps carry the change;
  an edit while playing is bit-identical to a compiled reload at that block. A
  copy that races a publication keeps its old sequence and repeats at the
  next block, so no executor waits.
- The GUI sends a control drag's edits once per frame (gui.md §2).

## Consequences

Level and pan drags cost a model commit and a few atomic stores, whatever the
session size and whether rate scheduling is on. Offline renders and the
real-time output of an unedited session are unchanged. Mute, solo, routing and
every other edit still compile; making them cheap is separate work. A value
published while a plan waits for adoption is heard when it is adopted. Native
macOS and Windows CI must pass before merging.
