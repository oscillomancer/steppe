# Generated reference

Everything in this directory is produced from the code and must never be
edited by hand:

- `sandbox/` — the sandbox's command reference, split by group
  (`<slug>-sandbox registry --markdown-dir`): read `sandbox/index.md` (the groups,
  one line each), then the one group's file (`sandbox/<group>.md`). The engine's is
  `engine/`.
- `sandbox-mcp-tools.json` — the MCP tool definitions the sandbox serves
  (`<slug>-sandbox registry --mcp-tools`).
- `engine/` (the same split for the headless DAW, `<slug>-engine registry …`) and
  `engine-mcp-tools.json`.
- `engine.lua`, `sandbox.lua` — LuaLS annotations of the scripting bindings
  (`<slug>-engine registry --lua`): the `q` table of a script, generated from
  the same schemas (`guides/lua.md`).

Regenerate with `tools/gen/update-generated.sh`.
- `plugins/<plugin>.md` — from each stock plugin's semantic schema.
- `notation-reference.md` — from the syntax and tables compiled into
  `libs/notation`, with examples compiled and decompiled by the generator.
- `dsp-examples.md` — the example DSP scripts (`plugins/suite/examples`),
  each tested, with a table of what each one is.

A test regenerates these files and fails when they differ from what is
committed, so the skill can never drift from the installed version.
