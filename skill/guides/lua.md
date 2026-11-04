# Scripting Steppe with Lua

Steppe runs Lua 5.4 scripts on the model thread, against the same command bus
the GUI and agents use (ADR-0006). A script is one command:

```json
{"command": "script.run", "args": {"name": "tidy", "code": "return args.threshold", "budget_ms": 250,
                                   "args": {"threshold": -30}}}
```

Over MCP that is the generic tool: `exec {"command": "script.run", "args": {...}}`.
The script's own commands are recorded as `lua:<name>` in the undo history, so a
human can see and undo what it did.

Inside the script:

| Call | What it does |
|---|---|
| `q.<namespace>.<command>{args}` | any registered command, e.g. `q.track.add{name = "Bass"}` → the result as a table |
| `q.run(name, args)` | by name, for names that are not Lua identifiers (`branch.render-ab`) or when a name collides |
| `q.batch(label, function() ... end)` | run the calls inside as **one** transaction (one undo step; an error rolls it back and is re-raised) |
| `q.on(topic, function(payload, topic) ... end[, filter])` | subscribe to bus events; returns a handle |
| `q.off(handle)` | drop a subscription |
| `print(...)`, `q.log(...)` | lines in the script's log, returned in `log` |
| `args` | the `args` object of `script.run`, as a table |
| `q.null` | the JSON null (Lua tables cannot hold nil) |

Errors are raised as tables, so `pcall` catches them and `err.code`,
`err.message`, `err.path`, `err.hint` and `err.details` tell you exactly what
the bus rejected and how to fix it.

The sandbox is strict: `string`, `table`, `math`, `utf8`, `coroutine` and the
base library only. There is no `io`, `os`, `debug`, `package`, `require`,
`dofile` or `loadfile`: there is no direct filesystem, clock or process API.
Registered commands can still access files (for example `project.save`).
`load` accepts text only, including chunks supplied by a reader function;
binary chunks and explicit `b`/`bt` modes return `nil` and an error. Chunk
names and custom environments still work.
`budget_ms` (default 250 ms, excluding time inside commands) stops a script
that runs away with `script.budget`; a memory cap stops one that allocates
too much with
`script.memory`. A value passed to a command or returned is converted to JSON
with at most 1,000,000 values and 16 MiB of text (`script.value_too_large`);
a table used twice counts twice.

Log lines are capped at 64 KiB each, including a visible
` ... [truncated N bytes]` marker. The script keeps at most 1 MiB of log text
and 1000 lines by default, dropping the oldest lines first. The `log` result
contains the retained lines printed by this run. `log_dropped` counts all
lines evicted since this named script state was created, including previous
runs and event handlers; a truncated line does not count as dropped.

Editor support: `skill/generated/engine.lua` (and `sandbox.lua`) are LuaLS
annotations generated from the registry — point `lua-language-server` at them
and `q.track.add{` autocompletes with typed, documented members.

## 1. Build an arrangement and mix it

```lua
-- name: "sketch", code:
local names = {"Drums", "Bass", "Keys"}
local ids = {}
q.batch("three tracks", function()
    for _, name in ipairs(names) do
        local made = q.track.add{name = name}
        ids[name] = made.track
        q.track.set{track = made.track, volume_db = -6}
    end
end)
for _, name in ipairs(names) do
    print(name .. " = " .. ids[name])
end
return {tracks = ids}
```

One undo step removes all three tracks and their settings.

## 2. Write MIDI and read it back

```lua
-- name: "bassline"
local bar = args.bar or 1
local made = q.item.add{track = "tr_1", bar = bar,
                        notation = "voice bass; 1: C2:q r:q Eb2:q r:q"}
local midi = q.midi.get{item = made.item}   -- the item's MIDI as notation text
print("item " .. made.item .. " is at beat " .. made.position)
if midi.text:find("Eb2") then q.log("the flat third is in") end
return {item = made.item, notation = midi.text}
```

## 3. A watcher that keeps running: subscribe instead of polling

```lua
-- name: "watcher" — run once; it keeps reacting to every later change.
seen = seen or {}
q.on("session.changed", function(payload)
    seen[#seen + 1] = payload.version
end, {branch = "main"})
print("watching main; " .. #seen .. " versions so far")
```

Run it again with the same `name` to read what it saw
(`return {versions = seen}`): the state and the subscriptions of a named script
persist between runs. Handlers run after the command that emitted the event, on
the model thread, one delivery per event.
