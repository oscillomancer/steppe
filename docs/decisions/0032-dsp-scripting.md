# ADR-0032: DSP scripting

**Status:** Accepted, 2026-10-13 (proposed 2026-10-12). Language reference: `skill/guides/dsp-language.md` (the
canonical spec, shared by the skill and the MCP tools). Builds on ADR-0010,
ADR-0011 and the plugin template's `framework/AGENTS.md`.

## Context

ADR-0006 left DSP scripting to a language of its own. The goal: a person or
an LLM working through MCP writes an effect or instrument as a script, hears
it in Quire at once, iterates, and exports it as a plugin for any DAW, either
as a prebuilt "player" with the script embedded (no compiler needed) or as a
buildable C++ project on the plugin template (the graduation path).

Constraints found while surveying the code:

- The template's framework ([oscillomancer/template](https://github.com/oscillomancer/template),
  1.15.3) binds a product at compile time: `product::kParams` is included
  by about 700 lines of framework code, and `product::info` (identity, kind,
  buses) by about 230. Nothing can host a parameter table or a plugin kind
  that is only known when a script loads.
- Quire stores a semantic plugin's **patch**, not its CLAP state, in the
  project (`FxInstance::patch`), keys presets by plugin ID, and lists and
  instantiates plugins by ID. A scripted plugin that should behave like a
  stock one therefore needs its own plugin ID, and its patch must hold
  everything that matters (parameters and file references).
- Generic hosts see ADR-0010's value domains: a float parameter's CLAP value
  is its normalized curve position. The framework's host value is the plain
  value for linear parameters.
- Quire is C++20 and CMake with every dependency vendored and the build
  offline. Plugins are built from the template as ExternalProjects.

## Decision

### 1. The language: a checked subset of C++20

A script is one C++ file. Every script the compiler accepts also compiles,
unchanged, as ordinary C++ against the runtime header `<supple/script.h>`,
and means the same thing. The converse need not hold: the compiler rejects
anything outside the subset with an error that names the subset's way to do
it ("dynamic allocation isn't available in process(); declare a fixed-size
`Array<float, 4096>` member, or a `Buffer<float>` sized in prepare()").

```cpp
#include <supple/script.h>

struct Tilt : Effect {
    static constexpr Info info = {.name = "Tilt", .id = "com.example.tilt", .version = "1.0.0"};

    Param tilt  = decibels("Tilt", -6, 6, 0).smoothed(20).help("Raises the highs and lowers the lows.");
    Param pivot = frequency("Pivot", 200, 5000, 1000);

    Array<Biquad, 2> low, high;

    void update() {
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(pivot, 0.707, -tilt / 2);
            high[c].highShelf(pivot, 0.707, tilt / 2);
        }
    }
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i)
                io[c][i] = high[c].process(low[c].process(io[c][i]));
    }
};
```

- **Shape.** One struct derives from `Effect`, `Instrument` or `NoteEffect`
  (the plugin's kind). `static constexpr Info info` gives its identity and
  buses. Parameters are `Param` members built with factories named like the
  framework's (`decibels`, `frequency`, `milliseconds`, `percent`, `ratio`,
  `choice`, `toggle`, ...); the member name is the patch key, so keys are
  stable across reordering, and the CLAP ID is a hash of the key (an
  explicit `.id(n)` overrides it). Hooks follow the framework's processor
  contract: `prepare()` (main thread: size buffers), `update()` (after
  parameter changes), `process(Block&)` (block-based, in place, double
  precision), `note()`, `midi()` and the voice hooks for instruments, and
  `editor()`/`draw()` for the UI.
- **Types.** `double`, `float`, `int`, `bool`; `Array<T, N>`; structs with
  data members, default member initializers and member functions; references
  as locals and parameters; `auto`; `const`/`constexpr`. No pointers, no
  constructors or inheritance of the script's own, no virtual functions,
  exceptions, templates, overloading, recursion, lambdas in audio code, or
  standard library. Built-in types (`Biquad`, `DelayLine`, `Buffer<T>`,
  `Voices<N>`, ...) come from the runtime.
- **Defined behaviour everywhere.** `Array`, `Buffer` and `Block` indexing
  is checked (an out-of-range read gives zero, a write is dropped, and the
  script's status counts it). Float-to-int conversion is only explicit and
  defined (`toInt`, `floorToInt`, `roundToInt`: saturating, NaN to 0);
  integer `/` and `%` take positive constant divisors (`idiv`, `imod` and
  `wrap` otherwise); shifts take constant amounts; signed overflow wraps
  (exports build with `-fwrapv`). Where C++ leaves evaluation order
  unspecified (operands of `+`, arguments of a call), the compiler rejects
  expressions whose side effects could interfere, so the JIT and every C++
  compiler agree. Exports build the script with `-ffp-contract=off`; every
  executor runs with FTZ/DAZ (ADR-0025).
- **Contexts.** The compiler knows where each function runs: init
  (`prepare`, member initializers: may allocate), audio (`process`,
  `update`, note hooks and everything they call: no allocation, no file
  access, no unbounded recursion), UI (`editor`, `draw`: may format text).
  UI code reads the DSP only through parameters and `Shared<T>` /
  `SharedArray<T, N>` members (atomics; double buffering for arrays), which
  exist identically in exported C++.
- **Drawing.** `draw(Canvas& g)` uses an HTML-canvas-style API (`fillRect`,
  `beginPath`, `arc`, `fillText`, ...) mapped onto the framework's NanoVG
  renderer, plus the framework's widgets (`knob`, `slider`, `toggle`,
  `meter`) in `editor()`. A script with neither gets the framework's
  automatic editor.

The reference (`skill/guides/dsp-language.md`) stays a few pages, and every
example in it is a test.

### 2. The compiler: a C++ front end and MIR

`plugins/suite/compiler` is a self-contained C++20 library (no monorepo
dependency) with a C ABI (`supple/script_compiler.h`): lexer, parser, semantic
analysis (C++'s conversion and overload rules for the subset, name lookup,
the context and evaluation-order checks), plugin extraction (identity,
parameter table, hooks), and code generation into **MIR** (Vladimir
Makarov's JIT, MIT, C11, vendored under `third_party/`). MIR generates
x86-64 and AArch64 code (Linux, macOS including Apple Silicon, Windows
x86-64) and has an interpreter for the same IR, which is the fallback for
hosts that forbid executable memory. Diagnostics are JSON (`code`, `message`,
`line`, `column`, `hint`), the same shape as the bus's errors.
(2026-10-14: the interpreter turned out not to serve as that fallback; see
"Final review" below.)

Back-edges in audio code count against a per-call budget; a script that
exceeds it is stopped, its output silenced and its status set, instead of
hanging the audio thread.

### 3. One runtime

`plugins/suite/runtime` holds `<supple/script.h>` and its implementation:
math, conversions, `Param`, `Block`, `Array`, `Buffer`, shared state, the
canvas shim, and script-facing wrappers over the framework's DSP library
(`dsp::Biquad`, `dsp::Voices`, `dsp::Oversampler`, `dsp::Convolution`,
`dsp::Stft`, the file worker...). The header includes no standard math
header: built-in objects are opaque storage whose methods are compiled once
in the runtime, so a script's unqualified `sin` or `Biquad` resolves to the
runtime in clangd as in an export. JIT code calls the same compiled methods
through C thunks generated from one binding list, so the JIT and an export
run the same machine code for every built-in. General DSP the framework
lacks is added to the framework (§6), never only to the runtime.

### 4. Scripts in Quire run in the player

The **player** is a product of the template plus the suite layer
(`plugins/suite/player`), built like the stock plugins. Its processor
(`supple::suite::ScriptProcessor`) runs a compiled script through the
framework's engine; its editor runs the script's UI through the framework's
GUI stack.

Inside Quire the player is loaded **as a CLAP plugin, through Quire's
ordinary hosting**, rather than as a separate non-plugin engine path. In
library mode its factory lists one plugin per script in the user's script
library (`<library>/<name>/<name>.cpp`, the library folder passed in
`SUPPLE_SCRIPT_LIBRARY`), each with the script's own ID, name and kind.
So a script is an ordinary plugin to Quire: `plugin.list`, `fx.add`,
`fx.param.set`, `fx.patch.*`, presets, automation, project save (by ID and
patch), isolation, PDC and the semantic conformance tool all apply
unchanged, and Quire's host cannot tell a scripted plugin from a stock one
by construction. In export mode the same factory lists the one embedded
script. Quire's own process links the compiler only for diagnostics and the
harness.

Iteration: `dsp.write` saves the script and compiles it in Quire for
structured diagnostics. Each live instance notices the change on its main
thread (a CLAP timer), compiles, and swaps at a block boundary with a short
crossfade, carrying over parameter values by key and state members by name
and type. A failed compile leaves the previous version playing. A change to
the parameter table goes through CLAP's restart and rescan, which the engine
already handles with a crossfaded plan swap.

### 5. Exports

- **Player export.** Copy the prebuilt player for the platform and format,
  append a payload (script, factory patches, assets, identity) and give it
  the script's ID. Single-file CLAP and DLL binaries carry the payload as a
  trailer the module finds through its own path; bundles carry it under
  `Contents/Resources`. clap-wrapper derives the VST3 class ID from the CLAP
  ID, so two exports never collide; macOS AU codes are written into the
  bundle's `Info.plist` and the bundle is signed ad hoc.
- **Source export.** `new_product.cmake` from the template version bundled
  with Quire (`plugins/template`, byte-identical to a released framework),
  then `suite/` (runtime and semantic layer, no compiler), the script
  verbatim under `product/script/`, and generated glue in `product/`
  (`product.cmake` identity, a `Processor.h` that binds the script type,
  factory presets from the script's patches, a README). The parameter table
  stays the script's own (registered at construction, as in the player);
  there is no second copy to keep in step. `suite/tools/update_suite.cmake`
  updates the layer as `update_framework.cmake` updates the framework.

### 6. framework / suite / product

| Layer | Contents | Owner |
|---|---|---|
| `framework/` | The template's, byte-identical to a release | the template repository |
| `suite/` | Semantic-params extension and JSON patches (ported from `plugins/common`, core-params and core-patch, no monorepo dependency), the script runtime, the player glue | Quire (`plugins/suite`) |
| `product/` | A script, or hand-written C++ | its author |

Framework changes, each a general feature made in the template first,
tested there with an example, and brought into Quire as a released version:

1. **Run-time products.** A product may declare its parameter table, its
   identity and its kind and buses at run time, per instance: the engine,
   the CLAP layer, state, presets and the automatic editor take them from an
   instance's descriptor. Compile-time products keep their constexpr tables
   and static checks; their descriptor is a constant.
2. **A layer seam.** Hooks to add CLAP extensions, reach host extensions,
   add a state chunk and read it back, run on the main thread (timers,
   callbacks), list several plugins from one factory, and change the
   parameter table between activations (restart, then
   `CLAP_PARAM_RESCAN_ALL`).
3. **Normalized host values** as an option for linear parameters, so a
   product can follow ADR-0010's domains.
4. **`core/` and `gui/` without CLAP**, enforced by the build, plus an
   offscreen canvas for rendering an editor without a window (the harness).

General DSP blocks the runtime needs and the framework lacks go into the
framework's DSP library the same way.

### 7. State

A scripted plugin's CLAP state is the framework's chunked format with one
added chunk holding the canonical patch, framed as core-patch §4. On load,
parameter values come from the patch (by key) when it is present, so a
session survives a parameter being reordered or its ID changing during
development. The patch document is `params` plus `properties` (file paths
and text the script declares), so Quire's project, which stores only the
patch, keeps everything. `semantic-params-extension.md` §5 is amended to
allow either layout: the patch as the whole state (the stock plugins today)
or as a chunk of the framework's state.

### 8. MCP and the skill

Commands in the `dsp.*` family, on the bus in the engine and the sandbox:
`dsp.write`, `dsp.check`, `dsp.get`, `dsp.list`, `dsp.test` (the harness),
`dsp.preview` (the editor as a PNG), `dsp.export`. Scripted plugins are then
loaded and controlled with the existing `fx.*`, `plugin_*` and `patch_*`
commands. The skill gains `guides/dsp-language.md` (the canonical language
reference) and `guides/dsp-workflow.md` (write, check, test, listen,
export; the stock plugins' conventions for parameters and patches); MCP
resources serve the same files.

### 9. Verification

Differential tests: every example script runs through the JIT, the MIR
interpreter and the script compiled as C++ (the same source, built into the
test executable), and their outputs must be bit-identical; later also
through a built player and a built source export. The semantic conformance
tool runs against scripts in the player and against both exports.

### Implementation notes (2026-10-12)

Where the work stands, and where it departs from the text above:

- Framework items 1 to 3 of §6 are in framework 1.16.0 (Definitions and
  catalogs with `PRODUCT_RUNTIME`, the layer seam in `plugin/Layer.h`,
  `normalizedHostValue()`). Of item 4, the offscreen canvas is in 1.20.0
  (`gui/Offscreen.h`); the build does not yet enforce `core/` and `gui/`
  without CLAP. Framework 1.16.1 adds
  `SUPPLE_PRODUCT_DIR` (a product folder outside the template's tree) and
  `SUPPLE_BUILD_VST3`, and moves the template's CLAP SDK and dr_libs pins to
  the versions in `third_party/`.
- The player (§4) builds from `plugins/template` with
  `-DSUPPLE_PRODUCT_DIR=plugins/suite/player` and this repository's
  `third_party` copies, CLAP only unless `SUPPLE_PLUGIN_VST3`, and is staged
  in `plugins/clap/` with the stock plugins. Its processor is
  `supple::suite::ScriptProcessor` (the glue, `plugins/suite/glue`); its
  editor is the framework's automatic one, since scripts have no UI yet.
- The library (§4, §8) is `AppOptions::scriptDir`: `<data>/<slug>/scripts`
  in the app, `<workdir>/scripts` otherwise. `EngineApp::create` names it
  first in `SUPPLE_SCRIPT_LIBRARY`, which the scanner and bridge processes
  inherit. `dsp.write`, `dsp.check`, `dsp.get`, `dsp.list` and `dsp.test`
  are in the engine and the sandbox, from `host/dsp-commands`; `dsp.write`
  rescans the plugins so `plugin.list` shows a new script at once.
  `dsp.test` (§9's harness) plays a sine, a sweep, an impulse, pink noise
  and silence through a fresh instance with the sandbox's offline renderer
  (`sandbox-render`), and reports levels, gain, THD, the impulse response
  at octave centres and problems in words. `dsp.preview` is not done. The
  MCP server has no resources: the skill carries the guides.
- Player exports (§5), 2026-10-13: `dsp.export` copies the module that
  plays the script (the player on the plugin path, found by the script's
  ID) and appends the trailer `supple/script_export.hpp` defines: a JSON
  payload (`supple.script-export/1.0`: the script's name and source), its
  length and a magic. The player finds its own file with `dladdr`
  (`GetModuleFileNameW` on Windows) rather than CLAP's `init` path, so the
  framework is unchanged; a module with a trailer offers its one script
  and reads no library. Exporting an export starts again from the player.
  Beside the plugin goes one notices file: the player's license and its
  NOTICES (the framework's, and MIR's, which the player's own NOTICES.txt
  adds). The payload carries no patches, assets or identity yet, and
  only single-file CLAP modules are exported (no VST3 or bundles). The
  exported plugin's definition, layer and state are the player's, which
  the semantic conformance tool checks in library mode; the export is
  tested for what differs (it offers only its own script, at the version
  exported, and plays it) in the sandbox's tests.
- Source exports (§5), 2026-10-13: `dsp.export {format: "source"}`. The
  player's definition builder, processor and status layer moved to
  `plugins/suite/glue` and run any `script::detail::Code`
  (`supple/script_code.h`, in the runtime with the description models the
  compiler used to own): the player's compiled `Program`, or
  `NativeCode<T>` (`supple/script_native.h`), the script compiled as C++,
  which takes its description from the script's own `Info` and each
  `Param`'s spec as it is constructed. A patch key comes from a member's
  name, which C++ cannot see, so the generated `product/Catalog.cpp` lists
  the keys the compiler worked out by member pointer (a renamed member
  fails to compile; a `Param` missing from the list takes its `withKey()`
  key or its name in snake_case). This is the one generated table, and
  holds keys only; the parameters stay the script's. The project is not
  made by `new_product.cmake`, which writes Supple Audio's identity:
  Quire copies the template's framework and root files and the suite
  without its compiler (`suite.cmake`, which the player includes too)
  from a kit staged beside the stock plugins (`plugins/script-export`),
  and writes the product's files from `plugins/suite/export` with the
  script's identity, the player's state tag for its ID, and a license
  the caller chooses (MIT or GPL-3.0-or-later). To make every script
  exportable, ids are reverse-DNS without `_` and versions
  major.minor.patch. `plugin.export.source` (slow) exports the player's
  tilt script, builds the project offline with its own tests passing,
  and checks that it and the player play the same bytes. An export's
  `suite/tools/update_suite.cmake` replaces its `suite/` with a newer one
  (another export's, or a newer kit's), as `update_framework.cmake` does
  for `framework/`; the suite carries a `VERSION.txt` for it, and the slow
  test runs it before building.
- Reloading: the layer's main-thread tick looks at the script file twice a
  second, so `dsp.write` only has to save it. An in-place swap carries the
  parameter values over; the script's members start from a fresh
  `prepare()`, under the 10 ms crossfade, rather than being carried by name
  and type. A changed parameter table redefines the plugin (restart and
  rescan).
- State (§7): the chunk is `PTCH` (magic, version, length, canonical
  patch). The patch holds parameters only, since scripts have no properties
  yet. `semantic-params-extension.md` has the amendment to §5 (2026-10-13).
- Instruments, 2026-10-13: a script may derive from `Instrument`. The
  glue gives its notes and controllers to the framework's voice manager
  (`dsp/Voices.h`; framework 1.17.0 adds a voice count chosen at run time
  and block-at-a-time glides), which calls the script's `startVoice(int
  v[, bool retrigger])`, `releaseVoice(int v)` and optional `stopVoice(int
  v)`. The script renders each voice playing and calls `voiceFinished(v)`
  when it has died away, which reports the note's end. `voiceKey`,
  `voiceVelocity`, `voicePitch(v, i)`, `voiceHz(v, i)`, `setVoiceMode`,
  `setGlide` and `setBendRange` reach the manager through the instance. A
  recompile restarts the sounding voices in the new code. Note effects
  stay refused until scripts send notes. `dsp.test` plays notes into an
  instrument (a note and a chord, A4 for the pitch heard, no notes).
- Built-ins over the framework's DSP, 2026-10-13: `Oversampler`, `Stft`,
  `EnvelopeFollower` and `compressorGainDb`, with an example each
  (`plugins/suite/examples`: drive, spectral, a sidechain compressor).
  Scripts pass no callbacks, so framework 1.18.0 gives `dsp::Stft` a
  sample-at-a-time form (`push`, `spectrum`, `pull`) that its callback
  form now runs on. Every example is compiled three ways and compared
  (`script-examples.tests`) and played by the player through the reset
  check (`plugin.player.examples.reset`, under RealtimeSanitizer in that
  preset).
- Per-note expression, 2026-10-13: the glue keeps each voice's CLAP note
  expressions (neutral when its note starts) and, for parameters a script
  marks `polyModulated()` (the framework's `polyModulatable()`), each
  voice's modulation, both routed by note address; `voiceVolume(v)` ...
  `voicePressure(v)` read the first, `param.forVoice(v)` the second, and
  the tuning expression joins `voicePitch`. An instrument declares all
  seven expressions (`noteExpressions`), so the VST3 wrapper offers them.
- Files, 2026-10-13: a script declares `AudioFile sample =
  audioFile("Sample")` or `Convolver room = impulseResponse("Room")` as a
  member, and each is a file property of the plugin (framework 1.19.0
  gives properties a kind, which reaches the loader, and help). The
  framework's file slots load it off the audio thread with the glue's
  `loadFile()` (the audio, resampled; for an impulse response also a
  `dsp::Convolution`, prepared for the session's channels) and hand it to
  the processor at the start of a block, which points the member at it
  (`supple/script_files.h`); the member is only that pointer, so
  generated code copies it as bytes. A reset rebinds the files and clears
  their convolutions; a hot swap moves them to the new code and the old
  lets go (its convolution is silent for the 10 ms crossfade, and the
  tail carries on). Paths travel in the patch's `properties`
  (`semantic-params-extension.md`, amendment of 2026-10-13), checked
  before anything changes. Streaming from disk is not exposed: a sample
  is loaded whole (its first ten minutes).
- Editors, 2026-10-13: a script draws its own editor with `void
  editor(Editor& e)` (once: `e.size()` and the framework's controls,
  `e.knob(param, x, y, w, h)` and the rest, bound by the Param) and `void
  draw(Canvas& g)` (each repaint: an HTML-canvas-style context over
  NanoVG, under the controls), and gets the automatic editor without them.
  It departs from §1 in how the audio reaches the editor: editor code runs
  in an instance of the plugin struct of its own (the glue's
  `ScriptEditor`, `supple/suite/script_editor.h`), where each Param reads
  the value heard and each `Shared<T>` / `SharedArray<T, N>` member what
  the audio code last sent. Those members are plain values on both sides
  (`set()` and `get()`, `[]`), not atomics: the processor copies them all,
  as of one moment, into a triple buffer whenever telemetry is due
  (`SharedChannel`), and the editor copies the newest into its instance. A
  member editor code and audio or init code both use is refused
  (`script.editor.member`), as are writes to Params and Shared members from
  editor code; editor code may format text (`format()`, printf's number
  conversions checked at compile time; `Param::text()`), and runs on a
  loop budget. The layer makes the editor (framework 1.20.0:
  `Layer::createEditor`, so the player keeps the automatic editor as the
  product's), and makes it again after a reload
  (`Instance::rebuildEditor`). `dsp.preview` draws it to a PNG without a
  window: the framework draws an editor offscreen through EGL
  (`gui/Offscreen.h`; `Instance::watch` and `Instance::drawEditor`), and
  the suite layer offers that to hosts as `com.suppleaudio.editor-picture/1`
  (`docs/specs/editor-picture-extension.md`). Not yet: pointer events for a
  script's own interactive drawing, and pictures on macOS and Windows.
- 2026-10-13: a script's drawing responds to the mouse. `pointer(const
  Pointer& p)` hears presses and the wheel where no control is, the drags
  and release of a press it took, hovers and the pointer leaving, and
  edits parameters as a user would (`Param::edit`, `editNormalized`,
  `reset`; only there, which the compiler checks). The glue sends the
  edits through the framework's `Ui` on hidden controls, so they keep its
  gesture rules (framework 1.21.0 adds `onWheel`, `onPointerLeave` and
  `Ui::wheelEdit`; in 1.22.0 one gesture holds every parameter a press
  edits, where it held two). The editor-picture extension's `draw` takes pointer
  input before drawing (framework: `PointerInput`, applied by
  `drawOffscreen`), and `dsp.preview` takes clicks, drags and wheel turns
  and reports what they edited, so an agent checks a script's interactive
  drawing without a mouse. Not yet: pictures on macOS and Windows.
- The compiler's API is C++ (`supple/script_compiler.hpp`,
  `supple/script_program.hpp`); the C ABI of §2 is not written, since
  nothing loads the compiler across a library boundary yet.
- 2026-10-13: agents see what went wrong in a live script. The suite
  offers `com.suppleaudio.plugin-status/1`
  (`docs/specs/plugin-status-extension.md`), a generic extension any
  plugin may implement: current problems, each with a code, a scope, the
  source line where known, a hint, and whether it silences the plugin.
  `fx.describe` and the sandbox's `plugin.describe` read it from the live
  instance (bridge protocol 12 forwards it): loop-budget stops,
  out-of-range indices, editor-code problems, and saves that failed to
  reload while the previous version plays on. Audio observations clear
  when new code is adopted, on reset and on activation. A failed query (a
  crashed bridge) is a host-scoped problem, not a failed description.
  Replacement code always crossfades: what the old code observes while it
  fades out is discarded, and code its budget stopped fades from silence.
- 2026-10-13: factory patches. A script declares them in `void
  factoryPatches(Patches& p)` (`p.add("Warm").set(tilt, -3)`), in patch
  values (a percent as 30), which the compiler checks against each Param.
  The JIT and the script compiled as C++ give the same table, from which
  the suite makes canonical patches and the framework's run-time presets
  (its preset bar and CLAP preset discovery; the framework is unchanged).
  `get_factory_patches` is an optional, size-checked addition to
  semantic-params `/1`, forwarded by bridge protocol 13. The engine lists
  and applies them with `fx.patch.list` and `fx.patch.set {name}`, the
  sandbox with `preset.list/load {source: "factory"}`. A change to a
  script's patches redefines the plugin. Framework 1.23.0 gives the preset
  library's defaults the layer's chunk, so a fresh instance is not
  "Default *". Source exports' patches are checked against the player's.
- 2026-10-13: `io.sidechainConnected` is false while nothing is routed to
  the key: framework 1.23.0 offers CLAP `audio-ports-activation`, and the
  host deactivates an unrouted key port (ADR-0018's note), so the
  compressor example keys from its own input. A host that always feeds
  the port still reports it connected.
- 2026-10-13: `dsp.test` drives what changes over time: parameter
  automation (sample-accurate, in physical units), a timed sidechain key
  (the sandbox renderer's `RenderRequest::sidechain`) and an instrument's
  note sequence. It compares the largest sample-to-sample step with a
  render without the automation (a screen for clicks and zipper noise),
  and the output while the key plays with a silent key (ducking).
- 2026-10-13: `plugin.export.source` (slow) exports and builds every
  example, runs the semantic conformance tool on each, and compares its
  signals, lengths and bytes with the player's (the sampler and reverb
  with generated files, the compressor with a key, instruments with a
  note sequence), and each export passes its own tests. A source export
  takes its kind and bus layouts from the script, and its host tests move
  a level in decibels when it has one (a neutral bell's frequency changes
  nothing).
- 2026-10-19: `dsp.test` takes `sample_rate` (8 to 384 kHz), rendering at
  that rate instead of the session's or project's, and reports the rate it
  used; a sine at or above half of it is refused (`dsp.frequency`).

### Review clarifications (2026-10-13)

The implementation notes above record successive stages; later dated notes
supersede earlier statements that preview, properties or script editors are
missing. Current contracts are in the specs and both DSP guides.

Source export currently requires a visible, automatable parameter because
the generated project's host tests use one. Native C++ exports retain
checked indexing but have no JIT loop-budget instrumentation. Both export
formats include the script's factory patch declarations; neither packages
external samples/impulse responses or captures a loaded instance's patch.
Player exports are Linux CLAP only; other platforms remain unvalidated.
The GUI app and default sandbox CLI share the user library; an explicit
workdir and the headless engine use their own `<workdir>/scripts`.

### Final review (2026-10-14)

- MIR's interpreter is a test and diagnostic backend (the differential
  tests, `SUPPLE_SCRIPT_INTERPRET`), not the fallback §2 describes: it
  allocates on its first call and still generates small machine-code
  shims for calls into C, so it is neither real-time safe nor usable where
  executable memory is forbidden. There is no fallback for such hosts.
  *Note 2026-11-16:* release builds of the Script Player ignore
  `SUPPLE_SCRIPT_INTERPRET`; only Debug builds honour it (review round 33).
- The budget counts calls as well as back edges, so a tree of calls with
  no loop in it stops too. Frames are limited to 64 KiB per function and
  256 KiB along any chain of calls, and nesting to a few hundred levels, so
  neither a script nor the compiler can exhaust a stack.
- The checker refuses what a C++ compiler would (a `noexcept` condition, a
  free function or constant used above its declaration, constant int
  overflow), so a source export builds whatever the player accepted.
- A reload swaps code in place only when the plugin is the same to a host
  and to an agent: everything the Definition says, its meaning, and its
  declared and prepared latency and tail; otherwise it redefines. A
  replacement's `prepare()` sees the running voice count and render mode,
  and `offline()` follows the host's render mode.
- A library holds one script per plugin ID: the player offers the first it
  finds, so the commands refuse a second (`dsp.id_taken`). A script player
  anywhere is left out of the scan cache.
- Framework 1.23.1 serialises the plugin factories (CLAP hosts may scan
  from several threads), so the player's catalog is called one call at a
  time from whichever thread the host scans on.

## Consequences

Scripts reuse all of Quire's hosting and agent tooling with no special
cases. The framework grows a run-time product mode, which is the largest
single change and the one every later step depends on. MIR is a young
dependency for a JIT; the interpreter path and the differential tests bound
the risk, and the backend sits behind the compiler's own C ABI. Windows
support for MIR is CI-tested upstream but untested here.

## Alternatives considered

- **Cranelift** (the brief's suggestion): better code, but Rust. It would
  add a toolchain and dozens of vendored crates to an offline C++ build and
  to every player build, and it has no interpreter for the no-JIT fallback.
- **AsmJit**: robust and portable, but one lowering per architecture and no
  optimizer; **sljit**: no register allocation; **LLVM**: too large to
  vendor or ship inside a plugin; **TCC**: LGPL.
- **A separate in-process script engine in Quire** (the brief's sketch):
  Quire would need a second path, beside CLAP hosting, for state, patches,
  the semantic view, automation, editors, isolation and the conformance
  checks, and the player would no longer be what Quire tested.
- **One generic player ID with the script in its state**: Quire saves only
  the patch for semantic plugins, and keys presets by plugin ID, so the
  script would be lost or every script would share one preset list.
- **A generated static parameter table in source exports** (the brief's
  "parameter declarations" glue): two copies of each declaration to keep in
  step after graduation, and an export whose glue differs from the player's.
- **The patch as the whole CLAP state**: loses the framework's editor size,
  preset tracking and per-project properties in other hosts.
- **A novel syntax, Faust, Cmajor, EEL2, Lua or WASM**: set aside in the
  brief, for LLM fluency and export fidelity.

### Native binary CLAP exports (2026-10-15)

This dated amendment supersedes the Linux-only player-export limit in the
implementation/review notes above. Linux retains the exact appended payload
format. Windows uses the same trailer on a DLL, found with GetModuleHandleExW
and GetModuleFileNameW and read through native wide paths. macOS copies the
CLAP bundle, stores the unchanged JSON schema in
Contents/Resources/supple-script.json, and locates it relative to the module
reported by dladdr. The Mach-O receives no trailer. The complete copied bundle
is always re-signed ad hoc using the system codesign tool before publication;
this seals resources consistently and preserves arm64 loadability. Missing or
failed signing is a structured dsp.sign error, with staging cleanup and the
previous export preserved. No signing identity, notarization or Authenticode
workflow is introduced. Current lifecycle, payload/size, publication and platform
contracts are in specs/sandbox.md. Native conformance verifies the signed bundle,
scanner/direct-host/bridge loading, factory patches and exact same-platform
player/binary/source audio. Headless editor pictures still require EGL in the
template shared stack and remain Linux-only.

### Native headless editor pictures (2026-10-15)

This dated amendment supersedes the Linux/EGL-only picture limit above.
The bundled template is framework 1.24.0 (template commits ccdcdf5 and ee51285).
(Note, 2026-10-18: now framework 1.25.0, which adds a serif face; see ADR-0047.)
`dsp.preview` and the editor-picture extension use its native renderer:
EGL on Linux/BSD, Metal with an unattached layer and synchronized RGBA texture
readback on macOS, and a never-shown WGL window with framebuffer objects on
Windows. Device/driver failures retain `dsp.no_picture`, now with the actual
renderer reason and a platform-specific hint. No picture ABI or schema changes.
The same state draws identically on one rendering setup; pixels need not match
across platforms. Pointer edits still use the normal gesture path and close
before return. CI runs the editor examples and PNG/pointer regressions on every
platform and requires drawing to succeed; Windows gets only Mesa's WGL DLLs,
not its Direct3D replacements. The picture/spec contracts are in
`specs/editor-picture-extension.md` and `specs/sandbox.md`.

### What earns a built-in; Oversampler phase; dsp.test meters (2026-10-20)

A new built-in is added for what a script cannot do in the language: reach
the host, the editor, files or the framework's own machinery, or do work the
loop budget forbids. One that only saves a script some code, however
convenient, is not added. The built-ins already here stay as they are. An
analyser (`Spectrum`) and a Linkwitz-Riley `Crossover` were written under
this amendment and withdrawn by that rule: the stock scripts Yarrow, Teasel
and Hickory already do both in script.

`Oversampler` reaches the framework's oversampler more fully.
`prepare(factor)` takes 1, which passes samples through: before, a used
instance could not go back to 1x, so a restart from 4x to 1x went on at 4x
unless the script passed it by. `prepare(factor, Oversampler::Phase::Linear)`
gives the framework's linear-phase half-bands (65 to 75 samples of latency,
against 3 to 5 for the minimum-phase default), which scripts could not
reach. Teasel offers both as its `phase` parameter.

`dsp.test` reports a semantic plugin's read-only parameters per signal
(`meters`, by patch key, in physical units) as they stood at the musical
end; the renderer records each stage's reported outputs until then. Before,
a script's readouts could be read only through a sandbox batch, after the
tail had moved them.

### A tail that follows the plugin (2026-10-20)

`setTail(samples)` (`-1` forever), from `prepare()`, `update()` or
`process()`, reports how long the output goes on after the input stops when
that follows the settings or the song: a delay's tail is its time, synced to
the tempo, times the repeats its feedback lets through. Before, a script had
only Info's fixed `.tail`, so a delay could say "forever" (renders then end
by silence detection or the cap) or a guess. The framework (1.25.2) now
reads a processor's tail after every block as well as after updates, so a
change made in `process()` reaches the host; a replacement's code starts
from Info's tail, and the old code's fading blocks do not set it.

### Parameters that apply only sometimes (2026-10-21)

A parameter declares when it applies: `.activeWhen(on)` while a toggle is
on, `.activeWhen(mode, {"Bell", "Notch"})` while a choice is one of those
options, `.activeUnless(...)` the other way about, `.activeAbove(voices, 1)`
or `.activeBelow(...)` while a number lies past a value. Conditions name a
parameter declared above (so they cannot loop), all must hold, and the
parameter each names must apply too. They are metadata the host reads, not
something a script could do for itself, so they pass the built-ins rule
above: the semantic layer marks such a parameter `"active": false` in its
schema while its conditions fail, with an `active_when` that says what would
make it apply, leaves it out of `describe`, and an editor, the automatic one
too (framework 1.28.0's `Definition::applies()`), dims its controls (`semantic-params-extension.md`, amendment of this date). The audio
code still reads every value. Before, the flat stock plugins reported every
parameter as live: an agent setting a cut band's gain was told nothing, and
the optimizer's `"auto"` could search dimensions that change nothing.

### Modulation sources and routes (2026-10-21)

A script declares its modulators' outputs as `Source` members
(`voiceSource("LFO 1")` for a value per voice, `source("Macro 1")` for one
the plugin shares, `.unipolar()` for 0..1) and sets their values; the
patch's `routes` member (`"lfo1>filter1.cutoff": {"amount": "+1.5 oct",
"via": "modwheel"}`) says where they go. Routes move a parameter marked
`.modulated()` (global sources) or an instrument's `.polyModulated()` (any
source), as far as its range or its `.routeRange()`. The script reads a
moved value where it uses it, `p.routed(i)` or `p.forVoice(v)`, computed
then from the sources' current values: there is no modulation rate of the
framework's, so renders stay block-size invariant as long as the script's
own ticks are. Instruments have ten sources of their own: velocity, key,
random, pressure and slide per voice; modwheel, breath, expression,
aftertouch and pitch bend for all.

This is what scripts could not do for themselves, so it passes the
built-ins rule: a script can compute an LFO but not give a patch a member
of its own, have its amounts validated in the destination's unit (octaves
for a frequency, dB for a level) with errors that name the unit, carry them
in state and presets, or read controllers (CC 1, 2, 11, channel pressure)
at all. A script-made matrix of parameters (`mod3.source`,
`mod3.destination`, `mod3.amount`) was the alternative; its amounts could
only be percentages of whatever range the chosen destination had, and
agents edit routes keyed by what they do far more reliably than numbered
rows. The routes are kept in a text property of their own (one route a
line, sorted), which the framework already saves, presets carry and the
audio thread receives without a lock; the glue reads it into a fixed
table there without allocating. The semantic layer shows it as the patch's
`routes` member, never among its properties (`semantic-params-extension.md`,
amendment of this date).

`setVoiceLimit(n)` lets an instrument play fewer voices than its pool, a
Voices parameter; the framework's voice manager already allowed it, and
CLAP's voice-info now reports the voices a script's mode and limit allow.

### The route list (2026-10-21)

Routes are edited in the editor as a list, a row a route: the automatic
editor shows it for a script with routes, and `e.routes(x, y, w, h)` places
it in a script's own. The routes' text property is hidden from the automatic
editor (framework 1.29.0), since a raw text field skipped every check a
patch's routes member makes. A list drawn by the script itself was the
alternative, but a script can neither read the routes nor write them, and
giving it both would have meant re-validating whatever it wrote; the list
writes through the patch codec, so it makes only what a patch can. The
automatic editor takes it through a generic hook of the template's,
`AutoPanel` (framework 1.30.0), a panel a layer draws after the parameters;
a plugin with no routes has no panel, so having routes is the switch.

### Traces (2026-10-24)

`trace("name", value)`, and `trace("name", value, i)` at frame `i`, record
a value for a host that traces the plugin: `dsp.test` returns each name's
last, least and greatest values over the signal, in `trace_points`
intervals, and says when one was NaN. It passes the built-ins rule: a
script cannot reach the host but through its parameters, and debugging a
stock plugin meant turning its meters into probes (one, a percent, clamped
the value it carried) and reading only where a render ended. The host side
is a CLAP extension of its own (`trace-extension.md`), so a source export
offers it as the player does; it works only while the plugin is inactive,
so the audio thread's writes and the host's reads never meet and the audio
code needs no lock. While nothing traces, a call is one atomic load. The
compiler holds a script to 16 names, a trace's limit, so none is dropped
while it runs. A stock script ships with none left in (`plugins/AGENTS.md`).
A script's own member named `trace` hides the base one, as in C++, so a
script that has one cannot trace.

### Files through dsp.test (2026-10-24)

`dsp.test` and `dsp.preview` play an audio file through an effect (the
`file` signal, with `audio_file`): material a generator cannot make, a
recorded instrument or a drum loop, now goes through the harness and its
measurements instead of a render and a separate read of the meters.

### Editing in place (2026-10-24)

`dsp.edit` replaces text in a library script (each `old` in exactly one
place, in order, all or none) and then does what `dsp.write` does. An agent
with only the MCP tools changed a line of a long script by sending all of
it back, which costs the whole file each time and can lose code to a slip
in the copy.
