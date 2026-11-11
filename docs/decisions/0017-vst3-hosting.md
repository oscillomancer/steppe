# ADR-0017: VST3 hosting

**Status:** Accepted, 2026-09-27. Build prompt Phase 4: "Plugin hosting: CLAP
first (including the custom semantic extension), VST3 second". Builds on
ADR-0013 (the `host::Instance` API with more than one backing).

**Event output amendment, 2026-11-10:** [ADR-0068](0068-plugin-midi-output.md)
activates a VST3's event output bus and reads its note, poly pressure and
legacy MIDI CC output.

**Hosting amendment, 2026-10-14:** [ADR-0036](0036-native-plugin-hosting.md)
adds native in-process loaders and a standalone supervised scanner, while
retaining the Linux audio bridge and the remaining engine/device/GUI gates.

## Context

Everything in the engine, the sandbox and the optimizer talks to plugins
through `host::Instance` (clap-host). Its API is CLAP-shaped but not
CLAP-bound at the call sites: plain parameter values with min/max/default,
text conversion, state blobs, latency/tail, start/stop processing on the
audio thread, `process()` with sorted note/param/MIDI events and a
transport. ADR-0013 already added a second backing (the bridge proxy)
behind the same class.

The VST3 SDK is MIT-licensed from 3.8.0 (ADR-0003, "re-verify at use").

## Decision

1. **Vendor only what a host needs**: `pluginterfaces/` and the hosting
   helpers of `public.sdk/source/vst/hosting` (module loading, plug
   provider, parameter changes, event list, process data) plus their
   `base/` dependencies, pinned to a 3.8.x tag, under
   `third_party/vst3sdk-<tag>`. The license is re-read at vendoring time;
   anything not MIT stops the work until the maintainer decides. No VSTGUI, no samples.
2. **A third backing of `host::Instance`**, selected by
   `PluginInfo::format == "vst3"` (a new field; CLAP is the default). The
   class keeps its API; each method forwards to the VST3 backing when set,
   as it does for the bridge. `raw()` stays CLAP-only (null for VST3).
3. **Scanning**: `.vst3` bundles (`<name>.vst3/Contents/x86_64-linux/
   <name>.so`) in `$VST3_PATH`, `~/.vst3`, `/usr/lib/vst3`,
   `/usr/local/lib/vst3`; `moduleinfo.json` when present, else the factory.
   Every "Audio Module Class" becomes a `PluginInfo`: `id` =
   `"vst3:" + 32 hex digits of the class ID` (stable across versions, as
   REAPER keys VST3s), name/vendor/version from the factory, `instrument`
   from the "Instrument" subcategory. The model's `PluginRef.format` is
   `vst3` for these.
