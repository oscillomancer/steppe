# Framework changes

The framework is versioned on its own (`framework/VERSION.txt`) so a product can
tell which one it carries. Each entry says what a product must do, if
anything, after `framework/tools/update_framework.cmake` brings it in.

## 1.33.4

What a product must do: nothing.

- **Resampling a file works out its filter once for every channel.**
  `dsp::resample` computes each output sample's tap weights once and sums
  every channel with them, instead of again per channel: a stereo file
  resamples in about half the time, a mono one as before, and the result
  is the same to the bit.

## 1.33.3

What a product must do: nothing.

- **The probe plays C2 as well as middle C.** `clap-probe` holds both
  notes from its first block, so a drum sampler whose pads start at C2 (the
  GM kick) is heard instead of failing macOS's embedding test as silent.

## 1.33.2

What a product must do: nothing.

- **A narrow file slot keeps the end of the file's name.** A name too long
  for its slot drops its extension before it is cut, so names that differ
  only at the end (`analog-open-hat.flac`, `analog-cymbal.flac`) still read
  apart in a slot as narrow as a drum pad's.

## 1.33.1

What a product must do: nothing.

- **A narrow file slot keeps the file's name.** A slot shows a factory
  file by its name (`kick.flac`, as its menu lists it) rather than the
  whole `factory:kick.flac`, lets a long name shrink a little before it is
  cut, and leaves out the loaded file's summary (`1 ch, 0.70 s`) when the
  name would not fit beside it; the summary stays in its tooltip. A slot
  as narrow as a drum pad's showed only "fa…".

## 1.33.0

What a product must do: nothing.

- **A file slot's menu offers the factory files.** Its context menu (a
  secondary click) lists, under "Factory", each of the product's
  `PRODUCT_ASSETS` the slot's property takes, by file name, the present
  one checked; choosing one sets `"factory:<name>"`. Until now a factory
  file reached a slot only from a default, a preset or the host, so a
  user could not put one back after choosing a file of their own.

## 1.32.0

What a product must do: nothing. A product that wants files inside its
binary lists them in `PRODUCT_ASSETS` and names them `factory:<name>`.

- **Factory files.** A file property may hold `"factory:<name>"`, which
  names one of the product's `PRODUCT_ASSETS` instead of a path: a sample or
  an impulse response that ships inside the binary, so every host on every
  system finds it and a copied plugin keeps it. `core/Files.h` adds
  `isFactoryReference()` and `factoryFile()`; `exists()` is true while the
  product has the file, and `relocate()` never looks for one it lacks.
  `dsp::AudioReader` (and so `loadAudioFile()` and `DiskStream`) decodes one
  from the binary's bytes. A property's default, a factory preset or a
  session sets one as it sets a path, and saves it as it is. The room
  example ships a plate (`factory:plate.flac`).

## 1.31.6

What a product must do: nothing.

- **An empty Metal frame is defined behaviour.** nanovg_metal's
  `renderFlush` took `&calls[0]` before any call had allocated the array,
  so a frame with nothing drawn applied an offset to a null pointer, which
  the asan preset's UBSan stops on (macOS `ui-tests`). A patch
  (`nanovg_metal-empty-frame`) starts from the array pointer itself; the
  loop over zero calls never reads it.

## 1.31.5

What a product must do: nothing.

- **A reset tells the host the tail it leaves.** `Engine::reset()` (and the
  safety resets after a non-finite block) re-read the processor's
  `tailSamples()` only at the next block, so a host asking right after a
  reset saw the tail from before it. The engine reads it again as it
  reinitializes, and announces a change as it does after a block.
- **State streams are held to what they were asked.** A host stream whose
  `read` reported more bytes than the 4096 it was given made `readAll` copy
  past the end of its buffer, and a `write` that reported more than it was
  given made `writeAll` count bytes it never wrote. Both now refuse the
  stream, and the load or save fails.

## 1.31.4

What a product must do: nothing.

- **`clap-probe` fails cleanly on an empty factory.** A plugin file with no
  plugins in it (a script player given no scripts) crashed the probe on a
  null descriptor; it now says so and exits 1, as the X11 and Win32 probes
  already stop.

## 1.31.3

What a product must do: nothing. On macOS, `ctest` now opens the editor in
a Cocoa window, as it already does in an X11 or Win32 one.

- **`mac-embedding`** (`cmake/Tests.cmake`): the CI wrapper's `clap-probe`
  scenarios (`.github/scripts/clap-probe-mac.sh`, which now takes the
  plugin binary and the probe as arguments) as a test, so the editor's
  embedding is checked wherever the tests run on macOS, not only in CI's
  release job. Registered when the product carries the wrapper.

## 1.31.2

What a product must do: nothing.

- **The fuzz preset builds in a run-time product.** Its build named
  `state-fuzz` and `state-fuzzer`, which a run-time product does not have
  (no tables describe its state), so the build stopped. It now builds
  `fuzz-tests`, which every product has: the state fuzzers when there are
  any, otherwise nothing, and the preset then has no tests to run.

## 1.31.1

What a product must do: nothing.

- **The editor probes wait for a drawn frame** (`tools/x11_probe.cpp`,
  `tools/win32_probe.cpp`). Their pixel checks looked a fixed time after a
  resize or a parameter change, so a heavy editor drawn in software on a
  loaded CI runner could fail them. They now look again until the frame
  is there, for up to about five seconds.

## 1.31.0

What a product must do: nothing.

- **An STFT for analysis only** (`dsp/Stft.h`). `prepare(channels, size,
  overlap, Stft::Mode::Analyse)` gives the same frames and bins with no
  inverse transform and no overlap-add, about half the cost, for an
  analyser or meter that only reads the spectrum. `push()` alone is enough
  each sample, the spectrum stays readable until the next `push()`,
  `process()` leaves the audio as it is and `pull()` gives 0. The default,
  `Mode::Resynthesise`, is unchanged.

## 1.30.1

What a product must do: nothing. A product that glides with `pitchAt()`
and `advance()` may sound different in the last bits.

- **A glide is the same in any host blocks** (`dsp/Voices.h`). A voice's
  glide was summed step by step, so one advanced in blocks of different
  sizes rounded differently, and a render was not bit-identical across
  host block sizes. Its pitch is now worked out from where it started, so
  `tick()`, `pitchAt()` and `advance()` agree to the bit however the
  blocks fall.

## 1.30.0

What a product must do: nothing.

- **A layer's panels in the automatic editor** (`gui/AutoEditor.h`,
  `gui/AutoEditor.cpp`). `gui::automaticEditor(host, panels)` makes the
  automatic editor with panels of a layer's own after the parameters'
  panels: state the parameters do not show, which the layer draws and
  edits itself. Each `gui::AutoPanel` says its title and size, draws its
  body, and takes the pointer there with the rules of `EditorBase`'s own
  hooks; `Ui::popup()` and `Ui::prompt()` give it menus and text fields.
  `product::createEditor()` is the same editor with none.

## 1.29.0

What a product must do: nothing.

- **Properties the automatic editor leaves out** (`core/Property.h`,
  `gui/AutoEditor.cpp`). A property marked `.hidden()` gets no text field in
  the automatic editor: text in the product's own format, that only its own
  controls or its host edit. State and presets keep it as before.

## 1.28.0

What a product must do: nothing. A run-time product whose settings can make
others moot overrides `Definition::applies()`.

- **Parameters that do not always apply** (`core/Definition.h`,
  `gui/AutoEditor.cpp`). `Definition::applies(index, host)` says whether a
  parameter matters with the current values: a band's controls while the
  band is off, a free rate while synced. It is always true unless a
  run-time product's definition overrides it. The automatic editor dims a
  control while its parameter does not apply (`Control::enabledWhen`), so it
  takes no input until it does.

## 1.27.1

What a product must do: nothing.

- **host-smoke no longer requires an effect to change its input**
  (`test/host_contract.h`). The requirement guarded the float/double and
  in-place comparisons against a render that never reached the DSP, which
  other checks catch, and it failed any effect that rightly passes its
  input through. The difference is printed instead. `PRODUCT_TRANSPARENT`
  still demands output equal to input exactly.

## 1.27.0

What a product must do: nothing. A product that passes its input through
unchanged (a meter, an analyser) sets `PRODUCT_TRANSPARENT ON`.

- **A transparent effect** (`test/host_smoke.cpp`, `test/host_contract.h`,
  `cmake/Tests.cmake`). host-smoke demanded that every effect change its
  input, which an analyser must never do. With `PRODUCT_TRANSPARENT` it
  demands the opposite: float and double renders that equal the input
  exactly.

## 1.26.0

