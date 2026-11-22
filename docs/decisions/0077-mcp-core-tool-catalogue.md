# ADR-0077: A bounded MCP core tool catalogue

**Status:** Accepted, 2026-11-14. Amends ADR-0007 and ADR-0063.

## Context

The engine's 219-tool list consumed 332,544 bytes before the first call.
Clients without deferred tool loading paid for hundreds of schemas and long
result definitions even though generic tools already reached every command.
The maintainer approved breaking existing dedicated-tool clients.

## Decision

Declare only the 37 common engine commands in command-bus §4.2 as McpTool;
other formerly dedicated engine commands become McpExec. Retain the sandbox's
exposure declarations. Pin the exact engine core plus describe/exec/query and
a 60,000-byte complete tools/list frame in tests.

Both the engine's --mcp mode and standalone proxy accept --tools core|all,
default core. All exposes every non-Internal command, including the previous
McpExec set. No command schemas, dispatcher behavior or command versions change.

Dedicated tools omit outputSchema. MCP 2025-06-18 Tools calls it optional;
Structured Content permits structuredContent independently, with matching JSON
text for compatibility. Full result schemas (including ADR-0063 warnings)
remain available through describe. Descriptions use the command description,
or summary when empty, and the existing describe pointer without repeating the
title. Both deployment paths and generated tool definitions follow this rule.

Argument-free describe groups compact [name, kind, summary] rows by namespace,
with column names stated once; existing filters and single-command descriptions
remain. The full engine index targets less than 25 KB. Initialization explains
how to discover and execute commands outside the core.

Spec basis: https://modelcontextprotocol.io/specification/2025-06-18/server/tools
(Tool, Structured Content, Output Schema).

## Consequences

Existing clients using dropped dedicated names must use exec/query or --tools
all. Every command remains reachable and discoverable. Structured results and
bus validation remain unchanged, while clients wanting a result schema fetch it
on demand. All mode intentionally has a larger catalogue. Sandbox definitions
also gain shorter descriptions and omit output schemas, but retain their tools.

### Registry index follow-up (2026-11-14)

[ADR-0079](0079-registry-index-exposure.md) adds exposure metadata to registry.list
so the proxy fetches only selected dedicated-tool descriptions at startup and
resolves other contracts on demand.

### Describe index, two-level (2026-11-22)

[ADR-0099](0099-three-step-describe-index.md) replaces the argument-free describe above:
`describe {}` now lists the command groups, one line each, then a group's commands, then one
command. The 25 KB single index and its budget are superseded; the core catalogue, `--tools`
modes and the 60,000-byte frame are unchanged.
