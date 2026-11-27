# `transport` commands

Transport: play, stop, seek, record, loop, punch, metronome, rate.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `transport.get@1`

The play position and whether it is playing.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**Stopped at the start** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": 0.0,
  "beat": 0.0,
  "sample": 0,
  "loop": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  },
  "punch": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  },
  "counting_in": false
}

```

## `transport.loop@1`

Set or toggle the loop range: playback reaching its end repeats it.

start and end as seconds, beat or bar (start_bar 5, end_bar 9 = bars 5-8); a range turns the loop on unless enabled says otherwise; enabled alone toggles the current range; clear removes the range and turns the loop off. Recording while looping makes one take per pass on one item. Transport state: not an undo step, not saved in the project.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `clear` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0..2147483647.0 |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "loop",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, end not after start, or clear with a range Hint: e.g. start_bar 5, end_bar 9

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

**Remove the range** (`empty`)

```json
{
  "clear": true
}

```

Result:

```json
{
  "enabled": false,
  "start_seconds": 0,
  "end_seconds": 0
}

```

## `transport.metronome@1`

Turn the metronome on or off and set its level; returns the settings.

A click on every beat of the meter while the transport plays (the bar's first accented), in the live output only: renders and recordings never contain it. count_in_bars: that many bars of clicks (at the meter and tempo where playback starts) before transport.record begins — the position holds and nothing is recorded until the count ends; count_in_playback counts in before transport.play too. The count clicks even with the metronome off. Transport state: not an undo step, not saved in the project. No arguments: the current settings.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `volume_db` | number or string | dB | -60.0..6.0 |  | Quantity strings are range-checked by the server. |
| `count_in_bars` | integer |  | 0.0..8.0 |  |  |
| `count_in_playback` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "metronome",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "volume_db": {
      "type": "number"
    },
    "count_in_bars": {
      "type": "integer"
    },
    "count_in_playback": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "volume_db", "count_in_bars", "count_in_playback"],
  "additionalProperties": false
}

```

### Examples

**On** (`empty`)

```json
{
  "enabled": true
}

```

Result:

```json
{
  "enabled": true,
  "volume_db": -6.0,
  "count_in_bars": 0,
  "count_in_playback": false
}

```

**Two bars of count-in before recording** (`empty`)

```json
{
  "count_in_bars": 2
}

```

Result:

```json
{
  "enabled": false,
  "volume_db": -6.0,
  "count_in_bars": 2,
  "count_in_playback": false
}

```

## `transport.pause@1`

Pause playback where it is (ends a recording).



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.play@1`

Start playback from the current position.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.playrate.apply-to-tempo@1`

Scale every tempo BPM by the master playrate, then reset it to 1.

One version: multiply every BPM by the saved scalar rate, preserving beat positions, ramp flags and meter changes, and reset rate to 1. Require stopped transport, no recording and no active nonempty master playrate envelope. Reject resulting BPM outside 1..1000 before mutation. Existing item timebases still apply; time-locked audio is not made sound-equivalent by this tempo edit.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

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

- `playrate.playing`: the transport is playing Hint: transport.stop before applying playrate to tempo
- `record.playrate`: a recording is active Hint: transport.stop first
- `playrate.envelope`: an active nonempty master playrate envelope exists Hint: disable it with env.set {active: false} before applying the scalar rate
- `tempo.range`: a scaled BPM is outside 1..1000 Hint: reduce the master rate or adjust the tempo map first

### Examples

**Apply the factory unity rate** (`empty`)

```json
{}

```

Result:

```json
{}

```

## `transport.playrate.get@1`

Get requested/effective master playrate, pitch mode and active envelope.

requested_rate is the saved project ratio; envelope names the active nonempty tr_0/playrate envelope, including automation items. No engine is required. effective_rate follows consumed output, including compensated source transitions and graph/backend output latency; it is 1.0 without a running engine. While the transport is stopped, a steady rate reads as the rate playing starts at (an envelope or a sync chase reads as last heard). GUI clients can use EngineApp::playrateState() without executing/logging a query each sync.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport_playrate",
  "properties": {
    "requested_rate": {
      "type": "number"
    },
    "effective_rate": {
      "type": "number"
    },
    "preserve_pitch": {
      "type": "boolean"
    },
    "envelope": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["requested_rate", "effective_rate", "preserve_pitch"],
  "additionalProperties": false
}

```

### Examples

**Factory settings** (`empty`)

```json
{}

```

Result:

```json
{
  "requested_rate": 1.0,
  "effective_rate": 1.0,
  "preserve_pitch": false
}

