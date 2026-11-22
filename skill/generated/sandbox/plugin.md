# `plugin` commands

The plug-ins the sandbox can load: list, load, remove, describe.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `plugin.describe@1`

Describe an instance: parameters, patch format, and current problems.

detail summary: parameters grouped as compact 'key = value (range)' lines plus a prose description of the current sound. detail full: every parameter's schema entry with current value and text, and the patch JSON Schema. inactive lists the parameters that do not apply with the current values (summary leaves them out): their entries carry "active": false, and active_when says what makes them apply (another parameter's values), or their module is not in the patch (add it with patch.merge). Semantic plugins are programmed with param.set (physical units) or patch.merge.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `detail` | string |  |  | "summary" | One of: summary, full. |

### Result

```json
{
  "type": "object",
  "title": "plugin_description",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "params": {},
    "patch_type": {
      "type": "string"
    },
    "patch_schema": {},
    "description": {
      "type": "string"
    },
    "docs": {
      "type": "string"
    },
    "status_available": {
      "type": "boolean",
      "description": "The instance supplied its current problems"
    },
    "problems": {
      "description": "Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Parameters that do not apply now"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "name", "params", "description", "status_available"],
  "additionalProperties": false
}

```

### Examples

**Summarize the effect** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "params": "<any>",
  "patch_type": "<any>",
  "description": "<any>",
  "status_available": false
}

```

## `plugin.list@1`

List the CLAP plugins the sandbox can load.

Scans the plugin path once (rescan: true to scan again). filter matches the id or name, case-insensitively. Load one with plugin.load.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `filter` | string |  |  |  | Text to match in the id or name. |
| `rescan` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "plugin_list",
  "properties": {
    "plugins": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "vendor": {
            "type": "string"
          },
          "instrument": {
            "type": "boolean"
          },
          "semantic": {
            "type": "boolean"
          },
          "features": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "path": {
            "type": "string"
          },
          "version": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean",
            "description": "Not loadable by id: an earlier location on the plugin path has the same id (load it by path)"
          }
        },
        "required": [
          "id",
          "name",
          "vendor",
          "instrument",
          "semantic",
          "features",
          "path",
          "version"
        ],
        "additionalProperties": false
      }
    },
    "scan": {
      "type": "object",
      "title": "scan_report",
      "properties": {
        "failures": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_failure",
            "properties": {
              "module": {
                "type": "string"
              },
              "reason": {
                "type": "object",
                "title": "scan_reason",
                "properties": {
                  "code": {
                    "type": "string"
                  },
                  "message": {
                    "type": "string"
                  },
                  "path": {
                    "type": "string"
                  },
                  "hint": {
                    "type": "string"
                  },
                  "details": {
                    "type": "array",
                    "items": {}
                  }
                },
                "required": ["code", "message", "path", "hint"],
                "additionalProperties": false
              },
              "cached": {
                "type": "boolean"
              }
            },
            "required": ["module", "reason", "cached"],
            "additionalProperties": false
          }
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "scan_reason",
            "properties": {
              "code": {
                "type": "string"
              },
              "message": {
                "type": "string"
              },
              "path": {
                "type": "string"
              },
              "hint": {
                "type": "string"
              },
              "details": {
                "type": "array",
                "items": {}
              }
            },
            "required": ["code", "message", "path", "hint"],
            "additionalProperties": false
          }
        },
        "cached_modules": {
          "type": "integer"
        }
      },
      "required": ["failures", "warnings", "cached_modules"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugins", "scan"],
  "additionalProperties": false
}

```

### Examples

**List everything** (`empty`)

```json
{}

```

Result:

```json
{
  "plugins": "<any>",
  "scan": "<any>"
}

```

## `plugin.load@1`

Load a plugin into the chain and return its instance id.

plugin is an id from plugin.list or a .clap path. An instrument goes first in the chain, anything else last, unless position says otherwise (instruments must stay first). Then describe it with plugin.describe.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  | Plugin id or .clap path. |
| `position` | integer |  | 0.0.. |  | Chain index (0 = first). |

### Result

```json
{
  "type": "object",
  "title": "plugin_load",
  "properties": {
    "instance": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "semantic": {
      "type": "boolean"
    },
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["instance", "name", "semantic", "chain"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_plugin`: no plugin with that id or path Hint: closest ids in the hint; see plugin.list
- `sandbox.chain_order`: an instrument would not be first, or a second instrument Hint: remove the existing instrument or load an effect
- `sandbox.position`: position past the end of the chain Hint: 0 .. chain length

### Examples

**Load an instrument** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "instance": "in_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["in_1"]
}

```

**Add an effect after the instrument** (`synth_loaded`)

```json
{
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "instance": "in_3",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["in_1", "in_2", "in_3"]
}

```

## `plugin.remove@1`

Remove a plugin instance from the chain.

Unloads the instance; its id is not reused.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |

### Result

```json
{
  "type": "object",
  "title": "chain",
  "properties": {
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["chain"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_instance`: no such instance Hint: the hint lists loaded instances

### Examples

**Remove the effect** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "chain": ["in_1"]
}

```
