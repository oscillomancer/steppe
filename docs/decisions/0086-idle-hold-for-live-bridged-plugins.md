# ADR-0086: The idle-latency request is held while a bridged plug-in plays live

**Status:** Accepted, 2026-11-16. Contracts: `AGENTS.md` rule 4,
`docs/specs/engine.md` (CPU idle latency), `docs/specs/gui.md` (Preferences),
the `engine.status` schema (`cpu_idle_latency.automatic`).

## Context

The 20 µs CPU idle-latency request (`/dev/cpu_dma_latency`, held while the
engine runs) has been opt-in, the **Keep CPUs out of deep idle** preference,
off by default. The item D measurements found that a bridged plug-in on a
live node waits for its bridge's audio thread to wake. When that thread's
core has idled into a deep state (C2 here, a 127 µs exit), the wake is late.
Holding the request cut bridge wakes over 200 µs three to six times and
removed the slow mode in every bridged row. It cost nothing measurable in
process. In-process plug-ins do not wait for another thread to wake, so they
do not need it.

## Decision

1. **Automatic while a bridged plug-in plays live.** While the device
   callback processes a bridged instance, the engine holds the request as if
   the preference asked for it. The callback processes every node with the
   in-callback executor, and only the live (monitored) nodes with the renderer.
   `Engine::bridgedLive()` says so for the last published plan; the app's
   model-thread tick passes it to the runtime protections, which act on a
   change only. The hold is released when no such instance is left, and
   when the engine stops.
2. **The preference keeps meaning "always".** With it on, the request is held
   whenever the engine runs, as before. Turning it off while a bridged
   plug-in plays live keeps the automatic hold.
3. **A per-process request, no system setting.** The request is the process's
   own open file. The kernel drops it when the file closes (engine stop, or
   the process exiting), and nothing is written to any setting that outlives
   it (`AGENTS.md` rule 4). While held, it keeps the CPUs out of deep idle
   states, which may cost power and heat. The manual says so.
4. **Linux only.** macOS and Windows have no per-process idle-latency request.
   Windows' power plan is system-wide, and Steppe leaves it alone. On both,
   the audio threads, the bridges' included, already ask for the platform's
   audio class (macOS's time-constraint policy, Windows' MMCSS "Pro Audio"),
   which is what those systems offer an audio thread. `automatic` stays false
   there and nothing is opened.
5. **Status, not a warning.** `engine.status.cpu_idle_latency` gains
   `automatic`. An automatic hold that fails, most often because only root
   may open the device, records its error there with a hint that fits: audio
   plays without it. It is not added to the engine's warnings, which would
   otherwise appear on most systems. A failure of the requested hold still
   warns. A failed automatic hold is retried only when the state changes,
   not on every tick.

## Consequences

- Live bridged plug-ins get the measured wake improvement without the user
  finding the preference, where the device allows it.
- Systems where the device is root-only get no hold. The Preferences note and
  `engine.status` say why. The manual says how to grant access.
- In-process sessions behave exactly as before.
