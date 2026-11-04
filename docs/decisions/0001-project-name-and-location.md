# ADR-0001: Project name and where it is defined

**Status:** Accepted, 2026-09-26.

**2026-11-04 note:** The DAW is now called Steppe (ADR-0059). How the name is
defined is unchanged.

## Context

The product needed a name, and renaming had to stay cheap. Supple Audio's
product names are short, concrete, crafted nouns: Ferric, Loupe, Plateau,
Vellum.

## Decision

- The DAW is called **Quire**. A quire is a gathering of folded sheets
  (vellum!) sewn into a book, and the archaic spelling of *choir*. It fits the
  family, is one syllable, is easy to type as a command prefix (`quire`), and
  no audio product uses it (a web search on 2026-09-26 found none; quire.io is
  an unrelated task-management app).
- The name is defined once, in `cmake/Product.cmake` (`PRODUCT_NAME`,
  `PRODUCT_SLUG`). C++ reads it from the generated `<supple/product.h>`.
  Binary names, config/data directories, socket paths, MCP server names and
  UI strings derive from it. Code namespaces use the vendor (`supple::`), so a
  rename never touches source files.
- Quire is one monorepo: the libraries, the host, the GUI, the stock plugins
  and their tooling build together (ADR-0002).

## Consequences

Renaming = edit `cmake/Product.cmake`, rename the directory, grep docs.
Review flags hard-coded names. Stock plugin names are chosen per plugin
(ADR-0011) and are not derived from the DAW name.

## Alternatives considered

Loom (taken by a well-known video product), Lathe, Stave (generic),
Palimpsest (apt for branches, too long), Folio (common), Codex (collides
with an existing coding-agent tool).
