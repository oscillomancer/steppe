# ADR-0033: DAW and plugin licensing

**Status:** Accepted, 2026-10-14. Amends ADR-0003.
(2026-11-06: amended by ADR-0062 §9: `modules/`, the rack runtime and the
stock modules, is MIT like `plugins/`.)

## Context

The maintainer has chosen a license before publication. The Windows port
needs Steinberg's ASIO SDK; stock plugin binaries must remain free of GPL code.

## Decision

- The top-level LICENSE contains GPL version 3. First-party code is
  GPL-3.0-or-later except for the MIT components below.
- Everything under `plugins/` remains MIT, including the template, suite
  compiler, runtime, semantic layer, player, examples and source export kit.
  Existing export `@LICENSE@` placeholders remain product choices.
- `libs/core-base`, `core-json`, `core-schema`, `core-params`, `core-patch`,
  `core-dsp` and `core-analysis` remain MIT, including their tests. The
  isolated plugin builds use the first six through
  `plugins/common/cmake/LinkMonorepo.cmake`; patch/schema/JSON and DSP/base
  dependencies are transitive. Analyzer links core-analysis, and EQ's GUI
  links it too. No other first-party library enters a stock plugin binary.
  (2026-10-21: `LinkMonorepo.cmake` was removed with Orrery, the last plugin
  built against the monorepo's libraries this way.)
- `cmake/product.h.in` remains MIT: plugin builds generate and include it
  through core-base's product-info dependency.
- `daw/`, `host/`, `mcp/`, remaining `libs/`, `tools/`, `tests/`, `skill/`,
  `docs/` and other first-party files use GPL-3.0-or-later. Nested MIT
  components retain their own LICENSE files; shared libraries gain one.
- ASIO SDK 2.3.4 (2025-10-15) is the sanctioned GPL-3.0-only third-party
  exception, statically linked into the Windows DAW only. It must never
  enter plugins. The manifest declares `scope: daw-only` and
  `platforms: [windows]`; the gate accepts GPL-3.0-only/-or-later only with
  that DAW-only scope and rejects GPL declarations for plugins.
- All first-party code remains original. Do not copy or read other
  GPL/AGPL projects while implementing; ASIO source is the sole sanctioned
  exception. LGPL/MPL still require explicit maintainer approval.

## Consequences

The DAW can use the SDK's GPLv3 option; distributions containing its GPLv3-only
code use GPLv3. Plugins can continue to be distributed under MIT independently.
Changes to plugin dependencies must preserve this boundary and update the map.
Vendored sources and license notices remain unmodified.

## Alternatives considered

A proprietary ASIO agreement would require a signed agreement. Licensing
plugins under GPL would undo their existing MIT distribution terms.
