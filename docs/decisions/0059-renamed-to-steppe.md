# ADR-0059: The DAW is renamed Steppe

**Status:** Accepted, 2026-11-04. Amends ADR-0001 (the name; where it is
defined is unchanged).

## Context

ADR-0001 named the DAW Quire. The maintainer judged that name a trademark
risk and chose a new one before the project went further. The stock plugins
are named after plants and landscape (Bracken, Ghyll, Drumlin, Marram), and
the new name follows them.

## Decision

- The DAW is called **Steppe** (`PRODUCT_NAME "Steppe"`, `PRODUCT_SLUG
  "steppe"` in `cmake/Product.cmake`). Binary names, config and data
  directories, socket paths, the MCP server name, the skill name and UI
  strings follow from the slug and name, as ADR-0001 set up.
- Current documents (README, guides, specs, skill, comments) say Steppe.
  Records stay as written: ADR bodies, `docs/research/` and the vendored
  plugin template (`plugins/template/`, a byte-identical copy of its own
  repository) keep the old name.
- Releases up to and including 0.5.0 shipped as Quire; their assets keep
  their names.

## Consequences

- Settings, templates, presets, chains and caches move with the slug
  (`~/.config/steppe` and so on). Nothing carries the files of an
  installation as Quire over; a user moves the folders by hand.
- Agents registered as `quire` (MCP server, skill link) must be registered
  again under the new name from Help.
- Project files do not depend on the name (the GUI saves `<name>.json`, the
  sidecar is `<file>-data/`), so existing projects open unchanged.
- The app icon and the theme notes (gui.md §7) still draw on the image of a
  quire, a gathering of sheets; that is artwork, not the name.
