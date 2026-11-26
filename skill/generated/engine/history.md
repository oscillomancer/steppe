# `history` commands

Undo, redo and the list of recent versions.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `history.list@1`

List the current branch's recent versions (newest first).



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_list",
  "properties": {
    "versions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "history_entry",
        "properties": {
          "version": {
            "type": "integer"
          },
          "label": {
            "type": "string"
          },
          "origin": {
            "type": "string"
          },
          "squashed_steps": {
            "type": "integer"
          }
        },
        "required": ["version", "label", "origin", "squashed_steps"],
        "additionalProperties": false
      }
    },
    "can_undo": {
      "type": "boolean"
    },
    "can_redo": {
      "type": "boolean"
    },
    "truncated": {
      "type": "boolean"
    },
    "retained_bytes": {
      "type": "integer"
    },
    "budget_bytes": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["versions", "can_undo", "can_redo", "truncated", "retained_bytes", "budget_bytes"],
  "additionalProperties": false
}

```

### Examples

**List** (`song`)

```json
{}

```

Result:

```json
{
  "versions": "<any>",
  "can_undo": true,
  "can_redo": false,
  "truncated": false,
  "retained_bytes": "<any>",
  "budget_bytes": 1073741824
}

```

## `history.redo@1`

Redo the last undone change.

Steps the current branch forward one version. The result is the version that is current afterwards and its label: the step that was redone.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Redo** (`song_undone`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```

## `history.undo@1`

Undo the last change on the current branch.

Steps the current branch back one version. The result is the version that is current afterwards and its label: the step before the one undone, not the undone step (history.list shows both).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Undo** (`song`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```
