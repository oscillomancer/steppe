# ADR-0063: Command warnings in successful replies

**Status:** Accepted, 2026-11-06. Amends ADR-0007.

## Context

`Context::warn` and argument decoders collect diagnostics, but dispatch
previously discarded that collection. Existing commands sometimes also
return their own `warnings` arrays. Socket, MCP and Lua callers need the
same diagnostics without changing successful replies that have no bus
warnings, or weakening a handler's result validation.

## Decision

After handler result validation and successful transaction completion,
Dispatcher merges context diagnostics into the result's `warnings` array.
Each bus diagnostic is `{code, message, path}`. Its path is retained exactly
as emitted: decoder paths are relative to the arguments object; handler
paths can identify arguments or a document. An empty path is global.

Command-owned warnings stay first, including strings and richer objects.
A context diagnostic already present in that list by code, message and
path is not duplicated. Other context diagnostics follow in emission order.
A command-owned non-array warning becomes the first array entry when bus
warnings are appended. Non-object results with bus warnings become
`{result: <original>, warnings: [...]}`; otherwise replies keep their shape.
A failed invocation returns the existing structured Error, with no success
warning payload. Batch diagnostics stay on the corresponding step's result.

The registry advertises optional warning metadata and mixed warning items,
and describes boxed non-object results, while retaining the raw schema for
handler validation. Markdown, MCP schemas and Lua annotations reflect the
same contract. RPC result values, MCP text/structured content and Lua command
returns use this single dispatcher result without transport-specific merging.
This is an additive protocol-1 extension; existing warning-free responses
and command versions are unchanged.

Review refinement (2026-11-06): per-command warning schemas use only
`{type: "array", items: {type: "object"}}`; legacy non-object warning items
remain allowed. Closed result schemas still admit diagnostics, while the
shared `{code: string, message: string, path: string}` contract is stated
once in the reference header, this spec/ADR and MCP server instructions.
This avoids repeating the same field descriptions in every tool definition.

## Consequences

Callers can inspect warnings consistently and command-specific diagnostics
are preserved. Consumers of warning-bearing scalar or array results use
`result` to obtain the original value. Scripts receiving a command result
can inspect or return its warnings; a batch caller inspects each step.
Diagnostics are local to an invocation and do not leak to subsequent calls.
