# `preset` commands

User presets and factory patches: list, load, save.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `preset.list@1`

List user presets, or a plugin's read-only factory patches.

source: user (default) lists saved presets. source: factory requires plugin and lists its immutable factory patches in declaration order.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `source` | string |  |  | "user" | One of: user, factory. |

### Result

```json
{
  "type": "object",
  "title": "preset_list",
  "properties": {
    "presets": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "preset",
        "properties": {
          "plugin": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "tags": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "read_only": {
            "type": "boolean"
          }
        },
        "required": ["plugin", "name", "tags"],
        "additionalProperties": false
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["presets"],
  "additionalProperties": false
}

```

### Examples

**List the effect's presets** (`synth_with_preset`)

```json
{
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "presets": [
    {
      "plugin": "com.suppleaudio.test.gain",
      "name": "Quiet",
      "tags": ["soft"]
    }
  ]
}

```

## `preset.load@1`

Load a named preset into an instance.

The preset must belong to the instance's plugin (see preset.list). source: factory applies a read-only factory patch; user (default) loads a saved preset.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `name` | string |  |  |  |  |
| `source` | string |  |  | "user" | One of: user, factory. |

### Result

```json
{
  "type": "object",
  "title": "description",
  "properties": {
    "description": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_preset`: no preset with that name for this plugin Hint: closest names in the hint

### Examples

**Load** (`synth_with_preset`)

```json
{
  "instance": "in_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "description": "<any>"
}

```

## `preset.save@1`

Save an instance's current settings as a named preset.

Stores the patch (semantic plugins) or the plugin state. Overwrites a preset of the same name for the same plugin. Names: 1-64 of A-Z a-z 0-9 space _ . + -.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `name` | string |  |  |  |  |
| `tags` | array |  |  |  |  |
| `notes` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "path",
  "properties": {
    "path": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.preset_name`: the name has characters outside the allowed set Hint: letters, digits, space _ . + -

### Examples

**Save** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "name": "Quiet",
  "tags": ["soft"]
}

```

Result:

```json
{
  "path": "<any>"
}

```
