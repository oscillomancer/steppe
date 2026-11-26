# `group` commands

Track groups: members, links and targets.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `group.add@1`

Create a named track group with optional links and members.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `active` | boolean |  |  |  |  |
| `members` | array |  |  |  |  |
| `links` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_ref",
  "properties": {
    "group": {
      "type": "string",
      "pattern": "^gr_[0-9]+$"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["group"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Group the drums** (`song`)

```json
{
  "name": "Drums",
  "members": ["tr_1"]
}

```

Result:

```json
{
  "group": "gr_1"
}

```

## `group.describe@1`

Describe a track group's membership, links and VCA ancestry.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_entry",
  "properties": {
    "group": {
      "type": "string",
      "pattern": "^gr_[0-9]+$"
    },
    "state": {
      "type": "object",
      "title": "track_group",
      "properties": {
        "name": {
          "type": "string",
          "default": ""
        },
        "color": {
          "type": "string",
          "default": ""
        },
        "active": {
          "type": "boolean",
          "default": true
        },
        "members": {
          "type": "array",
          "items": {
            "type": "string",
            "pattern": "^[a-z]{2}_[0-9]+$"
          },
          "default": []
        },
        "links": {
          "type": "array",
          "items": {
            "type": "string",
            "pattern": "^(volume|pan|mute|solo|armed|monitor|selection|edit)$"
          },
          "default": ["volume", "mute", "solo"]
        },
        "vca": {
          "type": "object",
          "title": "vca",
          "properties": {
            "volume_db": {
              "type": "number",
              "minimum": -150.0,
              "maximum": 12.0,
              "default": 0.0
            },
            "mute": {
              "type": "boolean",
              "default": false
            },
            "solo": {
              "type": "boolean",
              "default": false
            },
            "followers": {
              "type": "array",
              "items": {
                "type": "string",
                "pattern": "^[a-z]{2}_[0-9]+$"
              },
              "default": []
            },
            "learn": {
              "type": "array",
              "items": {
                "type": "object",
                "title": "control_learn",
                "properties": {
                  "param": {
                    "type": "string"
                  },
                  "midi": {
                    "type": "object",
                    "title": "midi_control",
                    "properties": {
                      "kind": {
                        "type": "string",
                        "enum": ["cc", "note", "pitch_bend", "program"]
                      },
                      "channel": {
                        "type": "integer",
                        "minimum": 1.0,
                        "maximum": 16.0
                      },
                      "number": {
                        "type": "integer",
                        "minimum": 0.0,
                        "maximum": 127.0,
                        "default": 0
                      },
                      "device": {
                        "type": "string",
                        "default": ""
                      }
                    },
                    "required": ["kind", "channel"],
                    "additionalProperties": false
                  },
                  "mode": {
                    "type": "string",
                    "enum": ["absolute", "relative1", "relative2", "relative3", "toggle"],
                    "default": "absolute"
                  },
                  "soft_takeover": {
                    "type": "boolean",
                    "default": false
                  }
                },
                "required": ["param", "midi"],
                "additionalProperties": false
              },
              "default": []
            }
          },
          "additionalProperties": false
        }
      },
      "additionalProperties": false
    },
    "controllers": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["group", "state", "controllers"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Describe drums** (`song_with_group`)

```json
{
  "group": "gr_1"
}

```

Result:

```json
{
  "group": "gr_1",
  "state": "<any>",
  "controllers": []
}

```

## `group.list@1`

List track groups, linked attributes and optional VCA controls.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "group_list",
  "properties": {
    "groups": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "group_entry",
        "properties": {
          "group": {
            "type": "string",
            "pattern": "^gr_[0-9]+$"
          },
          "state": {
            "type": "object",
            "title": "track_group",
            "properties": {
              "name": {
                "type": "string",
                "default": ""
              },
              "color": {
                "type": "string",
                "default": ""
              },
              "active": {
                "type": "boolean",
                "default": true
              },
              "members": {
                "type": "array",
                "items": {
                  "type": "string",
                  "pattern": "^[a-z]{2}_[0-9]+$"
                },
                "default": []
              },
              "links": {
                "type": "array",
                "items": {
                  "type": "string",
                  "pattern": "^(volume|pan|mute|solo|armed|monitor|selection|edit)$"
                },
                "default": ["volume", "mute", "solo"]
              },
              "vca": {
                "type": "object",
                "title": "vca",
                "properties": {
                  "volume_db": {
                    "type": "number",
                    "minimum": -150.0,
                    "maximum": 12.0,
                    "default": 0.0
                  },
                  "mute": {
                    "type": "boolean",
                    "default": false
                  },
                  "solo": {
                    "type": "boolean",
                    "default": false
                  },
                  "followers": {
                    "type": "array",
                    "items": {
                      "type": "string",
                      "pattern": "^[a-z]{2}_[0-9]+$"
                    },
                    "default": []
                  },
                  "learn": {
                    "type": "array",
                    "items": {
                      "type": "object",
                      "title": "control_learn",
                      "properties": {
                        "param": {
                          "type": "string"
                        },
                        "midi": {
                          "type": "object",
                          "title": "midi_control",
                          "properties": {
                            "kind": {
                              "type": "string",
                              "enum": ["cc", "note", "pitch_bend", "program"]
                            },
                            "channel": {
                              "type": "integer",
                              "minimum": 1.0,
                              "maximum": 16.0
                            },
                            "number": {
                              "type": "integer",
                              "minimum": 0.0,
                              "maximum": 127.0,
                              "default": 0
                            },
                            "device": {
                              "type": "string",
                              "default": ""
                            }
                          },
                          "required": ["kind", "channel"],
                          "additionalProperties": false
                        },
                        "mode": {
                          "type": "string",
                          "enum": ["absolute", "relative1", "relative2", "relative3", "toggle"],
                          "default": "absolute"
                        },
                        "soft_takeover": {
                          "type": "boolean",
                          "default": false
                        }
                      },
                      "required": ["param", "midi"],
                      "additionalProperties": false
                    },
                    "default": []
                  }
                },
                "additionalProperties": false
              }
            },
            "additionalProperties": false
          },
          "controllers": {
            "type": "array",
            "items": {
              "type": "string",
              "pattern": "^[a-z]{2}_[0-9]+$"
            }
          }
        },
        "required": ["group", "state", "controllers"],
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
  "required": ["groups"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**List groups** (`empty`)

```json
{}

```

Result:

```json
{
  "groups": []
}

```

## `group.members.set@1`

Replace a group's track membership; an empty array clears it.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `members` | array |  |  |  |  |

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

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Assign a track** (`song_with_group`)

```json
{
  "group": "gr_1",
  "members": ["tr_1"]
}

```

Result:

```json
{}

```

## `group.remove@1`

Remove a track group and its VCA, keeping its tracks.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |

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

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Remove** (`song_with_vca`)

```json
{
  "group": "gr_1"
}

```

Result:

```json
{}

```

## `group.set@1`

Change a group's name, color, links or active state.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `group` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `active` | boolean |  |  |  |  |
| `links` | array |  |  |  |  |

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

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Suspend linked edits** (`song_with_group`)

```json
{
  "group": "gr_1",
  "active": false
}

```

Result:

```json
{}

```

## `group.targets@1`

Resolve linked tracks or matching items for an edit or selection.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `item` | string |  |  |  | Item id, e.g. "it_1" (a clip's id is refused with item.kind). |
| `attribute` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "group_targets",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "items": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[a-z]{2}_[0-9]+$"
      }
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks", "items"],
  "additionalProperties": false
}

```

### Errors

- `group.unknown`: no such group Hint: use group.list
- `group.member`: invalid track membership Hint: use each existing track once
- `vca.cycle`: nested VCA control is cyclic Hint: remove the cycle's follower link
- `vca.limit`: more than 32 controlling VCAs Hint: remove a control link

### Examples

**Resolve volume links** (`song_with_group`)

```json
{
  "track": "tr_1",
  "attribute": "volume"
}

```

Result:

```json
{
  "tracks": ["tr_1"],
  "items": []
}

```
