# ADR-0064: Bounded rack latency reaches PDC

**Status:** Accepted, 2026-11-07. Amends ADR-0062. Builds on ADR-0051.

**2026-11-10 amendment:** [ADR-0067](0067-midi-sends.md) extends these MIDI
dependencies to channel sources and Notes destinations, including implicit
Notes readers of a channel.

## Context

A plug-in module can delay its output, including through oversampling.
Treating its rack as a zero-latency node makes that output late relative
to the song. A rack can expose several audio and MIDI outputs, contain
feedback and intentionally sum paths with different delays.

## Decision

On the main thread, after plug-in stages are prepared, compute each audio
Out and Note Out's largest sum of plug-in latencies on feed-forward paths
ending there. Script modules contribute zero. Remove exactly the cables
marked `feedback` by the schedule (backward edges in a feedback group's
module-id order); the remaining graph is a DAG. A plug-in on a forward
path counts even inside a loop, including a loop closed only by modulation.
The removed cables keep the schedule's one-frame feedback delay. Depending
on module-id order, the plug-in where a loop is entered can fall behind a
removed cable and its latency may not count. Module ids cannot be changed
in place: to keep that latency on a forward path, recreate (or cut and
paste) the module that should come later so it takes a higher id, or use
an explicit Delay module to align paths that need it.

The rack node's latency is the maximum over audio Outs and Note Outs with
an outgoing edge. Unrouted outputs do not add latency to the song.
Read-ahead PDC treats it like a strip with that chain latency. Notes,
Transport and automation share the node's shifted clock. Shorter audio
and Note Out edges receive the difference through the existing edge delay
mechanism.
Plug-in latency changes rebuild the plan as for strips. `rack.get` reports
the node and individual output latencies from the prepared plan. Its
brief/default path never instantiates plug-ins: without prepared timing,
latencies are null. An explicit `detail: "full"` may probe unprepared FX.

Paths that reach an Out only through modulation inputs count as latency.
This is a dependency policy, not alignment of every internal audio path.

Parallel paths inside a patch stay unaligned, as on hardware. A dry path
summed with a plug-in path is early by that plug-in's latency; patches
that need alignment insert a Delay module explicitly. The schedule gains
no path-alignment delay storage.

## Consequences

Independent rack outputs align with the song using existing PDC storage
and executors. Feedback remains an intentional timing effect rather than
an unbounded compensation problem. Feedback back edges and unequal paths
summed before an Out are not corrected. This bounded policy keeps timing
deterministic without changing patch sound or scheduling.