What a product must do: nothing.

- **A distance unit: metres** (`core/Param.h`). `Unit::Meters` and its
  factory `meters()`, for a room's size or a source's place: shown as
  "2.4 m", "18.0 m" and "120 m", and read back from "12 m", "12.5 metres",
  "250 cm" or "10 ft".

## 1.25.2

What a product must do: nothing.

- **A tail that changes as the processor plays reaches the host**
  (`core/Engine.h`). The engine read `tailSamples()` only after an update
  or a file, so a tail that follows what the processor hears (a delay's,
  the song's tempo) stayed as it was until a parameter moved. It is read
  after every split now, and the host is told once per change.

## 1.25.1

What a product must do: nothing.

- **host-smoke serves an outstanding restart before its state-and-reset
  check** (`test/host_smoke.cpp`). A product with a restart parameter (a
  lookahead) had its restart asked for when the state check set every
  value, and asks only once until restarted; the next check reset the
  count and so never restarted it, and compared an instance still at the
  old latency with a fresh one at the new.

## 1.25.0

What a product must do: nothing. A product that wants the serif draws with
`font::kSerif` or `font::kSerifItalic`.

- **A serif face** (`gui/fonts/Fraunces-*.ttf`, `gui/Fonts.cpp`,
  `gui/Theme.h`). Fraunces 9pt Soft, SemiBold and Italic, subset to Latin-1
  and common punctuation (about 28 KB each), registered as `serif` and
  `serif-italic` and falling back to Inter. For a product whose name and
  headings want a warmer voice than Plex Sans Condensed. Its OFL notice is
  in `NOTICES.txt` and `gui/fonts/LICENSE-Fraunces.txt`.

## 1.24.0

What a product must do: nothing. Internal callers of the renderer seam's
`canvasTarget*` functions must use the new opaque target API.

- **Windowless editor pictures work on macOS and Windows** (`gui/Offscreen.cpp`,
  `gui/Canvas.h`, `gui/CanvasMetal.m`, `gui/CanvasGL.c`). macOS uses headless
  Metal and synchronized RGBA texture readback; Windows uses a never-shown
  WGL window and an FBO, with a precise failure for drivers without FBOs.
  EGL setup and diagnostics remain unchanged on Linux/BSD. Every target owns
  its renderer and buffers, and readback returns top-row-first RGBA.
- **UI tests exercise the plugin's renderer**, including Metal on macOS.
  CI requires successful offscreen pictures instead of accepting a skip;
  Windows CI uses checksum-verified Mesa 24.3.4 software OpenGL.

## 1.23.2

What a product must do: nothing.

- **The Stages example builds on macOS before 13.3** (`examples/stages/Layer.cpp`).
  Its state used `std::to_chars` and `std::from_chars` for doubles, which
  Apple's libc++ offers only from macOS 13.3 (the deployment target is 11).
  It now writes and reads them with streams in the C locale, as exactly.
- **CI skips `gui-preview` and the AU checks for a run-time product**
  (`.github/workflows/build.yml`), which builds neither.

## 1.23.1

What a product must do: a run-time product's `Catalog` is now called from
whichever thread the host scans on (never the audio thread), one call at a
time; it must not assume the main thread. Nothing else.

- **Repeated editor rebuilds keep the attached editor alive** (`gui/Gui.cpp`).
  A native or fallback update could adopt a replacement before the GUI
  timer collected the previous editor. Rebuilding again in that interval
  destroyed the editor still attached to the window, crashing at its next
  draw or teardown. The previous retirement is collected before another
  replacement is queued.
- **Stages' state is locale-independent and rejects damaged numbers**
  (`examples/stages/Layer.cpp`). Its named-stage chunk uses round-trippable
  ASCII numbers in every locale, refuses malformed or non-finite values
  before applying anything, and reads stage names containing tabs intact.
- **Audio-port activation refuses invalid sample sizes** (`plugin/Plugin.cpp`).
  CLAP permits 0 (unspecified), 32 or 64; other values now fail without
  changing whether the sidechain is connected.
- **Hosts may scan from several threads at once** (`core/Catalog.h`,
  `plugin/Plugin.cpp`, `plugin/PresetDiscovery.cpp`). CLAP lets a host call
  the plugin and preset-discovery factories from any thread, concurrently;
  their lists of descriptors and the catalog were unguarded. They are now
  used under `catalogMutex()`.
- **A redefinition whose layer refuses the carried state says so**
  (`plugin/Plugin.cpp`). The plugin still starts from its defaults; the
  host's log now tells why.
- **`drawOffscreen()` documents its thread** (`gui/Offscreen.h`): the editor
  and what its edits call run on the drawing thread while the caller waits.

## 1.23.0

What a product must do: nothing.

- **A host can deactivate the sidechain** (CLAP `audio-ports-activation`,
  offered with `PRODUCT_SIDECHAIN ON`). A host with nothing routed to the
  key says so before activating, and `block.sidechainConnected` is then
  false though the host still passes (silent) buffers, so a product can
  key from its own input. Before, any buffer was a connection: a host that
  always passes the key port's buffers left a product listening to
  silence. VST3 hosts reach it through the wrapper's bus activation; a
  sidechain bus they leave inactive is now unconnected too. Only the
  sidechain can be deactivated, and only while the plugin is inactive.
- **The defaults are unmodified with a layer's chunk** (`core/PresetLibrary.h`).
  A fresh instance of a product whose layer saves a chunk showed its preset
  bar as "Default *": the library's own defaults and factory presets lacked
  the chunk every save carries. `StateAccess::appendChunks` adds it.

## 1.22.0

What a product must do: nothing.

- **A drawing's gesture holds every parameter it edits** (`gui/Ui.h`).
  `ui().dragEdit` calls until `endDragEdit()` are one host gesture, which
  each parameter joins with its first change, however many there are; a
  burst of `ui().wheelEdit` is likewise one. Before, the gesture held one
  parameter (or two, through the two-control form), and a drawing that
  edited a third on every move ended and began gestures as it went: an undo
  step per move in some hosts. A drawing that closed one handle's gesture
  only by starting another's now closes it at `endDragEdit()`.

## 1.21.0

What a product must do: nothing.

- **The wheel and the pointer leaving, for custom drawing**
  (`gui/EditorBase.h`, `gui/Ui.h`). `onWheel(x, y, dx, dy, mods)` takes the
  wheel before `Ui` does, and `ui().wheelEdit(control, value)` edits as a
  control's wheel does, one gesture per burst. `onPointerLeave()` says the
  pointer left the editor.
- **Pointer input for pictures** (`core/Picture.h`, `gui/Offscreen.h`,
  `plugin/Layer.h`). `drawOffscreen` and `Instance::drawEditor` take
  `PointerInput`s (move, press, release, wheel, leave, in design units)
  and apply them, as a window would, before drawing; the editor then loses
  focus, which ends every gesture they opened. What they edit reaches the
  host: a tool can show, and check, what a click or a drag does.

## 1.20.0

What a product must do: nothing. A layer that overrides nothing new behaves
as before.

- **Editors drawn without a window** (`gui/Offscreen.h`, `core/Picture.h`).
  `drawOffscreen(editor, scale, picture)` draws an editor once, at its
  design size times `scale`, to RGBA, with no window and no display: EGL
  with no surface on Linux (Mesa provides it on a GPU or in software;
  libEGL is loaded when first asked, so plugins gain no link dependency).
  Elsewhere it says it cannot. `ui-tests` draws the starter's editor so.
- **A layer's editors** (`plugin/Layer.h`). `Layer::createEditor(host)`
  may make the instance's editor, in place of `product::createEditor`
  (null leaves it to the product): a family of plugins with an editor of
  the family's. `Instance::rebuildEditor()` makes an open editor again;
  `Instance::watch(on)` sends telemetry as if an editor were open; and
  `Instance::drawEditor(scale, picture, why)` draws the editor without a
  window, so a layer can show a host what the plugin looks like. The
  stages example makes its file plugins' editors and draws them through
  its extension. `gui::Gui` takes the factory it makes editors with.

## 1.19.0

What a product must do: nothing.

- **Kinds of file, and help for properties** (`core/Property.h`). A
  property labels what its file is, `.ofKind(n)`, and `loadFile()` reads
  the label from `FileRequest::kind`: one product can load a sample and an
  impulse response differently. `.help(text)` gives a property a sentence
  of help, as a parameter has. Quire's script player uses both.

## 1.18.0

What a product must do: nothing.

- **The STFT a sample at a time** (`dsp/Stft.h`). `push(channel, x)` takes
  a sample and says when a frame's spectrum is ready in
  `spectrum(channel)`; `pull(channel)` gives the output sample. It is the
  same transform as `process()` with a callback, bit for bit (`process()`
  now runs on it), for code that cannot pass a callback: Quire's DSP
  scripts. Each channel now keeps its own spectrum.

## 1.17.0

What a product must do: nothing.

- **Voices for products that render a block at a time, with a polyphony
  chosen at run time** (`dsp/Voices.h`). `setCount(n, handler)` plays
  voices 0..n-1 of the N it has room for, stopping those above. `pitchAt(v,
  i)` is a voice's pitch `i` samples on, gliding in a straight line, and
  `advance(frames)` moves the glide on by a block; together they replace
  `tick()` per sample. Quire's script player uses them: a script declares
  its voice count and renders whole blocks.

## 1.16.3

What a product must do: nothing.

- **No unused timer constants.** A build without the editor
  (`SUPPLE_BUILD_GUI` off), or of a product without a layer, no longer
  warns about the editor's and the layer's timer intervals, which only
  those use.

## 1.16.2

What a product must do: nothing.

- **No glide before the first block.** A smoothed parameter, or the bypass,
  changed after activation or `reset()` and before any audio (a host's
  render: reset, flush the values it wants, process) now starts at its new
  value rather than gliding to it, so that render depends only on those
  values. Quire's reset check (`clap-reset-check`) failed without it.

## 1.16.1

Two pins moved, the VST3 is optional, and a product's folder can be
elsewhere. What a product must do: run `framework/tools/vendor.cmake` again
if it vendors its sources.

- **CLAP SDK 1.2.10.** The framework fetches it and hands it to clap-wrapper,
  whose own pin (1.2.6) is older; the configure no longer requires
  clap-wrapper to name the CLAP SDK's pin.
- **dr_libs `fa931f3`** (dr_wav 0.14.5, dr_flac 0.13.3), so a product built
  inside a repository that already vendors dr_libs builds from that copy.
- **`SUPPLE_BUILD_VST3`** (on). Off builds the CLAP, and on macOS the AU,
  without the rest of the VST3 SDK; the VST3 tests, packaging and validation
  follow it.
- **`SUPPLE_PRODUCT_DIR`**: the product's folder, `product/` by default. A
  repository with several products and one framework builds each from the
  same tree, `-DSUPPLE_PRODUCT_DIR=<its folder>`. Paths in `product.cmake`
  that start `product/` are in that folder; others are from the root, or
  absolute. The folder's own `LICENSE`, when it has one, ships with the
  plugins. Only a new product's root `CMakeLists.txt` reads the option (an
  existing one keeps `product/`); copy the template's to use it.

