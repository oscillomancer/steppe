# Working on the stock plugins

The rules in the root `AGENTS.md` apply here too. This file adds what is
specific to `plugins/`.

## Layout

Every stock plugin is a DSP script (ADR-0047) in a folder of its own here,
holding the script and the product folder a source export makes from it,
built from `template/` with `suite/`. `README.md` says how to change one.
Yarrow (`yarrow`, the EQ), Sundew (`sundew`, the compressor), Treeline
(`treeline`, the limiter), Gorse (`gorse`, the transient shaper), Hickory
(`hickory`, the stereo utility), Sorrel (`sorrel`, the gate), Teasel
(`teasel`, the saturation), Bracken (`bracken`, the delay), Ghyll
(`ghyll`, the reverb), Lichen (`lichen`, the analyzer), Marram (`marram`,
the synthesizer), Withy (`withy`, the chorus), Pollia (`pollia`, the
phaser), Umber (`umber`, the tape machine), Sedge (`sedge`, the
tuner), Gain (`gain`, a level change), Hornbeam (`hornbeam`, the guitar
amp and cabinet), Coppice (`coppice`, the sampler), Pollard (`pollard`,
the drum sampler) and Drumlin (`drumlin`, the drum machine). Orrery, the hand-written synthesizer
Marram replaced, was removed on 2026-10-21; git history has it.

`common/` holds the shared semantic-parameters extension
(`com.suppleaudio.semantic-params`, spec in
`docs/specs/semantic-params-extension.md`), the plugin test tools
(`clap-reset-check`, `clap-block-check`, `clap-state-check`, `clap-unload-probe`,
`semantic-conformance`), `clap-bench` (run by hand: a plugin's cost per
setup and factory patch, and whether its output is bit-identical to a saved
run) and the CMake glue that links plugins against the monorepo's libraries.

The monorepo builds each plugin as an isolated ExternalProject
(`plugins/CMakeLists.txt`, `supple_add_template_product` and
`supple_add_stock_script`); its own test suite runs as
`plugin.<name>.ctest`, apart from the embedding probes, which read an
editor's pixels off the shared screen and so run as
`plugin.<name>.embedding`, one plugin at a time (a `RESOURCE_LOCK`).
Built plugins land in `build/<preset>/plugins/clap/`.

`template/` is the plugin template at a released framework version, a
byte-identical copy (`git archive` of the template's commit): change the
template first, then copy it here, never edit it in place. `suite/` is DSP
scripting (ADR-0032): the script compiler and runtime, the semantic layer
(the extension as a framework layer), and the player, a run-time product
that `plugins/CMakeLists.txt` builds from `template/` with
`-DSUPPLE_PRODUCT_DIR=suite/player` and this repository's `third_party`
copies.

## Rules

- **Shared stack.** The template's shared stack (the window, renderer
  backends, Pugl/NanoVG integration, `Telemetry.h`, the host contract, the
  clap-wrapper patches and the probes) is the template's own: a fix that
  belongs there is made in the template and copied here, so no copy drifts.
- **Template CI before the copy.** A framework change passes the template's
  own native CI (`gh workflow run build.yml -f platforms=<...>` in the
  template's repository, on the platforms the change can affect) before it
  is copied here. The template's CI builds its starter and every example,
  which exercise parts of the framework no stock plugin uses (tables,
  presets, AU, VST3), and Steppe's CI only proves the parts the stock
  plugins use.
- **Permanent identifiers.** The CLAP id (`com.suppleaudio.<id>`), the AU
  subtype, the state tag and every parameter's key identify saved sessions
  and patches. Never change or reuse them; add new parameters with new keys.
  A factory file's name (`factory:<name>`, a stock plugin's samples in its
  `PRODUCT_ASSETS`) is one too, and so is its sound: sessions keep the name
  and play what it holds, so a re-rendered or changed sound ships under a
  new name and the old file stays.
- **One parameter table.** Names, ranges, defaults, units and value text
  come from the script's parameters only. Values in patches and on the bus
  are physical units (ADR-0010).
- **Determinism.** A render depends only on the plugin's state and input:
  seeded randomness, identical output across block sizes, and nothing left
  over after `reset()` (each plugin's reset test checks this).
- **Real time.** `process` and everything it calls follow the root rule:
  no allocation, locks or I/O; the script compiler enforces it for scripts.
- **Extension headers in pairs.** Each CLAP extension header lives in
  `common/include/supple/clap/` (the host's) and, byte for byte, in
  `suite/semantic/include/supple/clap/` (the plugins' and source
  exports'); change both together. `semantic-params-abi.copies` fails
  otherwise.
- **No traces left in.** `trace()` is for debugging with `dsp.test`; a
  committed stock script calls it nowhere, and has no member of its own
  named `trace` (one would hide the built-in, so the script could not be
  traced).

## Specs and guides

DSP and patch contracts: `docs/specs/stock-effects.md` (the effects) and
`docs/specs/marram.md` (the synthesizer). The agent
guides in `skill/guides/<id>.md` describe each plugin's patches for agents;
update them when a patch format or a parameter's meaning changes.

## Checks

The plugin suites run in every preset. Beyond them:
`tools/validator/run.sh build/dev/plugins/clap/<Name>.clap` (clap-validator)
and `python3 tools/reaper-check/check.py --build-dir build/dev --plugin
<CLAP id>` (headless REAPER load, render, automation, state; `--editor`
opens the editor).
