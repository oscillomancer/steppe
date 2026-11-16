# Spec: command bus, control protocol and MCP adapter

**Status:** Implemented. Interface changes go through an ADR.
**Decision record:** ADR-0007, ADR-0063, ADR-0077, ADR-0079.
**Libraries:** `libs/command-bus` (`supple::cmd`), `libs/control-protocol`
(`supple::rpc`), `mcp/` (`supple::mcp`). All run on the model thread only
(transports marshal requests onto it). Depend on core-json and core-schema.

---

## 1. Commands

### 1.1 Naming

`<noun>[.<noun>].<verb>`: lowercase segments `[a-z][a-z0-9-]*` joined by
dots. **No underscores** (so the MCP mapping `.`→`_` is reversible). Verbs:
`add`, `remove`, `get`, `set`, `list`, `describe`, `move`, `rename`,
`merge`, `render`, `analyze`, `compare`, `load`, `save`, `scan`, `reset`.
Examples: `plugin.load`, `patch.merge`, `track.add`, `fx.param.set`,
`render.stems`. Names are permanent once released.

### 1.2 Declaration

```cpp
namespace supple::cmd {

enum class Kind : std::uint8_t { Query, Edit, Action };
enum class Exposure : std::uint8_t { McpTool, McpExec, Internal };

struct ErrorSpec { std::string code; std::string when; std::string hint; };

struct Example {
    std::string title;         // "Set a cutoff with a unit string"
    json::Value args;
    json::Value result;        // expected; strings "<any>" match any value
    std::string fixture;       // named fixture the example runs against
};

template <class Args, class Res>
struct Command {
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary;       // imperative, ≤ 80 chars: "Merge a partial patch into a plugin instance."
    std::string description;   // when/why to use; limits; related commands
    std::vector<std::string> tags;
    schema::Schema<Args> args;
    schema::Schema<Res> result;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;          // ≥ 1 required for every non-Internal command
    std::function<Result<Res>(const Args&, Context&)> run;
};

struct NoArgs {};   // with schema::object<NoArgs>("no_args").build()
}
```

`Context` is supplied by the host application (sandbox, engine):

```cpp
class Context {
  public:
    const Origin& origin() const;
    template <class S> S& service();                // typed services registered by the host; asserts presence
    void warn(std::string code, std::string message, std::string path = {});
    const std::vector<Warning>& warnings() const;
};

struct Origin {
    enum class Type : std::uint8_t { Ui, Lua, Mcp, Rpc, Test, Internal } type;
    std::string client;   // "claude-code", "gui", "script:tidy.lua"
    std::string agent;    // agent/session label if any
};
```

`Edit` commands mutate host state through services (the engine's
`SessionService` makes them pure `(Session, Args) → Session` functions under
the hood; ADR-0008). The bus itself is state-agnostic.

### 1.3 Registry

```cpp
class Registry {
  public:
    template <class Args, class Res> void add(Command<Args, Res>);   // asserts: name rules, unique (name, version), ≥1 example unless Internal
    const CommandInfo* find(std::string_view name, int version = 0) const;  // 0 = latest
    std::vector<const CommandInfo*> all() const;                     // sorted by name, then version
    Error unknown(std::string_view name) const;                      // command.unknown with closest names
    json::Value exportJson() const;                                  // registry document (§1.6)
};

struct CommandInfo {
    std::string name; int version; Kind kind; Exposure exposure;
    std::string summary, description; std::vector<std::string> tags;
    json::Value argsSchema, resultSchema;       // JSON Schema 2020-12
    std::vector<ErrorSpec> errors; std::vector<Example> examples;
    Result<json::Value> invoke(const json::Value& args, Context&) const;   // validate → run → encode
    Status validate(const json::Value& args) const;
};
```

The library registers its own meta commands: `registry.list` (filter by
tag/kind/text; returns name, version, kind, summary, exposure) and `registry.describe`
(one command: full schemas, errors, examples). Both `Query`, `McpExec`.

Each `registry.list` row identifies one registered command version. `exposure`
is `mcp-tool`, `mcp-exec` or `internal`, matching that version's declaration.
Filters retain all five fields. This additive protocol-1 extension (ADR-0079)
lets consumers select tool definitions without fetching every full description.

### 1.4 Dispatch

```cpp
struct Invocation {
    std::string command;          // "patch.merge" or "patch.merge@1"
    json::Value args;             // object; absent = {}
    Origin origin;
};

class Dispatcher {
  public:
    Dispatcher(const Registry&, Host&);
    Result<json::Value> execute(const Invocation&);
};

class Host {    // implemented by the application
  public:
    virtual ~Host() = default;
    virtual Context makeContext(const Origin&) = 0;
    virtual Result<std::uint64_t> beginTransaction(const Origin&, std::string_view label) = 0;
    virtual Status commitTransaction(std::uint64_t) = 0;
    virtual Status rollbackTransaction(std::uint64_t) = 0;
    virtual Result<std::function<Status()>> invocationSavepoint() = 0;
    virtual void log(const LogRecord&) = 0;          // audit/command log (§1.7)
    virtual void emit(std::string_view topic, json::Value payload) = 0;   // events (§3.4)
};
```

