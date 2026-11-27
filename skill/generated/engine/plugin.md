# `plugin` commands

Scanned plug-ins: list, scan and override their metadata.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `plugin.list@1`

List scanned plugins; filter matches id, name, or vendor.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `filter` | string |  |  |  |  |

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
        "title": "plugin_entry",
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
          "format": {
            "type": "string",
            "description": "clap or vst3"
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
            },
            "description": "CLAP features (a VST3's subcategories): the main category first, then the kind of plugin, such as reverb"
          },
          "path": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean",
            "description": "Another location earlier on the plugin path has the same id; fx.add by id uses that one"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic", "features", "path"],
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

**Find a test synth** (`empty`)

```json
{
  "filter": "test.sine"
}

```

Result:

```json
{
  "plugins": "<any>",
  "scan": "<any>"
}

```

## `plugin.override@1`

Load a plug-in normally, on a bridge, or not at all for this run.

A safety choice for a plug-in suspected of crashing: mode bridge runs it in a dedicated bridge process (a crash then stays there; a live bridge can drop blocks under load), disable does not load it (its strips pass audio through and a rack module of it is silent), normal loads it as the project says. It holds until the app quits, is not in the project or its history, and is not saved: a project saved while it holds is byte-for-byte the one saved without. Instances of the plug-in restart, so state that was never saved is lost and held notes end. Stock plug-ins cannot be bridged, and bridge needs the bridge executable; both are refused with plugin.override_unavailable.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  | id or name, as plugin.list shows it |
| `mode` | string |  |  |  | One of: normal, bridge, disable. |

### Result

```json
{
  "type": "object",
  "title": "plugin_override_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "format": {
      "type": "string"
    },
    "mode": {
      "type": "string",
      "enum": ["normal", "bridge", "disable"]
    },
    "changed": {
      "type": "boolean",
      "description": "false: it was set that way already"
    },
    "fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "the fx of this plug-in in the project now"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["plugin", "name", "format", "mode", "changed", "fx"],
  "additionalProperties": false
}

```

### Examples

**Load normally** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "mode": "normal"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "format": "clap",
  "mode": "normal",
  "changed": false,
  "fx": []
}

```

## `plugin.overrides@1`

List the plug-ins this run bridges or has disabled.

What plugin.override set, with the fx of each plug-in in the project now. bridge_available: a bridge executable was found, so mode bridge can be set.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "plugin_override_list",
  "properties": {
    "overrides": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_override_entry",
        "properties": {
          "plugin": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "format": {
            "type": "string"
          },
          "mode": {
            "type": "string",
            "enum": ["normal", "bridge", "disable"]
          },
          "fx": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["plugin", "name", "format", "mode", "fx"],
        "additionalProperties": false
      }
    },
    "bridge_available": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["overrides", "bridge_available"],
  "additionalProperties": false
}

```

### Examples

**None set** (`empty`)

```json
{}

```

Result:

```json
{
  "overrides": [],
  "bridge_available": "<any>"
}

```

## `plugin.scan@1`

Rescan configured plugin paths and return the available plugins.

wait: false scans in the background (every module through the supervised scanner) and returns at once with the current list and the scan's job; the catalogue changes when the job is done. A background scan already running is returned instead of a second one.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `wait` | boolean |  |  |  | false: scan in the background (default true) |

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
        "title": "plugin_entry",
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
          "format": {
            "type": "string",
            "description": "clap or vst3"
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
            },
            "description": "CLAP features (a VST3's subcategories): the main category first, then the kind of plugin, such as reverb"
          },
          "path": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic", "features", "path"],
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
    "job": {
      "type": "string",
      "description": "the background scan (wait: false)"
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

**Scan** (`empty`)

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
