# `arrangement` commands

Lay a song out from scenes, each for some bars.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `arrangement.build@1`

Lay a song out from scenes, each for some bars, in order.

scene.insert for each entry of sequence in turn, each starting where the last ended, from bar or at (default bar 1), with a section for each. A scene may appear more than once. Every entry's refusal (scene.empty, scene.insert.period, scene.insert.occupied) names its index in path, and changes nothing; an empty sequence or one of more than 256 entries is scene.sequence. follow, pooled and each item's followed and context.mismatch are as scene.insert's. write_context writes each section's harmony from its scene's clips where the context track has none: a section with points under it is left alone, one whose clips disagree gets nothing, and each segment's context says which. One undo step.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `sequence` | array |  |  |  |  |
| `sequence[].scene` | string |  |  |  | Scene id, e.g. "sc_1". |
| `sequence[].bars` | integer |  | 1.0.. |  |  |
| `at` | number |  | 0.0.. |  | Start in beats, on a bar line. |
| `bar` | integer |  | 1.0..1e+05 |  | Start bar, 1-based [1]. |
| `pooled` | boolean |  |  | false |  |
| `follow` | string |  |  |  | How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow. One of: none, key, chords. |
| `write_context` | boolean |  |  | false | Write the scene's clips' own key and chords onto the context track under each section that has none (shifted to the section, repeating with a loop clip), in the same undo step, before the clips are laid. Left alone: a section with points under it; not written: one whose clips disagree. The answer's context says which (default false). |

### Result

```json
{
  "type": "object",
  "title": "arrangement_build",
  "properties": {
    "segments": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "arrangement_segment",
        "properties": {
          "scene": {
            "type": "string"
          },
          "region": {
            "type": "string"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          },
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
          }
        },
        "required": ["scene", "region", "start", "end", "items"],
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
  "required": ["segments"],
  "additionalProperties": false
}

```

### Errors

- `scene.sequence`: the sequence is empty or longer than 256 entries Hint: give 1 to 256 entries of {scene, bars}
- `scene.empty`: a scene in the sequence has no clips Hint: add clips with clip.add
- `scene.insert.occupied`: items are already in an entry's span Hint: move or remove them, or start at another bar
- `scene.insert.period`: a repeating clip no single looping item reproduces Hint: loop the clip over its whole content, or ask for fewer bars
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

### Examples

**A verse for four bars from bar 5** (`song_with_clip`)

```json
{
  "sequence": [
    {
      "scene": "sc_1",
      "bars": 4
    }
  ],
  "bar": 5
}

```

Result:

```json
{
  "segments": "<any>"
}

```