`execute`: resolve name (unknown → `command.unknown` with up to 3 closest
names in the hint); parse `@version` (unknown version → `command.version`
listing available versions); validate args with the command's schema (all
problems, as `command.invalid_args` whose `details` are the schema errors,
paths relative to the args object); for `Edit` commands wrap in a single-step
transaction; run; encode the result through the result schema (a handler
returning a value that fails its own result schema is a bug: `command.
bad_result`, logged loudly, and a test failure in examples); log; return.
Handler errors pass through unchanged (their own codes), with `path`
prefixed by `/args` when it is an args path.

Successful replies may have a `warnings` array. After validating the raw
handler result and committing any transaction, dispatch appends each
`Context::warn()` diagnostic, including argument-decoder warnings, as
`{code: string, message: string, path: string}`. The path is preserved as
supplied: decoder JSON Pointers are relative to the arguments object;
handler paths identify arguments or documents as that command describes.
An empty path is global, and non-pointer source positions are unchanged.

Command-owned warnings remain first (strings and richer objects included).
A matching command diagnostic with the same code, message and path already
reports a context warning, so dispatch does not duplicate it. Other context
warnings follow in emission order. An existing non-array `warnings` value
is retained as the first array entry when context warnings are added. For
non-object results, a warning-bearing reply is
`{result: <original value>, warnings: [...]}`. With no context warnings,
replies retain their original shape. Failed invocations use the existing
structured Error contract, without a success warning payload.

The registry's result JSON Schema includes optional
warning metadata: `warnings: {type: "array", items: {type: "object"}}`,
without repeating the entry fields or their description per command. Legacy
non-object command warnings retain mixed item schemas. The shared entry
shape is defined above and in the generated reference header; MCP
`initialize.instructions` states it once for both server deployments.
`validateResult` continues to validate the raw handler output before this
metadata is added; handlers cannot hide invalid results behind warnings.
Lua receives the same result table, MCP emits the same object as structured
content and JSON text, and RPC places it in the JSON-RPC `result`. This is
an additive extension of protocol 1 (ADR-0063).

### 1.5 `batch`

Registered by the library, `Kind::Edit`, `McpTool`.

```json
{
  "label": "Bass patch: darker, shorter release",
  "steps": [
    { "command": "plugin.load", "args": { "plugin": "com.suppleaudio.marram" }, "as": "synth" },
    { "command": "patch.merge", "args": { "instance": "${synth.instance}", "patch": { "params": { "filter1.cutoff": "800 Hz" } } } },
    { "command": "render", "args": { "notation": "1: C2:q r:q C2:q r:q", "analyze": "summary" }, "as": "r1" }
  ]
}
```

- The batch object permits only `label` (optional string) and `steps`
  (required array). Each step permits only `command` (required string),
  `args` (optional object) and `as` (optional string). Structural validation
  aggregates errors across the batch and its steps before any transaction
  begins: `command.invalid_args` carries individual schema errors in
  `details`, with `/args`-prefixed JSON Pointer paths and closest-member
  hints for misspelled keys.
- Steps run in order inside **one transaction** (one undo step in the
  engine). If any step fails, the transaction rolls back and the error is
  `batch.step_failed` with `path` `/steps/<i>` (and the step's own error in
  `details`, its paths prefixed with `/steps/<i>/args`).
- **Step references:** a string that is exactly `"${<name>.<path>}"` is
  replaced, before that step is validated, by the value at `<path>` (dot
  separated keys / array indices) in the result of the earlier step with
  `"as": "<name>"`. Unknown names/paths → `batch.bad_reference` with the
  available names. References are the only templating; there is no
  expression language.
- All steps are resolvable before running (`command` names checked first,
  so a misspelled step fails before anything runs).
- Result: `{"label": …, "results": [ … one per step … ]}`. Each step's
  warnings stay at `results[i].warnings`, including warnings from queries;
  they are not duplicated in a batch-wide list.
