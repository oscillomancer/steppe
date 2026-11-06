# Working on modules

The rules in the root `AGENTS.md` apply here too. This file adds what is
specific to `modules/`: the modular racks' modules (ADR-0062,
`docs/specs/rack.md`).

## Layout

- `stock/<name>.cpp`: one stock module each, a DSP script whose struct
  derives from `Module`. The build finds the struct in the file and
  compiles it into the DAW as C++ (`CMakeLists.txt`), with its text, which
  `module.get` shows and `module.fork` copies.
- `rack/`: the rack runtime (modules/rack, MIT): types, instances,
  schedules, the automatic layout.
- `tests/`: `test_stock.cpp` (what every stock module must be),
  `tests/stock/test_<name>.cpp` (each module's own behaviour),
  `module_harness.h` (one module run by itself on the rack's grid).

## Rules

- **The language is the script subset.** A stock module is a script first:
  it must compile in the JIT exactly as it compiles as C++
  (`skill/guides/dsp-language.md`, `skill/guides/modules.md`): no
  pointers, templates, lambdas, standard library, `new`, recursion,
  constructors or C arrays; locals initialised; `idiv`/`imod` for division
  by a variable; float to int only through `toInt` and friends. The JIT
  differential test compiles every stock module and requires the same bits.
- **Identity.** File `stock/<name>.cpp`, id `com.suppleaudio.rack.<name>`,
  vendor `Supple Audio`, version `1.0.0`. The id and every parameter and
  port key are permanent once shipped: add new keys, never rename. A key is
  its member's name in snake_case (`pulseWidth` → `pulse_width`) unless
  `.withKey()` gives one, in the JIT and in the C++ build alike (which reads
  the member names from the declarations, `cmake/stock_module.cmake`:
  declare one `Param`, `Input` or `Output` member per line).
- **Conventions.** Signals as ADR-0062 §2: audio ±1, pitch in octaves from
  C4, gates 0/1 (high at 0.5; triggers 1 ms), CV −1..1, unipolar 0..1,
  phase 0..1. Physical units on parameters, sensible ranges, musical
  defaults (a module should do something good the moment it is patched), a
  `.help()` sentence on every parameter and port, `.in()` groups once there
  are more than six or so controls, `.smoothed()` on anything that scales
  audio directly. Gate inputs use hysteresis (rise at ≥ 0.5, fall below
  0.25); trigger outputs are 1 ms high.
- **Poly.** Loop `for (int c = 0; c < channels(); ++c)` and keep per-channel
  state in `Array<State, 16>`. Read knobs that cables may move with
  `p.at(i, c)` where they matter per frame (cutoffs, levels, rates), so
  every knob is modulatable for free.
- **Determinism.** Randomness from `Random`/`Noise` seeded with `seed()` in
  `prepare()` (plus a `reset` input that reseeds where the module is
  generative). Nothing reads a clock. A module renders the same every time.
- **Real time.** Nothing allocates in `process()`; `Buffer` and
  `DelayLine` are sized in `prepare()`.
- **Original work.** Ideas from Eurorack, VCV Rack and Bitwig modules are
  welcome; their code, names, panel art and data tables are not. Never read
  GPL source while writing a module. Trademarked names (product names of
  other companies' modules) are not used for ours.
- **The face.** Without `panel()` a module gets the automatic layout,
  which suits most. A module with a display (a scope, a step grid, an
  envelope's shape) declares `panel()` with a `display()` and draws in
  `draw()` with `Theme` colours, fed by `Shared` members.

## Checks

`cmake --build build/dev --target rack-stock.tests && build/dev/modules/rack-stock.tests`.
Every module passes `test_stock.cpp`'s generic checks (described, finite
on any input, within budget, deterministic) and has a test of its own that
checks what it is for: an oscillator's pitch, a filter's response, an
envelope's times, a sequencer's steps.