## 1.16.0

Run-time products: a plugin the framework learns about when it runs, not
when it compiles. What a product must do: nothing. Its tables, checks and
code are as they were; framework code it reads changed underneath.

- **Definitions.** A plugin is now a `Definition` (`core/Definition.h`): its
  identity and buses (`PluginInfo`), parameters (`ParamTable`), properties
  (`PropertyList`) and factory presets. The engine and its stores, state,
  presets, the CLAP plugin and the editor read the Definition an instance
  was made with. A product with tables makes its one Definition from
  `product.cmake`, `Params.h`, `Properties.h` and `Presets.h`
  (`core/ProductDefinition.cpp`), and every compile-time check stays.
  Classes that took the product's tables implicitly (`ParamValues`,
  `ParamStore`, `PropertyStore`, `state::Snapshot`, `EditBridge`,
  `BasicEngine`, `PresetLibrary`) take a Definition or its tables, and keep
  default constructors that use the product's own. `FileSlots<P>` and
  `PropertyStore` lost their table template parameter. `EditorHost` has
  `pluginDefinition()`; `EditorBase` has `definition()` and `paramTable()`.
- **Catalogs.** The CLAP factory lists a `Catalog` (`core/Catalog.h`): a
  product with tables, its one plugin; a run-time product, whatever
  `product::makeCatalog()` returns, one plugin or many. Preset discovery
  lists one provider per plugin with presets.
- **`PRODUCT_RUNTIME ON`.** No `Params.h`: the product's catalog makes each
  plugin's Definition, and its processor is made from it (a constructor
  taking `const supple::DefinitionPtr&`). Its `product.cmake` kind and
  layouts describe the plugin host-smoke tests. CLAP and VST3 only (an AU
  component is one plugin). The tests and tools that read the tables as
  constants are not built; the product's own tests, host-smoke and the
  validators cover it. The `stages` example.
- **Layers (`PRODUCT_LAYER ON`).** `product::makeLayer()` gives every
  instance a `Layer` (`plugin/Layer.h`): CLAP extensions of its own, a state
  chunk saved after the framework's that can rewrite or refuse a state
  before it is applied, a main-thread tick and callbacks, and
  `Instance::redefine()`, which changes what an instance is -- its
  parameters, buses, name -- keeping its ID: the host restarts it, the swap
  happens at the deactivation with `CLAP_PARAM_RESCAN_ALL` (and an
  audio-ports rescan when the buses changed), the state carries over, and
  an open editor is rebuilt. Layer sources go in `PRODUCT_LAYER_SOURCES`.
- **`.normalizedHostValue()`**: a continuous parameter's host value is its
  0..1 position even when its taper is linear, for hosts that keep every
  continuous parameter normalized.
- `state::appendChunk` and `state::findChunk` for chunks beyond the
  framework's; `state::maxSavedBytes(definition)`.
- A plugin built without an editor (the tsan preset) compiles again: the
  context menu's parameter check was inside the editor's block.

## 1.15.4

Two AU threading bugs in clap-wrapper, found with a new test that hosts the
AU in process. What a product must do: nothing.

- `au-parameter-race` (macOS): the AU registered from its own bundle,
  rendering on one thread, automated from two more, while state loads (each
  a parameter rescan) run on the main thread. The CI workflow now runs the
  tsan preset on macOS too, where it is a race check.
- `SetParameter`, which hosts call from any thread and several at once,
  appended to the event list the audio thread was rendering from; under load
  that corrupted it (pluginval's parameter thread safety test crashed on it
  once). A patch (`auv2-parameter-queue`) passes changes through a fixed
  queue the next render takes.
- A state load rescans the parameters, and the rescan rewrote what
  `SetParameter` reads unlocked: each parameter's info (now only when it
  changed, `auv2-rescan-race`) and the bypass parameter's ID, cleared and
  set again (now atomic and never cleared, in `auv2-bypass-sync`).
  `GetParameterList` takes the parameter tree's lock.

## 1.15.3

What a product must do: nothing.

- `x11-probe` keeps the plugin loaded at exit, so the asan preset passes
  under Mesa's software GL (CI's Xvfb): the driver's own allocations, held
  in its globals, were reported as leaks once unloading unmapped them.

## 1.15.2

Windows passes its tests again, for the first time since 1.3. What a product
must do: nothing; `Presets.h` is written as before.

- Factory presets keep their values and texts by value (`PresetList`), not
  as `std::initializer_list` members, whose storage MSVC lost by the time
  the program ran: loading one of Chord's crashed.
- `files::fromUtf8` and `toUtf8` (and `PresetLibrary`'s, now the same
  functions) no longer throw: on Windows a path that is not UTF-8, as a
  damaged session can hold, aborted the plugin. It is empty instead.
- `files::writeAtomic` waits briefly when Windows refuses to replace a file
  that another write, or a reader, has in hand.
- MSVC's C4702 (unreachable code after `if constexpr (...) return;`) is off;
  two shadowed names and a needless lambda capture are gone, so the debug
  builds pass on Windows and macOS.
- The test runner flushes after each test and, on Windows, names a test
  that crashes; `host-smoke` writes unbuffered.

## 1.15.1

The first CI run since 1.3 found the framework did not build on macOS or
Windows. What a product must do: nothing.

- macOS and Windows link again: the editor refuses drags and pastes with
  `puglRejectOffer`, which pugl defined only for X11. A Pugl patch
  (`pugl-reject-offer`) adds it to both, and on macOS it also clears an
  earlier acceptance, so a drag that has left a file slot is no longer
  taken.
- MSVC builds `Preset` again: it does not implement a braced default for an
  `std::initializer_list` member.
- Chord builds with GCC 13 (Ubuntu 24.04), which crashed on its `Chord{}`.
- `gui-preview` says which step failed when it cannot make its window, and
  `--all` tries such a scenario once more.

## 1.15.0

Small fixes, each with the test that would have caught it. What a product
must do: nothing, unless its tables now fail to build -- a parameter with a
NaN or infinite number, or properties whose limits add up past 64 MiB of
state -- which were saving or showing something no host could use.

