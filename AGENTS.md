# Working on Quire

This file is for anyone changing or reviewing Quire, coding agents and
people alike. Quire is an LLM-native DAW for Linux, macOS and Windows plus a
suite of stock plugins (see `README.md`). The code is past the build-out stage: most work
now is maintenance, polish, new features on existing foundations, and
review. Treat existing behaviour, tests and documents as deliberate unless
you find evidence otherwise.

## Orientation

Read before designing anything:

1. `ARCHITECTURE.md`: the system map, threads and the real-time model.
2. The spec for the component you touch (`docs/specs/`) and the ADRs it
   cites (`docs/decisions/`). Specs are the contracts; ADRs say why.
3. `STATUS.md`: what works, known limits and open items in that area.

User-facing guides are in `docs/guides/`: `installing.md` (the packages),
`building.md` (building from source on each platform) and `manual.md`
(using the app, plugins and agents).

When code, a spec and an ADR disagree, do not silently pick one. Find out
which is right (tests and git history usually tell), fix the others in the
same change, and say what you found.

## Rules

1. **Licensing.** First-party DAW code is GPL-3.0-or-later; `plugins/`
   and the plugin-linked libraries remain MIT (ADR-0033). All first-party
   code and assets are original: never copy code from GPL/AGPL projects
   such as Ardour, or code, themes, icons or other assets from proprietary
   software. Do not read GPL source while implementing; notes about GPL
   software are prose in `docs/research/`, and someone else implements from
   them. Dependencies must be MIT,
   BSD-2/3, ISC, Zlib, Apache-2.0, BSL-1.0, CC0/public domain or Unlicense
   (fonts: OFL-1.1), vendored under `third_party/` with their license file,
   declared in `third_party/manifest.json` and listed in `THIRD_PARTY.md`.
   The ASIO SDK is the sanctioned GPL-3.0-only exception, Windows DAW
   only, never plugins; its source may be read for that integration. LAME
   is the sanctioned LGPL-2.0-or-later exception (ADR-0055), DAW only,
   never plugins or the MIT libraries.
   `tools/license-check` fails the build otherwise. LGPL and MPL code needs
   the maintainer's explicit approval first.
2. **Real-time safety is absolute.** Audio-path code never allocates,
   frees, locks, blocks, logs synchronously, throws, or touches files or
   the network. State reaches it through lock-free queues and atomic
   pointer swaps. Audio-path entry points are marked `SUPPLE_RT` and their
   tests run under RealtimeSanitizer (the `rtsan` preset).
3. **The product name lives in one place.** `cmake/Product.cmake` defines
   it, and C++ reads it from the generated `supple/product.h`
   (`supple::product::kName`, `kSlug`, …). Never hard-code "Quire" or
   "quire" in code, binary names, paths, socket names or UI strings. C++
   lives in namespace `supple::` (the vendor). Docs may say "Quire".
4. **Quire leaves the user's system alone.** The app and its installers
   change no system-wide configuration: no audio server settings, drivers,
   services or file associations. A rate or buffer size the user chooses
   is applied to the device, which may keep it after Quire quits (ADR-0050).
   Anything written outside Quire's own folders, such as the agent
   registration and skill link from Help, happens only when the user asks.
5. **Stock plugins keep the template's shared stack.** The stock plugins
   are instances of Supple Audio's plugin template. Their shared
   infrastructure (GUI stack, wrapper patches, `host_contract.h`,
   `Telemetry.h`, probes) stays byte-identical to the template apart from
   naming; see `plugins/AGENTS.md`.
6. **Every change works on Linux, macOS and Windows.** Quire ships on all
   three, and you are probably working on only one of them. A fix or
   feature covers every platform: when the code you touch has
   platform-specific paths (`_WIN32`, `__APPLE__`, `__linux__`,
   Objective-C++, per-platform sources), find the counterparts and fix them
   in the same change. Look for the same class of bug on the others:
   assumptions about environment variables, paths, sockets, file sharing,
   threads, audio/MIDI APIs or windowing rarely carry over unchanged.
   A local Linux run says nothing about macOS or Windows; native CI (see
   Build and test) proves them before the work merges. A gap you cannot
   close goes in `STATUS.md` and in your hand-back report.

## Making a change

- **Contracts first.** A change to a frozen interface (a library's public
  header, a command's schema, the control protocol, a file format, the
  bridge protocol) updates its spec in the same commit. A change of
  direction gets a new ADR that amends or supersedes the old one; ADR
  bodies stay as written apart from a dated note pointing to the new ADR.
- **Commands** are declared once in C++ (`host/engine/src/*commands.cpp`,
  `host/sandbox/src/`). Summaries are at most 80 characters; examples run
  as tests; errors carry `code`, `message`, `path` and a `hint` that tells
  the caller how to fix the call. After changing any command, run
  `tools/gen/update-generated.sh` and commit `skill/generated/`; a drift
  test fails otherwise.
- **Project files and other formats** are canonical JSON with a schema
  field. When you raise a limit or add a field, change the model's schema,
  the command's argument schema and the docs together, and add a save and
  reopen round trip to the test (a limit raised in only one place once
  produced projects that saved but would not reopen). Old files must keep
  loading; `host/engine/model/src/migration.cpp` handles older schemas.
- **Engine.** Offline renders are deterministic and the real-time output
  must stay bit-identical to them, in both executors (the in-callback one
  and the anticipative renderer) and through the bridge. Test new engine
  behaviour against those invariants (`host/engine/tests/test_realtime.cpp`
  shows how), and run new audio-path tests under the `rtsan` preset.
  Nothing may sit between a Console 9 encode and its decode (ADR-0019).