- `Query` commands are allowed inside batches (they see earlier steps'
  effects). `Action` commands are allowed only if the host declares them
  transaction-safe (the sandbox's `render`/`analyze` are).
- Limits: ≤ 256 steps.

### 1.6 Registry export

`exportJson()` →

```json
{
  "schema": "supple.command-registry/1.0",
  "product": "<name from product.h>", "version": "<version>",
  "commands": {
    "patch.merge@1": { "name": "patch.merge", "version": 1, "kind": "edit", "exposure": "mcp-tool",
                        "summary": "…", "description": "…", "tags": [...],
                        "args": { JSON Schema }, "result": { JSON Schema },
                        "errors": [ {"code", "when", "hint"} ], "examples": [ … ] }
  }
}
```

Generators (each a function in the library, deterministic output, committed
results checked by a drift test):

- `mcpToolList(registry, bool allTools = false)` — dedicated MCP definitions (§4.2);
  false selects McpTool, true selects every non-Internal command.
- `markdownReference(registry)` → `skill/generated/*-commands.md`: per command,
  heading, summary, description, an args table (field, type, unit, range,
  default, description), result shape, errors with hints, examples as JSON.
- `luaStubs(registry)` → LuaLS annotations (`skill/generated/*.lua`).

### 1.7 Command log

Every executed invocation (including each batch step) produces a
`LogRecord {seq, time_ms (host clock; tests use a fixed clock), origin,
command, version, args (after reference resolution), ok, error code,
result digest (first 200 chars), duration_us}`. The host writes it (the
sandbox to an NDJSON file per session; the engine into the session history).

### 1.8 Examples are tests

`runExamples(const Registry&, FixtureFactory, FixtureAvailable = {})` (test
helper in the library) executes every example against
`factory(example.fixture)` and checks the result matches (`"<any>"`
wildcards allowed); every Example of every non-Internal command must pass.
Each application registers its fixtures. `available(fixture)`, when given,
says whether this build can provide a fixture, and examples whose fixture it
cannot are skipped: `dsp_player_editor`, a script player with its editor,
in builds without a GUI (the sanitizer presets). Editor-enabled builds run
its PNG and pointer examples on Linux, macOS and Windows; CI requires the
native offscreen renderer to succeed. The binary `dsp_player_export` fixture runs on Linux, macOS and Windows.

### 1.9 Error codes owned by the bus

`command.unknown`, `command.version`, `command.invalid_args`,
`command.bad_result`, `batch.step_failed`, `batch.bad_reference`,
`batch.too_many_steps`, `batch.not_transactional` (an Action that is not
transaction-safe inside a batch), `rpc.*` (§3), `mcp.*` (§4).

---

## 2. Conventions for command designers

- **Units:** numeric args with physical meaning declare `.unit()` and accept
  unit strings.
- **References:** object references accept an ID string or a selector
  object; the `Ref` schema type (in command-bus) resolves via a host service
  and errors with candidates on ambiguity.
- **Results** are compact: return IDs and the few fields an agent needs next,
  not whole objects; offer `detail` args (`summary` / `full`) where outputs
  can be large.
- **Idempotence:** `set`/`merge` commands are idempotent; `add` returns the
  new object's ID.
- **Descriptions** say when to use the command, what it does not do, and
  which command to use instead.

---

## 3. Control protocol (`libs/control-protocol`)

- **Transport:** Unix domain stream socket at
  `$XDG_RUNTIME_DIR/<slug>/<instance>.sock` (directory 0700, socket 0600).
  Without `XDG_RUNTIME_DIR` (some MCP clients start servers with a reduced
  environment) the root is `/run/user/<uid>` when that is a private
  directory of the user, else `/tmp` (`paths::runtime()`). macOS uses
  `TMPDIR`, else the per-user temporary directory it normally names; Windows
  uses `LOCALAPPDATA\Temp`, else the native temporary directory (by default
  the same folder).
  Before binding, the product runtime directory itself must be a real directory:
  POSIX opens it without following links, checks the effective user's ownership,
  and applies 0700 through that handle; Windows refuses reparse points and retains
  inherited directory ACLs (ADR-0035). A refusal returns `rpc.path`, names the
  directory and explains how to choose a private owned directory without aliases.
  This checks the product directory, not every ancestor or arbitrary explicit
  socket parent. Endpoint-lock acquisition can still wait without a deadline.
  Accepted connection descriptors are nonblocking and close-on-exec.
  Framing: one JSON value per line (UTF-8, `\n`-terminated, compact JSON; no
  embedded newlines), max 16 MiB per message.
- **JSON-RPC 2.0.** Request `{"jsonrpc":"2.0","id":…,"method":"<command>","params":{…}}`;
  `method` is a command name (optionally `@version`). Notifications from
  clients (no `id`) are rejected with no response (logged). JSON-RPC array
  batches are rejected with `-32600` and a hint to use the atomic `batch`
  command.
- **Errors:** `-32700` parse error, `-32600` invalid request, `-32601`
  method not found, `-32602` invalid params, `-32000` command error. `data`
  is always the `supple::Error` JSON (code, message, path, hint, details).
  Invalid request ID types receive `id: null`, never the invalid value; a valid
  ID is preserved when another envelope field is invalid. MCP follows the same rule.
- **Handshake:** `rpc.hello {"client": "...", "agent": "...", "protocol": 1}`
  → `{"server": "<slug>", "product": "<name>", "version": "...",
  "protocol": 1, "commands": N}`; required before other methods. Sets the
  connection's `Origin` (type `Rpc`, or `Mcp` when the MCP proxy connects).
- **Events:** `rpc.subscribe {"topics": ["session.changed", …], "filter": {…}}`
  → `{"subscription": "sub_1"}`; server pushes
  `{"jsonrpc":"2.0","method":"event","params":{"subscription":"sub_1","topic":…,"payload":…}}`.
  `rpc.unsubscribe {"subscription": …}`. Per-connection bounded outbound
  queue; on overflow the connection gets one `event.dropped` notification
  with a count, never blocking the model thread.
- **Threading:** a small I/O thread per server accepts connections and
  frames messages; requests are posted to the model thread's queue and
  answered in order per connection.

The synchronous `rpc::Client::connectionGeneration()` returns zero when it is
disconnected, including when a nonblocking probe observes a closed peer. A live
connection has a generation incremented after every successful `rpc.hello`
connection handshake, including automatic reconnects. Closing the client does
not reset the counter. Observation consumes no data and sends no requests;
ordinary `call` retains its existing no-replay transport contract. This lets
adapters refresh peer metadata before relying on cached admission decisions.

---

## 4. MCP adapter (`mcp/`)

- **Protocol:** MCP over stdio (JSON-RPC 2.0, newline-delimited), protocol
  version negotiation per the MCP spec: support `2025-06-18` and newer
  revisions that are backward compatible; reply with the client's requested
  version if supported, else the newest supported. Capabilities: `tools`; the proxy declares
  `tools.listChanged: true`. `ping` supported. `logging`/`resources`
  later.
- **Envelopes:** validate JSON-RPC fields before suppressing replies to valid
  notifications. Malformed objects without `id` receive Invalid Request
  (`-32600`) with `id: null`; `params`, when present, must be an object or
  array. Array params in requests to this adapter return Invalid params
  (`-32602`).
- **Deployments:** in-process (`McpServer` wraps a `Dispatcher` directly —
  the sandbox or engine CLI `--mcp` mode) and proxy (`<slug>-mcp` forwarding to a
  control-protocol socket).

### 4.1 Tool naming

Tool name = command name with `.` replaced by `_` (`patch.merge` →
`patch_merge`); `@version` is not exposed (tools always call the latest
version). Generic tools: `describe` (→ `registry.describe`/`registry.list`),
`exec` (→ one invocation or a batch: `{"command", "args"}` or
`{"batch": {...}}`), and in the engine, `query`.

### 4.2 Tool definitions

Both `<slug>-mcp` and `<slug>-engine --mcp` accept `--tools core|all`,
defaulting to `core`; invalid or missing values fail with usage (exit 2).
Core lists only `McpTool` commands; `all` lists every non-Internal command,
including existing `McpExec` commands. Both use the latest command version.
The sandbox exposure declarations and the GUI's additional `gui.selection`
context query are unchanged.

The proxy makes one `registry.list` request, selects the latest version of
each command, then fetches exactly one `registry.describe` per dedicated tool
selected by the chosen mode. Its startup command-request count is therefore
`1 + dedicated tools` (excluding RPC handshakes). Internal commands are never
hydrated at startup. Kind metadata from the full index supports `query` for
commands outside the dedicated set. A named `describe` fetches its full contract
on demand; `exec` forwards directly to the dispatcher for validation and
execution without a schema-prefetch request. The compact MCP describe index
continues to contain only name, kind and summary.

The proxy requires a valid exposure on each latest index row. Missing or
unknown values fail startup with `mcp.registry` at `/commands` and a hint to
update the running application and proxy to matching versions, rather than
silently fetching every description. Older proxies can ignore the added field.

The engine core is exactly these 37 commands, plus `describe`, `exec`, `query`:

```
project.get project.open project.save session.describe
track.add track.set track.list track.remove
item.add item.set item.remove midi.write midi.get
fx.add fx.describe fx.patch.get fx.patch.merge fx.remove plugin.list
route.add env.add env.points.set tempo.set
transport.play transport.stop transport.seek
render.mix analyze.track analyze.render analyze.compare
branch.create branch.switch branch.diff branch.merge history.undo batch engine.start
```

All other formerly dedicated engine commands use `McpExec`. Every command
remains callable via `exec`, read-only commands via `query`, and discoverable
via `describe`. The complete core `tools/list` JSON-RPC frame (UTF-8, including
newline) must be at most 60,000 bytes; tests pin both this budget and the set.

Each dedicated definition contains `name`, `title` (summary), `description`
(command description, falling back to summary if empty, followed by
"See describe for errors and examples."), `inputSchema` (args JSON Schema,
with `x-unit` hints folded into field descriptions because clients ignore
unknown keywords), and `annotations` (`readOnlyHint` for Query,
`destructiveHint: false` for Edit — undoable — and `idempotentHint` where true).
Dedicated definitions omit `outputSchema` in both modes and deployments;
`describe` still exposes the complete result schema, and results still carry
structured content. MCP 2025-06-18 Tools / Output Schema makes the schema
optional and Structured Content permits JSON-object results independently:
<https://modelcontextprotocol.io/specification/2025-06-18/server/tools>.

`describe {}` returns `{columns: ["name", "kind", "summary"], namespaces:
{<namespace>: [[<name>, <kind>, <summary>], ...]}}`. Namespace is the first
name segment; standalone `batch` has its own group. Rows retain the registry's
name/version order and include every registered command; summaries are at most
80 characters. The full engine index targets less than 25,000 UTF-8 bytes.
Existing `tag`, `kind`, and `text` filters apply before grouping (`text`
searches names and summaries). `describe {command: <name>}` is unchanged:
full args/result schemas, errors, examples and command metadata. The underlying
`registry.list` contract is unchanged. Initialize instructions explain the
common tools, describe/exec/query workflow and argument-free index.

### 4.3 Calls and results

The adapter validates the generic describe/exec/query envelopes against their
advertised member names, types and required fields before invoking any command.
An invalid envelope returns structured `mcp.arguments` with the offending
`/arguments/...` path and a corrective hint. A present batch does not bypass
validation of sibling fields. The inner command still uses dispatcher validation.

`tools/call` → dispatcher `execute` with `Origin{Mcp, client name from
initialize, agent}`. Every valid `initialize` replaces the client and agent
identity; an absent `_meta.agent` clears the agent. Present `clientInfo` and
`_meta` must be objects;
present `clientInfo.name`, `clientInfo.version` and `_meta.agent` must be strings.
Invalid identity fields return Invalid params before changing the accepted
identity or sending a new handshake. The proxy forwards the accepted
identity in its RPC handshake. Success: `structuredContent` = result JSON
when the result is an object (MCP structured content is an object) and
`content: [{"type":"text","text": <compact JSON of the result>}]`. Failure:
`isError: true`, `content: [{"type":"text","text": <error rendered as
"code at path: message\nHint: hint" plus details>}]`, `structuredContent` =
the Error JSON. Protocol-level problems (unknown tool) are JSON-RPC errors
per MCP.

The proxy checks its RPC connection generation before consulting cached tool
names or query kinds. A disconnected peer is detected with a nonblocking,
non-consuming socket probe; explicit and automatic reconnects both change the
generation. After reconnect it fetches `registry.list` once. An identical index
retains the existing definitions without a notification or description requests.
A changed index stages fresh kinds, dedicated-tool mappings and descriptions
for the selected mode, then replaces them together. Failed or interrupted
refreshes leave catalogue admission invalid until a later refresh succeeds.

After initialization, a successful changed-index refresh queues
`notifications/tools/list_changed` (JSON-RPC 2.0, no id) on the existing MCP
stream. `serve` drains and flushes notifications before the triggering request's
response; embedders using `handle` must drain `takeNotifications()` after each
call. Multiple pending changes may coalesce into one notification. In-process
catalogues are fixed and omit the `listChanged` capability. MCP Tools /
[List Changed Notification](https://modelcontextprotocol.io/specification/2025-11-25/server/tools#list-changed-notification)
says servers advertising that capability SHOULD notify when their tools change.

A failed invocation proved unsent by local RPC delivery provenance
may be retried once, after refreshing the catalogue and rerouting the original
tool call through current query admission and tool lookup. A sent but unanswered
invocation (`rpc.read`, timeout or interruption) is never automatically replayed,
including edits whose committed result was lost. A sent transport failure may reconnect
and refresh metadata for subsequent calls while returning the original error.

The standalone MCP proxy gives its RPC client twice the command bus's maximum
job-wait duration (currently 60 seconds versus 30). Dispatch and response delivery
therefore have headroom when `job.wait` consumes its full server-side budget.
The generic RPC client's configurable 30-second default and no-resend recovery
contract remain unchanged.

### 4.4 Tests

Fixed-seed schema-derived argument mutations cover all registered engine and
sandbox commands, including nested/optional fields and numeric limits. Rejected
inputs pass through dispatch, must retain structured errors, and must leave
project/session state unchanged. The engine repeats this while recording in both
executors with manual dummy blocks and interleaved saves. Schema-accepted actions
are counted rather than executed without domain fixtures; see `tests/fuzz/README.md`
for the conservative contract oracle and replay output.

A scripted MCP client (C++ test or Python tooling in `tests/integration/`)
runs initialize → tools/list → a sequence of calls against the sandbox, and
checks results; its transcript (requests/responses, pretty-printed) is
committed as `docs/demos/mcp-synth-session.md` and drift-tested on Linux
(`mcp.demo`). Native runs retain every structured-result assertion and record
an informational Linux-reference diff; DSP values are deterministic per platform.

## Implementation notes (2026-09-26, Task A)

- The host implements `Host::makeContext` and registers typed services with
  `Context::provide(service)`. The dispatcher adds `const Registry` and
  `JobManager` services before invoking a handler. The sandbox can therefore
  register its session and render services without a bus dependency on host
  code. `Host::actionTransactional(name)` declares whether an Action can run
  inside a batch; it defaults to false.
- `Ref` accepts an ID string or selector object. A host supplies
  `RefResolver`; commands with selectors use the optional `Command::resolve`
  hook before the handler. The hook replaces selectors in the typed args with
  IDs, and the command log stores the resolved encoding.
- The bus owns job state for each `Dispatcher`. A handler may create a job via
  `ctx.service<JobManager>().create()` and return its ID; the host owns the
  worker and must stop/join workers before destroying the dispatcher. Job
  updates are thread-safe. Progress and completion events queue for the model
  thread, which calls `Dispatcher::flushJobEvents()` (also called after every
  `execute`). Model-thread follow-ups go through `Dispatcher::post`;
  work posted as `PostKind::Edit` (a render's `add_to_project` import) stays
  queued while a `transact()` transaction is open, so a batch or script
  waiting in `job.wait` sees the job finish but does not take the import into
  its own undo step or rollback: the import runs as its own step once that
  transaction closes. `job.get` reads status and progress (`job.status` remains an
  alias). `job.cancel` marks a job cancelled immediately and sets its
  cancellation flag for the worker. The most recent progress is retained even
  when progress events are throttled to 2 Hz.
- `runExamples` calls `Host::setupFixture(dispatcher, fixture)` after creating
  the host. This lets fixtures seed jobs and host services as well as ordinary
  session state. The test library commits deterministic registry, MCP tool and
  Markdown output under `libs/command-bus/tests/golden/` and compares them on
  every run.
- A libFuzzer target exercises JSON invocations, including batches and step
  references. Its smoke corpus runs in CTest when `SUPPLE_BUILD_FUZZERS=ON`.

## Implementation notes (2026-09-26, Task B)

- `rpc::Server` owns Unix socket I/O and frames NDJSON up to 16 MiB. A caller
  supplies `rpc::Post` to schedule requests onto its model thread; only that
  thread executes `Dispatcher` commands and calls `Server::publish` from its
  host event sink. The server uses bounded per-connection output queues,
  reports dropped events, and assigns per-topic event sequence numbers. The
  path helper uses `$XDG_RUNTIME_DIR/<slug>/<instance>.sock`; created product
  directories and sockets have modes 0700 and 0600.
- `rpc::Client` is synchronous and belongs on a thread other than the server's
  model thread. `mcp::Server` uses the same MCP stdio handler over either a
  local `Dispatcher` or this client, with the proxy discovering tool schemas
  from `registry.list` and `registry.describe` over RPC.
- The MCP handshake implementation supports `2025-06-18` and `2025-11-25`.
  Requests for incompatible newer revisions fall back to `2025-11-25`.
  `initialize` supplies client identity to `Origin`; the proxy updates its RPC
  handshake with that identity before forwarding calls.
- Both headless CLIs expose `--mcp`. `tests/integration/mcp_client.py` checks
  initialize, tools/list, a successful tools/call and a structured command
  error against each binary.

## Implementation notes (2026-09-26, Task B round 4)

- Both headless CLIs expose `serve` with optional `--instance` and `--socket`.
  The default instance is `default`; `rpc::socketPath` places it below the
  generated product slug in `$XDG_RUNTIME_DIR` (or `/tmp` when unset).
  The main thread runs a pipe-woken model queue and handles SIGINT/SIGTERM.
  The proxy accepts the same path options. Socket integration tests start
  sandbox and engine children; the sandbox test loads the test instrument and
  renders through the MCP proxy. Both tests terminate and wait for children.
- A socket response can fill a 16 MiB frame. Larger responses and responses
  that would exhaust the outbound queue produce `rpc.response_too_large`
  (`-32000`) with a hint to narrow the query. Each peer has at most 16 posted
  requests, 32 MiB of posted request frames and 32 MiB of unsent output;
  the I/O thread removes `POLLIN` while full. Events retain the existing
  drop-and-notify behavior.
- A server owns its socket through an exclusive lock on `<socket>.lock`, taken
  without waiting before startup inspects the path and held until the server
  has stopped and removed its socket; the OS releases it if the process dies.
  A lock held elsewhere reports `rpc.address_in_use`, whatever the socket
  answers. With the lock in hand, a connected address also reports
  `rpc.address_in_use`; only refused or missing sockets are stale. The connect
  probe is nonblocking on every platform; a pending connect or failed probe
  setup means occupied, never stale, and so does a full backlog on Linux and
  Windows. macOS refuses a connect to a full backlog as it does a dead socket,
  so there a listener that does not hold the lock is protected only while its
  backlog has room. Failed bind never unlinks the path, and shutdown checks
  the socket's device and inode before unlinking.
- MCP stdio framing retains no more than 16 MiB of one line. It discards the
  rest of an oversized frame through newline (or EOF) and returns `-32600`.
  RPC distinguishes invalid top-level JSON values (`-32600`, null id) from
  malformed `params` (`-32602`, structured `rpc.invalid_params` data).

## Follow-up boundary guarantees (2026-10-04)

Each command invocation inside a transaction is atomic through a host savepoint;
Lua `pcall` can catch an error without retaining that call's partial edits.
The savepoint hook returns `Result<std::function<Status()>>`: capture failure
refuses the invocation; restore failure is attached to the invocation error's
details and makes the outer transaction roll back even if the script catches
the error. Its outer failure is `transaction.restore_failed`; a failed outer
rollback retains this error in its details. Subsequent calls in a poisoned
transaction refuse, and the failure state is cleared at transaction end. Scoped
dispatch admission applies to nested batch/Lua invocations too.

Lua dynamic loading uses a text-only `load` wrapper for string and reader-function
chunks; explicit modes containing `b` return `nil` and an explanatory error.
Chunk names and custom environments retain Lua's behavior. `dofile`, `loadfile`,
`require` and `package` are unavailable. Lua initialization, host JSON pushes
and structured error decoding are protected
against memory/stack failures; error fields are read raw. Error decoding also
bounds expansion into host memory to 1,000,000 values and 16 MiB of text,
counting shared details once per use, and returns `script.value_too_large`
when that budget is exceeded. Once the instruction
budget expires, every subsequent hook check raises until the outermost run returns.
The time budget measures the script's own work: time spent inside the commands
it calls does not count, while event handlers those commands run in the same
script do.
`print` and `q.log` retain at most 64 KiB per line, including a visible
` ... [truncated N bytes]` suffix counting omitted source bytes. The retained
log holds at most 1 MiB of text and the configured line limit (1000 by default),
evicting oldest lines until both limits hold. `script.run.log` contains the
retained lines printed by that run; `log_dropped` is the cumulative number
of lines evicted from this named script's log, including prior runs and event
handlers. It resets when the script state is discarded. Truncated lines are
not counted as evicted. These host-owned text limits are separate from the
Lua allocator cap.
A deterministic instruction cap is available to embedding tests. Schema `$ref`s
use bounded root JSON Pointer resolution (with the schema library's local `$defs`
compatibility); named arrays convert as arrays. LuaLS types use the same references
and derive their namespace from generated product metadata.

RPC carries source `loc` with structured errors. Clients enforce the response
size bound before parsing and disconnect after timeout/interruption/framing errors;
the next call reconnects and repeats the handshake. Positive sub-millisecond waits
round up, huge waits clamp. Timer cancellation on the model thread prevents later
callbacks in the same due pass; cross-thread cancellation can leave one invocation
that already passed its final check, so owner destruction must synchronize with the
model thread.

Provider SSE fields are type-checked and incomplete streams refuse unless the
provider completion marker arrived. Credential configuration publishes through an
exclusive 0600 temporary file and atomic rename.


### Main-thread scheduling (ADR-0030)

`Host::workQueued()` wakes the model thread after `Dispatcher::post` publishes
work outside the posted-work lock. JobManager's optional `setWaker` is installed
before workers start and outlives them; queued progress/completion/failure/cancel
events invoke it after releasing the store lock. Rate-limited progress retains
its 500 ms publication limit. Raw progress flags remain available for snapshots;
engine offline jobs also publish progress events through the callback in
OfflineSettings. Wakers signal only; consumers call `flushJobEvents()` on main.
EngineApp's service flushes these events before delivering Lua subscribers.

`ModelLoop::watch(fd, pollEvents, callback)` returns a cancellable FdWatch;
`at(steadyDeadline, callback)` returns a one-shot Timer, inactive before its
callback starts. Already due deadlines execute on the next pass. `run()` polls
the wake pipe and watched descriptors until the earliest timer, indefinitely
without a timer. `pump()` handles ready descriptors without blocking. FD watches
have the timer's cancellation guarantee: model-thread cancellation suppresses
callbacks even from an already-ready snapshot, so cancel before freeing owners
or reusing descriptors. Cross-thread cancellation requires the same owner
synchronization as timers. New work/registration changes wake the pipe/embedding
waker. `wakeFd()` borrows the loop's read descriptor for embedding: watch for
POLLIN, then call `pump()` (only pump/run drains it), and stop watching before
destroying the loop. Signals write this pipe without calling the embedding
waker, so native owners watch it even when no timer exists.

MCP Tick retains its legacy `fn`/periodic `interval`. Optional `watches` and
`deadline` suppliers select event/deadline scheduling instead; sources are
recomputed after requests/service. The fd reader polls stdin plus those watches,
and checks deadlines/readiness at frame boundaries, every 4 KiB of buffered input
and whenever input buffering empties. Partial frames cannot block asynchronous
service; continuous buffered requests cannot starve it. The istream overload
retains blocking stream behavior. No MCP or control wire schema changes.

### Portable control and service sources (2026-10-14, ADR-0035)

`ModelLoop::watch(Waitable, events, callback)` retains descriptor calls through
Waitable's implicit POSIX descriptor constructor. `waitable()` borrows the loop's
native wake source; `wakeFd()` remains POSIX compatibility (-1 on Windows).
Cancellation, timer deadlines and ready-snapshot ordering are unchanged. Windows
accepts manual-reset event handles, up to 64 unique handles including the loop's
own wake source. MCP Tick::Watch carries `source: Waitable` instead of `fd: int`.
The Windows fd reader uses a joined, bounded input thread to allow service during
partial stdio frames. Stream overloads retain their existing blocking behavior.

Control transport remains AF_UNIX and protocol 1, with Winsock on Windows. macOS
paths must fit 104 bytes including the terminator; other native sun_path limits
are enforced at runtime. Windows paths use UTF-8 and endpoint cleanup checks the
volume/file identity. LockFileEx serializes competing starts. Default directories
come from paths::runtime() plus the generated slug; Linux XDG_RUNTIME_DIR and
/tmp behavior is unchanged. POSIX mode checks remain; Windows inherits its user
profile directory ACL. Explicit socket directories must be private to the user.

Windows MCP is intended for pipes or file descriptors backed by files. Interactive
console input is a known limit: CancelSynchronousIo is not guaranteed to interrupt
a console read during reader teardown, so shutdown can await a keypress. Pipe-based
MCP clients are the tested transport contract. Windows signal teardown clears the
borrowed wake handle and joins lock-free handler claims before the event can close;
Linux retains its existing signal/descriptor lifecycle.

### Explicit local delivery (2026-11-16, ADR-0087 D4)

`rpc::Client::callWithDelivery` returns `{result, delivery}`; `Delivery::Unsent`
means no complete newline-terminated command frame was transmitted, and
`Delivery::Sent` means it may have executed. A reconnect handshake has separate
delivery state. The existing `call` API returns just `result` and never retries.
Remote structured errors preserve their code/path/hint and carry Sent provenance.
The proxy retries once only on Unsent, after refreshing/rerouting; error-code
strings never authorize replay. Local adapter validation does not authorize retry.

### Request ID budget (ADR-0087 D5)

RPC and MCP request IDs must fit in 1,024 bytes of compact serialized JSON,
including quotes and escape expansion. Longer IDs receive -32600 with id:null
and structured rpc.id_size at /id, before handshake/dispatch or identity changes.
IDs are never truncated. RPC rechecks final fallback frame bounds; with a bounded
ID even its response-too-large/queue-full errors fit the 16 MiB wire limit.

### Optional null leniency (ADR-0087 D7)

Exported argument schemas deliberately describe a narrower client subset for
optional members: callers should omit an optional value rather than send null.
The decoder nevertheless accepts null as omitted, for LLM caller leniency.
This does not admit other wrong types or bypass numeric constraints on present
values. Exported schemas and tool-list byte budgets remain unchanged. The
schema-driven corpus keeps probing null and exempts it only in its conservative
exported-contract oracle.

### Untrusted JSON value budget (ADR-0087 D3)

RPC request/response frames and MCP stdin frames admit at most 1,000,000 JSON
values (containers and primitives, not object keys), checked before constructing
the next value. Refusal is a parse error with structured json.parse.too_many_values
and a corrective hint; the next newline-delimited frame remains usable. Raw byte
and depth limits remain. Trusted files keep the parser's unlimited value default.
MCP handle takes ownership of its request; transports move parsed requests/params
rather than deep-copying them. Embedders passing lvalues retain ordinary copy
semantics. No bad_alloc catch is introduced.

### Global admission (ADR-0087 D1)

A server admits at most1,024 posted-but-unexecuted requests and64 MiB of frame
bytes globally, including disconnected peers. Credits return when execution
begins or the last unexecuted closure is destroyed; per-peer credits also return
on destruction. Closure copies share frame/credit ownership. Admitted work still
runs after disconnect; stop/destruction retains its existing lifecycle rules.
Excess work is not posted or executed: a live peer receives structured rpc.busy
with its bounded ID and a retry-later hint. A bounded, serialized I/O preflight
extracts the ID without model dispatch. Existing per-peer16 limits remain; no
additional fairness scheme is added. Each peer admits at most64 subscriptions;
excess returns rpc.subscriptions with an unsubscribe hint.

### FD-backed output (ADR-0087 D2)

Production engine, sandbox and proxy stdio use `serve(inputFd, outputFd, Tick)`.
A joined native writer owns a noninheritable output duplicate. The single producer
reserves one complete packet's worst-case capacity before dispatch: queued plus
in-flight serialized bytes (notifications followed by reply, including newlines)
never exceed64 MiB. This conservative full-packet reservation pauses even buffered
stdin admission while output is pending; it does not wait for a consumer or take
a writer-held mutex in enqueue. Packet ownership uses an atomic SPSC publication
and native wakes; storage is freed before capacity returns.

During backpressure/drain the serving thread waits on writer progress plus Tick
watches/deadlines, excluding stdin, and continues model service. Windows pauses
new input reads; one already-buffered/in-flight64 KiB read-ahead may remain.
POSIX duplicates share status flags: this API requires exclusive transport use,
adds nonblocking output and restores its original nonblocking bit after join.
The writer blocks SIGPIPE only on its owned POSIX thread; Windows abort teardown
retries CancelSynchronousIo across the entry race, retaining handles through join.
Invalid descriptors and read/write/wait failures return actionable structured
transport errors. Pending delivery may be incomplete after an I/O error; an edit
may already have executed and is never replayed on that basis.

Normal EOF drains all replies while servicing Tick, then joins. A consumer that
keeps stdout open but never reads can delay lossless completion indefinitely;
there is no finite drain timeout or silent reply drop. Engine capture finalization
follows serve's return, so it is likewise deferred by an undrained connection.
The current API has no owner-triggered abort token; error teardown cancels/joins.
Arbitrary device/filesystem cancellation latency and Windows console input retain
their existing platform limits. No audio-thread work or system configuration changes.

Bounded/counting serialization precedes wire allocation. A single oversized or
over-deep reply becomes an explicit mcp.response_too_large error retaining its
bounded ID; final fallback packet bounds are checked. Tool result text/structured
content are both charged, without charging a fictitious additional escaping layer.
The `serve(istream, ostream, Tick)` and `serve(inputFd, ostream, Tick)` overloads
remain synchronous for tests/embedding only; they are not production stdio.
