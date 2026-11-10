# Spec: CLAP host library and the headless plugin sandbox

**Status:** Implemented. Interface changes go through an ADR.
**Libraries/binaries:** `host/clap-host` (`supple::host`), `host/sandbox`
(`supple::sandbox` library + `<slug>-sandbox` CLI with `--mcp` stdio mode).
**Depends on:** core-*, midi, notation, audio-file, command-bus,
control-protocol (optional socket mode), mcp.

The sandbox proves the core idea early: an LLM, over MCP, loads a plugin,
programs it in physical units and text patches, renders phrases written in
the notation, reads analysis reports, and iterates. It is also the plugin
team's test harness and the seed of the engine (the engine reuses
`clap-host` unchanged).

The sandbox CLI searches explicit `--plugin-path` locations first, then
the bundled stock plugin folder (including the script player), then the
standard CLAP/VST3 locations. Both `exec` and MCP use this order.

## 1. `host/clap-host`

```cpp
namespace supple::host {

struct PluginInfo {
    std::string id, name, vendor, version, description, path;
    std::vector<std::string> features;   // CLAP feature strings
    bool instrument;                     // declares note input and no main audio input
    bool semantic;                       // implements com.suppleaudio.semantic-params/1
};

Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles);   // supervised third-party probes; ADR-0013
std::vector<std::filesystem::path> defaultSearchPath();   // $CLAP_PATH, ~/.clap, /usr/lib/clap, /usr/local/lib/clap, + build dirs passed by the app

class Instance {   // owns one plugin instance; main-thread object
  public:
    static Result<std::unique_ptr<Instance>> create(const PluginInfo&, const HostIdentity&);
    const PluginInfo& info() const;

    // Lifecycle (main thread)
    Status activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames);
    void deactivate();
    std::uint32_t latency() const;     // samples, valid when active
    std::uint32_t tail() const;        // samples; UINT32_MAX = infinite (capped by the renderer)

    // Ports (queried after init; selects a stereo config if offered)
    int audioInputChannels() const;    // main input, 0 for instruments
    int audioOutputChannels() const;   // main output
    bool acceptsNotes() const;

    // Parameters: standard CLAP view
    std::vector<ParamInfo> params() const;             // id, name, module, min, max, default, flags
    Result<double> paramValue(std::uint32_t id) const; // CLAP value
    Result<std::string> paramText(std::uint32_t id, double clapValue) const;
    Result<double> paramFromText(std::uint32_t id, std::string_view text) const;
    // Queue a CLAP value change; applied at the start of the next processed block (or via params.flush when idle).
    Status setParam(std::uint32_t id, double clapValue);

    // Semantic view (only when info().semantic)
    Result<json::Value> semanticSchema();
    Result<std::string> getPatch(bool changedOnly);
    Result<void> setPatch(std::string_view json, int mode);   // errors: the plugin's error array as Error details
    Result<std::string> describe(bool changedOnly);

    // State. A VST3 blob holds the component's state, then the separate
    // controller's (ADR-0017); a controller whose getState answers
    // kNotImplemented (the SDK default; sfizz) has none, which is not an error.
    Result<std::vector<std::uint8_t>> saveState();
    Status loadState(std::span<const std::uint8_t>);

    // Processing (audio thread; RT-safe; SUPPLE_RT)
    void startProcessing(); void stopProcessing();
    bool process(const ProcessBlock&);   // see below
};
}
```

`ProcessBlock` carries planar float input/output pointers (in-place allowed
only if the plugin declares it), frame count, a sorted input event list
(note on/off with CLAP note ids, note expressions reserved, param value
events, MIDI 1.0 events for plugins without CLAP note ports' CLAP dialect),
an output event sink (param gestures/values, note ends, and the note and
MIDI output the plugin emits, ADR-0068), transport
(tempo, beat position, bar start, time signature, playing flag), and a
steady-time sample counter.

`ParamInfo::flags` uses CLAP parameter bits for both formats. In particular,
VST3 `kCanAutomate` maps to `CLAP_PARAM_IS_AUTOMATABLE`.

The host implements: `clap.log` (to the host log, never on the audio
thread synchronously: audio-thread log calls go to a lock-free ring),
`clap.params` (rescan/clear/request_flush), `clap.state` (mark_dirty),
`clap.latency` (changed), `clap.tail`, `clap.thread-check`,
`clap.audio-ports` (is_rescan_flag_supported), `clap.note-ports`,
`clap.timer-support` (main-thread timers, driven by the app loop),
`clap.thread-pool` (not provided), and `com.suppleaudio.semantic-params/1`
host side (records `changed` flags). `request_restart`/`request_process`/
`request_callback` set flags the app services on the main thread.

Fixed-size CLAP parameter names, modules and value text are read only within
their buffers, even if a plugin omits the terminating null. VST3 bus names,
parameter titles and value text are terminated within their 128-code-unit
buffers before UTF-8 conversion. VST3 host text conversion has no shared state;
unpaired UTF-16 surrogates become U+FFFD and invalid UTF-8 parameter input
returns `host.param_text`, without conversion exceptions.

Local and remote instances can install core-base's `supple::MainThreadWake` (public
header `supple/base/main_thread_wake.h`, core-base spec, ADR-0029) with
`setMainThreadWake(MainThreadWake&)` on main. Remote subscriptions are
forwarded by their BridgeProcess (ADR-0031). The relay must outlive the instance and all plug-in callback threads.
Installation also signals requests made during initialization. CLAP callbacks
publish their request/notification flags before notifying: callback, restart,
process, flush, dirty state, rescan/clear, latency/tail/semantic changes, GUI
requests and queued log lines. VST3 component-handler restart/edit and frame
resize requests use the same relay. Stopping processing signals a restart
that had to wait for audio to stop. The hook is optional: in-process callers
retain their existing main-thread servicing contract.

The relay coalesces notifications until main drains them. Producers exchange
an atomic pending bit; the first producer advances a private futex sequence
with release ordering and calls only `rt::wake` (ADR-0024). A parked,
non-audio relay thread observes that sequence with acquire and writes a
nonblocking eventfd watched by the bridge. Main reads the eventfd, exchanges
pending to false with acquire/release **before** servicing work, then drains
request flags. Coalesced publications are acquired by that exchange; a request
after it schedules another pass, including requests raised while servicing.
The relay captures the sequence before writing the eventfd, so a drain/new
request racing that write cannot be mistaken for a request already signalled.
There are no writes, locks, allocations or waits on the audio producer path.
Teardown joins the relay after destroying all instances.

`nextTimerMs()` returns -1 without local timers, 0 when due, otherwise the
nearest remaining duration rounded up to milliseconds and saturated at
`INT_MAX`. Timers and registered descriptors remain serviceable with no editor
open; registrations are main-thread operations, so the loop rebuilds its
watches and deadlines after callbacks and control requests. Local timer and FD
registration/modification/removal also notify the subscribed relay. Callbacks
for descriptors removed since the readiness snapshot are ignored. Idle output
publication (or overflow) notifies after updating its queue/counter. The engine
app subscribes persistent instances and probes to its one relay and includes
all local subscriptions in its service deadline/FD snapshot (ADR-0030).

