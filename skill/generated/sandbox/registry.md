# `registry` commands

The command registry: list, describe and group the commands.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `registry.describe@1`

Describe a command.

Describe a command.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `command` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  }
}

```

### Examples

**Describe registry.list** (`empty`)

```json
{
  "command": "registry.list"
}

```

Result:

```json
{
  "name": "registry.list",
  "version": 1,
  "kind": "query",
  "exposure": "mcp-exec",
  "summary": "List available commands.",
  "description": "List available commands.",
  "tags": [],
  "args": {
    "type": "object",
    "properties": {
      "tag": {
        "type": "string"
      },
      "kind": {
        "type": "string"
      },
      "text": {
        "type": "string"
      },
      "group": {
        "type": "string"
      }
    },
    "required": [],
    "additionalProperties": false
  },
  "result": {
    "type": "object",
    "properties": {
      "warnings": {
        "type": "array",
        "items": {
          "type": "object"
        }
      }
    }
  },
  "errors": [],
  "examples": [
    {
      "title": "List commands",
      "args": {
        "text": "registry.list"
      },
      "result": {
        "commands": [
          {
            "name": "registry.list",
            "version": 1,
            "kind": "query",
            "summary": "List available commands.",
            "exposure": "mcp-exec"
          }
        ]
      },
      "fixture": "empty"
    }
  ]
}

```

## `registry.groups@1`

List the command groups, with one line each.

List the command groups, with one line each.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  }
}

```

### Examples

**List one group** (`empty`)

```json
{
  "group": "registry"
}

```

Result:

```json
{
  "groups": [
    {
      "name": "registry",
      "count": 3,
      "summary": "The command registry: list, describe and group the commands."
    }
  ]
}

```

## `registry.list@1`

List available commands.

List available commands.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `tag` | string |  |  |  |  |
| `kind` | string |  |  |  |  |
| `text` | string |  |  |  |  |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  }
}

```

### Examples

**List commands** (`empty`)

```json
{
  "text": "registry.list"
}

```

Result:

```json
{
  "commands": [
    {
      "name": "registry.list",
      "version": 1,
      "kind": "query",
      "summary": "List available commands.",
      "exposure": "mcp-exec"
    }
  ]
}

```
