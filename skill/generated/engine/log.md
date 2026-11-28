# `log` commands

The recent command log: origin, arguments and outcome.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `log.list@1`

Recent commands with origin, arguments and outcome (the command log).

Newest last. Who did what: origin is gui, agent:<client>/<model>, mcp:<client>, lua:<script>, rpc. queries: include read-only commands (left out by default). since: only entries after this seq (poll with the last seq seen). origin: entries whose origin starts with this. The log keeps the last 2000 commands.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `limit` | integer |  | 1.0..2000.0 |  |  |
| `since` | integer |  | 0.0.. |  |  |
| `queries` | boolean |  |  | false |  |
| `origin` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**The last commands** (`song`)

```json
{
  "limit": 5
}

```

Result:

```json
{
  "entries": "<any>",
  "last_seq": "<any>"
}

```