### Native editors (ADR-0040)

`Instance::openEditor(NativeWindow, double scale)` accepts a borrowed X11 XID,
NSView pointer or HWND, tagged with its native kind. The X11 unsigned-long
overload remains for existing callers. `hasEditor()` probes the platform's
embedded CLAP API (x11/cocoa/win32), or VST3 platform type
(X11EmbedWindowID/NSView/HWND). Unsupported plugins return
`host.editor.unsupported`, path `/editor`, with a parameter-panel hint. Invalid
parents or scales return `host.editor.parent`, path `/editor/parent`, with a
hint to supply a live native parent and positive finite scale.

Open/resize sizes are pixels on Linux/Windows and points on macOS. Cocoa views
handle their own backing scale and are never sent a factor; Linux/Windows
receive CLAP set_scale or VST3 content-scale support when offered. Timers run on the main thread independent
of editor visibility. CLAP POSIX FD support is advertised on Linux/macOS and
serviced by their descriptor loops; Windows does not advertise it.

EditorOpen retains its operation number but now carries `u8 kind, u64 handle,
f64 scale` (17 bytes), followed since POSIX v19 / Windows v20 by an optional
window frame (below). Kinds: none=0, x11=1, cocoa=2, win32=3, floating=4.
POSIX v19 and Windows v20 reject earlier peers at notification subscription.
Windows HWND values retain all pointer bits. Cocoa proxies send Floating with
the DAW owner's server NSWindow.windowNumber (unsigned 32-bit, zero if absent),
instead of an NSView pointer; embedded Cocoa inputs become Floating/zero.
Oversize Floating handles are rejected before native window creation.
POSIX v17 / Windows v18 change this handle's semantics from the v15/v16
zero-only contract, retaining the 17-byte payload and all operation numbers.
The child creates an NSWindow hosting the native embedded plugin view,
and owns its resize/show/hide/close lifecycle. The open
reply and the other editor operation payloads were unchanged at v17/v18; there
is no audio ABI change. A successful open clears retained requests from the previous view
lifetime so a stale close cannot close its replacement. Reopening a Cocoa
floating editor reuses the existing window and updates its owner number.
The bridge orders it just above the owner (or front of normal level for zero),
without activating itself or making the editor key. Plugin-requested show uses
the retained owner number. DAW activation reorders its open bridged editors via
the same open operation; the GUI main-loop hook is supplied by port-gui.

`Instance::setEditorScale(double)` renegotiates the scale of an open editor
when its window's display scale changes (a monitor move or a system scale
change): CLAP `gui.set_scale` then `get_size`, or VST3
`IPlugViewContentScaleSupport::setContentScaleFactor` then `getSize`. A plug-in
that did not change its own size and is resizable keeps its logical size: the
pixel size before the call is scaled by new/old, then goes through
`adjust_size`/`set_size` (CLAP) or `checkSizeConstraint`/`onSize` (VST3). It
returns the pixel size the window must take. Non-positive or non-finite factors
return `host.editor.scale`, path `/editor/scale`; a closed editor returns
`host.editor`. Cocoa editors (points, backing scale observed by the view) are
never told: the call only reports the current size. Bridged instances carry it
as `EditorScale` (appended operation number): request `f64 scale`, reply as
EditorOpen (`u32 width, u32 height, u8 resizable`). This arrived with protocol POSIX
v18 / Windows v19; the audio ABI is unchanged.

**Window frames (macOS, POSIX v19 / Windows v20).** A bridge-owned Cocoa editor
window has a content *frame* `{x, y, width, height}` in points, origin at the top
left of the primary display: the space SDL uses for window positions, so the DAW
stores it like any other editor placement (gui.md §4.5). On the wire an optional
frame is `u8 present, i32 x, i32 y, u32 width, u32 height` (17 bytes, fields zero
when absent; `present` above 1, nonzero fields without `present`, or a size above
2^20 is malformed). `EditorOpen` ends with the frame to restore (a size only when
the editor is resizable; the plug-in may adjust it), `EditorRequests` replies end
with the latest frame the bridge saw move or resize since its last report, and
the `EditorClose` reply is the window's final frame. The bridge compares the live
window with the frame it last reported on each service pass (no timer), and
reports the final frame when the user or the plug-in closes the window. Other
platforms send absent frames. `Instance::EditorRequests::frame` carries it to the
host (merged latest-wins; a frame alone wakes the DAW) and
`Instance::takeClosedEditorFrame()` returns the close reply's frame after
`closeEditor()`. Reopening an existing window ignores the frame.

Windows bridge main waits for IPC and native messages together; Cocoa uses
AppKit with CFFileDescriptor run-loop sources. There is no editor polling timer.
Bridge death/quit destroys its native windows. The DAW remembers Cocoa bridge
placement as above; their focus is not part of its MIDI-learn focus predicate.
Cross-process transient ownership on macOS is
unavailable; normal-level windows can be covered by other applications.
Windows child input is native, with plugin-dependent keyboard/IME behavior.
Real window tests require SUPPLE_GUI_DISPLAY_TEST=1 and a desktop session;
normal native CI runs hermetic editor fixtures only. The Cocoa display opt-in
also checks cross-process relative z-order and normal level with
CGWindowListCopyWindowInfo, unchanged frontmost application on open/reopen,
and replacing the owner without recreating the editor. The DAW shell itself
requires the separate port-gui integration.

### Bridge notifications (POSIX v20, Windows v21; notification design ADR-0031)

The bridge isolates crashes of ordinary plugins; it is not a security sandbox
against hostile plugins. A shared bridge crash affects every instance in that
process. Linux shared-memory memfds are sealed against shrink and grow after
initial sizing, while their mapped contents remain writable. This does not
change the wire protocol. Windows mappings already have fixed extents; macOS
continues to use its existing shared-memory backing.

