# ADR-0007: Command bus, registry, control protocol and MCP

**Status:** Accepted, 2026-09-26. Detailed contract: `docs/specs/command-bus.md`.

## Context

Principle 1: every state mutation is a named, typed, JSON-schema'd command;
UI, Lua and MCP use the same bus; the registry is the single source of truth
from which tool schemas, docs and bindings are generated. Phase 2 asks for a
native control protocol with MCP as a thin adapter, so the same protocol later
serves the DAW, Lua and external tools.

## Decision

### Registry (library `command-bus`)

A command is declared once, in C++, with a typed argument struct bound to a
schema through `core-schema` (member-pointer builders, no macros):

- `name` — dotted, lowercase, noun-first: `track.add`, `item.move`,
  `fx.param.set`, `render.region`. Stable forever once released.
- `version` — integer. A breaking change registers `name@2` and keeps `@1`
  behind an adapter until the next major release.
- `kind` — `query` (reads the model, no side effects), `edit` (produces a
  new session version; undoable), `action` (side effects outside the model:
  transport, rendering, file export, device control; not undoable).
- `summary` (one line, imperative), `description` (when and why to use it),
  argument and result schemas with per-field docs, units and examples,
  `errors` (codes it can return, each with a hint), `examples` (argument →
  result pairs), `exposure` (`mcp-tool`, `mcp-exec`, `internal`), `tags`.
- **Examples are tests.** A generated test runs every example through the
  real dispatcher against a fixture session and checks the result against
  the result schema, so docs and schemas cannot drift from behaviour.

The registry exports `registry.json` (JSON Schema 2020-12 per command). From
it we generate: MCP tool definitions, the skill's command reference
(`skill/generated/`), Lua bindings and LuaLS annotations, the GUI action list,
and the native protocol's method table. Generated files are committed; a test
regenerates and diffs them.

### Dispatch

`Dispatcher::execute(Invocation) -> Result<json::Value>` where an invocation
is `{command, args, origin, branch?, txn?}`. `origin` records who acted:
`ui`, `lua:<script>`, `mcp:<client>/<agent>`, `rpc:<client>`, `test`. Every
invocation is validated before execution; validation errors list *all*
problems with JSON-pointer paths and fix hints (closest valid name by edit
distance, valid range with units, expected type with an example).

`edit` commands are pure functions `(const Session&, const Args&, EditCtx&)
-> Result<Session>` executed on the model thread (ADR-0008). `batch` applies a
list of invocations atomically as one transaction and one undo step, with an
optional label; it is the preferred shape for agents. References to objects
accept an ID or a selector (`{"name": "Bass"}`, `{"index": 2}`), resolved at
validation; ambiguity is an error listing the candidates. Logs store resolved
IDs only.

### Events

Each committed version emits `session.changed {branch, version, origin,
label, changes[]}` where changes are `{op: add|remove|update, id, kind,
fields[]}`. Clients subscribe with filters (by kind, id, branch). Transport,
meters and analysis streams are separate rate-limited topics. Nobody polls.

### Control protocol (library `control-protocol`)

JSON-RPC 2.0, one JSON object per line (NDJSON, UTF-8), over a Unix domain
socket at `$XDG_RUNTIME_DIR/<slug>/<instance>.sock` (mode 0600). Methods are
command names; meta-methods under `rpc.` (`rpc.hello` for version
negotiation, `rpc.subscribe`, `rpc.unsubscribe`). Errors carry the structured
`supple::Error` in `error.data`. The dispatcher is transport-agnostic; the
socket server is one adapter, an in-process call API another.

### MCP

MCP is a thin adapter over the same dispatcher, in two deployments:
in-process (the sandbox or engine binary serves MCP on stdio directly) and
proxy (`<slug>-mcp` speaks MCP on stdio and the control protocol to a running
engine). Transport: stdio; streamable HTTP later if cheap.

Tool exposure is chosen per command. A small, stable set of high-value
commands are dedicated tools (`mcp-tool`). The long tail is reachable through
three generic tools — `describe` (search/list commands, return schemas),
`exec` (one command or an atomic batch) and `query` (session tree reads) — so
an agent's context holds a dozen tool schemas, not three hundred. The
sandbox's ~10 commands are all dedicated tools.

## Consequences

One implementation of every behaviour; the UI cannot do anything an agent
cannot. Pure edit functions make undo, replay, branch rebase and testing
straightforward. Schema'd examples give every command a regression test for
free. The generic `exec` tool trades some schema visibility for context
economy; `describe` restores it on demand (progressive disclosure).

## Alternatives considered

- gRPC/protobuf: binary, needs codegen toolchains, poor for LLM-readable
  errors; JSON-RPC matches MCP's own wire format.
- One MCP tool per command: clearest per call but floods agent context in the
  full DAW (hundreds of commands).
- Commands as IDL files with C++ codegen: a second language and build step;
  C++ declarations with member-pointer binding keep types checked by the
  compiler and still export JSON Schema.
- Length-prefixed framing (LSP style): NDJSON is simpler to debug with
  `socat` and every JSON writer we have emits single-line output on request.

### Follow-up clarification (2026-10-04)

Hosts provide invocation savepoints so caught errors inside a batch restore that
call's edits. Scoped admission follows nested calls, including Lua, to enforce
agent branch/project isolation. RPC preserves source locations, enforces framing
bounds before parsing, and reconnects after invalidated calls. Model-loop timer
cancellation guarantees are documented on `rpc::Timer`; cross-thread owner
lifetime requires synchronization.

### Invocation snapshot failure contract (2026-10-04)

`Host::invocationSavepoint()` returns `Result<std::function<Status()>>`.
Capture failure refuses that invocation before its handler runs. A failed
restore is appended to the invocation error's details and poisons the outer
transaction: it rolls back rather than committing even if Lua `pcall` caught
the call error. Further calls in that failed transaction are refused. The
outer error is `transaction.restore_failed`; a failed outer rollback retains
that initiating error in its details. Poison is cleared when that outer
transaction ends. A host without mutable invocation state may return a
successful empty callback.

### Warning replies (2026-11-06)

[ADR-0063](0063-command-warnings.md) defines how context warnings reach
successful replies and their exported schemas across RPC, MCP and Lua.
