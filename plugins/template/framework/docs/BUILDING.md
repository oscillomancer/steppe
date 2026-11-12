# Building, testing and validating

Dependencies are fetched at configure time and pinned to exact commits in
`framework/cmake/Pins.cmake`: CLAP SDK, clap-wrapper (with the VST3
and AudioUnit SDKs it pins), dr_libs, pugl, NanoVG, nanosvg, MTS-ESP (with
`PRODUCT_TUNING`) and, on macOS, nanovg_metal. Git and network access are
needed for the first configure, unless the sources are vendored (below).
There are no submodules.

## Presets

| Preset | Build directory | What it is for |
| --- | --- | --- |
| `release` | `build/release` | Shipping builds, and the everyday test run |
| `debug` | `build/debug` | Debugging; warnings in framework and product code are errors |
| `asan` | `build/asan` | AddressSanitizer + UBSan over our code (framework, product, tests, the built plugin's own objects); clap-wrapper and the SDKs are linked uninstrumented |
| `tsan` | `build/tsan` | ThreadSanitizer over our code, without the GUI: the cross-thread tests are the point |
| `rtsan` | `build/rtsan` | RealtimeSanitizer (LLVM clang 20 or later): every audio-thread entry point is a real-time context, and an allocation, lock, blocking call or file access anywhere beneath it -- the product's processor included -- aborts the test with a stack trace |
| `fuzz` | `build/fuzz` | libFuzzer with AddressSanitizer and UBSan (clang), no GUI: builds the state fuzzers only, and its tests run `state-fuzzer` for a minute |

The presets are defined in `framework/cmake/presets.json`, which the root
`CMakePresets.json` includes, so a framework update brings new ones. A
product adds its own presets to the root file.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Options, for a hand-made build directory: `SUPPLE_BUILD_PLUGIN`,
`SUPPLE_BUILD_VST3`, `SUPPLE_BUILD_GUI`, `SUPPLE_BUILD_TESTS` (all on),
`SUPPLE_WARNINGS_AS_ERRORS`, `SUPPLE_SANITIZE` (`address`, `thread` or
`realtime`), `SUPPLE_FUZZ` (clang), `SUPPLE_METAL` (macOS, on),
`SUPPLE_CLAP_VALIDATOR` (path to clap-validator), `SUPPLE_VENDOR_DIR`
(below), `SUPPLE_PRODUCT_DIR` (the product's folder, `product/` unless a
build names one elsewhere: `product/...` paths in its `product.cmake` are
in it, and its own `LICENSE`, if it has one, ships with the plugins), and
the signing options under Packaging. A configure without a build type
defaults to Release. MSVC rejects nonempty `SUPPLE_SANITIZE`:
sanitizer presets require a supported Clang/GCC toolchain.

## Offline builds

```sh
cmake -P framework/tools/vendor.cmake                    # into vendor/ at the root
cmake -DDIR=/somewhere/else -P framework/tools/vendor.cmake
```

copies every pinned source -- clap-wrapper's own SDKs included, for every
platform and option -- into one folder, with no Git history (about 40 MB).
A configure that finds `vendor/` at the root uses it by itself; any other
folder is given with `-DSUPPLE_VENDOR_DIR=<folder>`. Nothing is downloaded
then: FetchContent is disconnected and clap-wrapper is handed its SDKs.
Each vendored folder records its pin, and one that no longer matches
`Pins.cmake` (after a framework update) stops the configure until
`vendor.cmake` has run again; it fetches only what changed. Commit
`vendor/` to the product's repository for a self-contained source tree, or
keep it outside for a CI cache or an air-gapped machine.

Patches are never applied to a vendored folder, nor to any source given with
`FETCHCONTENT_SOURCE_DIR_<NAME>`: the build copies that dependency to
`build/<preset>/_deps/<name>-patched` and patches the copy. Built and tested
here with the network removed (`unshare -rn`): configure, build and every
test. External Git source overrides are fingerprinted by revision, status
and dirty contents; a source without Git metadata is recopied each configure.

Wrapper and AU metadata are generated at configure time. After changing
`product/product.cmake`, configure again; after changing identity, configure
from a clean build directory.

## Platforms

**Linux** (built and tested here: Arch, GCC 16, CMake 4.4, Mesa, X11 and
XWayland). CLAP and VST3, rendered with OpenGL 2.

```sh
sudo apt install build-essential cmake git ninja-build libx11-dev libxcursor-dev \
    libxrandr-dev libxext-dev libgl1-mesa-dev     # Debian/Ubuntu equivalents
```

**macOS** (Xcode command-line tools, CMake 3.27+). CLAP, VST3 and AUv2,
rendered with Metal (`-DSUPPLE_METAL=OFF` builds the OpenGL path instead).
Deployment target 11.0, the first release that runs arm64. The AU is
ad-hoc signed after its real `Info.plist` is staged; `auv2-bundle` checks
both. The CLAP and VST3 carry the linker's ad-hoc signature only.
Built and tested by CI on macOS 15 (arm64, Xcode 16) from framework 1.2.0:
the release, debug and asan presets pass, `auval`, pluginval and
clap-validator pass, and `clap-probe` opens the Metal editor in a Cocoa
window in every scenario below. The runner is a virtual machine at 1x with
a paravirtualised GPU, so that is not a substitute for opening the editor
in real hosts on real hardware, at 1x and on a Retina (2x) display.

**Windows** (Visual Studio developer prompt with CMake 3.27+, Ninja and Git).
CLAP and VST3, OpenGL entry points resolved at runtime. Configure with
`cmake --preset release -G Ninja`: the build is single-configuration, one
directory per preset, and refuses Visual Studio's multi-configuration
generator (and Xcode's). The C++ runtime is linked statically, so a plugin
needs no Visual C++ redistributable. The editor needs an OpenGL 2 driver;
Windows' own fallback (the GDI renderer on a machine without a GPU driver)
stops at 1.1, and `set_parent` then fails rather than embed a blank view.
Built and tested by CI on Windows Server 2022 (MSVC 2022, x64) from
framework 1.3.0: the release and debug presets pass, including
`win32-embedding`, clap-validator and pluginval, with the editor drawn by
Mesa's software OpenGL because the runner has no GPU. Open it in real hosts
on a real Windows machine, at 100% and at 200% display scaling, before
shipping. The framework also cross-compiles with MinGW-w64, and its tests
run under Wine, which is a quick check from Linux but proves nothing about
MSVC or Windows itself.

Outputs land in `build/<preset>/plugins/`: `Name.clap`, `Name.vst3` and, on
macOS, `Name.component`, with the license and third-party notices inside each
bundle (or beside the bare `.clap` on Linux and Windows). On Windows,
clap-wrapper puts them one level down, in `plugins/CLAP/Name.clap` and
`plugins/VST3/Name.vst3`, both single files.

## Packaging and signing

`cmake --build build/release --target product-package` writes, to
`build/release/package/`:

- `<Name>-<version>-<platform>.zip` (`.tar.gz` on Linux): `CLAP/`, `VST3/`
  and on macOS `Components/`, with `LICENSE.txt`, `NOTICES.txt` and
  `INSTALL.txt` (where each format goes). The platform is `macos-arm64`,
  `windows-x86_64`, `linux-x86_64` and so on (`macos-universal` when
  `CMAKE_OSX_ARCHITECTURES` names two).
- macOS: `<Name>-<version>-<platform>.pkg`, one choice per format, all
  installed to `/Library/Audio/Plug-Ins/...`, never relocated to wherever
  another copy of the bundle is, and offered only on the architectures the
  binary has (an Apple silicon Mac is not asked for Rosetta).
- Windows: `<Name>-<version>-<platform>-setup.exe`, by Inno Setup 6.3 or
  later when it is installed (found in Program Files, or given as
  `-DSUPPLE_ISCC=...`): the formats as components, into
  `C:\Program Files\Common Files\CLAP` and `VST3`, with an uninstaller.
  x64 only: `product-package` refuses an ARM64 Windows build until ARM64
  is supported.

The `package` test makes the archive alone, to check what goes in it.
Signing is opt-in, by configure options or the same names in the
environment (for CI secrets):

| Option | Effect |
| --- | --- |
| `SUPPLE_SIGN_IDENTITY` | macOS: the bundles are signed with this Developer ID Application identity, hardened runtime and timestamp. Without it they are signed ad hoc, which seals the license and notices added to them |
| `SUPPLE_INSTALLER_IDENTITY` | macOS: the `.pkg` is signed with this Developer ID Installer identity |
| `SUPPLE_NOTARY_PROFILE` | macOS: a `notarytool` keychain profile (`xcrun notarytool store-credentials`). The bundles and then the `.pkg` are notarized and stapled; needs both identities |
| `SUPPLE_SIGN_COMMAND` | Windows: a command that signs the file named after it, run on each plugin and the installer, e.g. `signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a` |

Linux archives are not signed. Built and checked here on Linux only; the
`.pkg` and the installer are made by CI's macOS and Windows jobs, and none
of the signing paths has been run without a certificate to run them with.

## Installing

`cmake --build build/release --target product-install` copies every format to
the user's plugin folders together (`~/.clap` and `~/.vst3` on Linux,
`~/Library/Audio/Plug-Ins/*` on macOS, `%LOCALAPPDATA%\Programs\Common\*` on
Windows), so a host can never load a stale CLAP beside a fresh VST3. Ordinary
builds never touch those folders.

## Tests

`ctest` runs all of these. A product keeps every one and adds its own:
claims in `product/test/` (one `processor-tests` executable), and anything
that needs its own program -- a long measurement, a command-line tool for
analysis scripts -- in `product/targets.cmake`, which the framework includes
last when it exists.

| Test | Proves |
| --- | --- |
| `core-tests` | Parameter text and tapers for every product parameter and for every unit, off-at-minimum and custom format; DSP blocks; the Engine's contracts (bit-identical output under random automation at any block sizes -- aux outputs and every note and MIDI message sent included, with the sidechain and notes, MIDI, expressions and per-note modulation fed when the product takes them -- float = double, in place = separate buffers, bypass exact after its fade, telemetry rate independent of event density, NaN never reaching the host from the processor, through bypass or by float overflow, the transport each split sees, reset preserving values, and a state loaded before reset rendering exactly what a fresh instance renders); smoothed parameters gliding from each change's exact frame, identically at any block sizes, settling exactly and snapping on reset and on a change flushed before the first block after it; outputs reaching the store at 30 Hz and ignored when a host sets them; levels exact at 0 dB and silent when off; state format, outputs excluded, properties and the preset's name kept and a preset leaving the session out; properties delivered to the processor after prepare and before the next block, the last of several changes only; offline rendering reaching `prepare()` and the next block; a tuning read at the frame asked, chosen at its event's frame, and unmapped keys retired unheard; note names announced when the processor says; the editor's audio feed carrying input and output only while it is open, and only for a processor that asked; the editor's notes played at the next block and the keys held reaching telemetry; the measurement helpers (`Testing.h`: tone amplitude, THD, delay) exact on known signals, and the latency an effect reports equal to the delay measured through it at its defaults; user presets saved, listed, loaded, renamed and deleted, files not ours ignored, the defaults unmodified when a layer's chunk follows the state, factory presets setting what they name, A/B, settings as text, the session remembering the preset, preset names valid as files everywhere; file references found by their tail under remembered folders and under this user's home, remembered folders bounded and newest first, and (for a product with a file property) files loading at activation, missing ones reported and kept, moved ones relocated, a failed one explained, a new sample rate reloading, and loads racing a running audio thread arriving whole; the edit bridge, single- and multi-threaded and against a host that takes one event at a time |
| `dsp-tests` | The DSP library (`framework/dsp`), measured: every cookbook biquad against its response, Butterworth to order 16 as the bilinear transform of its prototype, Linkwitz-Riley halves summing to an all-pass, one-pole and DC blocker; the gain computer's knee and slope, followers' time constants, the sliding maximum against brute force, lookahead never late, true peak between samples and a frequency/phase sweep through 0.455 fs, never below the sample peak; linear-phase oversampling a pure delay at 2x to 16x, images and aliases 95 dB down in both phases; the STFT an exact delay, its bins filtering; PCG32 repeatable, polyBLEP shapes aliasing less than naive ones, wavetables alias-free at any pitch, LFO shapes and sync, noise slopes; ADSR segment times and curves; the voice manager stealing, holding with the pedal, mono and legato, glide, bend and MPE, and ending every note exactly once through 4000 random events; fractional and all-pass delays; ADAA antiderivatives and their aliasing; K-weighting to BS.1770's table and EBU 3341 loudness readings with gating; convolution equal to the direct sum in any blocks; prepared primitives and resets inside an RTSan-marked runner; the worker and hand-off (nothing freed on the audio thread); WAV, AIFF and FLAC decoding (whole, or only a long file's start), resampling, and a disk stream read exactly, restarted, on a real thread |
| `processor-tests` | The product's own claims (`product/test/`) |
| `state-fuzz` | Saved state is untrusted input: every kind of state the product makes (defaults, random settings, session and preset contexts, pasted text), each damaged 300 ways from a fixed seed, loaded through CLAP into a fresh plugin. A refused load changes nothing; an accepted one saves, loads into another instance and saves again byte for byte; some also process, finitely |
| `state-fuzzer` (`fuzz` preset) | The same check under libFuzzer, coverage-guided, for a minute from those states (`state-corpus` writes them); `build/fuzz/state-fuzzer build/fuzz/state-corpus -max_total_time=3600` runs it longer |
| `benchmark` (release) | The product's CPU cost in three scenarios (steady, 32-frame blocks, every parameter automated), excluding Engine construction and warm-up, in units of a reference workload timed alongside, against `product/benchmark.txt` for this machine; more than 1.5 times (and 0.05 reference units) over it fails. Skipped on a machine with no baseline (`benchmark --update` records one) |
| `previews` | Every scenario in `product/previews.txt` renders, into `build/<preset>/previews`. Skipped without a display |
| `package` | The release archive holds every format, the license, notices and install notes (macOS: bundles signed ad hoc and verified) |
| `package-windows-arm64` | Packaging an ARM64 Windows build is refused before anything is staged |
| `ui-tests` | Windowless drawing through the plugin renderer (EGL, WGL or Metal), repeated byte-identical pictures, top-row-first RGBA colours and pointer gestures; interaction rules (drag, Shift, wheel, keys, text entry, menus, focus loss); a property's text field editing UTF-8 within its limit; text fields selecting by click, drag, double-click and Shift, copying, cutting and pasting, and placing the caret where a click lands in scrolled text; a press a pointer hook takes holding the pointer, the wheel and keys waiting; long text shrinking, then cut with an ellipsis; popup menus skipping headers and scrolling; prompts refusing or handing over to a confirmation; the right-click context menu (reset, enter value, the host's items flattened and performed, the host's own popup preferred); the preset bar stepping, comparing, saving, replacing, renaming, deleting and copying settings through the clipboard; controls on an output display only; layout helpers; tooltips; `enabledWhen`; buttons and momentary controls; tab pages; an XY pad's two-parameter gesture; a custom drawing's one gesture over every parameter it edits; an envelope's handles; lists by pointer, wheel and keys; the keyboard's notes always ending; the FFT and spectrum calibration and RMS; SVG parsing, both fill rules' windings, and every product asset; dropped URI lists and the Linux dialog commands; a preset file dropped on the editor; a file slot choosing through the dialog, taking a drop of what it accepts, clearing from a key and its menu, offering the product's factory files it takes in that menu, and showing what became of its file; repaint on host-side changes, and on an output only where it is shown; layout lint (pages may share space); a 20 000-event fuzz of the real editor that fails on any nested or unbalanced gesture, more than two parameters in a control's, or a note left sounding |
| `plugin-tests` | The CLAP surface in process: descriptor, parameter list, state into a fresh instance, every layout on every port with the sidechain and aux outputs connected or not, a sidechain the host deactivated (`audio-ports-activation`) heard as unconnected whatever its buffers hold, layouts negotiated port by port (`configurable-audio-ports`), bypass (aux outputs silent), editor edits reaching a stopped host, edits and preset recalls during playback on real threads, telemetry gating, the restart protocol, the output constant hint cleared on every output, tail changes announced, output events in time order, a product with note input ending every note it was given and taking notes, MIDI, expressions and per-note modulation in any mix, note ports and voice info as declared, output parameters read-only, sent to the host at most 30 times a second with `get_value` agreeing, and absent from state; properties set by the editor saved and restored, the state-context extension leaving the session out of presets, presets loaded by location (factory by name, user by file) with errors reported, the editor's loads reported to the host, and preset discovery listing the factory presets and the user's files; in a debug build, every call on a thread CLAP does not allow logged once, and none in a session that keeps the rules; trouble on the audio thread (a narrow bus) and damaged state logged from the main thread; render mode offered only to a processor with an offline mode; "Reset to Default" in a host's context menu as one whole edit; the host's context menu flattened for the editor; the track's name and colour, and their changes; note names as the processor gives them, the host told after every parameter change that renames a key; the host's CLAP tuning retuning notes and retiring unmapped keys at their frame; the editor's keyboard playing through the plugin, lighting its key and ending its note; a session finding the file it names after its folder moves, and keeping (and saving) the reference when it is gone |
| `host-smoke-<layout>` | The built `.clap` loaded from disk, once per declared layout (`-2ch`, `-1ch`, `-5.1`, `-ambi1`, `-notes` for a note effect): every port's width, type and flags, the surround channel map or ambisonic config, text round trips and rejection, float/double and in-place renders with the sidechain fed (with `PRODUCT_TRANSPARENT`, output equal to input exactly), no restart storms, the latency protocol when `PRODUCT_SMOKE_LATENCY_PARAM` is set, state into a fresh instance and byte-identical resave, CLAP's reset contract: a state loaded into a used instance and reset (and restarted, if it asked to be: a restart parameter moved) renders bit-identically to a fresh instance with that state, every factory preset found through preset discovery and loaded mid-stream, render mode, context menu and note names where offered, and nothing logged as misbehaviour by a debug build checking every call's thread |
| `vst3-host` (Linux) | The VST3 against the CLAP it wraps: its buses (the sidechain an aux input, aux outputs by name, event buses), every declared layout chosen through `setBusArrangements` with its speaker arrangement and every bus activated, MIDI controllers and pitch bend mapped, and the same audio, notes, pitch bend, mod wheel and automation rendered through both formats in every layout giving the same output on every bus and the same notes and MIDI out; with note input, 96 notes held at once across a stop and start render alike too. Each VST3 process call is a real-time context, so under `rtsan` the wrapper's own work is checked as well |
| `patch-*` | Each clap-wrapper and Pugl patch is still applied to the pinned checkout |
| `probe-audio-tests` | Probe audio buffers match mono, multichannel and instrument ports and a note effect's none, with separate input/output storage and the output peak reported |
| `bin2c`, `sanitizer-config`, `new-product`, `patched-source`, `ci-product-names` | Byte-array line wrapping and contents, unsupported MSVC instrumentation refused, portable product names (no Windows device names or trailing dots) and reserved identities, patched source-copy invalidation, and shell-safe CI product names |
| `framework-update` | `update_framework.cmake` makes a product's `framework/` the template's and changes no other file; a refused update changes nothing |
| `ci-probe-checker` (template only) | The macOS CI step's checks reject a probe whose editor did not open or resize to the size asked for |
| `exports-clap`, `exports-vst3` (Linux) | The modules export their entry points and nothing else |
| `vst3-unload` (Linux) | The VST3 unloads cleanly and survives process exit while loaded |
| `x11-embedding` (Linux) | A real X11 parent: mapped child, non-blank pixels, resize, `set_scale(2)` doubling the editor, hide/show, three create/destroy cycles while audio runs. Skipped without a display |
| `x11-embedding-overlapped` (Linux) | Six `x11-probe`s at once on one display, their windows on top of each other: each still reads its own editor's pixels. Skipped without a display |
| `mac-embedding` (macOS) | A real Cocoa host window in each `clap-probe` scenario (`.github/scripts/clap-probe-mac.sh`): the editor opens, resizes and reopens, CLAP-reported and native sizes agree after parenting and resizing, and the audio stays live. Reports and pictures in `mac-probe/probe` |
| `win32-embedding`, `win32-embedding-no-host-timer` (Windows) | The same against a real Win32 parent HWND, plus native sizes after resize, sizing before `set_parent`, and module reload. On an inactive instance, automation must provoke a repaint before another capture and change the pixels. The second refuses the host timer and also requires `WM_TIMER` delivery |
| `win32-module-ownership` (Windows) | CLAP and VST3 loaded together: each window belongs to its own DLL; VST3 unload preserves CLAP editors, and class cleanup follows the last instance |
| `auv2-bundle` (macOS) | The AU carries clap-wrapper's AudioComponents plist and a valid signature |
| configure | No file in an include root (`framework/`, `framework/test`, `product/`) shadows a standard header on a case-insensitive filesystem |
| `clap-validator` | free-audio's clap-validator, when found or passed as `SUPPLE_CLAP_VALIDATOR`. With presets on, its `preset-discovery-crawl` and `-load` are left out: 0.4.1 deadlocks in its own receiver on a second factory preset; `plugin-tests` and `host-smoke` crawl and load them instead |

