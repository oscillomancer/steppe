# `scene` commands

Launcher scenes (columns): add, insert, move, capture, set.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `scene.add@1`

Add a scene (launcher column) at the end or an index.

at is the column (0-based, default: after the last); the scenes from there on move one column right. A project holds at most 1024 scenes.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `at` | integer |  | 0.0..1024.0 |  | Column, 0-based. |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Errors

- `scene.limit`: the project has 1024 scenes Hint: remove a scene first
- `scene.index`: at is past the last column Hint: use 0 to the scene count

### Examples

**A verse** (`song`)

```json
{
  "name": "Verse"
}

```

Result:

```json
{
  "id": "sc_1"
}

```

## `scene.capture@1`

Make a scene from a section's items, one clip per track.

For each channel the items overlapping the region must be one item lying wholly inside it: several are scene.capture.items, one that starts before or ends after it scene.capture.edge, one that starts after its start scene.capture.offset (a clip begins at its own start); a region holding no item is scene.capture.empty. Items on a master are skipped with a warning. Each item becomes a clip as clip.add {item} makes it (MIDI copied unless pooled; a warped take is clip.unsupported): loop mode when it fills the region, once when it is shorter, named as the item is (its active take's name). The scene is added after the last, named after the region unless name is given, with its colour. The region and the items are unchanged. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `region` | string |  |  |  | Region id, e.g. "rg_1". |
| `name` | string |  |  |  |  |
| `pooled` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "scene_capture",
  "properties": {
    "scene": {
      "type": "string"
    },
    "clips": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "laid_clip",
        "properties": {
          "clip": {
            "type": "string"
          },
          "source_item": {
            "type": "string"
          },
          "track": {
            "type": "string"
          }
        },
        "required": ["clip", "source_item", "track"],
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
  "required": ["scene", "clips"],
  "additionalProperties": false
}

```

### Errors

- `scene.capture.empty`: the region holds no item Hint: choose a section with items
- `scene.capture.items`: several items of one track inside the region Hint: consolidate them (item.consolidate) or capture a smaller region
- `scene.capture.edge`: an item starts before or ends after the region Hint: split it at the region's edge (item.split) or widen the region
- `scene.capture.offset`: an item starts after the region's start Hint: move it to the start of the region (item.move)
- `clip.unsupported`: a warped take Hint: reset its warp first
- `scene.limit`: the project has 1024 scenes Hint: remove a scene first
- `clip.limit`: the project has 16384 clips Hint: remove clips first
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

### Examples

**Capture the verse** (`song_with_section`)

```json
{
  "region": "rg_1"
}

```

Result:

```json
{
  "scene": "<any>",
  "clips": "<any>"
}

```

## `scene.insert@1`

Lay a scene's clips on the timeline as items and a section.

The span starts at bar (1-based) or at (beats, on a bar line) and lasts bars whole bars. Each clip becomes an item on its track at the start, as item.add {clip} makes it: a once clip lasts its length or the span's if shorter; a loop clip lasts the span, as one looping item when it repeats within it, which needs its period to be its content: loop off with its length (as clip.add makes it), or loop on over the whole content with a length of whole repeats (scene.insert.period otherwise); an audio clip with fades that repeats is laid one item per pass, each faded as its pass, and a pass the span's end cuts inside its fade-out plays it out past the end. Takes are copied, MIDI with them unless pooled; an item takes its clip's name (its active take's) when the clip has one. follow (none, key or chords; default: each clip's own) is set on each laid MIDI take with the clip's context, so its notes sound as the key or chords under the item say, mapped from those they were written against (ADR-0098, "following is playback"): the notes stay as written, a context edit moves them, and take.bake writes the sounding pitches. Each item's followed says how they sound, by bar, and copied why a clip that cannot follow was left as it is; pooled may follow too. With none the notes sound as written, and context.mismatch warns once for each item whose clip's key (or, in the same key, chord) is not the project's under it. An item already in the span on a track that receives a clip is refused (scene.insert.occupied), and an empty scene (scene.empty). A section named after the scene (name overrides) is added over the span. write_context writes the clips' own key and chords onto the context track under the span first (context in the answer says what happened). One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `at` | number |  | 0.0.. |  | Start in beats, on a bar line. |
| `bar` | integer |  | 1.0..1e+05 |  | Start bar, 1-based. |
| `bars` | integer |  | 1.0.. |  |  |
| `name` | string |  |  |  |  |
| `pooled` | boolean |  |  | false |  |
| `follow` | string |  |  |  | How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow. One of: none, key, chords. |
| `write_context` | boolean |  |  | false | Write the scene's clips' own key and chords onto the context track under each section that has none (shifted to the section, repeating with a loop clip), in the same undo step, before the clips are laid. Left alone: a section with points under it; not written: one whose clips disagree. The answer's context says which (default false). |

### Result

```json
{
  "type": "object",
  "title": "scene_insert",
  "properties": {
    "items": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "laid_item",
        "properties": {
          "clip": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "track": {
            "type": "string"
          },
          "followed": {
            "type": "object",
            "title": "followed",
            "properties": {
              "mode": {
                "type": "string",
                "enum": ["none", "key", "chords"]
              },
              "regions": {
                "type": "array",
                "items": {
                  "type": "object",
                  "title": "followed_region",
                  "properties": {
                    "from_bar": {
                      "type": "integer",
                      "description": "First project bar of the run."
                    },
                    "to_bar": {
                      "type": "integer",
                      "description": "Last project bar of the run (inclusive)."
                    },
                    "from_key": {
                      "type": "string"
                    },
                    "to_key": {
                      "type": "string"
                    },
                    "by": {
                      "type": "string",
                      "enum": ["transpose", "degree", "chords", "copy"]
                    },
                    "interval": {
                      "type": "integer",
                      "description": "Semitones, for transpose."
                    }
                  },
                  "required": ["from_bar", "to_bar", "by"],
                  "additionalProperties": false
                }
              },
              "folded": {
                "type": "integer"
              },
              "copied": {
                "type": "string",
                "enum": ["drums", "audio", "no_key", "no_notes"]
              },
              "mismatch": {
                "type": "object",
                "title": "followed_mismatch",
                "properties": {
                  "kind": {
                    "type": "string",
                    "enum": ["key", "chord"],
                    "description": "What differs: the key, or (in the same key) the chord."
                  },
                  "clip_key": {
                    "type": "string",
                    "description": "kind key: the clip's key."
                  },
                  "key": {
                    "type": "string",
                    "description": "kind key: the project's key there, which differs."
                  },
                  "clip_chord": {
                    "type": "string",
                    "description": "kind chord: the chord the clip was written over."
                  },
                  "chord": {
                    "type": "string",
                    "description": "kind chord: the project's chord there, which differs."
                  },
                  "bar": {
                    "type": "integer",
                    "description": "The first project bar where they differ."
                  }
                },
                "required": ["kind", "bar"],
                "additionalProperties": false,
                "description": "Set with the context.mismatch warning: the notes play as written under a key, or in the same key over a chord, that is not the clip's."
              }
            },
            "required": ["mode"],
            "additionalProperties": false
          }
        },
        "required": ["clip", "item", "track"],
        "additionalProperties": false
      }
    },
    "region": {
      "type": "string"
    },
    "start": {
      "type": "number",
      "description": "The span's start, in beats."
    },
    "end": {
      "type": "number",
      "description": "The span's end, in beats."
    },
    "context": {
      "type": "object",
      "title": "context_written",
      "properties": {
        "written": {
          "type": "boolean"
        },
        "keys": {
          "type": "integer",
          "description": "Key points written under the section."
        },
        "chords": {
          "type": "integer",
          "description": "Chord points written under the section."
        },
        "reason": {
          "type": "string",
          "enum": ["present", "disagree", "holds", "no_context"],
          "description": "Why nothing was written: the context track already has points under the section, the clips disagree, it already says the same, or no clip has a context."
        },
        "clips": {
          "type": "array",
          "items": {
            "type": "string"
          }
        },
        "bar": {
          "type": "integer"
        },
        "message": {
          "type": "string"
        }
      },
      "required": ["written", "keys", "chords", "clips"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["items", "region", "start", "end"],
  "additionalProperties": false
}

```

### Errors

- `scene.at`: at is not on a bar line, or neither or both of at and bar Hint: give bar, or an at on a bar line
- `scene.bars`: bars is out of range Hint: give a whole number of bars from 1
- `scene.empty`: the scene has no clips Hint: add clips with clip.add
- `scene.insert.occupied`: an item is already in the span on a track that gets a clip Hint: move or remove it, or choose another bar
- `scene.insert.period`: a repeating clip no single looping item reproduces Hint: loop the clip over its whole content, or ask for no more bars than it is long
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

### Examples

**A verse at bar 5** (`song_with_clip`)

```json
{
  "scene": "sc_1",
  "bar": 5,
  "bars": 4
}

```

Result:

```json
{
  "items": "<any>",
  "region": "<any>",
  "start": 16.0,
  "end": 32.0
}

```

## `scene.list@1`

List scenes in column order with clip counts.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "scene_list",
  "properties": {
    "scenes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "scene_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "order": {
            "type": "integer"
          },
          "clips": {
            "type": "integer"
          }
        },
        "required": ["id", "name", "order", "clips"],
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
  "required": ["scenes"],
  "additionalProperties": false
}

```

### Examples

**Two scenes** (`song_with_clip`)

```json
{}

```

Result:

```json
{
  "scenes": [
    {
      "id": "sc_1",
      "name": "Verse",
      "order": 0,
      "clips": 1
    },
    {
      "id": "sc_2",
      "name": "Chorus",
      "order": 1,
      "clips": 0
    }
  ]
}

```

## `scene.move@1`

Move a scene to another column.

to is the column it ends up in (0-based); the others close up around it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `to` | integer |  | 0.0..1023.0 |  | Column. |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "additionalProperties": false
}

```

### Errors

- `scene.index`: to is past the last column Hint: use 0 to the count minus 1

### Examples

**Chorus first** (`song_with_clip`)

```json
{
  "scene": "sc_2",
  "to": 0
}

```

Result:

```json
{}

```

## `scene.remove@1`

Remove a scene with its clips and their takes.

The scenes after it move one column left. A clip.removed warning names the clips removed with it.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "additionalProperties": false
}

```

### Examples

**An empty scene** (`song_with_clip`)

```json
{
  "scene": "sc_2"
}

```

Result:

```json
{}

```

## `scene.set@1`

Rename or recolour a scene.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "additionalProperties": false
}

```

### Examples

**Rename** (`song_with_clip`)

```json
{
  "scene": "sc_1",
  "name": "Verse 1"
}

```

Result:

```json
{}

```