4. **Mapping.**
   - Buses: the main audio input/output buses at stereo (arrangement
     negotiated with `setBusArrangements`; mono or other layouts fall back
     like CLAP's, reported by `audioInputChannels()`), the first event
     input bus for notes.
   - Parameters: `ParamInfo` in plain values (`normalizedParamToPlain` of 0,
     1 and the default; `stepCount` → stepped/enum), text via
     `getParamStringByValue`/`getParamValueByString`; `setParam(id, plain)`
     converts and queues a normalized change in the next block's
     `IParameterChanges` at offset 0; parameter automation from the engine
     arrives as param events with sample offsets and becomes points of the
     same queue. Read-only, hidden and bypass flags map to ours. A VST3 has
     two parameter domains — the plain value every command uses and the
     normalized value the processor is given with its events — so
     `Instance::paramsNormalized()` is true for it, `Instance::normalizedParam`
     / `plainParam` convert between them (main thread) and the engine's
     automation compile moves a plain lane's envelope points into the
     normalized domain, so `Event::Param` values are normalized for a VST3
     and no conversion happens on the audio thread (ADR-0010's semantic lanes
     already interpolate in their normalized domain).
   - Notes: CLAP-style note events become `NoteOnEvent`/`NoteOffEvent`
     (note ids kept); raw MIDI note-on/off become the same note events, and
     CC/pitch bend/aftertouch map through
     `IMidiMapping` to parameters (a VST3 has no raw MIDI input), unmapped
     ones are dropped. Notes the host delivered are remembered, because a
     VST3 cannot be asked to end them: `reset()` marks them and the next
     `process()` sends their note-offs first (the engine's "no hanging notes"
     calls depend on it). Overlapping notes retain counts and ids; wildcard
     note-off/choke expands to explicit offs for each tracked count.
   - Transport → `ProcessContext` (tempo, time signature, project time in
     samples and quarters, bar position, playing/looping flags and loop
     range from ADR-0015).
   - State: one blob = a small container (magic, version) holding the
     component state and the controller state, restored in that order with
     `setComponentState` in between (the SDK's documented sequence). Loading
     advances the queued-edit generation; only the audio thread consumes that
     queue. Inactive saving first delivers pending edits via a zero-sample,
     zero-bus processor flush, so component state includes controller edits.
   - Latency: `getLatencySamples()`, re-read after `restartComponent
     (kLatencyChanged)`; tail: `getTailSamples()` (`kInfiniteTail` →
     UINT32_MAX).
   - Threading: component/controller creation, `setActive`, state and
     parameter queries and the two domain conversions on the main thread;
     `setProcessing(true/false)` and `process()` on the audio thread (our
     start/stop processing), which matches the SDK's guidance. The audio
     thread never calls the edit controller. `IComponentHandler` edits and the
     restart flags other than `kLatencyChanged` are *not* handled yet and wait
     for editors (Phase 5): `performEdit` currently drops the value, so a
     plug-in that reports its own parameter changes does not update the
     session, and `kIoChanged`/`kReloadComponent` are ignored.
5. **No semantic view**: VST3 plugins have no semantic extension
   (`info().semantic` false). The LLM-facing commands work through plain
   parameters and their text conversion, exactly as for non-semantic CLAPs.
6. **Isolation works unchanged**: the bridge hosts VST3 instances with the
   same backing inside (`<slug>-bridge` links it); the protocol carries the
   format.
7. **Tests** use a small MIT VST3 test plugin built in-tree from the SDK's
   headers (gain with one parameter, an instrument that emits a sine per
   note, a latency parameter), not a third-party binary: scan, params and
   text, state round trip, bit-exact processing vs a reference, notes,
   latency change, isolation, and the engine's RT ≡ offline test with a
   VST3 in the chain.

## Consequences

- One engine code path for both formats; the model already names the
  format. Plugin lists show both; `fx.add {plugin}` accepts a VST3 id.
- VST3 editors (GUI embedding) wait for Phase 5 with CLAP GUIs.

## Alternatives considered

Hosting VST3 through a VST3→CLAP adapter (no permissive, maintained one
exists for hosting direction); a separate `Instance` subclass hierarchy
(every call site would have to know the format; the forwarding backing is
already the pattern); vendoring the whole SDK (tens of MB of samples and
VSTGUI we do not need).

## Implementation notes (2026-09-28)

- The SDK subset is `third_party/vst3sdk-v3.8.0_build_66` (`9fad977`, MIT,
  every file checked); `tp-vst3-host` builds the module loader, event list,
  host classes and string helpers. `host/clap-host/VST3.md` has details.
- Parameter changes use preallocated bounded queues (`FixedChanges`), not the
  SDK's `ParameterChanges` (its point vectors grow on the audio thread).
  rtsan is clean for the in-tree module; the plug-in's own `process` is its
  responsibility, but the host no longer calls the edit controller from the
  audio thread: `setParam` converts on the main thread and the engine's
  automation lanes are compiled into the normalized domain, so the events the
  processor receives need no conversion.
- Buses: a plug-in that adapts the layout offered in `setBusArrangements`
  (`kIoChanged` is not handled, so this happens once, at activate) is reported
  and served its own width: `inputChannels()`/`outputChannels()` follow what it
  settled on, the main buses are declared with that width and padded (silence
  in, scratch out) so the bus never reaches past the host's channel array, and
  the host clears any output channel the plug-in does not write.
- Modules are never unloaded (a strong, deliberately leaked cache): two
  locally built plug-ins exported the same GNU-unique `fmt` symbols, and
  unloading one after the scan crashed the process at exit.
- Host sequence as the SDK's: a host context (`IHostApplication` with our
  name and `IMessage` creation) for `initialize`; a separate controller is
  connected to its component (`IConnectionPoint`) and gets the component
  state; every audio bus gets an arrangement and buffers (main buses carry
  the track, auxiliary ones silence or scratch), since strict plug-ins
  refuse a partial layout and output silence; `setProcessing` answering
  `kNotImplemented` counts as started.
- Non-semantic plug-ins (VST3 and plain CLAP): `fx.describe {full}` lists
  their plain parameters (id, name, range, default, saved value; `filter`,
  at most 512 with `params_total`); `plugin.list` reports the format.
- Checked against the locally installed VST3s (thirteen classes including
  sfizz and plug-ins with sidechain buses and eight outputs): all scan,
  instantiate, list parameters and render audio through the engine.
- Not yet: a persisted scan cache (each start loads every bundle without a
  `moduleinfo.json`), out-of-process scanning, editors (Phase 5), a
  VST3-specific crashing fixture (isolation is shared with CLAP), component
  handler edits and the other restart flags (they need an editor to matter; a
  plug-in's own `performEdit` is dropped, see the Threading bullet), and a
  consumer for `OutputEvents` (the VST3 backing reports normalized values; a
  main-thread consumer converts with `Instance::plainParam`).
