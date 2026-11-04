# Contributing

Thanks for looking at Steppe. The engineering rules in `AGENTS.md` apply to
every contribution, whether you write it yourself or with a coding agent.
This page covers the process around them.

## Reporting a problem

Open an issue with:

- what you did, what you expected and what happened;
- the Steppe version or commit, your operating system, and your audio setup
  (JACK or PipeWire, Core Audio, WASAPI or ASIO; buffer size, sample rate);
- the smallest project file that shows it, when one does (Steppe projects
  are JSON, with their media in the `media/` folder beside them);
- for crashes, the terminal output and a backtrace if you can get one.

Plugin problems are easier to pin down with the plugin's name, format
(CLAP or VST3), version and isolation mode.

Security problems are the exception: report them privately, as `SECURITY.md`
describes, not in an issue.

## Proposing a change

For anything larger than a fix, open an issue first and describe the
change. Design changes that touch a file format, a command's schema, the
control protocol or a library's public interface need a spec update, and a
change of direction needs an ADR (`docs/decisions/README.md` explains the
format).

A pull request should:

- do one thing, with commits staged from explicit paths and subjects in the
  form `Area: what changed`;
- include tests that fail without the change (fixes) or cover it
  (features);
- update the spec, the generated files (`tools/gen/update-generated.sh`)
  and `STATUS.md` where they are affected;
- pass `tools/ci/run-all.sh` locally; say in the description which presets
  you ran if you could not run them all;
- state what you verified and how, including anything checked by hand in
  the GUI.

Code generated with an agent is welcome on the same terms: you are
responsible for every line, and you must have run what you claim works.

## Licensing of contributions

Contributions use the license of the component they change: GPL-3.0-or-later
for the DAW, MIT for plugins and their shared libraries (ADR-0033).
Contributions must be original: no code copied from GPL or AGPL projects,
and no code or assets from proprietary software (see `AGENTS.md`). The vendored ASIO SDK
is the sanctioned exception for Windows DAW integration.

## Continuous integration

`.github/workflows/build.yml` runs only on demand: use Actions → Build and
test → Run workflow, or
`gh workflow run build.yml --ref <branch> -f platforms=macos`.
Choices are `linux`, `macos`, `windows`, and `all` (the default).
Ordinary branch pushes never start a build.

A deliberate CI tag also starts a run:

```sh
git push origin port-ci
git tag -a ci/port-ci-1 -m 'platforms=all'
git push origin refs/tags/ci/port-ci-1
gh run list --workflow build.yml
gh run watch <run-id> --exit-status
gh run view <run-id> --log-failed
git push origin :refs/tags/ci/port-ci-1
git tag -d ci/port-ci-1
```

Use `ci/<branch>-<n>` with a new number each time. Annotate with
`platforms=linux`, `platforms=macos` or `platforms=windows` to target only
one platform; unannotated tags run all. Superseded builds cancel per branch
and platform, including across different tag numbers. Delete remote CI tags
once their runs finish. Dependencies and Linux compiler output are cached;
run only the platforms needed for the change.

GitHub isolates caches by ref: a fresh CI tag cannot restore a cache saved
under another tag. Caches help same-ref reruns and, after integration,
manual runs on the same branch. Linux ccache also reuses shared compilation
within each run's isolated plugin builds. See GitHub's
[cache access restrictions](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache).

Linux (`ubuntu-24.04`, Clang 22) builds the existing `dev` preset with all
its tests, under Xvfb and software OpenGL. SDL3 and the pinned CLAP validator
are built and cached in the runner's workspace; Ubuntu 24.04 predates SDL3.
macOS (`macos-15`, arm64) uses
AppleClang and the system libc++, with deployment target 14.4. Its build preset
uses two jobs, also enforced explicitly by the workflow, to keep the engine
command translation units within the hosted runner memory budget. Native macOS
CI allows 150 minutes for a fresh build plus serial source-export tests;
Windows allows 120 minutes and Linux retains 60. The root
project uses `-gline-tables-only` in this preset: assertions and unoptimized code
remain enabled, with file/line stack traces. For full variable/type debugging
on a larger machine, configure with `-DCMAKE_CXX_FLAGS_DEBUG=-g`. Isolated stock
projects retain their own Debug flags. Homebrew
supplies Ninja/SDL3, while zlib comes from the SDK. Float/double
parsing uses the vendored MIT fast_float library through core-base on every
platform; integer parsing remains `std::from_chars`. This avoids the Apple
libc++ floating-point parsing gap without shipping a second C++ runtime.
AppleClang's advisory `-Wperf-constraint-implies-noexcept` is suppressed
when supported for the existing RT signatures; `nonblocking` annotations
and the other warnings remain enabled.
Windows (`windows-2022`, x64) uses clang-cl, Ninja
and the Windows SDK, with vcpkg SDL3/zlib and a binary cache. Windows
10 1903 is the minimum (ADR-0044: every executable runs with the UTF-8 code page). No packaging or installers run here. Local platform
commands are `cmake --workflow --preset macos` or `windows`; the latter
requires `VCPKG_ROOT` and an initialized Windows SDK environment.

