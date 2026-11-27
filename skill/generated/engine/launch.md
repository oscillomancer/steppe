# `launch` commands

Launcher playback: launch scenes and clips, stop, capture, commit.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `launch.arrangement@1`

Return tracks to the arrangement at the next boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. Returning does not chase notes: an arrangement note already under way stays silent until its next start. Without track: every track not following the arrangement, or queued to leave it. A frozen track already follows the arrangement: nothing changes.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: an audio clip with take FX or envelopes cannot play while the master playrate keeps the pitch Hint: turn off the master playrate's preserve pitch, or set it to 1 without an envelope, or launch a copy of the clip without take FX
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating, or the master playrate is changing Hint: launch again in a moment

### Examples

**Nothing left the arrangement** (`song_with_clip_running`)

```json
{}

```

Result:

```json
{
  "action": "arrangement",
  "tracks": [],
  "started": false
}

```

## `launch.capture@1`

Start or end capturing launches for the timeline.

While on, every applied launch is kept with the running time it applied at (song time: under a master playrate, the song samples played), and what each track played when the capture began. on: true begins at the position heard now (stopped: where playing starts); on: false ends it there, or with quantize (other than none, while playing) at that quantize's next boundary, where a launch of every track with clips (a scene launch) made now would land; one track's launch can land a boundary earlier, and is then in the capture. It captures until it hears that boundary, and launches from there on are not in it. ends_at (launch.state's capture_ends): the beat of the sample it ends on, which launch.commit's to will be, unless the transport loop wrapped meanwhile (then the song's beat in the loop, while to runs on by running time). on: true before then cancels the end; another on: false replaces it. A stop, a seek or a loop change ends a capture that has run, ending or not; one that has not run begins again at the new position. launch.commit writes the capture onto the timeline. from and to: its span on the timeline in beats so far, laid out by running time. It runs through master playrate edits: the committed arrangement plays the same song positions, not the same durations, at another rate. Refused while a launched audio clip plays and the master playrate keeps the pitch; a playrate that starts keeping it then ends a running capture, with a warning naming the clip. While the pitch is kept, a launch that takes a track off its arrangement, or back to it, where an audio item plays within 0.25 s of the launch (times the master rate above 1, across a loop's wrap), ends the capture on that launch's boundary, with a warning.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `on` | boolean |  |  |  |  |
| `quantize` | string |  |  |  | on: false only: end at this quantize's next boundary (none, or left out: now) One of: bar, beat, 1/2, 1/4, 1/8, 1/16, 1/32, none. |

### Result

```json
{
  "type": "object",
  "title": "launch_capture",
  "properties": {
    "on": {
      "type": "boolean"
    },
    "from": {
      "type": "number"
    },
    "to": {
      "type": "number"
    },
    "ends_at": {
      "type": "number"
    },
    "version": {
      "type": "integer",
      "minimum": 0.0
    },
    "warnings": {
      "type": "array",
      "items": {
        "anyOf": [
          {
            "type": "string"
          },
          {
            "type": "object"
          }
        ]
      }
    }
  },
  "required": ["on"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: a launched audio clip plays while the master playrate keeps the pitch Hint: stop it (launch.stop or launch.arrangement), or turn off the master playrate's preserve pitch, then capture
- `launch.later`: the transport is relocating, or the playrate is changing Hint: end the capture again in a moment

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "on": true
}

```

Result:

```json
{
  "on": true,
  "from": 0,
  "to": 0,
  "version": "<any>"
}

```

## `launch.clip@1`

Play a clip on its track from the next quantize boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. Launching while stopped starts the transport. The track leaves the arrangement (or the clip it played) there: its sounding notes end and audio crossfades over 5 ms. A clip whose follow is not none follows the project's key or chords live, as it would laid there (launch.state says how); launch.follow_budget warns when its context and the project's are too varied to follow fully (a chords clip then follows its key, else it plays as written).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `clip` | string |  |  |  | Clip id, e.g. "cl_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: an audio clip with take FX or envelopes cannot play while the master playrate keeps the pitch Hint: turn off the master playrate's preserve pitch, or set it to 1 without an envelope, or launch a copy of the clip without take FX
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating, or the master playrate is changing Hint: launch again in a moment
- `track.frozen`: the clip's track is frozen Hint: track.unfreeze first
- `launch.memory`: the clip's audio did not fit the budget Hint: use shorter audio or fewer files in clips

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "clip": "cl_1"
}

