# ADR-0090: Offline renders wait for bridged plug-ins, and fail when one fails

**Status:** Accepted, 2026-11-16 (approved by the maintainer). Amends
[ADR-0013](0013-plugin-isolation.md) §5 and §6. Spec: `docs/specs/engine.md`
§3 and §5, `docs/specs/sandbox.md`.

## Context

ADR-0013 §5 gives a bridged call made off the device callback a failure
deadline: 2 s for `process`, 7 s for lifecycle commands. It says this is
"long enough that a loaded machine never gets a healthy plugin killed". §6
then makes a bridge that misses it, or dies, a failed node: `process()`
outputs silence and returns success, so "the rest of the session keeps
playing".

On 2026-11-16 both turned out wrong for offline renders. Under memory
pressure (two test suites at once, zram swap), a healthy bridge stalled
past 2 s and was killed. Its node went silent, and the offline render
succeeded with different audio and no warning. Stopping a bridge with
SIGSTOP for 2.3 to 2.6 s reproduces it (`reports/lead-round26.md`).

For playback, silence and a session that plays on are right: the device
cannot wait. An offline render (an export, a freeze, an applied FX) can
wait, because waiting costs only time. Its output is a file the user keeps,
so a silent plug-in in it is a wrong result, not a glitch.

Which offline renders bridge at all: they make their own instances (engine
§3). An fx set to `shared_bridge` or `dedicated` runs bridged there. One
on `inherit` runs in process, since an offline pool keeps the in-process
default.

## Decision

1. **An offline render gives its bridged calls a deadline of its own: 60 s.**
   - **Scope.** `renderOffline` sets it on every instance it processes, for
     as long as it runs, and restores the process-wide deadline after
     (`host::Instance::setCallDeadlineMs`,
     `OfflineSettings::bridgeDeadlineMs`, `kOfflineBridgeDeadlineMs`).
     Lifecycle commands get 5/2 of it plus 2 s, as before, so 152 s.
   - **What keeps its deadline.** The live paths are unchanged: the
     callback's cycle deadline (ADR-0081), and the process-wide 2 s/7 s off
     the callback (the anticipative renderer, workers, the main thread),
     `host::setBridgeDeadlineMs`.
   - **Why 60 s.**
     - It is 30 times the live value.
     - It is well past any stall measured: failures at 2.3 to 2.6 s; renders
       held through 4 to 6 s stops with a 60 s deadline.
     - It equals the engine tests' patience, which held through those runs.
     - A plug-in that really hangs still fails the render within about a
       minute (two and a half for a lifecycle call), so a job does not wait
       forever.
     - A longer value buys little. A stall long enough to miss 60 s means
       the machine is unusable for the render anyway, and a hung plug-in's
       render would only take longer to fail.
   - **Cancellation is checked between blocks, not inside a bridged wait.**
     A cancel while a plug-in hangs takes effect when the deadline kills it.
   - **The control plane is not covered.** Its socket requests (create
     20 s, others 10 s) run while instances are made, before the render
     starts, and keep their timeouts.

2. **A plug-in that fails during an offline render fails the render.**
   - **When a plug-in counts as failing during the render.** An instance
     the render processes counts if it turns failed after the render
     begins. That covers starting, any block, or stopping, whether by the
     deadline, a crash or the bridge exiting.
   - **What happens.**
     - The block loop stops at the end of that block.
     - `renderOffline` returns `engine.fx_failed`. The message names the
       plug-in, its fx id and the reason, then any others that failed with
       it (a shared bridge). The path is `/fx/<id>` of the first in plan
       order. The hint says to render again, since a render makes its own
       instances, and to bypass or replace the FX if it fails again.
   - **No output is written.** It returns before any caller has its audio,
     so none writes the render's output.
   - **Instances that had failed before the render began keep today's
     behaviour.** A restart failure renders as a warned, compensated
     pass-through. A bridged instance found failed at the start refuses to
     start (`engine.start_failed`). Only a reused pool can hold one: the
     product's renders make fresh instances, so a plug-in that failed in
     the session is made again and renders normally.

3. **Playback keeps ADR-0013 §6.** A failed node is silent, `fx.failed` is
   reported, and the session plays on.

## The timeouts that stay fixed, and why

Review round 30 (F3) asked for every bridge timeout that the render's
deadline does not replace to be listed with its reason. In the host
(`bridge_client.cpp`):