If `build/macos` was configured with the earlier Homebrew LLVM toolchain,
start with `cmake --fresh --preset macos` to discard its compiler/runtime
cache, then build and test with the `macos` presets.

### Temporary port gates

`cmake/Platforms.cmake` defines the boundaries. Later port steps remove
them as implementations and tests become available. Linux coverage is
unchanged. macOS and Windows currently build/test JSON/schema/params/patch,
core-base native waits/wakes/thread setup, command bus, control-protocol/MCP,
Lua, DSP/Console 9/analysis, MIDI/notation/timeline/optimizer,
session model, plugin support, all stock SDK-free DSP engines, and the
script compiler/runtime and suite semantic layer. Audio-file I/O, engine/CLI,
dummy/offline rendering, sandbox and their portable tests run on all three
platforms. Native core-base sources/tests, JSON-RPC control transport/model loop
and standalone MCP/proxy no longer share the hosting gate (ADR-0035).
Linux-specific descriptor/permission/signal probes and memory-residency/rlimit
tests remain guarded with portable boundary coverage alongside them.
These gates are explicit, with no no-op RT implementations:

| Gate | Off on | Reason / follow-up |
|---|---|---|
| JACK/fake JACK and hardware capacity benchmark | macOS, Windows | Linux graph/hardware tooling; native device audio uses PortAudio (ADR-0038) |

Research GUI spikes and host fuzzers reject configuration on non-Linux
platforms rather than silently building incomplete targets. Renders must
be deterministic on each platform; cross-platform bit identity is not
required (ADR-0034).