ADR-0037 extends the subscription to native WakeEvent signal targets:
Linux eventfd (unchanged), a nonblocking write-only pipe on macOS, and a
manual-reset event duplicated into the child on Windows. POSIX negotiates
version 20; Windows negotiates native version 21 (ADR-0068's note output). Invalid resource types,
counts, versions and duplicate subscriptions remain structured errors.
The Windows control channel transfers recipient-owned handle values; the
parent rejects handle-bearing replies without acting on the claimed value.
Notification publication, acknowledge/rearm and retained snapshots remain
identical on each platform.

ChildProcess additionally accepts LaunchOptions: `control = -1` by default
(no extra inheritance), otherwise one native bridge resource (POSIX remapped
to fd 3; Windows retained in the explicit handle list); `captureOutput = true`
by default, false directs stdout to the null device. `inheritStderr = false`
by default retains the scanner's null stderr. The bridge sets it true to retain
plug-in diagnostics: POSIX inherits fd 2; Windows duplicates a valid parent
STD_ERROR_HANDLE as inheritable and adds only that copy to the explicit handle
list, falling back to NUL when it cannot be duplicated. Parent handle flags
are unchanged. Existing scanner callers retain their API and behavior.
`pid()` and `nativeProcess()` expose borrowed identity for bridge exit watches/handle transfer. The owner keeps the process
or unreaped POSIX leader until final tree cleanup; bridge users must depart
before owner destruction. Launch and cleanup are setup/control APIs. The
existing bounded bridge-wait exception sanctions the nonblocking killTree()
on timeout; it never reaps there. Linux retains its pidfd death watch (socket
HUP fallback), while main-thread exit observation uses WNOWAIT until final
helper cleanup. The memfd/futex audio path remains unchanged.

The eventfd/futex/pidfd descriptions below name Linux mechanisms; the native
objects above implement the same publication and lifetime contracts.

Before Create, the Linux parent sends process-level `SubscribeNotifications` with
instance id 0, exactly one dedicated nonblocking eventfd via SCM_RIGHTS, and
`u32 version = 19`; the reply is `u32 version = 19`. The descriptor is checked
for eventfd type, read/write access and O_NONBLOCK; received copies are CLOEXEC.
Missing, wrong-type, extra or duplicate descriptors, duplicate subscriptions,
wrong versions/instance ids and trailing payloads are rejected with structured
errors; rejected descriptors are closed. A host/bridge mismatch is detected
even when no instance is activated. Existing operation numbers are preserved;
SubscribeNotifications and AckNotifications are appended after TakeIdleOutputs.
`AckNotifications` uses instance 0, no payload/fd and an empty success reply.
Protocol 12 appends the main-thread `PluginStatus` query after it (no payload,
one JSON string reply); the data plane and earlier operation numbers are
unchanged. See [plugin-status](plugin-status-extension.md).
Protocol 13 appends `FactoryPatches` after `PluginStatus`: no payload,
one JSON string reply containing the canonical patch array, empty when
unsupported. See semantic-params §3.4. Existing operations and data-plane
layout remain unchanged.
It requires an established subscription. Replies never carry unsolicited fds
or asynchronous notification frames. Scan-only mode is unaffected.

The child keeps parent-visible work in per-instance snapshots. On each main
service pass it captures new notifications, ordered clears, process requests,
log lines, idle outputs/drop counts and editor requests, plus latency/tail and
restart/flush-state transitions. Only new captures advance its process-level
publication generation. Capturing after a callback/timer/fd or Service also
covers work those operations generate. An unchanged query, a timer that only
repaints, and old retained flags do not advance the generation. Audio lifecycle
completion wakes child main; regular audio blocks only need that additional
wake when serving a pending flush. Process outputs already returned through
the shared arena remain the parent's audio owner's responsibility.

Child main writes its outward eventfd once for new publications, then latches
until ACK. ACK acknowledges all publications captured before it and rearms;
it does not consume the snapshots. The parent's parked process watcher drains
the outward fd, publishes a pending delivery and calls subscribed app relays'
notify(). Before pulling snapshots, parent main ACKs the drained delivery and
refreshes every registered member, including retired/editor instances. Results
remain in proxy caches until the existing per-instance consumers drain them.
A publication during/after ACK or during those pulls creates another event.
Rescans supersede older clears; clear accumulation is bounded at 4096. Idle
snapshots and proxy output caches each hold 1024 FIFO events, drop newest on
overflow, and retain the accumulated drop count. Editor resize requests retain
the latest dimensions while closed/show/hide flags coalesce.

Remote subscription requests an initial snapshot, including work or failure
that predates subscription. Subscribed service, idle-output and editor-request
consumers issue no IPC without a new delivery or a pending refresh; unrelated
app wakes only drain retained snapshots. Without a subscription these consumers
continue pulling on call. Remote `nextTimerMs()` returns -1: timers run in the child;
there is no 30 ms editor poll. Editor request reads consume proxy snapshots
rather than querying the child on each UI frame.

The process watcher also watches pidfd and control HUP/ERR without consuming
reply bytes. Death disables those level-ready watches after publication.
Parent-local failure (including invalid process output counts from a still-live
child) publishes atomic state and notifies an internal core-base relay; only
the non-audio watcher forwards to the synchronized subscriber registry. Each
distinct subscribed app relay is notified once per forwarding pass. Removing
an instance synchronizes with forwarding before its relay may be destroyed;
a cached shared process can outlive its last subscriber. Stop/join the watcher
before closing its descriptors; its failure/stop relay outlives that join.
Channel setup failures are actionable errors, never a periodic-poll fallback.

The child's audio thread parks indefinitely on the shared request futex while
idle. Main-thread shutdown publishes quit, advances the request word with
release ordering and wakes it before joining. Audio rechecks quit before
reading cmd. This covers shutdown before/during parking, including control
HUP/EOF, without fabricating a processing command. Parent request/done safety
deadlines and bounded in-flight failure checks are unchanged.

Thread discipline follows CLAP: create/activate/params-info/state/GUI on the
main thread; `process` on one audio thread. The sandbox's audio thread is
simply the render thread (offline), but the RT rules still hold and the
render loop runs under rtsan in the `rtsan` preset.

## 2. The sandbox model

A sandbox session holds:

- **instances** `in_1`, `in_2`, … (loaded plugins with their CLAP state);
- a **chain**: an ordered list of instance ids, the first may be an
  instrument; audio flows through the chain in order;
- **renders** `rd_1`, … (WAV files under the session work dir plus their
  analysis reports, kept for comparison);
- **settings**: sample rate (default 48000), block size (default 256),
  tempo (default 120 BPM), meter (default 4/4), render tail limit (default
  5 s), output format (float32 WAV).

Audio source references are render IDs only when they are `rd_` followed by
one or more decimal digits. Other strings name audio files, including names
such as `rd_reference.wav`.

Work dir: `$XDG_CACHE_HOME/<slug>/sandbox/<session-id>/` (or `--workdir`).
The command log is `commands.ndjson` in it.

## 3. Rendering

`render` builds the input (notation → `notation::compile`, or a MIDI file,
or an audio file resampled to the session rate with the audio-file
resampler), then runs the chain offline:

- activate all chain instances at the session rate with `[1, blockSize]`;
- recheck the negotiated main buses before starting audio: every output and
  each input after the first stage must still be stereo (`render.layout`);
- set the transport (tempo, meter; playing = true; bar/beat positions
  advanced per block);
- process fixed-size blocks (the last block may be short); note events and
  param events land at their exact sample offsets within blocks; each
  block's chain runs with FTZ and DAZ set, as an engine node does (engine
  spec §2.4, ADR-0025), so a chain renders here as it does in a session;
- after the input ends, continue for the tail: until the chain's reported
  tail elapses or output stays below −120 dBFS for 200 ms, capped at the
  tail limit. The reported tail is read again after every block, so a tail
  that changes while the chain processes (a delay learning the render's
  tempo, a parameter adopted on the audio thread) moves the end; a bridged
  plug-in's tail is the one known when the render starts;
- compensate total chain latency by discarding that many leading samples;
- write float32 WAV; analyze if requested.