- A list's last row, cut off by its bottom edge, takes a click on the part
  drawn (Pulse's preset list had one) and scrolls into view.
- Lists and the graph widgets (`plot`, `trace`, `spectrum`, `waveform`,
  `oscilloscope`) clip within a clip they are drawn inside, rather than
  replacing it, and leave it as it was.
- The RMS meters (`inputRms()`, `outputRms()`) fall, as silence would make
  them, once the host has stopped sending audio, and the editor repaints
  until they reach the floor.
- Parameter tables with a NaN or infinite minimum, maximum, default, curve or
  smoothing time are build errors. So is a set of properties that, all at
  their limits, would save more state than the 64 MiB every reader takes
  (`state::kMaxBytes`, `state::maxSavedBytes()`, now one limit for hosts'
  streams, preset files, preset discovery and pasted text). A preset name
  too long to load back is no longer saved in a session.
- `new_product` refuses Windows device names (CON, PRN, AUX, NUL, COM1-9,
  LPT1-9, before any dot) and names ending in a dot.
- `product-package` refuses an ARM64 Windows build: the installer is x64
  only until ARM64 is supported.
- macOS: a host's `set_size` that would overflow in pixels is refused
  instead of wrapping to a small size (`Window::pixels`); and a new Pugl
  patch (`pugl-mac-drop-location`) gives file drops the view's own
  coordinates, as the mouse has, so a drop lands on the file slot under it.
  Neither has run on macOS yet.
- Linux: a file dialog finishes even when the host reaps its process first
  (a SIGCHLD handler, or children ignored), from the dialog's output.
- A product font (`.ttf`, `.otf` asset) that will not load is drawn in
  Inter, as documented (`loadFace`).
- `gui-preview` refuses `--menu` or `--context` on a control that opens no
  menu, and a scenario that leads back to itself; a click it stages reaches
  the DSP (and a `restartOnChange()` parameter restarts it), as a host's
  edit would. Drive's `shape-menu` scenario, on a segmented control that
  never had a menu, is now `shape-context`.
- `vst3-host` makes every VST3 process call a real-time context, so the
  rtsan preset checks clap-wrapper's own work too, and with note input
  plays 96 notes held at once across a stop and start; it fails without
  the wrapper's note-capacity patch (`clap-wrapper-vst3-note-capacity`).
- The preset list's rows are built once per change to the library
  (`PresetLibrary::revision()`), not for every row drawn.

## 1.14.0

Text fields, custom drags, long text, SVG fill rules and long audio files.
Nothing a product must do; what it may:

- Every text field -- a value's, a property's, `ui().prompt()` -- selects
  by click and drag, a word by double-click, extends with Shift (click or
  arrows), takes Ctrl/Cmd-A, -C, -X and -V through the window's clipboard,
  and puts the caret where a click lands, in text scrolled along or not.
  `Ui::TextEdit` has `anchor` (with `selected()`, `from()`, `to()`) and
  `scroll` in place of `selectAll`; `Ui::setTextWidth()`, which EditorBase
  sets from its fonts, measures for it.
- A press a product's `onPointerDown` takes holds the pointer until that
  button is released: the moves and the release come to the hooks, and
  the wheel and keys wait, so nothing else ends the gesture its
  `dragEdit` opened. A release the hooks never took the press for goes to
  `Ui`, not to `onPointerUp`.
- Long text follows one rule everywhere: shrink toward a minimum, then cut
  at a character with an ellipsis (`widgets::fitText`, for a product's own
  drawing). Knob names and values, slider and readout names (which give way
  to the value), dropdowns, menus, lists, tabs, buttons, the preset name,
  text fields and file slots. A tooltip stays inside the editor: it wraps
  narrower when the editor is, and its last line ends in an ellipsis when
  more would not fit.
- SVGs honour `fill-rule="evenodd"` where contours nest without crossing,
  as icons' holes do.
- `dsp::loadAudioFile(path, out, error, maxSeconds)` and
  `ProcessorBase::loadFile(request, error, maxSeconds)` decode only a long
  file's start; the room example reads ten seconds of an impulse response,
  not the whole file.
- Documented: tempo is constant across a host block; a ramp or a transport
  jump within one is seen from the next.

## 1.13.0

An audit: fixes across the framework, its tools and the examples, each with
the test that would have caught it. What a product must do: run
`benchmark --update` and commit `product/benchmark.txt` (the benchmark now
times the render alone, so old baselines read about 10% high and would hide
a regression of that size); and a product using `dsp::Voices` passes its
handler to `setMode(mode, handler)`. A product showing `dsp::TruePeak` reads
higher, correct, values on high-frequency material.

Engine and plugin:
- Note ends a processor sends from `reset()`, or that the host's queue
  refuses, are delivered at the start of the next block instead of lost;
  note ends sent from `update()` are stamped at that update's frame, not
  the previous block's last.
- The editor's keyboard never loses a note-off when its queue is full (a
  note-on is dropped instead), asks the host to process, and reset clears
  the held keys it shows.
- Activation waits for pending file loads even when the session is
  unchanged, as documented; a file property's non-empty default is loaded.
- Activation delivers every pending property, and a full retirement queue
  asks for the main-thread callback that empties it.
- `polyModulatable()` parameters advertise per-note modulation to CLAP
  hosts; `notAutomatable()` also clears it. Truncated CLAP events are
  ignored rather than read past their end.
- The audio feed carries silence, not the previous block, for an input
  channel the host leaves unconnected.

VST3 and AU (five more clap-wrapper patches, `cmake/patches`):
- AU: a restart-on-change parameter takes effect -- the wrapper now answers
  `request_restart` by deactivating and activating the plugin from its idle
  loop (a render arriving meanwhile is silent), where it used to ignore it.
- AU: a state the plugin refuses to save or load is reported to the host as
  an error, not as saved or restored.
- AU: the native bypass property follows the Bypass parameter when
  automation, the editor or a loaded state moves it.
- VST3: a tail change reaches the host from the UI thread (it was called
  from the audio thread, with no check that the host gave a handler).
- VST3: the notes a host leaves sounding are tracked in a fixed 1024 slots
  made at activation, so many held notes no longer allocate on the audio
  thread.

State and files:
- A/B slots remember their preset by name or file rather than by list
  position: deleting or renaming a preset no longer leaves a slot pointing
  at another one, or past the end of the list.
- Presets and the remembered-folders file are written through a uniquely
  named temporary and replaced atomically (`MoveFileExW` on Windows); a
  failed replacement reports failure and leaves the original. Preset files
  and CLAP preset discovery read at most 64 MiB, refusing special files.

Editor:
- A momentary control, an XY pad or a tab page let go of what they hold when
  focus is lost, the control is disabled or hidden, or the editor closes; a
  menu or text field whose control becomes unavailable closes, discarding
  the edit. An XY pad keeps its place when Shift is released mid-drag.
- One host gesture per parameter even when two slots of a gesture name it.
- A product's `onPointerDown` that starts a `dragEdit` keeps its gesture,
  and pointer motion reaches open menus and text fields first.
- File slots show their path as a tooltip, and Space no longer opens their
  dialog. The meters' last falling frame is drawn.
- `Spectrum::fall(seconds)` lets a trace decay when no audio arrives; the
  starter uses it, so the analyser no longer freezes when the host stops.
- The automatic editor accepts a product with nothing to show.
- Windows: the file dialog's thread ID is published atomically.
- `gui-preview` fails when a scenario names a control that does not exist or
  cannot take input (`preview-invalid-control` test).

DSP:
- `TruePeak` never reads below the sample peak, and its interpolator is
  within 0.5 dB to 0.45 fs (it under-read by up to 7 dB near Nyquist).
- `Voices::setMode(mode, handler)` releases sounding voices on a change of
  mode, so a held note cannot stick. Sustain and CC120/123 are documented as
  omni.
- `Lfo`'s random shapes change at the cycle boundary, not a sample early.
- `Loudness` keeps very loud blocks (above +100 LUFS) in an overflow bin
  that still gates; integrated loudness no longer reads silence for them.
- Decoded non-finite float samples become zero; `DiskStream` refuses a file
  whose length is unknown; `saveWavFile` rejects a bad rate or ragged
  channels before opening the file.

Tools and build:
- `new_product` refuses the starter's and the examples' names and IDs as
  well as their codes, and names with characters not allowed in file names;
  packages strip only a name's final extension and escape it in installer XML.
- `update_framework` refuses a source inside its own staging folders.
- `benchmark` times the render alone, not construction and warm-up.
- A patched copy of a `FETCHCONTENT_SOURCE_DIR_*` checkout is refreshed when
  that checkout changes.