| Timeout | Applies to | On expiry | Why it stays |
|---|---|---|---|
| The cycle's bridge deadline: its period or 2 ms, whichever is larger (ADR-0081) | A bridged call in the device callback | That block is silent; the request stays outstanding | The device cannot wait. |
| 50 ms of consecutive late calls (ADR-0081) | The same | The instance fails; main kills the bridge | A plug-in that never catches up must not cost every cycle its deadline. |
| 2 s for `process`, 5/2 of it plus 2 s (7 s) for lifecycle commands, process-wide (`host::setBridgeDeadlineMs`) | Data-plane calls off the callback outside offline renders: the anticipative renderer, workers, the main thread | The bridge is killed; its instances fail (ADR-0013 §6) | The live session plays from what these produce, so a longer wait stalls playback behind one plug-in. |
| 20 s (`kCreateTimeoutMs`) | `Create`, the control request that loads a plug-in | The bridge is killed | Loading may page in a large binary. It runs before any render, while instances are made. |
| 10 s, the control plane's default | Every other control request: activate, parameters, state, patches, editors, service, notifications | The bridge is killed: an unanswered request leaves the socket stream out of step | They run on the main thread, which they block; 10 s bounds how long a hung bridge can freeze it. |
| 2 s | `Destroy` (`~RemoteInstance`) and `EditorClose` | The bridge is killed, and with it every member of a shared bridge | None worth the cost: under memory pressure, removing one effect or closing its editor kills its healthy neighbours. To be raised to the 10 s default (review round 30, F3). |
| 2 s | `~BridgeProcess`: the child's exit after its control socket closes | The child is killed | The process is going away and no instance uses it any more. |

The bridge itself has no fixed timeout: its main loop waits for requests
and for its plug-ins' timers.

## Commands that render offline

All of them reach `renderOffline` and propagate its error. Behaviour before
this ADR, when a bridged plug-in failed during their render: the command
succeeded with that plug-in silent and no warning. After it, each fails
with `engine.fx_failed`.

| Command | What it does with a render error |
|---|---|
| `render.mix` | The job fails; no file is written. (`render.dry-run` renders nothing.) |
| `render.stems`, `render.regions`, `render.queue.run`, `branch.render-ab` | The job fails at the failing task. Files written for earlier tasks of the same job stay on disk with their render ids. This is unchanged and applies to every error, cancellation included (see Consequences). |
| `track.freeze` | Fails; its pending file is removed (`Artifact`). It already refused any render warning. |
| `track.unfreeze`, and `track.freeze`'s schedule check before it commits | Not affected: these probes return before processing. |
| `item.consolidate` | Fails; pending files are removed. It already refused warnings. |
| `item.mix-to-track` | Fails; the pending file is removed. It already refused warnings. |
| `item.apply-fx` | Fails before its file is written. |
| `analyze.render`, `analyze.track`, `analyze.compare`, `analyze.masking` | Fail; they write nothing. |
| `rack.probe`, `rack.capture` | Fail; they write nothing. |

## Consequences

- An export on a stalled machine takes longer instead of differing.
- An export that loses a plug-in fails, names it and writes nothing.
- An agent's or the user's render job can now fail where it used to
  succeed. The error says which FX failed and what to do.
- **Open question.** A multi-file job (stems, regions) that fails at its
  third task keeps the first two files on disk. They are complete renders
  of their tracks. Whether a failed job should remove what it wrote is a
  separate decision, and it applies to every error, so this ADR leaves it
  unchanged.
- **Tests.**
  - The engine tests' process-wide patience (`bridge_patience.cpp`, 60 s)
    now matters only for live pools. Offline renders use their own
    deadline either way.
  - `test_offline_bridge.cpp` stops bridges for 3 s with the product's
    2 s live deadline, and kills or stops them mid-render. Each case
    decides by its outcome: the render, its audio, its error.

## Alternatives considered

- **No deadline offline.** A hung plug-in would hang the job and its
  cancellation for good.
- **A longer deadline for everything off the callback.** The anticipative
  renderer would stall the live session behind a stuck plug-in.
- **A warning instead of an error.** Freeze, consolidate and mix-to-track
  already refuse warnings, but a plain `render.mix` would still publish a
  file with a silent plug-in in it.
- **Waking a bridged wait on cancellation.** It needs the bridge killed or
  a request abandoned mid-call. A minute's bound was judged enough for now.