- **GUI.** Check visual changes in a real render:
  `build/dev/daw/gui/quire --offscreen --no-socket --audio dummy --generate 6
  --size 1400x900 --frames 5 --snapshot out.png` (add `--action ui.mixer`
  and similar to open panels). Golden images regenerate with a full run of
  the suite (`SUPPLE_UPDATE_GOLDEN=1`, no `-tc` filter); look at every
  changed PNG before committing it. UI behaviour is specified in
  `docs/specs/gui.md`; update it with the change.
- **Docs.** Update the spec, and `STATUS.md` when a feature lands, a known
  limit appears or one is lifted. Keep `STATUS.md` a picture of the present,
  not a log; history is in git. When something a user sees changes (a
  menu, a dialog, a default, a file location, a build requirement or a
  package), update the guide in `docs/guides/` that describes it.

## Build and test

```sh
cmake --workflow --preset dev    # configure, build and test (Clang, Debug)
tools/ci/format.sh               # clang-format; --check to verify only
tools/ci/run-all.sh              # full local CI: license, format and every preset
tools/ci/clean-clone.sh          # fresh offline clone of HEAD: build and test (dev)
```

`docs/guides/building.md` lists each platform's requirements. Presets:
`dev`, `release`, `gcc`, `asan` (with UBSan), `tsan`, `rtsan` (Linux),
`macos` and `windows`. Build the whole `gcc` preset when checking GCC, not only test
targets: warnings such as `-Wshadow` show up in the app sources.
`run-all.sh` prints a summary table and exits non-zero on any failure; read
both. A full run takes about half an hour. `clean-clone.sh` tests committed
files only, from scratch and with downloads disabled: run it before a
release and after adding files or dependencies. Keep `TMPDIR` short; socket
tests fail when their path exceeds the 108-byte Unix socket limit.

Native CI (`.github/workflows/build.yml`) builds and tests Linux, macOS arm64
and Windows (clang-cl) on GitHub, on demand only. To run it, push the branch,
then `gh workflow run build.yml --ref <branch> -f platforms=<linux|macos|windows|all>`
and `gh run watch <id> --exit-status` (`gh run view <id> --log-failed` for
failures); `CONTRIBUTING.md` has the details. It must pass before a branch
merges to `main`.

Code conventions: C++20, CMake and Ninja, warning-clean with
`-Wall -Wextra -Wpedantic` on Clang and GCC; `.clang-format` at the root
(23.1); doctest, at least one test executable per library, named after it
(`core-json.tests`); `supple::Result<T>` instead of exceptions across
library APIs; library headers under `libs/<name>/include/supple/<name>/`,
small and documented. Text formats are canonical: identical content
serializes to identical bytes.

CTest labels: `rt` (audio-path tests, run everywhere), `tsan-skip`
(timing-heavy tests excluded from `tsan`), `slow` (end-to-end tests left
out of `tsan` and `rtsan`; `plugin.export.source` also skips `gcc` and
`asan`, where its unsanitized nested builds would repeat `dev`). A test
that depends on wall-clock time must not decide pass or fail by it.

Ordinary tests never open real devices or desktop windows: they use the
dummy audio backend and offscreen rendering, and so should manual runs of
the app that do not need sound (`--audio dummy`). Opt-in tests, skipped
unless their variable is exactly `1`: `SUPPLE_AUDIO_DEVICE_TEST` opens the
real audio device (two seconds of silence); `SUPPLE_GUI_DISPLAY_TEST` opens
plugin editor windows on the native display; `SUPPLE_MIDI_DEVICE_TEST` opens
native MIDI inputs on macOS/Windows (with a fake audio driver, so no audio
device is opened).

Other checks, run on demand: `tools/validator/build.sh` and
`tools/validator/run.sh <plugin.clap>` (clap-validator 0.4.1, built in the
user's cache); `python3 tools/reaper-check/check.py --build-dir build/dev`
loads each stock plugin headless in REAPER with a private configuration
(`tools/reaper-check/README.md`).

## Reviewing

A review looks for, in this order:

1. Correctness, with a concrete failure scenario for every finding.
2. Real-time safety on the audio path, and RT/offline equivalence.
3. Determinism of renders and serialized formats.
4. A test that fails without the change (for fixes) or covers the new
   behaviour (for features), at the right layer.
5. Structured, actionable errors at the command and file boundaries.
6. The spec, ADRs, guides, generated files and `STATUS.md` updated with the
   code.
7. No new license exposure, no hard-coded product name.
8. The change holds on Linux, macOS and Windows, and native CI has run, or
   is due before the merge, where it touches shared or platform-specific
   code.

Report findings with file and line, what goes wrong, and how to see it.
Separate findings from suggestions.

## Commits

One topic per commit, staged with explicit paths. Subject line
`Area: what changed` (for example `Engine: plug-ins run while stopped`),
with a body that says why when it is not obvious. Keep formatting-only
changes in their own commit. `main` moves only on a green `run-all.sh`.

## Definition of done

The code, its tests and its docs are updated; the relevant local presets
pass; native CI has passed on every platform the change can affect before it
merges; `STATUS.md` reflects the change; there are no new license issues;
and someone other than the author has reviewed it. Never report something as
working that you did not run. When you hand work back, list the files
changed, the exact commands you ran with their results, and the open
questions, including any native CI still to run.