The C++ `RenderRequest` also accepts `sidechain` (a borrowed
`const audio::AudioData*`, default null), feeding only the first chain stage.
It shares the main input's sample origin and render rate; one channel is
duplicated to stereo, two are used as supplied. A short key is zero-padded
through the remaining input and tail. It does not extend the musical length.
The renderer validates the rate, channel lengths, mono/stereo layout, frame
cap and destination key input before starting audio; errors identify
`/sidechain` (`render.input_rate`, `render.input_layout`, `render.too_long`,
`render.sidechain`). Key activation is set before plugin activation, with
reactivation if it changes. Buffers are allocated on main; the audio loop
only copies samples into them and passes them to the first stage. Other
stages receive no key from this request. The sandbox `render` command does
not expose this pointer; `dsp.test` uses it for its generated key.

**Determinism:** two renders of the same session state and arguments are
bit-identical (test: render twice, compare bytes). Block-size invariance for
stock plugins is tested separately (render at block 1, 64, 256, 1024; null
within −120 dBFS for the stock test synth).

## 4. Commands (all `McpTool`)

| command | kind | args (abridged) | result (abridged) |
|---|---|---|---|
| `plugin.list` | query | `filter?` (text), `rescan?` | `plugins: [{id, name, instrument, semantic, features}]` |
| `plugin.load` | edit | `plugin` (id or path), `position?` (chain index, default: instrument first else end) | `instance`, `name`, `semantic`, `chain` |
| `plugin.remove` | edit | `instance` | `chain` |
| `plugin.describe` | query | `instance`, `detail` (`summary`/`full`) | `params` (key/unit/range/current, summary: grouped compact list of the parameters that apply; full: each schema entry, with `active: false` while the parameter does not apply — its module is not in the patch, or its `active_when` conditions do not hold), `inactive` (keys of those that do not apply, when any), `patch_type`, `patch_schema` (full only), `description` (prose, of what applies), `docs`, `status_available`, `problems` when available (one `host`-scoped entry when the query fails) |
| `param.get` | query | `instance`, `keys?` | `values: {key: {value, text}}` |
| `param.set` | edit | `instance`, `values: {key: number \| "unit string" \| enum key \| bool}` (third-party: CLAP id or name, `{"text": "…"}` allowed) | `values` as applied with text; `active: false` on a value that does not apply with the new values |
| `patch.get` | query | `instance`, `detail` (`changed`/`full`) | `patch` |
| `patch.set` | edit | `instance`, `patch` (complete) | `description` |
| `patch.merge` | edit | `instance`, `patch` (RFC 7396 merge patch) | `changed` (the effective diff), `description`, `inactive` (keys its `params` set that do not apply with the new values, when any) |
| `preset.save` | action | `instance`, `name`, `tags?`, `notes?` | `path` |
| `preset.load` | edit | `instance`, `name`, `source?` (`user` default / `factory`) | `description` |
| `preset.list` | query | `plugin?`, `source?` (`user` default / `factory`) | `presets: [{plugin, name, tags, read_only?}]` |
| `render` | action | one of `notation`, `midi_file`, `audio_file`; `bars?`/`seconds?`; `tempo?`; `tail_s?`; `name?`; `analyze?` (`none`/`summary`/`standard`/`full`) | `render` (id), `path`, `seconds`, `peak_dbfs`, `analysis?` |
| `analyze` | query | `render` or `file`, `detail`, `note_off_s?` | `report` |
| `compare` | query | `reference` (render/file), `test` (render/file), `reference_note_off_s?`, `test_note_off_s?` | `deltas` (notable first), `ltas_rms_db`, `summary` (prose), `loss` (the optimizer's score: `total`, `terms`, `note_off_s`) |
| `masking` | query | `masker`, `maskee` (render/file) | masking report |
| `profile.evaluate` | query | `render`, `profile` (inline document) | per-target results, `loss` |
| `session.get` | query | — | settings, chain, instances, renders |
| `session.set` | edit | `sample_rate?`, `block_size?`, `tempo?`, `meter?`, `tail_limit_s?` | settings |
| `session.reset` | action | — | — |
| `dsp.write`, `dsp.edit`, `dsp.check`, `dsp.get`, `dsp.list` | actions, queries | as in the engine (ADR-0032): the session's script library, `SessionOptions::scriptDir` (`<workdir>/scripts`; the CLI without `--workdir` uses the app's, `<data>/<slug>/scripts`) | diagnostics with hints, the plugin's id, parameters and files (keys for a patch's `properties`) |
| `dsp.export` | action | `script` (name); `format?` (`clap`, the default, or `source`); `folder?` (absolute; default `<library>/<script>/export`, for source `export/<script>-source`, which must be new or empty); `license?` (source: `MIT`, the default, or `GPL-3.0-or-later`) | `path`, `id`, `name`, `format`. clap: `path` is `<folder>/<plugin name>.clap`, the platform player copied with the script embedded (a Linux/Windows file trailer or macOS bundle resource; it offers that one plugin), `player` (the module copied), `bytes`, `notices` (the player's licenses, written beside it). source: `path` is a project of the plugin template (the framework, the suite without its compiler, the script under `product/script/`, the product's files) that builds the same plugin |
| `dsp.preview` | action | `plugin` (id) or `script` (name); `patch?` (merge); `signal?` (an effect's `sine`, the default, `sweep`, `impulse`, `noise`, `silence`, or `file`, the default with `audio_file`; an instrument's `notes`, the default, `note`, `silence`); `audio_file?` (the absolute path of a WAV, FLAC or MP3 the `file` signal plays, as `dsp.test` does); `level_db` (-12); `frequency_hz` (1000); `seconds` (1; 0 plays nothing); `scale` (1, 0.5 to 3); `pointer?` (up to 64 steps before the picture, in its pixels at scale 1: `{action, x, y}` with `action` `move`, `press`, `release`, `click`, `double_click`, `drag` (with `to_x`, `to_y`) or `wheel` (`steps`, 1; up positive), and `shift`, `ctrl`, `alt`) | `path` (a PNG of the editor, drawn without a window after the signal played through a fresh instance and the pointer acted, in the renders folder), `width` and `height` in pixels, `plugin`, `edits` (a merge patch: what the pointer changed), `problems` (the editor code's loop budget or out-of-range index, with the line), `log`. Needs the plugin's `com.suppleaudio.editor-picture/1` (`docs/specs/editor-picture-extension.md`), which the script player has |
| `dsp.test` | action | `plugin` (id) or `script` (name); `patch?` (merge); `signals?` (an effect's `sine`, `sweep`, `impulse`, `noise`, `silence`, all by default, and `file`, the only one by default with `audio_file`; an instrument's `notes`, `note`, `silence`, all by default); `audio_file?` (the absolute path of a WAV, FLAC or MP3 of 10 minutes at most: resampled to the render's rate, mono on both channels, of more channels the first two, its first 60 s); `level_db` (each generated signal's peak, -12); `frequency_hz` (1000); `seconds` (1; a file plays its own length, while `automation` and a `sidechain` key stay within `seconds`); `sample_rate?` (Hz, 8000 to 384000; the session's by default); `save`; `trace_points` (1 to 4096, 100); `automation?`, `sidechain?`, `notes?` (below) | per signal: `peak_dbfs`, `rms_dbfs`, `dc`, `clipped`, `denormals`, sine `gain_db` and `thd_percent`, impulse `response_db` at octave centres, note `pitch_hz`, an instrument's `release_ms`, `path` when saved, optional `automation` and `sidechain` measurements (below), a semantic plugin's `meters` (below), and what a script traced (`traces`: `name`, `last`, `low`, `high` per interval of `trace_interval_s`, `non_finite`; `trace-extension.md`); `sample_rate` (the rate rendered at), `latency` (samples at that rate), `cpu_percent` (measured), `problems` (prose), `log` |

Source exports carry the MIT core-base floating-point parser and the pinned
fast_float headers/license used by the suite's semantic layer. They build
offline without linking the rest of the monorepo. Player and export notices
include fast_float's selected MIT license.

A CLAP `dsp.export` requires the source player's license and notices (or an
existing export's combined notices) to be readable before it writes the
plugin. Missing files fail with `dsp.notices` at `/script`, with a hint to
restore the files. Every successful CLAP export returns its `notices` path.

The script player's `dsp.preview` pictures use EGL on Linux/BSD, headless
Metal on macOS and WGL on a never-shown window on Windows. An editor build
and an available renderer are required; Windows needs OpenGL framebuffer
objects. Missing devices/drivers return `dsp.no_picture` with the renderer's
reason and a platform-specific corrective hint. Pictures return top-row-first
RGBA through the extension; repeated draws of the same state are identical
on the same rendering setup. Native editor windows are independent.

`dsp.test` and `dsp.preview` locate patch validation errors beneath
`/patch` (the command boundary adds `/args`), for example
`/args/patch/params/gain`, preserving the plugin's code and corrective hint.

Saved WAV/PNG artifact names use up to 64 bytes of the plugin ID, retaining
ASCII letters, digits, `.`, `_` and `-`, and replacing other bytes with `_`.
An ID stays unchanged only if it is nonempty, safe, at most 64 bytes, and
does not end in `-` followed by exactly 16 hexadecimal digits (either case).
All other IDs gain `-` and the 16 lowercase hexadecimal digits of their full
UTF-8 ID's FNV-1a 64-bit hash; reserving that suffix keeps raw IDs distinct
from encoded names. Stock plugin IDs keep their existing names. The usual
`dsp-test-`/`dsp-preview-` prefix and signal/extension suffix remain.
The result's `plugin` field keeps
the full ID; callers use the returned `path` to find the artifact.

`dsp.edit {name, edits: [{old, new}]}` changes part of a library script:
each edit replaces `old`, which must be in the script exactly once, with
`new`, in order, each on the script as the edits before left it. All or
none: an `old` that is nowhere or in several places is `dsp.edit` at
`/edits/<i>/old`, nothing saved, and its message gives the lines of the
places, or where the longest start of `old` that is in one place parts from
the script. A script with Windows line ends (`\r\n`) takes `old` and `new`
written with plain ones, and keeps its own. Then it saves, compiles and
answers as `dsp.write`.

The player offers one plugin per ID, so a library holds one script per ID:
`dsp.write` refuses, without saving, a script whose `Info.id` another script
in the library has (`dsp.id_taken` at `/source`; `dsp.edit` at `/edits`), and `dsp.test`,
`dsp.preview` and `dsp.export` by `script` refuse a name that shares its ID
with another (`dsp.id_taken` at `/script`). A source export's host tests
move a parameter they find by name through CMake, so `dsp.no_parameter`
also covers a script whose automatable parameters' names all hold `"`,
`\`, `$`, `;` or `@`.

### `dsp.test`: changes during a render

All previous arguments/defaults remain. These optional inputs apply to each
selected signal, including silence, with a fresh instance and the initial
patch for every primary/reference pass:

- `automation`: 1..1024 `{param, at_s, value}` entries, all fields required.
  `param` is a semantic patch key; `value` uses physical-unit numbers, enum
  keys, booleans or unit-bearing text as accepted by core-params. Resolve
  through `fromDescription`/`fromJson`/`toClap` before audio starts. Require
  the semantic extension, an active, writable, automatable parameter and a
  matching live parameter ID/flags. Values are validated without clamping.
  `at_s >= 0` must round to a sample before musical end. Discrete events
  hold their value until the next point; no ramp is inferred. Same-time
  parameter changes retain caller order.
- `sidechain`: `{signal?, level_db?, frequency_hz?, start_s?, length_s?}`.
  `signal` is sine (default), sweep, impulse, noise or silence. Level is
  -60..0 dB (default -6), frequency 10..20000 Hz (default 100). Start
  defaults to `0.25 * seconds`, length to `0.5 * seconds`, independently.
  A nonempty key span must fit in the musical render. The generated stereo
  signal starts at its own phase origin; a sweep spans its own length;
  outside the span the connected key is silent. Reject a plugin without
  a key input.
- `notes`: instrument-only, 1..1024 `{key, velocity?, start_s, length_s}`.
  Key is integer 0..127; velocity is (0,1], default 0.8. Notes must start
  inside the musical render and end at or before it; reject zero samples
  after rounding. Overlapping notes have distinct note IDs, on port and
  channel 0. This replaces the `notes` stimulus; defaults become `notes`
  and `silence`. An explicit `note` (the A4 check), or selecting only
  silence with custom notes, is an error. At equal sample times, parameter
  events precede note-offs, which precede note-ons.

Times convert once using `llround(time * sample_rate)`. Span ends use
`llround((start_s + length_s) * sample_rate)`. Errors carry code, message,
JSON Pointer and corrective hint: `dsp.automation`, `dsp.sidechain`,
`dsp.notes`, or the core-params value error rebased to `/automation/i/value`.
The command boundary prefixes runtime paths with `/args`; schema errors
appear in its `/args/...` details.

Per-signal additions:

- `automation: {max_step, baseline_max_step, excess_step}`: the largest
  absolute difference between adjacent samples over all output channels,
  including block boundaries, in linear full-scale amplitude. Baseline is
  the same stimulus/patch/notes/key without automation; excess is
  `max(0, max_step - baseline_max_step)`. A step above both four times
  baseline and 0.05 produces a prose warning about possible clicks/zipper
  noise and parameter smoothing. This is a cheap screening heuristic:
  intended timbre/level changes can increase it; small values do not prove
  inaudibility. Existing clipping/DC/nonfinite/loop/tail diagnostics also
  run on reference outputs, labeled by pass.
- `meters: {key: value}`: a semantic plugin's read-only parameters (its
  meters and readouts) by patch key, in physical units (floats to four
  decimals), each as the plugin last reported it in a block that started
  before the musical end: the tail after the signal leaves them as they
  were. A meter never reported keeps its value from before the render.
  Omitted for plugins without the semantic-params extension.
- `sidechain: {rms_dbfs, reference_rms_dbfs, gain_change_db}`: compare
  output RMS in the middle half of the active key span against the same
  samples with a connected silent key, keeping automation and notes.
  Fractional sample boundaries round outward (floor at the beginning,
  ceiling at the end); outputs are latency-compensated. RMS uses the
  existing finite -200 dBFS floor. Gain change is active minus reference
  in dB, negative for ducking; it is null if either output is below
  -120 dBFS. This measures that window, not guaranteed steady state for
  slow attack. No required reduction threshold is imposed on keyed effects.

Custom `release_ms` starts at the sequence's actual last note-off, without
an A4 pitch/tuning measurement. Static THD and impulse response are omitted
with automation or a non-silent key; sine `gain_db` then describes the
changing render, not static gain. Only primary output is saved. There are
two passes with automation or a key, at most three with both; `cpu_percent`
includes the render time and audio duration of all passes.

Plus `batch` and `registry.*` from the bus. Every command has at least one
example running against fixtures (`empty`, `synth_loaded` with the probe
synth, `synth_and_render`).

Transactions: the sandbox implements `beginTransaction` by snapshotting the
state of affected instances (CLAP state + chain) and restores them on
rollback. Begin fails with `sandbox.snapshot_failed` at `/instances/<id>`
when any state cannot be saved; its `details` retain the plug-in error.
Rollback returns `sandbox.rollback_failed` with `sandbox.restore_failed`
details for each instance and nested plug-in errors. The command bus retains
the initiating command/commit error as a final detail when rollback fails.
Renders created inside a failed batch are deleted. Invocation savepoints are
independent of that outer snapshot; a failed step restores its plug-in states,
chain, settings, render set and pending Lua/host events while the transaction
remains open. The Result-returning invocation hook refuses the command when
capture fails. Restore failure is attached to the command error and aborts the
outer transaction even if Lua catches the call error. Snapshot/restore failures
also emit `sandbox.savepoint_failed` with the same structured error fields.

Preset documents accept `supple.preset/1.0` (optional numeric `version: 1`).
An absent schema is legacy; unknown/newer schemas or explicit versions fail
with `sandbox.preset_schema` or `sandbox.preset_version` before state changes.

Factory listing requires `plugin` and reads the semantic-params factory
patches (from a loaded instance of that ID, or a temporary instance).
Entries retain declaration order and have `read_only: true`. Factory loads
use case-sensitive names and apply the canonical patch with REPLACE
semantics, including defaults for omitted parameters. No user file is read
or written. `preset.save` still writes user presets only, so a user copy
may share a factory name; `source` disambiguates them. User listing/loading
and its file-name restrictions are unchanged. Unknown factory names use
`sandbox.unknown_preset` at `/name` with an available-name hint; missing
`plugin` for factory listing uses `sandbox.preset_plugin` at `/plugin`.
Semantic schema entries require a numeric id, name, recognized type and the
range/choices for that type; their ID set must match `clap.params`. Invalid
schemas warn and fall back to plain CLAP parameters. `param.get` and
`plugin.describe` return `sandbox.plugin_contract` with a value path and a
restart/replace hint when no current value is available. State loads invalidate
queued pre-restore edits; inactive saves flush acknowledged pending edits
before writing plug-in state.

## 5. CLI

```
<slug>-sandbox --mcp [--workdir DIR] [--plugin-path P]...   # MCP over stdio
<slug>-sandbox --socket [PATH]                               # control protocol server
<slug>-sandbox exec '<json invocation or batch>'             # one-shot, prints result JSON
<slug>-sandbox registry [--markdown | --mcp-tools]           # generator output (used by drift tests)
```

## 6. Test instrument

Tests take the smallest instrument that proves their point:

- the fixture plugins in `host/clap-host/tests/fixture_plugins.cpp`
  (`com.suppleaudio.test.sine` and others), each made for the behaviour a
  host test needs;
- the script player's test instruments (Pluck) and the example scripts
  (`plugins/suite/examples`) for DSP scripts;
- the stock synth, Marram (`plugins/clap/Marram.clap` in the build), where a
  real synth is needed: the MCP demo, the optimizer tests and the evaluation
  tasks. (Orrery's headless build served until Orrery was removed,
  2026-10-21.)

(Until 2026-10-20 an internal template-derived instrument,
`plugins/probe-synth`, also existed to exercise the template, the extension
and the MCP loop before Orrery; nothing loaded it by then and it was
removed.)

## 7. Acceptance

A scripted end-to-end test drives the sandbox over MCP stdio: list plugins
→ load the stock synth → describe → set params with unit strings → merge a
patch → render a notation phrase with summary analysis → change the patch →
render again → compare → optimize. Its transcript is committed as
`docs/demos/mcp-synth-session.md` (the `mcp.demo` test). A second test runs the same flow with an
invalid patch and an invalid notation string and asserts the errors carry
paths, locations and hints.

The slow `plugin.export.source` test builds fresh source exports for every
script in `plugins/suite/examples` on Linux, macOS and Windows, plus the library tilt regression with
its non-default patch. Each export passes the semantic conformance tool,
then renders the same explicit signal list as the player. Each export
also passes its own template test suite (the sampler without host-smoke,
whose instrument render rejects a sampler silent with no sample loaded).
The test checks signal names/counts, WAV frame counts and byte equality.
Sampler/reverb receive generated audio files with audible patches; the
compressor receives a timed key, and instruments receive custom notes.
Factory patches must match: the player, binary export and built source project
list the same names in the same order on every platform, and each applies the same
full patch; tilt's must be Flat, Warm, Air, with Warm at tilt -3 dB,
pivot 800 Hz, output 0 dB.

## Implementation notes — `host/clap-host` (2026-09-26)

- Output events go to a caller-preallocated `OutputEvents` sink (fixed
  storage + count + dropped), never a growing container, so `process()` is
  allocation-free; it is `SUPPLE_RT` and exercised under rtsan.
  `OutputEvent::flags` preserves CLAP event flags, including `DontRecord`
  (do not record automation) and `IsLive`. Note-end events retain their input
  note address `(port, channel, key, noteId)`; -1 is an unspecified/wildcard
  component, so a note id of -1 can be matched using the other components.
  Parameter values also retain their target address. VST3 output parameter
  values are normalized, with zero flags and address components of -1.
  Native VST3 note-off outputs are not synthesized as CLAP note-end events.
  CLAP output events are rejected when their header or supported event body is
  undersized.
  After a bridge process acknowledgement, the client snapshots the output-event
  count once and validates it against the room it passed (below). An oversized
  count, or an event kind past `Midi`, fails the bridge and silences
  main/auxiliary output before any output copy; `failed()` and structured
  failure queries expose the protocol violation. Traversal of a valid response
  uses the bounded snapshot.
- **Note and MIDI output (ADR-0068).** `OutputEvent::Kind` appends `NoteOn`,
  `NoteOff`, `NoteChoke` and `Midi` (`midiOutput()` is true for these). Notes
  carry the output note address `(port, channel, key, noteId)` (an on has a
  concrete channel 0..15 and key 0..127; an off or a choke may use -1) and the
  velocity 0..1 in `value` (0 for a choke). `Midi` carries one MIDI 1.0 short
  message in `midi[3]`, status first and unused data bytes zero, with its
  `port`. The struct stays 40 bytes: `midi` sits in the padding after `kind`.
  Sources: CLAP `NOTE_ON/OFF/CHOKE` (notes); `CLAP_EVENT_MIDI` short messages;
  `CLAP_EVENT_MIDI2` UMP types 1 and 2 exactly and type 4 down-converted (note
  on/off, velocity 16 → 7 bits and a note on never 0; poly and channel
  pressure; CC 32 → 7 bits; pitch bend 32 → 14 bits; program change without
  bank); a `CLAP_NOTE_EXPRESSION_PRESSURE` on a concrete channel and key, as
  poly pressure (0..1 → 0..127, rounded); VST3 note on/off (notes), poly
  pressure and `kLegacyMIDICCOutEvent` (CC, aftertouch, pitch bend LSB/MSB,
  program change, poly pressure, quarter frame, song select and position, tune
  request, start/continue/stop, active sensing), as `Midi`. Notes stay notes and
  MIDI stays MIDI. Not representable and counted in
  `OutputEvents::unsupported`: SysEx (CLAP `MIDI_SYSEX`, VST3 data events),
  other note expressions (all VST3 ones), other MIDI 2.0 messages, VST3
  chord/scale events and cable select, and malformed events (a data byte at or
  above 0x80, an undefined status, a note on without a concrete channel and
  key, a non-finite velocity or value). Note and MIDI times are clamped to
  0..frames-1. Each `process()` sorts the events it appended by time, stably,
  with an allocation-free insertion sort, for every backing; parameter output
  times are left as reported. `Instance::outputsNotes()` reports a CLAP output
  note port (`note-ports`, `is_input = false`) or a VST3 event output bus,
  which `activate()` now activates; the VST3 output event list (2048) counts
  the events it refuses in `dropped`. A `params.flush` has no block: note and
  MIDI output there is dropped and counted in the idle drain's `dropped`, and
  `TakeIdleOutputs` replies still reject kinds past `NoteEnd`.
  Bridged instances run the conversion in the child. Since POSIX v20 / Windows
  v21 the Create reply ends its fixed fields with `u8 outputsNotes` (after
  `u8 editor`, before the auxiliary ports), and the Header carries `outSink`
  (the host passed a sink), `outEventCapacity` (its room, at most
  `kMaxOutEvents` = 1024) and `outEventsUnsupported`. The child's sink has
  exactly that room, so the plug-in sees the same pushes accepted and refused
  as in process: events, `dropped` and `unsupported` are bit-identical in
  process, in a shared bridge and in a dedicated one whenever the room is at
  most 1024 (tested in `tests/test_note_output.cpp`, with RTSan on the process
  path). The engine does not publish note or MIDI output to its main-thread
  plugin-output queue.
- `takeIdleOutputs()` drains CLAP `params.flush` output gestures/values on the
  main thread, local or bridged, including active-but-stopped audio-thread
  flushes. The instance retains a FIFO of 1024 events independently of the
  process sink; overflow drops newest and the returned `dropped` count is
  cleared on drain. Returned events have time 0 and the same flags/address
  form as process outputs. A drain is bounded by the queue's captured size;
  concurrent flush outputs not in that capture wait for the next drain.
  No allocation or lock occurs in the flush output callback. The returned
  vector allocates only on main. This is a CLAP flush channel; the VST3
  backing does not expose CLAP idle outputs. A failed bridge returns an empty
  drain and remains observable through `failed()`.
- `setParam()` flushes immediately through `params.flush` while the plugin is
  inactive (main thread); while active it queues into an SPSC ring delivered
  at time 0 of the next `process()`, or by `flushParams()` on the audio thread
  (CLAP requires the audio thread for flush while active).
- `serviceMainThread()` answers `request_callback`, performs `request_restart`
  (deactivate + activate with the same rate and max block) when not
  processing, handles `request_flush` while inactive, and fires due timers.
  `takeNotifications()` reports state-dirty, params-rescan, latency-changed
  and semantic-changed flags.
- Plugin log calls on the audio thread go to a lock-free ring (copied by
  hand, no formatting calls); other threads use a mutex-protected list;
  `takeLog()` drains both on the main thread.
- `scan()` loads each binary once (a path-keyed library cache shared with
  instances keeps each `.clap` loaded exactly while needed) and instantiates
  each plugin briefly to detect the semantic extension.
- Instances select a stereo `audio-ports-config` when offered; notes use the
  CLAP dialect unless the plugin only supports MIDI.
- The semantic-params ABI header lives at
  `plugins/common/include/supple/clap/semantic_params.h` (exported as the
  CMake target `supple::semantic-params-abi`).
- Hermetic tests use in-tree fixture plugins (`host/clap-host/tests/
  fixture_plugins.cpp`): a gain effect with the semantic extension,
  restartable latency and JSON state, a sine instrument, and a note echo
  with output note ports (its VST3 twin is in `fixture_vst3.cpp`). The host was also
  checked by hand against the template's `Starter.clap` (scan, text I/O,
  flush, processing, state).

## Implementation notes — review round 2 (2026-09-26)

- **Lifecycle is enforced:** `startProcessing()` returns false when the plugin
  refuses (then `process()` returns false); `deactivate()` while processing is
  ignored and logged; destroying a processing instance stops it first (caller
  bug, recovered). `request_restart` raised while processing stays pending
  until processing stops (`restartPending()` says so, also for a bridged
  instance and a VST3 `kLatencyChanged`, which VST3 answers with a
  deactivate/activate; one reported during activation is only a notification); `request_flush` while active is served by the next
  `process()`/`flushParams()` on the audio thread (`needsAudioFlush()`).
- **Event arena:** queued `setParam()` changes only take the slots left after
  the block's own events; the rest stay queued for the next block. Block
  events beyond the arena are dropped and counted
  (`Notifications::droppedEvents`). MIDI-only note ports get a choke as a
  note-off.
- **Tail policy:** when every chain stage implements `clap.tail` with a finite
  value, the render stops exactly at musical end + latency + the sum of the
  reported tails (or earlier on the silence hold); an unreported or infinite
  tail leaves it to silence detection and the cap. `Notifications::tailChanged`
  reports `clap.tail.changed`.
- **Render limits:** at most 2^25 frames per render (~11.6 min at 48 kHz),
  checked before allocating; negative or out-of-range event times, non-finite
  or out-of-range tail/silence settings are errors (`render.too_long`,
  `render.event_time`, `render.tail`, `render.silence`). An explicit
  `lengthSamples` is authoritative; events past it land in the tail.
- **Session** (`session.h`): the model behind the §4 commands, independent of
  the bus. Third-party plugins' params are addressed by CLAP id or name with
  CLAP values or `{"text": …}`; semantic plugins' params go through
  `set_patch` merges so the plugin validates units and ranges (error paths are
  rebased from `/params/…` to `/values/…`). Presets live in
  `<presetDir>/<plugin folder>/<name>.json`, the folder named as the DAW's
  (session-model.md: the safe ID and a hash of it; the older bare folder is
  still read when the preset there is the plugin's). IDs are never reused, even after a
  rollback.


### Catalogue diagnostics (2026-10-04)

`plugin.list` always returns `scan` beside `plugins`, including when no plug-ins
are available or a filter matches none. `scan.failures` contains `{module,
reason: {code, message, path, hint, details?}, cached}`; `scan.warnings` carries
structured errors and `scan.cached_modules` counts reused module results.
The report belongs to this catalogue and survives unrelated host scans;
`rescan: true` replaces both the catalogue and its report.

## Headless platform roots (2026-10-14, ADR-0035)

The CLI work directory uses paths::cache() and the script library paths::data(),
with the generated product slug appended. Linux retains XDG/HOME fallbacks.
macOS uses Library/Caches and Library/Application Support; Windows LOCALAPPDATA
and APPDATA. Socket and MCP service use the portable control layer; plugin hosting,
scanning, editor embedding and the bridge remain separate port steps.

## Native hosting and scanner (2026-10-14, ADR-0036)

CLAP loads native Windows DLLs and macOS bundles (Contents/Info.plist names
Contents/MacOS's executable). Entry init receives the canonical bundle path,
UTF-8 on all platforms. VST3 uses the pinned SDK's native bundle loader:
Contents/MacOS, Contents/x86_64-win or Contents/arm64-win, and the existing
Linux layout. CLAP retains scoped library ownership; VST3 modules remain
loaded for the process lifetime. Discovery stops recursion at either format's
bundle directory. Cache fingerprints cover binary and metadata changes within
bundles; script players, including bundled players, remain uncached.

CLAP_PATH and VST3_PATH precede standard locations, with `;` separators on
Windows and `:` on POSIX. Standard locations are ~/.clap and ~/.vst3 plus
/usr/{,local/}lib/{clap,vst3} on Linux; ~/Library/Audio/Plug-Ins/{CLAP,VST3}
and /Library/Audio/Plug-Ins/{CLAP,VST3} on macOS; %COMMONPROGRAMFILES%/{CLAP,VST3}
and %LOCALAPPDATA%/Programs/Common/{CLAP,VST3} on Windows. Empty environment
entries are ignored. Scan cache roots use paths::cache() plus the product slug.

The standalone `<slug>-scanner --scan <module>` speaks the existing canonical
scan JSON schema and keeps plugin stdout off the reply stream. Linux retains
the bridge's existing scan mode by default; other platforms locate the scanner
in the same build-tree/sibling locations. Windows executable discovery appends
`.exe` to the bridge/scanner name. ScanOptions::bridgeExecutable can
explicitly select either executable. Missing-helper messages and hints name
the bridge on Linux and the standalone scanner on other platforms; the
compatibility error path remains `/bridge`. Missing scanner behavior and supervised
background requirements are unchanged. ChildProcess::spawn(executable, args)
uses native process creation with redirected stdout and null stdin/stderr.
Windows `cmd.exe` switches stay bare because cmd parses its own switches.
Its single batch-file path after `/c` or `/k` gets inner path quotes and an
outer pair for `/s` to strip; other executables retain CRT quoting. Help uses
this path for npm-installed agent CLIs.
collect(steady_clock deadline, maxBytes) returns {text, exitCode, crashed};
errors are child.spawn/io/wait/timeout/output_limit with path and hint. Scan
rebases those error codes to host.scan.*. Destruction kills descendants and
reaps the child; killTree() explicitly cancels it. Spawn, capture and destruction
are non-RT; the bridge sanctions only the nonblocking timeout kill as described
above.

Isolated Instance::create and createBridgeProcess use the native bridge on
Linux, macOS and Windows; shared and dedicated crash scopes remain identical.
Native editors use the typed parent and platform contract below (ADR-0040).
The engine, sandbox and offline renderer run on macOS and Windows through
native audio-file I/O (ADR-0038). Sandbox rendering accepts isolated host
instances and retains exact local/bridged output equivalence on each platform.
The sandbox session's plugin.load command continues to load in process. Native
device backends remain separate from bridge isolation.

Binary CLAP exports run on Linux, Windows and macOS. Linux and Windows copy
only the original player bytes (stripping a previous export's trailer), then
append the unchanged `supple.script-export/1.0` JSON, little-endian uint64 length
and `SPLSCRPT` magic (`supple/script_export.hpp`). Windows paths are UTF-8 at
command boundaries and native wide paths for module/file operations; unsigned
DLL overlays are supported, Authenticode signing is outside this contract.

macOS exports copy the complete `.clap` bundle, resolve its executable through
CFBundleExecutable, and store that same JSON without a footer in
`Contents/Resources/supple-script.json` (at most 8 MiB). The player locates its own
Mach-O with dladdr, then reads this sibling resource before consulting a library.
No script bytes are appended to the Mach-O. Every copied bundle is signed ad hoc
with `/usr/bin/codesign --force --sign - --timestamp=none`, even if the original
signature did not seal resources. Missing/failed signing returns `dsp.sign` at
`/folder` with a recovery hint. Signing happens in an adjacent staging directory
before replacing a previous export; failed signing removes staging and preserves
the previous export. Publication failure attempts to restore the previous bundle;
if that fails, the error names the retained backup. `bytes` counts the regular
files in the signed bundle (a module file's bytes elsewhere). Required combined
notices still go beside `<name>.clap` as `<name>-NOTICES.txt` on all platforms.

An export of an export replaces its script payload and offers only the newly
embedded definition. No external samples/IRs or current instance patch are copied.
The binary-export command example and lifecycle tests run natively, including
Unicode output paths, generic host and bridge loading. Each of the thirteen
export cases checks factory patches, standalone CLAP semantic conformance and
exact same-platform audio against both library player and built source; macOS
also verifies each bundle with `codesign --verify --strict`.

Install into a platform CLAP directory or a folder named in CLAP_PATH: `;`
separates Windows entries and `:` separates macOS/Linux entries. Standard folders
are those listed above; export does not modify CLAP_PATH or install system files.
`dsp.preview` uses the template's native offscreen renderers on all three
platforms; renderer availability, PNG determinism and pointer edits are tested
natively. See `editor-picture-extension.md` for the picture contract.

Script-player source reads require a regular file before opening the bounded
(1 MiB) stream; directories are rejected even where ifstream reports an empty
EOF.

The real-synth MCP demo runs its raw structured-result assertions through stdio
and proxy on native systems. Its exact numeric documentation transcript remains
a Linux drift check: DSP is deterministic per platform, not bit-identical
between platforms (ADR-0034). Portable mode retains an informational Linux
reference diff in the CTest log for review.

Source-export integration inherits the native compiler/toolchain and macOS
architecture/deployment target, uses only vendored dependencies (including Metal
and AudioUnit SDK on macOS), and supplies CI software GL beside exported Windows
test executables. Nested builds/tests stay within two jobs on macOS, three
elsewhere, and run serially relative to the outer suite. Source and binary exports both participate in native conformance and exact
same-platform player audio comparisons.
