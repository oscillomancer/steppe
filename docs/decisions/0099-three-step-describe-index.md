# ADR-0099: A three-step describe index

**Status:** Accepted, 2026-11-22. Amends ADR-0077.

## Context

ADR-0077 made argument-free `describe` return every command as compact rows (name, kind,
summary), targeting under 25 KB. The engine's index reached 24,932 of 27,000 bytes, so the
next few commands would not fit, and every agent that wanted one command paid all of it, every
session. The test that held the budget already said to make the index two-level rather than
raise it again. The maintainer allows breaking existing MCP clients.

## Decision

Group = the part of a command name before its first `.`. Each binary declares one line per
group beside its commands (`Registry::describeGroup`, at most 70 characters; tests fail on a
group with no line or a line with no commands). `describe` then walks in three steps: `describe
{}` lists the groups with their command counts and lines; `describe {group}` lists that group's
public commands as name, kind and summary; `describe {command}` is unchanged. `text`, `kind`
and `tag` search across groups (`text` over names and summaries, ASCII case folded), capped at
60 rows: a longer answer says `truncated` with a hint to narrow, never a count and never a
silent cut. An unknown group, kind or tag is an error naming the valid values; an empty search
says why and names the groups whose line has the word.

Budgets, in compact JSON bytes and tested on the real registries: groups 5,000 (at most 60
groups); a group 4,000; one command 16,000; a search 6,000. A test fails with the name of the
group or command over its budget; the response is to split the group or shorten its text,
never to raise the budget or truncate. The skill's generated command reference is split the
same way: `skill/generated/{engine,sandbox}/index.md` and one file per group, compared as a
tree by the drift tests.

New bus commands: `registry.groups`, and a `group` argument on `registry.list`. Internal
commands are no longer listed by `describe`.

## Consequences

`describe {}` no longer returns every command: a client that parsed `namespaces` out of it
must call `describe {group}` (or run `<slug>-engine registry`, the complete document). There is
no compatibility switch. An agent now pays about 3.5 KB to see every group, one group's rows
(at most 2.3 KB today) and one command, instead of 25 KB; the capacity before a budget fails
is about 13 more groups and, for the largest group, about 20 more commands. A proxy and the app
must come from one installation, as before: an older app has no `registry.groups`.

Spec: command-bus §1.3, §1.6, §1.9, §4.2. Amends the describe paragraph of
[ADR-0077](0077-mcp-core-tool-catalogue.md).