ADR-0036 enables native CLAP/VST3 hosting, the standalone supervised scanner,
stock plugin builds and their conformance/reset checks on macOS/Windows.
ADR-0037 enables the native bridge executable and shared/dedicated isolation
tests. Linux `/proc`, raw descriptor/subreaper and shell-script probes remain
Linux-only. Native tests cover fixture loading, state/audio, crash/hang
isolation, cache and process capture/deadlines. The pinned clap-validator is
built in every job and checks each stock plugin and the player (on Windows,
through Git's bash); its tests skip where it is absent, and the product,
semantic and host checks still execute.

Windows stock builds enable the CRT math constants with _USE_MATH_DEFINES in
platform build glue. Product-owned semantic stress tests use a native loader;
template host/probe/shared-stack sources are unchanged.

Monorepo macOS stock GUI builds keep the template's default Metal renderer:
Steppe-owned CMake glue points its `nanovg_metal` FetchContent at the vendored
pin in `third_party/nanovg_metal`, so offline builds need no download.

Apple libc++'s missing jthread/stop-token facilities are supplied through the
core-base non-RT job layer, retaining standard types elsewhere. The VST3 loader
uses Objective-C++20 with ARC. The stock projects' existing AUv2 builds use the
vendored Apache-2.0 AudioUnitSDK at clap-wrapper's pin; no downloads or template
source changes are required.

Native audio builds PortAudio only in the root DAW project (CoreAudio on macOS;
WASAPI + ASIO on Windows). Isolated stock-plugin projects never build or link it.
Fake-driver engine-portaudio.tests runs on all platforms and under Linux RTSan /
TSan. Real native-device tests require SUPPLE_AUDIO_DEVICE_TEST=1 exactly; leave
it unset in CI. Native MIDI uses CoreMIDI / WinMM through the PortAudio backend (ADR-0042).
`SUPPLE_MIDI_DEVICE_TEST=1` enables a short native input test using fake audio;
leave it unset in CI. It opens MIDI inputs but never the audio device.

GUI tests use Linux EGL offscreen windows or hidden native Cocoa/Windows
windows. Native runners need a desktop session and OpenGL 3.3 (macOS 4.1).
The Windows workflow deploys a pinned CI-only Mesa/llvmpipe driver beside GUI
executables; it changes no system configuration and is not shipped. Linux
reference PNGs are never updated on native systems. SUPPLE_GUI_DISPLAY_TEST
still requires exactly `1`; ordinary tests do not open plug-in editors or
real audio devices. DAW editor integration fixtures compile on all platforms;
Linux pipe/poll injection fixtures run alongside the portable watcher coverage.
Native platform smoke images are uploaded with test logs.

GUI socket/screenset integration and source-export integration tests are enabled
on all three platforms. The GUI agent fixture uses the existing AF_UNIX transport
(including Windows 10 1903+), bounded anonymous-pipe readiness and an orderly
native window close after saving its private project. Native snapshots account
for framebuffer density. Source-export tests inherit the compiler, toolchain,
macOS architecture/deployment target and vendored renderer dependencies. They
build every exported script and check its own tests, factory patches and exact
same-platform player audio, including binary exports. Linux/Windows binary
exports retain the appended format; macOS bundles carry a JSON resource and are
re-signed ad hoc with the system codesign tool before publication. Native tests
verify macOS signatures, scanner/direct-host/bridge loading, factory patches and
exact player/source/binary audio. No signing identity or system installation is
needed; failed/missing macOS signing reports dsp.sign. Headless `dsp.preview`
pictures run through the template's EGL (Linux), Metal (macOS) or WGL (Windows)
renderer. CI requires successful PNGs, repeated identical pictures and pointer
edits; Windows supplies only Mesa's WGL DLLs beside the test executables.
`dsp-pictures-<platform>` artifacts contain the untouched and dragged editor
PNGs. Missing renderer devices/drivers produce `dsp.no_picture`.

Evaluation harness unit fixtures run everywhere: Python replaces executable shell
fixtures; stdio readiness uses the portable pipe helper. Windows attempts use a
kill-on-close Job, assigned to a gated Python bootstrap before any evaluated
command runs. POSIX keeps sessions/process groups. A descendant-cleanup fixture
checks cleanup after the leader exits. Credentials remain references: a symlink,
or on Windows without symlink privilege a same-volume hard link (different-volume
failure is explicit; credentials are never copied). Real sandbox scorer checks
still require SUPPLE_EVAL_INTEGRATION=1, and real endpoint runs are never CI tests.

## Building distribution packages

See [ADR-0048](docs/decisions/0048-packaging-and-distribution.md) and the
[distribution contract](docs/specs/distribution.md). Build only the required
Release targets, then stage and package (Linux example):

```sh
cmake --preset linux-package
cmake --build --preset linux-package --target package-runtime --parallel 6
cmake --install build/linux-package --prefix /tmp/package-tree --component Runtime
cmake --install build/linux-package --prefix /tmp/package-tree --component Plugins
python3 tools/package/linux.py --build-dir build/linux-package
```

Artifacts go to `dist/`; `--output-dir` selects another folder. Linux scripts
require Python 3, xz, ldd and binutils (nm) to audit exported symbols. For
published builds use the Ubuntu 22.04 container in `package.yml`; packaging
on a newer workstation warns because it raises the glibc minimum. The
runtime audit must pass: no shared libstdc++/libgcc_s dependency or runtime
exports. CI uses Clang 22 with GCC 12's headers and static libstdc++, selected
explicitly with `-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12`.
See [ADR-0049](docs/decisions/0049-isolated-linux-package-runtimes.md). Do not bundle JACK or graphics drivers.

On macOS use `cmake --preset macos -DCMAKE_BUILD_TYPE=Release -DSUPPLE_BUILD_TESTS=OFF -DSUPPLE_PACKAGE_ONLY=ON`, build
`package-runtime`, then `python3 tools/package/macos.py --build-dir build/macos`.
It requires Apple Silicon, hdiutil, ditto, install_name_tool and codesign;
signing is ad hoc without an account. On Windows use the `windows` preset
with `-DCMAKE_BUILD_TYPE=Release -DSUPPLE_BUILD_TESTS=OFF -DSUPPLE_PACKAGE_ONLY=ON`, build `package-runtime`, then
`python tools/package/windows.py --build-dir build/windows`. Install NSIS
in the build environment first (Zlib compressor only).

`gh workflow run package.yml --ref <branch> -f platforms=all` manually builds
and smokes packages on native runners (Linux in an Ubuntu 22.04 container).
`-f draft_release=true` additionally attaches downloads to a **draft**
`v<version>` release; it never publishes or modifies a published release.
Creating or updating a draft requires `platforms=all`. Re-running it replaces
the draft's files only when its target is the exact commit being built. To
release a different revision, delete the draft (or bump the product version),
then run all platforms from one commit. Partial-platform runs can still produce
workflow artifacts with `draft_release=false`.
Run `build.yml` separately for native regression coverage. CI smokes installed
artifacts with dummy audio, isolated preferences and no real agent registration.

GitHub discovers dispatch-only workflows on the default branch. A newly added
`package.yml` cannot be dispatched from a topic branch until the workflow file
exists on that default branch; the CLI returns 404 in that case. Do not change
the default branch just to bypass review. Once available there, `--ref` selects
the implementation branch to validate.

Before the workflow reaches the default branch, a maintainer-authorized
throwaway `package-ci` branch can add a single push-trigger commit for branch
validation. Keep that trigger off the integration branch; delete the temporary
branch locally and remotely after validation. Push runs default to all platforms
and never create draft releases.

`SUPPLE_PACKAGE_ONLY=ON` selects the template's existing CLAP and notices
targets, avoiding unshipped macOS AUv2 wrappers. It requires tests off; native
regression builds keep the full plugin formats and test coverage. Packaging CI
uses a shared compiler cache for Linux/macOS template instances.
