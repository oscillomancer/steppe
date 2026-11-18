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
   - **Scope.** `renderOffline` sets it on every instance the render made,
     as soon as it has made them, and restores the process-wide deadline
     when it returns, whatever it returns
     (`host::Instance::setCallDeadlineMs`,
     `OfflineSettings::bridgeDeadlineMs`, `kOfflineBridgeDeadlineMs`).
     Lifecycle commands get 5/2 of it plus 2 s, as before, so 152 s.
   - **Control requests inside the render.** Activation, and the readouts
     of meters once the render has succeeded, are control-plane requests
     made by the render's thread. With an instance deadline set, each gets
     at least that deadline's lifecycle value (152 s) instead of its
     control-plane timeout (10 s). Review round 34 (F2) found them on the
     10 s: a stall that the render's blocks survive could still fail its
     activation.
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
   - **A stop or a cancel does not wait for the deadline.** The block loop
     checks both between blocks. A watch thread also checks them every
     10 ms, from the deadline's start until the render's last request, and
     on either kills the render's own bridges
     (`host::Instance::abandonBridge`: the bridge is failed as abandoned,
     then killed). A wait in a bridged call then returns at its next 10 ms
     step, and a control request as its socket closes. That covers
     activation, reset, flush, start, a block, stop and the readouts. The
     render returns `job.cancelled`, checked before any failure of the
     instances it killed. Every caller gives a render its own instance
     pool, so no one else uses those bridges.
     - Review round 34 (F1) found that a cancel used to take effect only
       when the deadline killed a hung plug-in: up to 152 s, during which
       quitting or opening a project waited for the job's thread.
     - The 10 ms is how often the watch looks, not a deadline: it decides
       nothing by time, and the tests hold each call at a gate until they
       have stopped the render.
     - **Not covered:** making the instances. `Create` (20 s) and the
       patch requests run before the render has instances to watch.

2. **A plug-in that fails during an offline render fails the render.**
   - **When a plug-in counts as failing during the render.** An instance
     the render processes counts if it turns failed after the render
     begins. That covers starting, any block, or stopping, whether by the
     deadline, a crash or the bridge exiting. A plug-in that fails while
     the render activates it fails the render with the activation's own
     error, as before.
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
| 20 s (`kCreateTimeoutMs`) | `Create`, the control request that loads a plug-in | The bridge is killed | Loading may page in a large binary. In a render it runs while the render makes its instances, before its own deadline and watch apply. |
| 10 s, the control plane's default | Every other control request: activate, parameters, state, patches, editors, service, notifications, and `Destroy` and `EditorClose`. An offline render's own instances get at least 152 s (above) | The bridge is killed: an unanswered request leaves the socket stream out of step | Outside a render they run on the main thread, which they block; 10 s bounds how long a hung bridge can freeze it. |
| 2 s | `~BridgeProcess`: the child's exit after its control socket closes | The child is killed | The process is going away and no instance uses it any more. |

The bridge itself has no fixed timeout: its main loop waits for requests
and for its plug-ins' timers.

`Destroy` and `EditorClose` had 2 s. On a shared bridge, a stall past that
while one plug-in was removed, or its editor closed, killed every other
plug-in in the bridge, live sessions included. They now have the 10 s
default like every other main-thread request; a test destroys one of two
instances while their bridge is stopped for 3 s and plays the other.

## Commands that render offline

All of them reach `renderOffline` and propagate its error. Behaviour before
this ADR, when a bridged plug-in failed during their render: the command
succeeded with that plug-in silent and no warning. After it, each fails
with `engine.fx_failed`.

| Command | What it does with a render error |
|---|---|
| `render.mix` | The job fails; no file is written. (`render.dry-run` renders nothing.) |
| `render.stems`, `render.regions`, `render.queue.run` | The job fails at the failing task. Files written for earlier tasks of the same job stay on disk with their render ids, and the failed or cancelled job's `result` lists them (see Consequences). |
| `branch.render-ab` | Fails, returning only the error: it renders both branches synchronously, without a job. If branch B fails after A was written, A's file stays on disk and is listed nowhere, as for any call made without `job`. |
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
- **A multi-file job that stops early keeps what it wrote.** A stems or
  regions job that fails at its third task keeps the first two files:
  they are complete renders of their tracks. Decided in review round 34,
  for every error and for cancellation: the job's snapshot carries a
  `result` beside its `error`, listing the finished tasks' entries in
  `files` and any file a task that did not finish had already written in
  `unfinished_task_files` (`JobManager::fail` takes the partial result;
  engine §3). A call made without `job` returns only the error.
- **Tests.**
  - The engine tests' process-wide patience (`bridge_patience.cpp`, 60 s)
    now matters only for live pools. Offline renders use their own
    deadline either way.
  - `test_offline_bridge.cpp` stops bridges for 3 s with the product's
    2 s live deadline, and kills or stops them mid-render. It holds the
    gain fixture inside each call at a gate (`lifecycle_gate.h`), stops or
    cancels the render, and checks the outcome and why the plug-in failed.
    Each case decides by its outcome: the render, its audio, its error.

## Alternatives considered

- **No deadline offline.** A hung plug-in would hang the job and its
  cancellation for good.
- **A longer deadline for everything off the callback.** The anticipative
  renderer would stall the live session behind a stuck plug-in.
- **A warning instead of an error.** Freeze, consolidate and mix-to-track
  already refuse warnings, but a plain `render.mix` would still publish a
  file with a silent plug-in in it.
- **Waking a bridged wait on cancellation by killing the bridge.** First
  rejected as more than needed with a minute's bound; adopted in review
  round 34 once that bound was found to hold up quitting and opening a
  project (Decision 1).
- **A stop callback instead of the watch.** It would kill on the stop
  token at once, from the thread that asks for the stop, but a job's
  cancel flag has no callback, so the watch would be needed anyway.
