# Command reference

Generated from the command registry.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code, message, path}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

## `batch@1`

Run commands atomically.

Run commands atomically.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `label` | string |  |  |  |  |
| `steps` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**Empty batch** (`empty`)

```json
{
  "steps": []
}

```

Result:

```json
{
  "label": "",
  "results": []
}

```

## `job.cancel@1`

Cancel a job.

Cancel a job.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**Cancel job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "cancelled",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.get@1`

Get a job's status and progress.

Get a job's status and progress.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**Get render job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.list@1`

List session jobs.

List session jobs.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**List jobs** (`empty`)

```json
{}

```

Result:

```json
{
  "jobs": []
}

```

## `job.status@1`

Get a job's status.

Get a job's status.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**Get job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.wait@1`

Wait for a job.

Wait for a job. Blocks for up to timeout_s seconds (default 30) until the job finishes; a larger timeout_s is accepted and clamped to 30, and the result then carries "timeout_clamped_s": 30. Otherwise the result is what job.status returns.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |
| `timeout_s` | number |  | 0.0.. |  | Seconds to block (default 30). A larger value is clamped to 30 and reported as timeout_clamped_s. |

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  }
}

```

### Examples

**Wait for job** (`job_running`)

```json
{
  "job": "jb_1",
  "timeout_s": 0
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

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
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
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
          "type": "object",
          "properties": {
            "code": {
              "type": "string"
            },
            "message": {
              "type": "string"
            },
            "path": {
              "type": "string"
            }
          },
          "required": ["code", "message", "path"],
          "additionalProperties": false
        },
        "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
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
            "summary": "List available commands."
          }
        ]
      },
      "fixture": "empty"
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

### Result

```json
{
  "type": "object",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
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
      "summary": "List available commands."
    }
  ]
}

```

## `toy.add@1`

Add to the toy counter.

Returns the new counter value.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `value` | integer |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

**Add three** (`empty`)

```json
{
  "value": 3
}

```

Result:

```json
{
  "count": 3
}

```

## `toy.bad@1`

Return an invalid result.



Kind: `query`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "message",
  "properties": {
    "text": {
      "type": "string",
      "minLength": 2
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["text"],
  "additionalProperties": false
}

```

### Examples

## `toy.fail@1`

Fail after changing the toy.



Kind: `edit`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

## `toy.get@1`

Get the toy counter.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

**Get zero** (`empty`)

```json
{}

```

Result:

```json
{
  "count": 0
}

```

## `toy.safe@1`

Run a safe action.



Kind: `action`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

## `toy.set@1`

Set the toy counter.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `value` | integer |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

**Set five** (`empty`)

```json
{
  "value": 5
}

```

Result:

```json
{
  "count": 5
}

```

## `toy.start@1`

Start a long toy job.



Kind: `action`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "job_started",
  "properties": {
    "job": {
      "type": "string"
    },
    "status": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["job", "status"],
  "additionalProperties": false
}

```

### Examples

## `toy.target.get@1`

Get a toy by reference.



Kind: `query`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `target` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

## `toy.unsafe@1`

Run an unsafe action.



Kind: `action`. Exposure: `internal`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "code": {
            "type": "string"
          },
          "message": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["code", "message", "path"],
        "additionalProperties": false
      },
      "description": "Command warnings followed by bus diagnostics {code, message, path}; absent when none."
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples
