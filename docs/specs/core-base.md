# core-base: main-thread wake relay

**Decisions:** ADR-0029, ADR-0035. Public header:
`supple/base/main_thread_wake.h`, namespace `supple`.

`MainThreadWake` turns real-time-safe notifications into a native waitable:
Linux eventfd, macOS nonblocking pipe, Windows manual-reset event HANDLE. It owns one parked relay thread. It is noncopyable;
construction
and destruction run off audio. Every producer must stop or join before the
relay is destroyed. Destruction wakes and joins the relay, then closes its
descriptor.

- `waitable()` returns a borrowed `Waitable`; check `valid()` before subscribing
  producers. It outlives all watchers. `waitFor(span<WaitItem>, timeoutMs)`
  snapshots readiness and returns the number of ready entries, including
  duplicate sources. Native registrations are deduplicated on all platforms
  and readiness is delivered to every interested watch. Negative timeout waits
  indefinitely. Readiness flags
  `kWaitRead/Write/Error/Hangup/Invalid` match POSIX poll bits. Windows accepts
  manual-reset event handles (read readiness), at most 64 unique handles;
  socket-event registration/reset is the owner's responsibility. Its descriptors
  are rejected with `kWaitInvalid`. Invalid default sources are ignored.
- `fd()` is POSIX compatibility and returns the owned, nonblocking, close-on-exec descriptor
  (eventfd or pipe), or -1 if
  creation failed or the source is a Windows handle. The caller checks this before subscribing
  producers or waiting. Callers borrow it for polling; they neither close it
  nor read it directly in normal service.
- `notify()` is `SUPPLE_RT`, callable by concurrent producers. Publish work
  before calling it. Notifications coalesce until main drains: the first
  producer sets a pending bit, advances a private futex sequence and calls
  the sanctioned `rt::wake`. Later producers still publish through the
  pending bit, but do not wake the relay again. Only the non-RT relay signals
  the native event; producers never write descriptors, allocate, lock or wait.
- `drain()` is non-RT, called by one main thread **before** consuming the
  associated queues or flags. It returns false when no event was read. On
  success it acquires coalesced publications, clears the pending bit and
  returns true. A request arriving during subsequent service signals
  another pass. Interrupted reads are retried. Do not clear/re-arm by
  separately reading the descriptor.

The notification stores no application work. Queues and flags retain their
own capacity, ordering and lifetime contracts. A wake does not mean the
producer's work is complete or authorize reclaiming audio-owned objects.
There is no periodic relay timeout.

The bridge uses this shared relay through optional
`host::Instance::setMainThreadWake(MainThreadWake&)` subscriptions (sandbox
spec §1). EngineApp also subscribes its local instances (ADR-0030); the relay
outlives producers and their worker joins.

## Native events and address waits

`WakeEvent` owns the cold-path event underlying the relay. `notify()` may
coalesce; `drain()` removes all current readiness, including a pipe burst. Drain **before** consuming published work,
so a notification racing reset cannot strand work. It is never called by audio
producers; those use `MainThreadWake`. POSIX notify is signal-safe.
`rt::wait/waitUntil/wake` operate on one process-local lock-free 32-bit atomic
with one waiter. Publish with release before waking; predicates are acquired and
rechecked after spurious returns. Expired deadlines still observe publication
before reporting false. These primitives cannot serve cross-process Windows IPC.

## Thread setup

`ThreadPolicy` retains `None/Normal/Fifo/RoundRobin` and adds `High` (`"high"`).
FIFO/RR and their existing rlimit checks are Linux-only. Windows High uses
THREAD_PRIORITY_HIGHEST; macOS High uses user-interactive QoS and can only be
applied by the owning thread. Neither implies `realtime()`. Normal resets native
priority. Errors retain actionable `thread.policy`/`thread.rttime` diagnostics.
Names retain the 15-byte limit. `availableCpus()` respects affinity with a
hardware-count fallback; `allowedCpuCount()` returns zero if Linux affinity
cannot be read, preserving conservative live-helper admission. `realtimePriorityLimit`
clamps to Linux RLIMIT_RTPRIO and leaves other platforms unchanged.

Page faults (ADR-0084): `currentThreadHandle()` is the calling thread's
native handle without a system call (`pthread_self()`; Windows'
pseudo handle), so an audio path may record it. `kernelThreadId(handle)` is
a live thread's tid on Linux, decoded from its CPU clock id
(`pthread_getcpuclockid`; glibc and musl encode `(~tid << 3) | 6`), and 0
elsewhere. `threadPageFaults(tid)` reads `minflt`/`majflt` from
`/proc/self/task/<tid>/stat` (nullopt for an id no thread has, which an ended
thread's becomes only once the kernel releases it, possibly after its join
returns, and which a later thread may then reuse; and always on
macOS and Windows, which keep no per-thread count). `processPageFaults()` is
the process's: `getrusage` on Linux and macOS, `GetProcessMemoryInfo` on
Windows with `major` -1, since it does not separate them. None of these but
the first may run on an audio path.

## Directory roots

Header `supple/base/paths.h`, namespace `supple::paths`, functions returning
filesystem paths without a product slug or directory creation:

| Root | Linux | macOS | Windows |
|---|---|---|---|
| home | HOME | HOME | USERPROFILE |
| config | XDG_CONFIG_HOME, else HOME/.config | HOME/Library/Application Support | APPDATA |
| data | XDG_DATA_HOME, else HOME/.local/share | HOME/Library/Application Support | APPDATA |
| cache | XDG_CACHE_HOME, else HOME/.cache | HOME/Library/Caches | LOCALAPPDATA |
| runtime | XDG_RUNTIME_DIR, else /run/user/UID when it is a private directory of the user, else /tmp | TMPDIR, else the per-user temporary directory (confstr `_CS_DARWIN_USER_TEMP_DIR`), else native temporary directory | LOCALAPPDATA/Temp, else native temporary directory |

