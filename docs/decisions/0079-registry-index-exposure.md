# ADR-0079: Exposure metadata in the registry index

**Status:** Accepted, 2026-11-14. Amends ADR-0007 and ADR-0077.

## Context

The MCP proxy fetches a full description of every registered command at startup
because registry.list reports name, version, kind and summary but omits exposure.
The core catalogue reduces client context while still paying for every remote
description. The registry must remain the single source of exposure policy.

## Decision

Add exposure to every registry.list row, with the existing mcp-tool, mcp-exec and
internal values. Preserve per-version metadata and existing filters. This is an
additive protocol-1 extension; command arguments and versions remain the same.

The proxy fetches the index once, selects latest versions before applying the
chosen core/all exposure policy, and describes only the dedicated tools that
policy selects. Startup requires exactly one index request plus one description
per dedicated tool, excluding transport handshakes. Internal descriptions are
never fetched at startup, including in all mode.

Use kind metadata from the index for generic query admission. A named describe
fetches its contract on demand; exec delegates validation and execution to the
remote dispatcher without fetching a schema first. The compact MCP index keeps
its existing name/kind/summary columns.

Require valid exposure metadata rather than falling back to eager discovery.
A proxy connected to an older application fails with mcp.registry and an
instruction to update both to matching versions. Old proxies accept the extra
field and retain their previous discovery behavior.

## Consequences

Core startup scales with the small dedicated catalogue instead of the entire
registry. All mode describes every latest non-Internal command. Generic commands
retain full schema validation and errors at the dispatcher. Tests count requests,
exercise versioned exposure changes, and validate/execute an undescribed command.

The proxy and application must agree on the index extension even though the
transport remains protocol 1. The installer supplies matching binaries.

### Reconnect catalogue refresh (2026-11-14)

The proxy observes RPC connection generations before admitting cached queries
or dedicated tool names. Explicit and automatic reconnects both refetch the
index. An identical index reuses definitions; a changed index stages all kinds
and the selected dedicated descriptions, then atomically installs the result.
A failed refresh prevents cached admission until refresh succeeds.

The proxy advertises tools.listChanged and queues
notifications/tools/list_changed after a changed-index refresh. Stdio serving
flushes the notification before the triggering reply; handle embedders drain
takeNotifications. The MCP Tools specification's List Changed Notification
section uses SHOULD for notifications when an advertised tool list changes:
https://modelcontextprotocol.io/specification/2025-11-25/server/tools#list-changed-notification

Retry only requests known not to have been sent, after rerouting against fresh
kinds and tool mappings. Never replay a sent-but-unanswered edit. A regression
restarts the peer with a Query changed to Edit, a new query and different tools;
another drops an edit's committed reply and proves it executes only once.

**2026-11-16 note:** [ADR-0087](0087-control-boundary-budgets.md) replaces
error-code-based retry eligibility with explicit local RPC delivery provenance.
Remote structured errors never establish that a command was unsent.
