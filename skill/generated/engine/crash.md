# `crash` commands

Crash reports and runs that ended unseen: list, read, clear.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `crash.acknowledge@1`

Mark a crash report, or a run that left none, as seen.

Writes a small marker beside the report; the report itself is never changed. An acknowledged report no longer counts for the restart notice. It does not end the suspicion of a plug-in: seen is not resolved, and crash.settle does that. Repeating it changes nothing.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report or ended run has that incident id Hint: list the reports with crash.list

### Examples

**Acknowledge a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "acknowledged": true
}

```

## `crash.clear@1`

Delete every finished crash report and record of an ended run.

Removes what crash.list shows as over: reports and unreported runs, with their acknowledgements. It cannot be undone. Live runs, reports still being written and files that are not valid reports are left alone. Returns how many incidents went.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Clear the reports** (`song_with_crash_report`)

```json
{}

```

Result:

```json
{
  "deleted": 1
}

```

## `crash.delete@1`

Delete one crash report, or the record of a run that left none.

Removes the report file, its acknowledgement and an idle run marker. It cannot be undone. A run that is still live or a report still being written is refused.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report or ended run has that incident id Hint: list the reports with crash.list

### Examples

**Delete a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "deleted": true
}

```

## `crash.list@1`

The crash reports earlier runs left, newest first.

Each report says when it was written, which build and role, how the run ended (kind: crash, unexpected_exit, bridge_exit for a plug-in bridge process that died while the app lived, or other for a kind this build does not know), the attribution level (fault_in_image, active_token, concurrent_activity, recent_command, bridge_exit, unknown; a stated confidence, not a verdict) and the plug-in it names, if any, as the plug-in described itself. unreported lists runs that ended and left no report (the supervisor died or could not write). A report counts once its run has ended: reports_pending are still being written; live_runs are running now, this one included. ignored counts files that are not valid reports. limit caps each list (default 50); the _total fields count them all. Reports hold plug-in and command names, never project content.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `limit` | integer |  | 1.0..200.0 |  |  |

### Result

```json
{}

```

### Examples

**The reports left behind** (`song_with_crash_report`)

```json
{}

```

Result:

```json
{
  "reports": "<any>",
  "reports_total": 1,
  "unreported": [],
  "unreported_total": 0,
  "live_runs": 0,
  "reports_pending": 0,
  "ignored": 0
}

```

## `crash.read@1`

One crash report in full.

The report as the supervisor wrote it (schema supple.crash-report/1.0): the run, the plug-ins it had announced, the threads with module-relative frames, the last commands, the attribution and everything it could not collect (unavailable, dropped). Frames are not symbolised: the offline resolver does that. Reports are tens of kilobytes and at most 4 MiB.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report has that incident id Hint: list the reports with crash.list

### Examples

**Read a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "report": "<any>"
}

```

## `crash.settle@1`

Mark the suspicion a crash report raised as resolved.

Writes a small marker beside the report; the report itself is never changed. A settled report no longer makes its plug-in a suspect (project.plugins, project.open's project.suspect_plugin warning). Use it once the plug-in was loaded normally and ran well, or when the person chose to load it normally; crash.acknowledge only says the report was seen. A report whose run has not ended cannot be settled. Repeating it changes nothing.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `incident` | string |  |  |  | A report's incident id, from crash.list. |

### Result

```json
{}

```

### Errors

- `crash.no_report`: no finished report has that incident id Hint: list the reports with crash.list

### Examples

**Settle a report** (`song_with_crash_report`)

```json
{
  "incident": "0123456789abcdef0123456789abcdef"
}

```

Result:

```json
{
  "incident": "0123456789abcdef0123456789abcdef",
  "settled": true
}

```