```

## `transport.playrate.set@1`

Set the project's master playrate ratio and preserve-pitch flag.

rate: finite 0.25..4.0 (default 1); preserve_pitch defaults false in a new project. Omitted fields keep their values. Updates during a drag use Origin.gesture like track faders, and write/touch/latch capture tr_0/playrate coarsely: unlike track levels (ADR-0061), the curve keeps playing while it is written (no hold or touch return). Recording requires unity. Settings are saved/undoable. File sources convert before take FX and live input mixing; instruments retain their pitch. While playing, scalar edits ramp (while stopped, playing starts at the new rate) and converter changes crossfade for 10 ms after priming. Edits include the render horizon and compensated source lookahead; newer requests coalesce while a transition finishes. Long-latency take chains can exceed the usual 200 ms response. effective_rate follows audible output, independently of requested_rate.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `rate` | number |  | 0.25..4.0 |  |  |
| `preserve_pitch` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "transport_playrate",
  "properties": {
    "requested_rate": {
      "type": "number"
    },
    "effective_rate": {
      "type": "number"
    },
    "preserve_pitch": {
      "type": "boolean"
    },
    "envelope": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["requested_rate", "effective_rate", "preserve_pitch"],
  "additionalProperties": false
}

```

### Errors

- `record.playrate`: recording requires unity master playrate Hint: stop recording before changing rate or enabling a non-unity rate envelope

### Examples

**Store double speed with pitch preservation** (`empty`)

```json
{
  "rate": 2,
  "preserve_pitch": true
}

```

Result:

```json
{
  "requested_rate": 2.0,
  "effective_rate": 1.0,
  "preserve_pitch": true
}

```

## `transport.punch@1`

Set or toggle the punch range: recordings keep only that range.

start and end as seconds, beat or bar; a range turns punch on unless enabled says otherwise; clear removes the range and turns punch off. The recording keeps running; the committed items cover only the punch range (the file keeps everything). Not an undo step.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `clear` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0..2147483647.0 |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "punch",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, end not after start, or clear with a range Hint: e.g. start_bar 5, end_bar 9
- `record.punch`: the active recording cannot represent this punch Hint: clear the punch or use a range within 2^48 recording frames

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

**Remove the range** (`empty`)

```json
{
  "clear": true
}

```

Result:

```json
{
  "enabled": false,
  "start_seconds": 0,
  "end_seconds": 0
}

```

## `transport.record@1`

Play and record every armed track's input or master's output.

Recording requires saved master rate 1 and no active master rate envelope that leaves 1 anywhere, including automation items. Non-unity recording and an armed track with unsupported output record mode are refused before opening files. An armed master records its output (after its fader, before its own items) as an item on the master; while it records, its items are silent in the punch range, or everywhere without punch. Armed channels in midi_overdub or midi_replace also record every incoming Note Out route, alongside an optional MIDI device input; no instrument is required. Rack Out audio inputs record stereo after input FX, before items and track inserts.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)
- `record.rack_input`: the loaded plan has no armed rack input for the target Hint: select an Out with track.set {record_rack, armed: true} and load it before recording
- `record.midi_route`: a Note Out recording target is not armed in the loaded plan Hint: arm the destination in a MIDI record mode and load it before recording
- `record.playrate`: master playrate can leave unity Hint: set rate to 1 and disable non-unity master playrate automation
- `record.output_unsupported`: output recording is not supported yet Hint: use track.set {record_mode: input} on the reported track

### Examples

**While the engine runs** (`engine_armed`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": true,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.seek@1`

Move the play position (one of seconds, beat or bar).

With master playrate enabled, prepare file-source converters off-thread before publishing the audible seek. Existing playback and live monitoring continue until ready; preparation errors leave the old seek epoch intact.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `seconds` | number |  | 0.0.. |  |  |
| `beat` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0..2147483647.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `transport.position`: not exactly one position Hint: give seconds, beat or bar

### Examples

**To bar 2 (120 BPM, 4/4)** (`engine_running`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": 2.0,
  "beat": 4.0,
  "sample": 96000,
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```

## `transport.stop@1`

Stop playback and return to where it started (ends a recording).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "recording": {
      "type": "boolean"
    },
    "counting_in": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "stacked": {
            "type": "boolean",
            "description": "The takes went into an existing item (ADR-0103)."
          },
          "warnings": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "recording_warning",
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
                "track": {
                  "type": "string"
                },
                "file": {
                  "type": "string"
                },
                "reason": {
                  "type": "string",
                  "enum": [
                    "source",
                    "cycle",
                    "limit",
                    "source_removed",
                    "target_removed",
                    "source_unfit",
                    "target_unfit"
                  ]
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes", "stacked"],
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
  "required": ["playing", "recording", "counting_in", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}, or arm a master
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>",
  "counting_in": false
}

```