clap-validator: `cargo install --git https://github.com/free-audio/clap-validator`.
Its `process-audio-denormals` timing check can warn when it runs alongside
other tests: its own denormal generation runs in the timed region without
flush-to-zero. Run alone it reports 1.3-1.9x for this framework (the engine
measures 1.0x), against 2-3x for a plugin without FTZ.

Before a release, also: `cmake -DPRESET=release -P framework/tools/validate.cmake`,
which builds, then runs pluginval 1.0.4 (fetched and checksummed into
`build/tools`, or `-DPLUGINVAL=...`) at strictness 8 on the VST3 -- and on
macOS installs the formats and runs it on the AU, then `auval -strict` with
the product's own codes -- and clap-validator when it is found. Run here on
Linux: pluginval passes on the VST3, editor tests included. Then every
format opened in real hosts on every
platform you ship (REAPER, Bitwig, Ableton Live, Logic for AU), with
automation recorded and played back, state saved and reloaded, the editor
resized, closed and reopened; sample rates 44.1 to 192 kHz; mono and stereo.

Tests that follow the product's own table and capabilities (smoothing,
outputs, levels, notes, MIDI, a sidechain, aux outputs, surround) report
themselves skipped in a product without the feature, or run on what it has.
The template's examples have each, which is why a framework change also runs
them:

```sh
cmake -DPRESET=release -P framework/tools/test_examples.cmake   # any preset; -DEXAMPLES="pulse" for one
```

It generates each directory of the template's `examples/` with
`new_product.cmake` into `build/examples/<preset>/<example>`, then
configures, builds and runs ctest there with the preset. Later runs keep
those build directories. Logs are beside them. clap-validator runs there too
when it is on the PATH, passed as `-DCLAP_VALIDATOR=...`, or known to the
template's own build of that preset.

## Tools

- `render-cli`: a WAV file through the same Engine and DSP the plugin runs.
  `--list` prints the parameter table (outputs marked); `--set "Name=value"`
  uses the parameter's own text; `--at <frame> "Name=value"` automates;
  repeated `--block` sizes cycle, for boundary bugs; `--sidechain key.wav`
  feeds the sidechain; `--preset "Name"` starts from a factory preset and
  `--property "Name=text"` sets a property; `--offline` renders as a host
  bounces, for a processor with an offline mode. `--list` also lists both. Each aux output is written beside the output as
  `<output>-<bus name>.wav`. Output parameters' final values are printed
  after the render. A note effect has no audio, so nothing to render.
- `gui-preview out.png`: the real editor rendered offscreen to a PNG, with
  transient states staged: `--hover`, `--focus`, `--menu`, `--text` (by
  parameter or property name; `--menu Presets` opens the preset menu),
  `--set`, `--property "Name=text"`, `--preset "Name"`, `--scale`;
  `--context NAME` opens a control's context menu with stand-in host items,
  and `--track "Name"` with `--track-colour RRGGBB` plays the host's track
  info. `--tooltip NAME` rests the pointer on a control until its tooltip
  shows, `--page N` turns the first tab bar to page N, `--play KEY` holds a
  key down on the first keyboard, and `--drop` drags files over the editor.
  A control can also be named by its own label (a button's). The programme
  it plays includes a kick on the sidechain and a held C major chord for a
  product that takes notes, and reaches the editor's audio feed for one
  that asked.
- `x11-probe plugin.clap [capture.ppm]`: the Linux embedding probe, also run
  by ctest.
- `win32-probe plugin.clap [capture.ppm] [--no-host-timer]`: the Windows
  embedding probe, also run by ctest; writes the capture at 1x, resized and
  at `set_scale(2)`.
- `clap-probe <binary inside .clap> [blocks] [--mono --resize --presize
  --reopen --drop-parent --destroy-hot --no-host-timer]`: a GUI host for
  macOS (window embedding, sizing before and after parenting, AU-style timer
  refusal, hot teardown) that writes `probe-window.png`; elsewhere it
  exercises everything but the window. Audio goes to every declared port
  (`--mono` keeps the default layout when there is no mono one); a plugin
  that takes notes gets two held notes, C2 and middle C (so a drum
  sampler's first pad sounds too), and a note effect passes on the notes it
  sends.

## Continuous integration

`.github/workflows/build.yml` runs on demand only: the repository's Actions
tab, "Build and test", "Run workflow", or
`gh workflow run build.yml -f platforms=macos` (`linux`, `windows`, `all`;
`-f sanitizers=true` adds asan and tsan on macOS and Linux). Each platform builds the
release and debug presets and runs `ctest`. In the template, each example
under `examples/` also gets a release job of its own on each platform
(`-f examples=false` leaves them out): the job puts the example in place of
`product/`, as `new_product` would under the example's own name and codes,
and runs everything below on it, so the sidechain, aux outputs, surround,
the instrument and the note effect meet a real AU (`auval` with the
example's own type: `aufx`, `aumu`, `aumi`), VST3 and CLAP. A product has no
`examples/`, so a copy of the workflow there runs only its own jobs. The
release build then goes through what a host would do with it:

- **macOS**: `product-install`, the AU's signature and plist, `auval
  -strict`, pluginval (strictness 8, VST3 and AU, editor included),
  clap-validator, `ctest` (so `mac-embedding` runs), `clap-probe` in each
  scenario above on the release build, keeping its pictures (failing if the
  editor does not open, resize or reopen, or the audio goes silent; both
  CLAP-reported and native Cocoa dimensions are checked after parenting
  and resizing), and
  `gui-preview` (every named scenario, and 2x), and `product-package`
  (unsigned: bundles ad hoc, the `.pkg` unsigned).
- **Linux**: `ctest` under Xvfb (so `x11-embedding` runs), clap-validator,
  pluginval on the VST3, `gui-preview`, `product-package`.
- **Windows**: MSVC with Ninja, `ctest` (so `win32-embedding` runs), the
  install target, clap-validator, pluginval on the VST3 (editor included),
  `gui-preview`, and `product-package` with Inno Setup (installed from
  Chocolatey if the runner lacks it). The runner has no GPU, so CI places Mesa's llvmpipe
  `opengl32.dll` beside the test executables and pluginval
  (`.github/scripts/get-mesa-windows.sh`); a DLL beside the executable is
  loaded before System32's.

A run-time product (`PRODUCT_RUNTIME`, such as the `stages` example) has
no fixed editor to preview and builds no `gui-preview`, nor an AU (a
component's codes are fixed in its bundle); those steps say so and pass.

Each job uploads the plugins, the release packages, the probe's window
captures and the previews as an artifact. A failing step repeats its compiler diagnostics and log tail
as error annotations. GitHub's macOS runners are 1x virtual machines with a
paravirtualised GPU, and the Windows runners draw with a software
renderer: a pass there is evidence about the platform code, not a
substitute for real hardware.
