# `context` commands

The song context (key, chord, section) at a beat: read and write.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `context.at@1`

What key, chord and section hold at a beat or bar.

Give `at` (project beats) or `bar` (1-based). The key and chord are the last points at or before it; a section is a region with section true that covers it (the latest start wins). known false means the point is there but its scale or chord symbol is not one this build reads.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `at` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_at",
  "properties": {
    "beat": {
      "type": "number"
    },
    "bar": {
      "type": "integer"
    },
    "beat_in_bar": {
      "type": "number"
    },
    "key": {
      "type": "object",
      "title": "context_key",
      "properties": {
        "id": {
          "type": "string"
        },
        "tonic": {
          "type": "string"
        },
        "scale": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "tonic", "scale", "from_beat", "known"],
      "additionalProperties": false
    },
    "chord": {
      "type": "object",
      "title": "context_chord",
      "properties": {
        "id": {
          "type": "string"
        },
        "symbol": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "symbol", "from_beat", "known"],
      "additionalProperties": false
    },
    "section": {
      "type": "object",
      "title": "context_section",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "start": {
          "type": "number"
        },
        "end": {
          "type": "number"
        }
      },
      "required": ["id", "name", "start", "end"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["beat", "bar", "beat_in_bar"],
  "additionalProperties": false
}

```

### Examples

**At bar 2** (`song_with_context`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "beat": 4,
  "bar": 2,
  "beat_in_bar": 1,
  "key": {
    "id": "cx_1",
    "tonic": "Eb",
    "scale": "major",
    "from_beat": 0,
    "known": true
  },
  "chord": {
    "id": "cx_3",
    "symbol": "Cm7",
    "from_beat": 4,
    "known": true
  },
  "section": {
    "id": "rg_1",
    "name": "Verse",
    "start": 0,
    "end": 8
  }
}

```

## `context.get@1`

List the key and chord points in a range of beats.

Points from `from` (default 0) to `to` (default: the end), by beat, with the key and chord that hold at `from`, and `text`: the same as a lead sheet in project bars, one line per bar, for context.write. In it `{Cm7}:h` is a chord point at the cursor, `r:h` a chord point with no chord, `~:h` a hold (no point), `key Eb major` a key point at the cursor; a bar's durations fill the bar. The bars run from the one holding `from` to the one holding `to` (exclusive when `to` is a bar line), or to the last point.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | number |  | 0.0.. |  |  |
| `to` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_get",
  "properties": {
    "text": {
      "type": "string"
    },
    "points": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "context_point_row",
        "properties": {
          "id": {
            "type": "string"
          },
          "type": {
            "type": "string"
          },
          "beat": {
            "type": "number"
          },
          "bar": {
            "type": "integer"
          },
          "tonic": {
            "type": "string"
          },
          "scale": {
            "type": "string"
          },
          "symbol": {
            "type": "string"
          }
        },
        "required": ["id", "type", "beat", "bar"],
        "additionalProperties": false
      }
    },
    "key": {
      "type": "object",
      "title": "context_key",
      "properties": {
        "id": {
          "type": "string"
        },
        "tonic": {
          "type": "string"
        },
        "scale": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "tonic", "scale", "from_beat", "known"],
      "additionalProperties": false
    },
    "chord": {
      "type": "object",
      "title": "context_chord",
      "properties": {
        "id": {
          "type": "string"
        },
        "symbol": {
          "type": "string"
        },
        "from_beat": {
          "type": "number"
        },
        "known": {
          "type": "boolean"
        }
      },
      "required": ["id", "symbol", "from_beat", "known"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["points"],
  "additionalProperties": false
}

```

### Examples

**Nothing yet** (`empty`)

```json
{}

```

Result:

```json
{
  "points": []
}

```

## `context.write@1`

Write key and chord points from a lead sheet.

text is context.get's lead sheet: one line per project bar, 'N: ' then {SYMBOL}:DUR (a chord point at the cursor), r:DUR (a chord point with no chord), ~:DUR (hold: no point) and 'key TONIC SCALE' (a key point at the cursor, no time), where DUR is the notation's duration (q, h., h+q, 480t) and a bar's durations must fill it. Bars run in order from the first named to the last, one per line or, as in notation, after a ; ('1: ... ; 2: ...') or after a | token ('1: ... | 2: ...', the number after | optional). The write is a diff over that span: points equal in beat and content are left alone (ids kept), points in the span the text omits are removed, the rest are added or replaced. History records the key.set, chord.set and remove operations it came to, so branches that write different bars merge. Keys and chords both follow the text: to leave the key alone, include it. Errors name the bar; nothing is changed when one occurs.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `text` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "context_write",
  "properties": {
    "first_bar": {
      "type": "integer"
    },
    "last_bar": {
      "type": "integer"
    },
    "added": {
      "type": "integer"
    },
    "replaced": {
      "type": "integer"
    },
    "removed": {
      "type": "integer"
    },
    "unchanged": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["first_bar", "last_bar", "added", "replaced", "removed", "unchanged"],
  "additionalProperties": false
}

```

### Examples

**Two bars of changes** (`empty`)

```json
{
  "text": "1: key Eb major {Ebmaj7}:w\n2: {Cm7}:h {Fm7}:h\n"
}

```

Result:

```json
{
  "first_bar": 1,
  "last_bar": 2,
  "added": 4,
  "replaced": 0,
  "removed": 0,
  "unchanged": 0
}

```