- MSVC with `SUPPLE_SANITIZE` set fails at configure instead of building
  uninstrumented.
- `render_cli` refuses `--at` on a restart-on-change parameter; the offline
  WAV writer reports a failed write.
- The X11 embedding probe uses the plugin's real buses.
- New tests: `bin2c`, `sanitizer-config`, `new-product`, `patched-source`,
  `ci-product-names`, and a realtime-marked runner for prepared DSP
  primitives in `dsp-tests`.
- The template's workflow passes product names to shell steps through the
  environment, safe for any legal name.

Examples: Chord retires a note beyond its capacity; Tone's offline triangle
stays bounded below 20 Hz and at extreme tuning; Duck's reduction trace comes
from each telemetry frame and its history fits any sample rate; Drive's
Quality restart is exercised by the host smoke tests.

## 1.12.0

Packaging and verification. Nothing for a product to do: the new tests run
or skip by themselves. Worth doing once: write `product/previews.txt` with
the editor states worth looking at, and run `benchmark --update` on your
machine and commit `product/benchmark.txt`. A product that pointed
`FETCHCONTENT_SOURCE_DIR_*` at its own checkouts now has them copied into
the build before patching, rather than patched where they are.

- **Release packages**: `product-package` writes an archive of every format
  with the license, notices and install notes; on macOS a `.pkg` (a choice
  per format, not relocatable, the binary's own architectures); on Windows
  an Inno Setup installer when Inno Setup 6.3+ is installed. A `package`
  test checks the archive.
- **Signing hooks**, all opt-in, by configure option or environment:
  `SUPPLE_SIGN_IDENTITY` (Developer ID, hardened runtime; otherwise ad hoc),
  `SUPPLE_INSTALLER_IDENTITY`, `SUPPLE_NOTARY_PROFILE` (notarize and staple
  the bundles and the `.pkg`), and on Windows `SUPPLE_SIGN_COMMAND`.
- **Offline builds**: every pin now lives in `cmake/Pins.cmake` (clap-wrapper's
  SDK pins repeated there, and checked against clap-wrapper's own);
  `tools/vendor.cmake` copies every source into `vendor/`, which a configure
  then builds from with no network. Patches go to a copy in the build.
- **State fuzzing**: `state-fuzz` (every preset) loads thousands of damaged
  states through CLAP from a fixed seed: a refused load changes nothing, an
  accepted one round-trips byte for byte, and processes finitely. The `fuzz`
  preset (clang) builds `state-fuzzer` for libFuzzer and runs it for a
  minute.
- **CPU benchmark**: `benchmark` measures three scenarios in units of a
  reference workload, against a per-machine baseline in
  `product/benchmark.txt` (a ctest in release; skipped where there is no
  baseline). `new_product` leaves the source product's baseline behind.
- **Test helpers** (`Testing.h`): `toneAmplitude`, `thd`, `delayBetween`,
  `gainDb`, `thdOf`, `measureLatency`, `impulseResponse`. core-tests checks
  them on known signals, and that an effect's reported latency is the
  latency measured through it.
- **Validators locally**: `tools/validate.cmake` builds a preset and runs
  pluginval 1.0.4 (fetched, checksummed) on the VST3 and, on macOS, the AU
  and `auval`, plus clap-validator.
- **Named previews**: `product/previews.txt`; `gui-preview --scenario`,
  `--all`, `--list`; a `previews` test renders every one; CI renders them
  all.

## 1.11.0

The DSP library, and files. Nothing for a product to do unless its editor
switches over `Widget` without a default (`File` was added), or its
Processor already has members called `file`, `loadFile`, `describeFile` or
`FileContents` (they are now hooks: rename yours). Every addition is used
by including it, or by declaring a file property.

- **The DSP library** (`framework/dsp/`): header-only but for file
  decoding, depending on nothing of the product's (`dsp/Math.h` now holds
  `kPi`, `dbToGain` and `gainToDb`; `core/Notes.h` the note and MIDI types;
  both still arrive through `core/Dsp.h` and `core/ProcessorBase.h`),
  allocation-free in `process`, the same samples on every platform:
  - `Filters.h`: cookbook biquads with their responses, Butterworth and
    Linkwitz-Riley cascades to order 16, a two-band `Crossover`, `OnePole`
    (low, high, all-pass; safe under modulation), `DcBlocker`.
  - `Dynamics.h`: `EnvelopeFollower`, `RmsFollower`, `GainComputer`,
    `SlidingMax`, `Lookahead` (a limiter's gain, never late), `TruePeak`.
  - `Oversampler.h`: 2x to 16x, linear phase (a pure delay in the passband,
    `latency()` whole samples) or minimum phase (HIIR's elliptic
    half-bands), images and aliases 95 dB down. `Shapers.h`: hard, tanh and
    cubic clippers with antiderivative anti-aliasing.
  - `Stft.h` (overlap-add; `Fft::inverse` added) and `Convolution.h`
    (uniformly partitioned, zero latency by computing the first partition
    directly).
  - `Oscillators.h`: `Random` (PCG32), `Noise` (white, pink, brown),
    polyBLEP `Oscillator`, mip-mapped `Wavetable`, `Lfo` with `syncTo` the
    song position. `Envelope.h`: `Adsr` with curves. `Interpolation.h`:
    Hermite, Lagrange, `FractionalDelay`, `AllpassDelay`.
  - `Voices.h`: allocation and stealing, the sustain pedal, mono and legato,
    glide, bend, MPE, every note reported over exactly once; the sound stays
    the product's.
  - `Loudness.h`: BS.1770 K-weighting (to the specification's table at
    48 kHz), momentary, short-term and gated integrated LUFS, `Correlation`.
  - `Worker.h`: a worker thread and `Handoff<T>`, for results the audio
    thread takes without a lock and never frees. `AudioFile.h`: WAV, AIFF
    and FLAC in (dr_wav and dr_flac, MIT-0, now fetched at configure),
    WAV out, `resample()`, and `DiskStream` for files too long to hold.
- **File properties**: `property(k, "Impulse").audioFile()` (or
  `.file("scl tun")`) holds a path. On a session load the framework
  relocates a file that moved -- under this user's home, under folders the
  user picked files from (remembered per product in its settings folder;
  `SUPPLE_SETTINGS_DIR` for tests), matching as much of the path's tail as
  it can -- reports one found nowhere as missing and keeps its reference,
  and loads it on a worker thread with the processor's `loadFile()`
  (default: decoded audio at the session's rate; declare `FileContents` to
  build anything else there), handing it to `file(id, contents)` at the
  start of a block. Session loads and activations wait for their files.
  `Engine::setProperty` does all this; `Engine::fileStatus` and
  `EditorHost::fileStatus` say what became of a file.
- **File slots**: `ui().fileSlot(property, rect)` shows a file property's
  file and its state, opens the dialog on a click, takes a file dropped on
  it, and clears from Delete or its menu. The automatic editor gives each
  file property one; `gui-preview` reports file states.
- **Smoothing settles in proportion**: the framework's glides now snap to
  their target within a billionth of the parameter's range (in log space,
  of its value), where before it was 1e-9 absolute, which a 200 ms range
  took far longer to reach. `Smoother::setPrecision()` sets it. Output
  differs only below that.
- The engine re-reads a processor's tail, latency and voices after a file
  arrives, as after an update.
- `host-smoke` restarts the plugin when it asked to after a state load, as
  a host does, before comparing it with a fresh instance.
- Tests: `dsp-tests` (new); core, UI and plugin tests for files, relocation
  and file slots, which run in a product with a file property (the new
  `room` example).

## 1.10.0

The editor toolkit. Nothing for a product to do unless its editor switches
over `Widget` without a default (new kinds were added) or calls
`ui().find(0)` to find a preset bar (it is now `kNoParam`). Every addition
is used by declaring it.

- **Layout** (`gui/Layout.h`, where `Rect` now lives): cut a strip off any
  side of a rectangle (`cutTop(h, gap)`, ...), `Grid` cells and spans,
  `row`/`column`, `centred`, and `panelBody(panel)` for what goes under a
  panel's title. Constexpr, so layouts can be constants.
- **Help and tooltips**: `.help("...")` on a parameter is its control's
  tooltip, shown after the pointer rests for 0.6 s; `withTooltip()` gives
  any control one.
- **enabledWhen(rule)**: a control dims and takes no input while the rule
  is false, checked every tick, letting go of whatever it held. Replaces
  setting `enabled` from `animate()`.
- **Controls**: `button(rect, label, action)`; `momentary(param, rect)`, on
  while held; `xyPad(across, up, rect)`; `tabs(rect, labels)` with
  `onPage(k)` / `onAllPages()` for what each page shows; `list(param,
  rect)`, `list(rect, ListSource&)` and `presetList(rect)`; `keyboard(rect,
  low, high)`, which plays notes through the processor (`EditorHost::
  playNote`) and lights the keys held anywhere (`TelemetryFrame::keys`);
  `envelope(rect, attack, decay, sustain, release)`, a handle per stage.
- **Gestures over two parameters**: an XY pad's drag, an envelope's corner
  and `ui().dragEdit(controlX, x, controlY, y)` (an EQ node) are one gesture
  touching two parameters, each begun with its own first change, so an
  axis that does not move never touches its automation. `ui-tests` now
  allows two open at once, never two gestures.
- **The audio, in the editor**: `static constexpr bool kAudioFeed = true;`
  in the Processor copies the main input and output, as the host gives and
  receives them, to an open editor (`core/AudioFeed.h`, lock-free); the
  editor keeps 2.7 s of it (`audio()`, `gui/AudioHistory.h`) and its RMS
  (`inputRms()`, `outputRms()`). `gui/Spectrum.h` analyses it (a small
  radix-2 FFT, `core/Fft.h`; a full-scale sine reads 0 dB).
- **Graphs** (`gui/Plot.h`, `widgets::`): `Axis` (linear, logarithmic),
  `frequencyGrid`, `levelGrid`, `plot(f)` for responses and transfer
  curves, `trace` of a `ValueHistory`, `spectrum`, `waveform` (scrolling),
  `oscilloscope` (triggered), `reductionMeter`, and `meter` with RMS.
- **Assets** (`PRODUCT_ASSETS` in product.cmake, `core/Assets.h`): files
  compiled in and found by name. `drawSvg(vg, "logo.svg", rect, &tint)`
  draws an SVG as vectors (nanosvg, zlib licence, fetched and pinned;
  gradients with their first and last stops, no text or filters);
  `drawImage` a PNG or JPEG (NanoVG's stb_image); a `.ttf` or `.otf` is a
  font face named by its stem.
- **Themes**: `setTheme(theme)` swaps the palette from the next frame.
- **Files**: `fileDialogs()->open/save(options, done)`, never blocking the
  host: zenity or kdialog on Linux, the common item dialog on a thread of
  its own on Windows, NSOpenPanel/NSSavePanel on macOS. Files dropped on
  the editor reach `acceptsDrop()` / `onDrop()`; a `.preset` dropped on an
  editor with presets loads. The preset bar's menu imports and exports
  preset files (`PresetLibrary::exportFile`).
- **The automatic editor**: a product with `PRODUCT_EDITOR_SOURCES ""` gets
  every parameter laid out by group, with the bypass, presets and meters
  (`gui/AutoEditor.cpp`). The new `drive` example has no editor code.
- **Tools**: `gui-preview --tooltip NAME`, `--page N`, `--play KEY`,
  `--drop`; a control can be named by its label (`--focus Light`). It now
  passes the audio feed and played notes through.
- Not done: loudness (LUFS) metering, which needs the DSP library's
  filters; following the system's light or dark setting; keyboard access
  to an envelope's stages (Backspace resets them) and to a momentary
  control. File dialogs and drops are verified on Linux only; the macOS
  and Windows code builds in CI and has not been used by hand.

## 1.9.0

Host integration. Nothing for a product to do: every addition is offered
only to a product that uses it, or works without a line of product code.

- **Context menu**: a right-click on a control opens the host's own menu
  where it can show one (CLAP context-menu popup), with the plugin's "Reset
  to Default" in it; otherwise the editor's, with reset and Enter Value
  first and the host's items (automation, MIDI learn; VST3 hosts' through
  the wrapper) after them. `MenuItem` moved to `gui/Menu.h`.
- **Offline rendering**: a processor declaring `kOfflineMode = true` is told
  when a host bounces (CLAP render; VST3's offline process mode) in
  `setup.offline` and `block.offline`. `render-cli --offline`.
- **Microtuning** (`PRODUCT_TUNING ON`, needs note input): an MTS-ESP client
  (fetched, pinned, 0BSD, its notice added to the product's) and CLAP's
  tuning extension. The processor reads `retuning(key, channel, frame)`;
  unmapped keys never reach `note()` and are retired at once.
- **Note names**: `static void noteNames(const ParamValues&, NoteNames&)` on
  the processor names keys for hosts' piano rolls (CLAP note-name), and
  `noteNamesChanged()` from `update()` tells them to read again.
  `supple::keyName(60, ...)` is "C4".
- **Track info**: `track()` in the editor (name, colour, return, bus,
  master), from CLAP track-info or VST3's channel context, repainting when it
  changes. `gui-preview --track "Name" --track-colour RRGGBB`.
- **Log and thread checks**: `host().log(level, text)` writes to the host's
  log. The framework logs a damaged or foreign state it refused, and, from
  the main thread, what the audio thread found: a non-finite sample
  silenced, notes dropped, buses a host did not provide. A debug build checks
  every CLAP entry point's thread (the host's thread-check, or the thread
  that created the plugin) and logs each call on the wrong one, once.
- `host-smoke` answers `request_callback` after the block or flush that
  asked, as hosts do, not inside it: run inside `process()`, the plugin's
  main-thread work tripped RealtimeSanitizer.
- MIDI controllers and pitch bend through VST3 were already checked by
  `vst3-host` (1.7.0): they reach the processor and render as in the CLAP.
- Not done: AU hosts get no offline mode, note names or host context menu
  (clap-wrapper's AUv2 maps none of them); VST3 hosts get no note names.

## 1.8.0

State and presets. Nothing for a product to do: one that uses none of it
saves the same bytes as before. A product with the old preset-less state
loads into the new framework unchanged.

- **Properties** (`product/Properties.h`, optional; `core/Property.h`):
  state that is not a parameter, by ID, as text or bytes, `.perProject()`
  for what presets leave alone. The processor hears them in
  `property(id, value)` on the audio thread, through a lock-free handoff
  (`core/PropertyStore.h`); the editor edits one with `ui().textField(id,
  rect)` or `host().property/setProperty`.
- **Presets** (`PRODUCT_PRESETS ON`, factory ones in `product/Presets.h`,
  `core/Preset.h`): checked at compile time; user presets saved, renamed and
  deleted in a per-platform folder (`core/PresetLibrary.h`); the editor's
  preset bar (`ui().presetBar(rect)`) with A/B comparison and the whole state
  copied and pasted as text; CLAP preset discovery and preset-load for
  hosts' browsers; the host told what the editor loaded, and that the
  session changed (`state.mark_dirty`).
- **State context**: CLAP's state-context extension. A preset leaves out the
  editor size, perProject properties and the preset name; loading one leaves
  them alone. A session remembers its preset and whether it changed.
- **Editor**: `ui().popup()` for a menu of the product's own (headers,
  separators, disabled items, scrolling when long), which the dropdown now
  uses too, and `ui().prompt()` for a line of UTF-8 text. `Ui::Menu::count`
  is now a method. The window provides the system clipboard (pugl's).
- **Tools**: `render-cli --preset` and `--property`; `gui-preview --preset`,
  `--property`, `--menu Presets`, `--text <property>`.
- `noteValueOf("1/8")`: a note value's plain value, for preset tables.
- clap-validator runs without its `preset-discovery-crawl` and `-load` tests
  for a product with presets: 0.4.1 deadlocks in its own receiver on a
  container's second preset. The framework's tests crawl and load instead.
- Not yet: file references that survive a moved project. They arrive with
  file loading, which needs a file chooser and a background loader first.

## 1.7.1

The CI probes take any kind of product. Nothing for a product to do; a
product with a copy of the template's workflow may take the new one, which
also gets the AU type of a note effect (`aumi`) and of an effect taking notes
(`aumf`) right for `auval`.

- `clap-probe` sizes its audio from the plugin's ports (sidechain, aux
  outputs, surround, none at all), sends a plugin that takes notes a held
  note, and counts a note effect live when it sends notes. `--mono` keeps
  the default layout when there is no mono one, rather than failing.
- `tools/ProbeAudio.h` (`win32-probe`) accepts a plugin with no audio ports
  and reports the output peak.

## 1.7.0

Buses and kinds: what a product switches on in `product.cmake` to take a
sidechain, add outputs, run in surround, take MIDI, send notes, or be a note
effect. A product that switches nothing on keeps its ports, IDs and sound.

- **Capabilities in `product.cmake`** (`cmake/Capabilities.cmake` checks
  them): `PRODUCT_SIDECHAIN ON`, `PRODUCT_AUX_OUTPUTS "Name" ...`,
  `PRODUCT_NOTE_INPUT ON` (an effect taking notes and MIDI: AU `aumf`),
  `PRODUCT_NOTE_OUTPUT ON`, `PRODUCT_NOTE_EXPRESSIONS tuning volume ...`, and
  a third kind, `PRODUCT_KIND "note-effect"` (no audio; AU `aumi`).
- **Layouts**: `PRODUCT_CHANNELS` takes `5.1`, `7.1`, `7.1.4`, `ambi1` to
  `ambi3` and plain counts up to 64. CLAP's surround and ambisonic
  extensions describe them; `setup.layout` tells the processor
  (`core/Layout.h`). Every port is as wide as the main bus.
- **Layout negotiation for VST3**: the plugin implements
  `configurable-audio-ports`, so a VST3 host's `setBusArrangements` selects
  any declared layout. Before, a VST3 host could only have the first one;
  now a stereo product can be mono in VST3 too.
- **Sidechain and aux outputs** in `Block`: `block.sidechain` (silence when
  unconnected, `block.sidechainConnected`), `block.aux(bus)`. Aux outputs
  share the main latency and fall silent under bypass.
- **MIDI, expressions, per-note modulation**: `midi(const MidiMessage&)` gets
  controllers, pitch bend, pressure and program changes (an instrument
  dropped them before); `noteExpression()` and `noteModulation()` get CLAP
  note expressions and per-note modulation of parameters marked
  `.polyModulatable()`. `NoteAddress` matches events to voices;
  `modulatedPlain()` / `v.modulated()` turn an amount into a value.
- **Note output**: `sendNote()` and `sendMidi()` with `PRODUCT_NOTE_OUTPUT`.
  `NoteEnds` became `NoteOutput` (`Engine::noteOutput()`, entries with a
  `kind`): **a product's tests that read `engine.noteEnds()` must read
  `noteOutput()` and keep the `Kind::End` entries.** `noteEnded()` is
  unchanged.
- **Voice info**: a processor declaring `kVoices` (and `kOverlappingNotes`)
  is offered to hosts through CLAP voice-info; `voiceCount()` may say fewer.
  An instrument that already had a `kVoices` constant now advertises it.
- **VST3 note expressions** follow `PRODUCT_NOTE_EXPRESSIONS`. Before, the
  wrapper offered polyphonic pressure alone, to every instrument; **a
  product that uses polyphonic pressure from VST3 hosts lists `pressure`.**
- **Two more clap-wrapper patches**: surround and ambisonic ports get VST3
  speaker arrangements; the plugin's MIDI output reaches VST3 hosts.
- **Tests**: `vst3-host` (Linux) renders the same performance through the
  VST3 and the CLAP and requires the same output on every bus and the same
  notes and MIDI out; `host-smoke-<layout>` runs per declared layout,
  checking channel maps; `plugin-tests` covers every port, port-by-port
  negotiation, mixed MIDI/expression/modulation input, note ports, voice
  info and output event order; `core-tests`' block-size and reset checks
  now feed the sidechain and notes and compare aux outputs and everything
  sent. `test::render` takes `sidechain`, `aux`, `events`, `sent`, `frames`
  and `activate`, and no longer feeds an instrument the input signal.
- **Tools**: `render-cli --sidechain key.wav` and aux outputs written beside
  the output; `gui-preview` plays a sidechain kick and a held chord;
  `new_product -DKIND=note-effect`; `test_examples` runs clap-validator in
  the examples when the template's build knows where it is.
- **Examples**: `duck` (sidechain, aux output, surround and ambisonics) and
  `chord` (a note effect); `instrument` gains pitch bend, the mod wheel,
  tuning and volume expressions, per-note modulation of Level, and a Bend
  Range parameter.

## 1.6.0

Parameters: what a host can display, what the framework smooths, and the
text a level, an angle, a pan or a note needs. Nothing changes for a product
that uses none of it; its sessions, IDs and sound are as they were.

- **Output parameters** (`.output()`): read-only, reported by the processor
  with `setOutput(id, plain)`. The editor sees each block's value; the host
  hears it as `CLAP_EVENT_PARAM_VALUE` (VST3 output parameter changes) about
  30 times a second when it moved, once after every activation, and
  `get_value` returns what it last heard. Never automated, never set by a
  host or the editor, never in state. `ui().readout(id, rect)` shows one;
  any control bound to an output only displays it, and an output repaints
  the editor only where it is shown (or `watch(id)`ed).
- **Framework smoothing** (`.smoothed(ms)`): the processor reads
  `block.smoothed(id)`, the plain value at each frame, glided from each
  change's exact frame (in log space for a log taper), snapped after
  prepare and reset, exactly the target once settled; `block.gliding(id)`
  says whether it moved in this block.
- **Off at the minimum** (`.offAtMinimum()`): a level reads "-inf dB" there
  (anything else "Off") and `v.gain(id)` / `levelGain(def, dB)` is exactly
  0. The editor draws −∞. `host_contract.h` now lets such a level accept
  "-inf" for exactly its minimum and still rejects it everywhere else.
- **Custom text** (`.formatted(format, parse)`) for what no unit covers.
- **Units and factories**: `degrees` (a degree sign in the editor),
  `samples`, `octaves`, `bpm`, `pan` ("50L", "C", "50R"), `midiNote` ("C#4"
  is 61), `cents`, and `noteValue(id, name, "1/8.")` over
  `kNoteValueLabels`, 1/64 triplet to four bars, with `v.beats(id)`.
- **Tempo**: `block.tempo(fallback)` and `beatsToSeconds()`.
- **Examples moved out of the framework**: they are in the template's
  `examples/` now, so products no longer carry them (an update removes
  `framework/examples`). `new_product.cmake -DEXAMPLE=<name>` starts from
  any of them (`KIND=instrument` still works), and
  `framework/tools/test_examples.cmake` builds and tests them all under a
  preset. A new example, `pulse`, uses everything above.
- Tests that follow the product's table skip a feature it does not use
  and say so; the examples exercise them.

## 1.5.0

Foundations for the features that follow: a newer language, a sanitizer for
the real-time rules, and CLAP's reset contract kept exactly.

- **C++20.** Framework and product code compile as C++20. A product that
  relied on something C++20 removed (implicit `this` capture in `[=]`,
  `volatile` compound assignment, comparison rewrites) gets a compiler error
  pointing at it.
- **The presets moved into `framework/cmake/presets.json`**, which the root
  `CMakePresets.json` includes, so future framework updates bring new presets
  with them. **A product must replace its root `CMakePresets.json` with the
  template's** (three lines), adding back any presets of its own.
- **`rtsan` preset**: clang's RealtimeSanitizer. The framework's audio-thread
  entry points (`Engine::process`, `flush`, `reset`; CLAP `process`,
  `params.flush`, `reset`) are marked `SUPPLE_REALTIME` (`core/Realtime.h`),
  so every test that renders aborts, with a stack trace, on any allocation,
  lock, blocking call or file access beneath them -- in the product's
  processor included. Needs LLVM clang 20 or later. A test host's output
  event list that allocates is caught too: `plugin-tests`' now does not.
- **Reset adopts a state loaded just before it.** `Engine::reset()` read the
  values the processor already had, so a state loaded and then reset glided
  from the old values instead of snapping to the new ones, and a reused
  instance did not render what a fresh one with the same state renders.
  `core-tests` (`after_reset_output_depends_only_on_the_state`) and
  `host-smoke` (through the built `.clap`) now check that contract
  bit-for-bit. A product whose `reset()` leaves anything behind -- a filter's
  memory, a random generator's position, an envelope -- fails them.

## 1.4.0

What the first product moved onto the framework (Ferric) needed. Nothing
breaks an existing product; delete `build/` and run every preset.

- **`.plainHostValue()`** for a tapered parameter whose sessions and
  automation were saved with plain values before the product moved onto the
  framework: the host keeps Hz (or ips, or kA/m), the knob keeps its taper.
  New parameters do not use it. `hostTapered()` says which a parameter is.
- **`product/targets.cmake`**, included last when it exists, for a product's
  own programs: measurement suites, command-line tools for analysis scripts,
  further test executables.
- **`EditorBase::usePeakMeters(false)`** for an editor that does not draw the
  framework's peak meters. They otherwise keep the editor repainting for
  levels nobody sees: a product with a noise floor never idled.
- **gui-preview runs programme through the real engine**, so a product's own
  telemetry (a scope, a needle, a trace) is previewed as the DSP produces it,
  not as one resting frame. It also re-asks for the requested scale once the
  window is mapped (KWin puts a new XWayland window back to its first size)
  and refuses to write an image at a scale it did not get.
- Test assumptions that only the starter satisfied:
  - `non_finite_output_never_reaches_the_host` no longer requires a safety
    reset from a processor that cleans its own input; it checks that such a
    processor's output is finite on its own.
  - `a_restart_parameter_asks_once_from_the_main_thread` changes a
    two-option restart parameter to a value that differs, not to "0.5".
  - `silence_does_not_repaint_and_signal_does` feeds real silence (it
    processed in place without re-zeroing, so the second block's input was
    the first block's output), takes a steady-state frame rather than the
    first, and makes its loud frame from signal through the engine.

## 1.3.2

- `update_framework.cmake` replaces `framework/` and nothing else. The
  `asan` preset migration from 1.2.1 and the fallback to the pre-1.2.0
  `framework/VERSION` file are gone: no product was ever generated from
  1.1.0 or earlier. From now on, a framework change that needs an edit
  outside `framework/` says so here, and the product's owner makes it.
- `framework-update` now checks that the update changes no file outside
  `framework/`, removes files the template no longer has, and that each
  refused update leaves the product untouched.

Nothing for a product to do.

## 1.3.1

- Pugl's Windows window creation and class cleanup now use the owning
  DLL's instance handle. A CLAP editor cannot accidentally use the same
  product's VST3 window procedure; closing or unloading one format leaves
  the other's editors intact. `win32-module-ownership` checks both real
  modules together, including two CLAP instances and VST3 unload.
- The Windows embedding probe allocates separate input and output buffers
  from the plugin's advertised audio ports, including mono, multichannel
  layouts and instruments with no input bus. `probe-audio-tests` exercises
  those layouts on every platform and under the sanitizers.
- Both Windows embedding runs now automate a visible parameter after the
  initial paint on an inactive instance, require an unprompted repaint and
  changed pixels, and, without a host timer, require actual `WM_TIMER`
  delivery. Taking a screenshot cannot manufacture the repaint pass.

After updating, delete `build/` and run the presets and Windows CI. No
product IDs, state, parameter ranges or DSP behavior change.

## 1.3.0

The first Windows build: MSVC 2022, x64, built and tested by CI. Nothing
changes on macOS or Linux, and nothing breaks a product's sessions, IDs or
sound. After updating: delete `build/`, run every preset.

- **Windows builds and passes every test,** editor embedding included.
  What it took: `NOMINMAX` and `WIN32_LEAN_AND_MEAN` on every framework
  target (`<windows.h>` arrives through pugl's GL header, and its `min` and
  `max` macros broke `std::min` and `std::max`); the GL 1.2-1.4 enums
  NanoVG uses, which Windows' GL headers lack; the static C++ runtime for
  every binary, matching clap-wrapper (one binary cannot mix the two, and a
  plugin then needs no VC++ redistributable); `_CRT_SECURE_NO_WARNINGS`;
  and MSVC's C4127 and C4324 turned off as noise.
- **Product code must build cleanly with MSVC `/W4`** in the debug preset,
  where warnings are errors. The usual catch is an implicit narrowing GCC
  and Clang accept silently, such as a ternary of CLAP event enums assigned
  to `header.type` (`static_cast<uint16_t>` it).
- New test `win32-embedding` (and `-no-host-timer`): `win32-probe`, the
  Windows counterpart of `x11-probe`, against a real parent HWND, including
  unloading and reloading the module. It needs an OpenGL 2 driver.
- On Windows the built plugins are in `plugins/CLAP/` and `plugins/VST3/`.

## 1.2.1

- `update_framework.cmake` removes `detect_leaks=1` from an existing
  product's `asan` test preset too, changing only that value: other options,
  other presets and the file's formatting are left as they were. Where the
  value cannot be found exactly once it warns instead, to be edited by hand.
  When updating from 1.1.0, bootstrap with this version's updater as described
  below. A product already updated to 1.2.0 must also copy this updater into
  `framework/tools/` first for this migration, then run the update as usual.
- The macOS probe now queries `get_size` after parenting and processing
  events, and checks the native Cocoa child view's dimensions in points.
  CI rejects a wrong realized or resized editor, rather than comparing the
  cached size read before parenting. No plugin runtime behavior changes.

## 1.2.0

The first build and run on macOS since the rebuild, by the new CI workflow
(`.github/workflows/build.yml`, on demand). Nothing breaks a product's
sessions, IDs or sound. After updating: delete `build/`, run every preset.

- **`framework/VERSION` is now `framework/VERSION.txt`.** `framework/` is
  an include root, and on case-insensitive filesystems (macOS, Windows)
  libc++'s `#include <version>` opened that file, so no C++ file compiled
  on macOS. Configure now refuses any file in `framework/`,
  `framework/test` or `product/` that a standard header name resolves to,
  so a `product/Math.h` is caught on Linux before it breaks a Mac.
  The 1.1.0 `update_framework.cmake` looks for the old name, so for this
  one update copy the new `framework/tools/update_framework.cmake` into the
  product first, then run it as usual.
- **Editor sizes set before `set_parent` are honoured.** Pugl applies a
  current size only to a realized view, so a host's `set_size` between
  `gui.create` and `gui.set_parent` was refused and the editor opened at
  1x; `gui-preview --scale 2` rendered at 1x on every platform. `x11-probe`
  checks this now, and `clap-probe --presize` on macOS.
- **Metal draws strokes without `NVG_STENCIL_STROKES`.** nanovg_metal's
  version of it marks partially covered pixels in its first pass and then
  skips them, so strokes lost their antialiased edges and one-unit lines
  vanished at non-integer editor scales. GL is unchanged. Translucent
  multi-segment strokes now double-blend at their joins under Metal; the
  framework's widgets draw none.
- The `asan` test preset no longer forces `detect_leaks=1`, which macOS
  rejects at startup. Linux still detects leaks (its default).
- `clap-probe` sizes its window from the editor's `get_size`, prints the
  backing scale, and takes `--presize`.

## 1.1.0

Fixes from an independent audit. Nothing breaks a product's sessions, IDs or
sound at its settings; rebuild and run every preset.

- **macOS editor size (affects every product on Retina).** Pugl's sizes are
  backing pixels; CLAP's Cocoa sizes are points. They were passed straight
  through, so a 2x screen got a quarter-size view drawn at the wrong scale.
  `Window` now converts at its boundary and draws in pixels. Not yet run on
  a Mac: check `clap-probe` and a real host at 1x and 2x.
- **Instruments must call `noteEnded(address, frame)`** whenever a voice
  ends (silent after release, choked, stolen, reset); the framework sends
  CLAP NOTE_END. Without it the VST3 wrapper's active-note list grows on the
  audio thread forever. Match releases on the whole note address with -1 as
  a wildcard. See the instrument example, which does both.
- Products with custom pointer interaction override `onCancel()` to drop it;
  focus loss and teardown call it. Draw what is heard from `effective(id)`,
  which includes host modulation.
- The non-finite guard now also covers the host's input passing through
  bypass and doubles that overflow a float buffer.
- `block.transport` is advanced to each split's start while playing.
- Tail changes while active are announced to the host.
- The output buffer's `constant_mask` is cleared.
- Editor edits reach a host whose output queue takes one event per block;
  before, a refused END made the VALUE repeat forever.
- State recall asks the host before resizing the editor and keeps its size
  if refused. Sizes beyond 65535 pixels or zero are refused, not wrapped.
- A renderer or font failure fails `set_parent` instead of embedding a blank
  view.
- Choice and toggle labels are limited to 31 bytes at compile time, so their
  text always round-trips.
- Build: CMake 3.27 is the real minimum (clap-wrapper needs it);
  multi-configuration generators are refused (use Ninja or Makefiles);
  Windows test tools define `NOMINMAX`; a newly created
  `product/NOTICES.txt` is picked up without reconfiguring; the Linux VST3
  bundle carries its notices inside; Objective-C classes are prefixed from
  the plugin ID, not the display name.
- Tools: `new_product` accepts only letters and digits for CODE and rejects
  text CMake or C++ would interpret; `update_framework` resolves symlinks and
  swaps the new framework in only once it is copied completely; the WAV
  reader rejects formats it cannot decode.
- `Engine` is now `BasicEngine<product::Processor>`, so framework tests can
  drive probe processors through it. Products see no difference.

## 1.0.0

The rebuild. Products move onto it by porting their DSP into `product/`
(see `framework/AGENTS.md`); there is no automatic migration from the
earlier per-product templates. A product with sessions in the wild must
implement `Processor::loadLegacyState` for its old state format, and keep its
plugin ID, AU subtype and parameter IDs exactly as they shipped.
