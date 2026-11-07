# ADR-0064: Bounded rack latency reaches PDC

**Status:** Accepted, 2026-11-07. Amends ADR-0062. Builds on ADR-0051.

## Context

A plug-in module can delay its output, including through oversampling.
Treating its rack as a zero-latency node makes that output late relative
to the song. A rack can expose several audio and MIDI outputs, contain
feedback and intentionally sum paths with different delays.

## Decision

On the main thread, after plug-in stages are prepared, compute each audio
Out and Note Out's largest sum of plug-in latencies on feed-forward paths
ending there. Script modules contribute zero. Collapse feedback groups;
an entire feedback group contributes zero, including its plug-ins. Its
internal cables already have the patch's one-frame feedback delay.

The rack node's latency is the maximum over its audio Outs. Read-ahead PDC
treats it like a strip with that chain latency. Notes, Transport and
automation share the node's shifted clock. Shorter audio and Note Out edges
receive the difference through the existing edge delay mechanism.
Plug-in latency changes rebuild the plan as for strips. `rack.get` reports
the node and individual output latencies.

Parallel paths inside a patch stay unaligned, as on hardware. A dry path
summed with a plug-in path is early by that plug-in's latency; patches
that need alignment insert a Delay module explicitly. The schedule gains
no path-alignment delay storage.

## Consequences

Independent rack outputs align with the song using existing PDC storage
and executors. Feedback remains an intentional timing effect rather than
an unbounded compensation problem. Latency inside a feedback group and
unequal paths summed before an Out are not corrected. This bounded policy
keeps timing deterministic without changing patch sound or scheduling.