`data/cache(allowTemporary = false)` retain strict roots for GUI callers.
With true, CLI fallback uses a temporary root; Linux keeps the former /tmp
fallback and distinguishes unset HOME from empty HOME (relative .cache and
.local/share). Empty config/data/cache means the user directory cannot be resolved. Empty
runtime means the native temporary directory could not be resolved. Environment
is read off audio before worker startup; callers append the generated slug and
choose any existing component-specific fallback. Windows uses wide environment
APIs to preserve non-ASCII user paths, and every Windows executable runs with
the UTF-8 process code page (ADR-0044), so narrow path text and environment
variables are UTF-8 there too. Linux retains existing relative/empty
XDG behavior; the supervised scanner's stricter absolute-path policy is separate.

## Floating-point parsing

`supple/base/chars.h` declares `supple::fromChars(first, last, value)` for
`float&` and `double&`. Both return `std::from_chars_result` and implement
the default general-format grammar: no leading whitespace or plus sign,
optional minus sign, decimal/scientific notation, and infinity/NaN. The
pointer stops after the matched prefix; an unmatched exponent is not consumed.
Invalid input returns `invalid_argument` with the pointer at `first`;
overflow or underflow returns `result_out_of_range` after the numeric prefix.
Both errors leave the destination unchanged. Parsing is locale-independent
and rounds to nearest, ties to even, using vendored fast_float under MIT on
every platform. Integer parsing remains `std::from_chars`. No additional
format argument or long-double overload is provided.

## Printing errors

`supple/base/result.h` declares `operator<<(std::ostream&, …)` for `Error`
(its `toString()`), `Status` and `Result<T>` (`ok`, or the error). The header
includes only `<iosfwd>`; callers that print include `<ostream>`. doctest
finds the operator, so a failed `REQUIRE(result)` reports the error's code,
path, message and hint. Tests check a result directly rather than
`result.ok()`, which reports only `false`.

## Platform builds

Core-base sources and platform-neutral tests build on Linux, macOS and Windows
(ADR-0035). Tests that inspect Linux memory residency or scheduling limits stay
Linux-only. Hosting/engine/GUI remain separately gated until their port steps
land. File read/write
helpers build on all platforms; Windows atomic writes use native wide paths,
exclusive temporary creation, flushed binary writes and POSIX-style rename through the open native handle.
Native file reads allow delete sharing. Concurrent replacement preserves
the previous file for readers that permit delete sharing and permits
successive writers to replace the same destination. When another program holds
the destination open without delete sharing, the replace retries sharing and
lock violations (and access denied on an existing file) with a short backoff
for about one second (`retryWhileFileBusy`, shared with audio-file and the
engine); a longer hold gives a structured `file.write` error that names the
file, says another program has it open and tells the caller to close it and
save again, leaving the previous file in place.
File diagnostics convert native Windows paths to UTF-8 explicitly, so failed
reads/publications for non-ANSI filenames return Result errors rather than
throwing during path conversion (including recording sidecars).

## Cooperative job cancellation (2026-10-14, ADR-0036)

`base/stop.h` supplies `StopToken`, `JoiningThread` and `StopCallback<F>` for
non-RT work. Where available, token/thread are aliases of std::stop_token and
std::jthread, and the callback wraps std::stop_callback. Apple's system libc++
uses an original fallback, avoiding a second C++ runtime. Host job and offline
render signatures use StopToken; Linux/Windows types and ABI remain unchanged.
Fallback implementation types use `supple::stop_detail`, so importing the vendor
and a component namespace does not introduce an ambiguous `detail` namespace.

JoiningThread accepts a callable, optionally receiving a token, is movable,
and requests cancellation then joins in its destructor and before replacing
an owned thread. join(), joinable(), request_stop() and get_stop_token() follow
those standard operations. A default token cannot stop; stop_requested() is an
acquire observation. A token remains stopped after its source is destroyed;
otherwise stop_possible() becomes false when its source is gone.

StopCallback registration either joins the next stop request or invokes
synchronously if cancellation is already requested. Requests deliver at most
once and are noexcept; a throwing callback terminates. Destroying a registration
waits for an in-flight invocation and prevents any later invocation. Callbacks
may remove themselves or another registration. Invocation happens outside the
state mutex. The fallback snapshots shared slots under that mutex and guards
each invocation with a recursive slot mutex, so self-removal retains the running
callable. Callbacks and tokens may outlive the thread. These operations allocate,
lock and join: never call them from audio. The standalone forced-fallback test
must link only libraries that do not include stop.h, to avoid mixed type definitions.

## Borrowed native UI parent (ADR-0040)

`NativeWindow` carries a Kind (Unavailable, X11, Cocoa, Win32, Floating) and a
uintptr_t handle. X11 is a server XID, Cocoa an NSView pointer, Win32 an HWND.
It conveys no ownership. Floating requests a bridge-owned parent without
transferring a process-local pointer. Its handle is the Cocoa owner's server
`NSWindow.windowNumber` (unsigned 32-bit), or zero when no owner is available.
It is an ordering reference, never an embedded parent pointer. `nativeWindowKind()` identifies the
platform's embedded API; consumers validate kind, lifetime and thread affinity.

Unavailable is tag0 (the bridge wire's none); its spelling avoids Xlib's None macro.