```

Result:

```json
{
  "action": "clip",
  "tracks": [
    {
      "track": "tr_1",
      "clip": "cl_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": true
}

```

## `launch.commit@1`

Write the captured performance onto the timeline as items.

One undo step. The capture (launch.capture), or one kept across an engine stop or a device restart (with the engine stopped or started again), is laid out linearly from where it began, by running time, whatever the transport loop did: a launch heard s seconds into the capture lands s seconds after its start (under a master playrate, s seconds of song: the same song positions, not the same durations). On each track that left the arrangement, its clip, stopped and held stretches replace the arrangement there (its items are split, trimmed or removed); each clip becomes copies of its takes at the phase it was heard, its MIDI copied unless pooled, and the 5 ms switch crossfades become item fades; an audio clip's own fades go on its pieces' pass edges, standing in for a switch's as they did live; a clip still fading out at the capture's end is laid to its pass's end, the track's items under that tail removed and the next one split with a 5 ms fade-in, and to says where it ends. Tracks that followed the arrangement are untouched, and so is the arrangement a loop repeated. A scene launched whole (launch.scene: every clip that could play started on one boundary) also writes a section named after it, from that boundary until one of its tracks is launched again or the capture ends, as scene.insert does; clips launched singly write none. The notes are laid as written; each laid MIDI take carries its clip's context and the follow the plan used for that clip (none, key or chords, after the per-clip budget), so the committed timeline sounds as the performance did and moves with the context track as a launched clip does (take.bake writes the sounding pitches). A clip played as written warns context.mismatch once, when its key (or, in the same key, chord) is not the project's under it, however many periods it is laid as. item.locked, clip.unsupported and launch.commit.fade name the launch whose stretch met them (path /performance/launches/N). A running capture ends first, unless a quantized end is pending (launch.capture.ending); the capture stays, so committing again writes it again over itself. performance is what history records: leave it out and launch.commit fills it from the capture.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `pooled` | boolean |  |  | false | Share the clips' MIDI with the items (pooled) instead of copying it |
| `performance` | object |  |  |  | What history records; launch.commit fills it from the capture |
| `performance.sample_rate` | number |  | ..1536000.0 |  |  |
| `performance.start` | integer |  | 0.0..140737488355328.0 |  |  |
| `performance.frames` | integer |  | 0.0..140737488355328.0 |  |  |
| `performance.loop` | object |  |  |  |  |
| `performance.launches` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "launch_commit",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "items": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "sections": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "committed_section",
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
          }
        },
        "required": ["scene", "region", "start", "end"],
        "additionalProperties": false
      }
    },
    "mismatches": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "committed_mismatch",
        "properties": {
          "clip": {
            "type": "string"
          },
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
        "required": ["clip", "kind", "bar"],
        "additionalProperties": false
      },
      "description": "Each launched stretch of a MIDI clip copied under a key that is not the clip's, as its context.mismatch warning says it."
    },
    "from": {
      "type": "number"
    },
    "to": {
      "type": "number",
      "description": "The capture's end, or later where a clip still fading out at it plays out to its pass's end."
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["tracks", "items", "sections", "from", "to"],
  "additionalProperties": false
}

```

### Errors

- `launch.capture`: nothing has been captured Hint: launch.capture {on: true}, play and launch clips, then launch.commit
- `launch.capture.ending`: the capture ends on a boundary not heard yet Hint: commit once launch.state says capturing is false, or launch.capture {on: false} to end it now
- `launch.commit`: a captured track or clip is gone Hint: undo the change, or capture the performance again
- `item.locked`: a locked item is in the way Hint: unlock it with item.set first
- `clip.unsupported`: a clip's loop window has no item equivalent Hint: set its loop to the whole content, or off
- `launch.commit.fade`: a piece would start or end inside a clip's own fade Hint: capture again with the switches and loop wraps outside the clip's fades, or set its fades to 0 (clip.set); the performance is kept
- `modulation.limit`: the copy's routes would pass the project's route limit Hint: remove routes first (modulation.list), for instance the copied chain's

### Examples

**A captured bar of cl_1** (`song_with_clip`)

```json
{
  "performance": {
    "sample_rate": 48000,
    "start": 0,
    "frames": 96000,
    "launches": [
      {
        "frame": 0,
        "track": "tr_1",
        "action": "clip",
        "clip": "cl_1",
        "scene": "sc_1",
        "anchor": 0,
        "anchor_beat": 0
      }
    ]
  }
}

```

Result:

```json
{
  "tracks": ["tr_1"],
  "items": ["<any>"],
  "sections": [
    {
      "scene": "sc_1",
      "region": "<any>",
      "start": 0,
      "end": 4
    }
  ],
  "from": 0,
  "to": 4
}

```

## `launch.scene@1`

Play every clip of a scene together at one boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. All the scene's tracks switch at the same boundary, the project's launch_quantize. Tracks with no clip in the scene keep playing what they play. A frozen track is skipped with a track.frozen warning and a clip past the memory budget with a launch.memory warning. Clips whose follow is not none follow live, with launch.follow_budget for each one the follow budget holds back.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `scene` | string |  |  |  | Scene id, e.g. "sc_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: an audio clip with take FX or envelopes cannot play while the master playrate keeps the pitch Hint: turn off the master playrate's preserve pitch, or set it to 1 without an envelope, or launch a copy of the clip without take FX
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating, or the master playrate is changing Hint: launch again in a moment
- `launch.scene`: the scene has no clip that can play Hint: add clips to the scene, or unfreeze their tracks

### Examples

**While stopped** (`song_with_clip_running`)

```json
{
  "scene": "sc_1"
}

```

Result:

```json
{
  "action": "clip",
  "tracks": [
    {
      "track": "tr_1",
      "clip": "cl_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": true
}

```

## `launch.state@1`

What each track with clips plays, and what it has queued.

plays is arrangement, clip, held (a once clip past its length, silent) or stopped, as heard. position is the playing clip's place in its period, in beats for a MIDI clip and seconds for an audio clip. queued is a launch not heard yet, with the song position it applies at. followed (none, key or chords) is what the clip it plays, else the one it has queued, follows by in the running plan (ADR-0098); follow_budget: true when the follow budget forced that (a chords clip by its key, or any as written). Tracks without clips always follow the arrangement and are not listed. capturing: launch.capture is on; capture_ends: the beat a quantized end (launch.capture {on: false, quantize}) ends it on, until it does; capturing is true from launch.capture {on: true}, armed or running. capture_kept (when there is a capture): true while one is kept beside the engine's, from before an engine stop or a device restart, or the one a newer capture replaces once that hears a frame: both can hold at once. capture_version: the session version the capture launch.commit and render {performance: "captured"} take began on (its default version to render). unlogged: false until this engine has lost a launch's report from the audio path (a full queue), then true for the engine's life: the tracks' entries may then be stale, and no capture running then or begun since is as heard; restarting the audio clears it. With the engine stopped it answers for a kept capture (no tracks).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "launch_state",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "capturing": {
      "type": "boolean"
    },
    "capture_ends": {
      "type": "number"
    },
    "capture_kept": {
      "type": "boolean"
    },
    "capture_version": {
      "type": "integer",
      "minimum": 0.0
    },
    "unlogged": {
      "type": "boolean"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_track_state",
        "properties": {
          "track": {
            "type": "string"
          },
          "plays": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          },
          "position": {
            "type": "number"
          },
          "queued": {
            "type": "object",
            "title": "launch_queued",
            "properties": {
              "action": {
                "type": "string"
              },
              "clip": {
                "type": "string"
              },
              "beat": {
                "type": "number"
              },
              "seconds": {
                "type": "number"
              }
            },
            "required": ["action", "beat", "seconds"],
            "additionalProperties": false
          },
          "followed": {
            "type": "string",
            "enum": ["none", "key", "chords"]
          },
          "follow_budget": {
            "type": "boolean"
          }
        },
        "required": ["track", "plays"],
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
  "required": ["playing", "capturing", "unlogged", "tracks"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**Before any launch** (`song_with_clip_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "capturing": false,
  "unlogged": false,
  "tracks": [
    {
      "track": "tr_1",
      "plays": "arrangement"
    }
  ]
}

```

## `launch.stop@1`

Stop a track's clip, or all clips, at the next boundary.

It applies at the next quantize boundary (the clip's quantize, else the project's launch_quantize) at or after the earliest moment every track can still take it: about one device block ahead for a track the audio callback runs, about the render-ahead (0.2 s) for one the renderer runs. beat and seconds give that song position. While the transport is stopped it applies at the play position when playing starts. A stopped track is silent until a clip is launched or it returns to the arrangement. Without track: every track a clip holds or is queued to hold. A frozen track already follows the arrangement: nothing changes.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "launch_done",
  "properties": {
    "action": {
      "type": "string"
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "launch_target",
        "properties": {
          "track": {
            "type": "string"
          },
          "clip": {
            "type": "string"
          }
        },
        "required": ["track"],
        "additionalProperties": false
      }
    },
    "beat": {
      "type": "number"
    },
    "seconds": {
      "type": "number"
    },
    "started": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["action", "tracks", "started"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `launch.playrate`: an audio clip with take FX or envelopes cannot play while the master playrate keeps the pitch Hint: turn off the master playrate's preserve pitch, or set it to 1 without an envelope, or launch a copy of the clip without take FX
- `launch.clip`: the running plan has no player for the clip Hint: wait for the edit to reach the engine, then launch again
- `launch.queue`: too many launches are waiting Hint: wait for them to play
- `launch.later`: the transport is relocating, or the master playrate is changing Hint: launch again in a moment
- `launch.track`: the track has no clips in the running plan Hint: mute the track to silence it

### Examples

**One track, while stopped** (`song_with_clip_running`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{
  "action": "stop",
  "tracks": [
    {
      "track": "tr_1"
    }
  ],
  "beat": 0,
  "seconds": 0,
  "started": false
}

```
