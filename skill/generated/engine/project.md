# `project` commands

The project: new, open, save, recover and its settings.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `project.get@1`

Show the project: name, rate, tempo, meter, file, version and branch.

recoverable: recordings a crash left out of the project (project.recoverable). autosave_available: a newer autosave exists next to the file; open it as a project if you want it (nothing is loaded silently).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Examples

**A new project** (`empty`)

```json
{}

```

Result:

```json
{
  "name": "",
  "path": "",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": 48000.0,
  "tempo": 120.0,
  "meter": "4/4",
  "timecode": {
    "rate": "30",
    "start": "00:00:00:00"
  },
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.new@1`

Start a new, empty project (replaces the current one and its history).

The new project has one master and no file until project.save {path}. Unsaved changes to the current project are lost: save first.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `sample_rate` | number |  | 8000.0..384000.0 |  | Hz; default 48000. |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Examples

**New** (`song`)

```json
{
  "name": "Sketch"
}

```

Result:

```json
{
  "name": "Sketch",
  "path": "",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": 48000.0,
  "tempo": "<any>",
  "meter": "<any>",
  "timecode": "<any>",
  "version": 0,
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.open@1`

Open a project file and its branches (replaces the current project).

recoverable > 0: recordings a crash left out of the project; project.recoverable lists them and project.recover puts them back. autosave_available: a newer autosave exists next to the file (it is not loaded silently). branch: the branch current when saved. warnings: project.history when the saved branches could not be restored (main opens alone; the first save over the file keeps a copy of it); also project module copies that could not be read or compiled, and name types using a library version instead or left unavailable; project.suspect_plugin when a crash report points at a plug-in the project holds (it loads normally: the engine never prompts or changes how a plug-in loads by itself; plugin.override first to protect the run).

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `expect_hash` | string |  |  |  | The hash project.plugins returned: the file is opened only if its bytes are those it checked (project.changed otherwise) |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Every branch the project holds; the file keeps them all"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "timecode": {
      "type": "object",
      "title": "project_timecode",
      "properties": {
        "rate": {
          "type": "string"
        },
        "start": {
          "type": "string"
        }
      },
      "required": ["rate", "start"],
      "additionalProperties": false,
      "description": "The timecode frame rate and the label at project time 0"
    },
    "version": {
      "type": "integer"
    },
    "generation": {
      "type": "integer"
    },
    "recoverable": {
      "type": "integer"
    },
    "autosave_available": {
      "type": "boolean"
    },
    "recoverable_sessions": {
      "type": "integer",
      "description": "Unsaved projects earlier runs left behind (project.recoverable lists them)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": [
    "name",
    "path",
    "branch",
    "branches",
    "sample_rate",
    "tempo",
    "meter",
    "timecode",
    "version",
    "generation",
    "recoverable",
    "autosave_available",
    "recoverable_sessions"
  ],
  "additionalProperties": false
}

```

### Errors

- `file.open`: the file cannot be read Hint: check the path
- `schema.invalid`: the file has problems Hint: details list each with its path

### Examples

**Open** (`saved`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "name": "<any>",
  "path": "<any>",
  "branch": "main",
  "branches": ["main"],
  "sample_rate": "<any>",
  "tempo": "<any>",
  "meter": "<any>",
  "timecode": "<any>",
  "version": "<any>",
  "recoverable": 0,
  "generation": "<any>",
  "autosave_available": false,
  "recoverable_sessions": 0
}

```

## `project.plugins@1`

Plug-ins a project or recovery names, and any a crash report suspects.

Reads the file as project.open would and loads nothing: no plug-in, no probe. Every branch's head is read, not the history steps behind them. plugins lists each (format, id) once with the name, vendor and version the file saves, uses (its fx in the saved branch), branches, mode (this run's plugin.override), stock and bridgeable, and resolved: the installed plug-in (id, name, vendor, version, path) from the catalogue as it stands, or null (not installed, or the catalogue is empty: catalogue_empty says it is, and no scan was started; plugin.scan first). suspect is null, or says that a crash report points at it and the installed file is that build (build same), that version when the report names no build (version), or could not be checked against it (unreadable): attribution (fault_in_image or active_token) and the incidents, newest first. A plug-in is never loaded to find out. hash is the SHA-256 of the file's bytes: pass it to project.open as expect_hash so the check and the load read the same file. template: true reads a project template (a folder or file) instead. Recovery is checked the same way, before it opens anything: session reads the autosave project.recover {session} would open, autosave: true the one project.recover {autosave: true} would (autosave_file names one, as there). The result's source says which, and autosave names the file read: pass it to project.recover as autosave_file so the check and the load read one file. Give exactly one of path, session and autosave. project.open and project.recover also warn (project.suspect_plugin) of a plug-in still on the normal load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `template` | boolean |  |  |  |  |
| `session` | string |  |  |  | A session project.recoverable lists |
| `autosave` | boolean |  |  |  | The open project's newer autosave |
| `autosave_file` | string |  |  |  | With autosave: which one, as offered |

### Result

```json
{}

```

### Errors

- `file.open`: the file cannot be read Hint: check the path
- `schema.invalid`: the file has problems Hint: details list each with its path
- `project.plugins_args`: the source is not exactly one of path, session, autosave Hint: give one
- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.no_autosave`: no autosave is newer than the saved file Hint: open the project as saved

### Examples

**Which plug-ins** (`saved`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "source": "file",
  "path": "<any>",
  "hash": "<any>",
  "catalogue_empty": "<any>",
  "plugins": "<any>"
}

```

## `project.recover@1`

Put back what a crash left behind: recordings, or an unsaved project.

files: as project.recoverable lists them (omit for all). Each becomes an item on the track it was recorded on (or track) at the position it was recorded, in one undo step. session: instead opens that unsaved project from its newest readable autosave in place of the current one (unsaved changes are lost: save first), still unsaved, and puts back its leftover recordings as one more undo step. autosave: instead opens the saved project's autosave newer than its file (project.get autosave_available) in place of what is open, keeping its path: it is unsaved until saved over the file. autosave_file names the autosave to open (as project.recover returned or a client offered it); it must be newer than the file as this project was opened. A plug-in an earlier crash report points at raises project.suspect_plugin among the result's warnings (project.plugins session or autosave says so before the call), and loads normally.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `files` | array |  |  |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `session` | string |  |  |  | A session project.recoverable lists |
| `autosave` | boolean |  |  | false | The saved project's newer autosave |
| `autosave_file` | string |  |  |  | With autosave: which one, e.g. autosave-3.json, as offered |

### Result

```json
{
  "type": "object",
  "title": "recover_result",
  "properties": {
    "recovered": {
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
                }
              },
              "required": ["code", "message", "path", "hint", "track", "file"],
              "additionalProperties": false
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    },
    "autosave": {
      "type": "string",
      "description": "The autosave it opened"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["recovered"],
  "additionalProperties": false
}

```

### Errors

- `recover.nothing`: there is nothing to recover Hint: project.recoverable
- `recover.unknown_file`: a file is not a recording to recover Hint: project.recoverable lists them
- `recover.duplicate_file`: a file is listed twice Hint: list each once
- `recover.no_track`: the track it was recorded on is gone Hint: give track
- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.busy`: the session belongs to an app that is running Hint: recover it in that app
- `recover.damaged`: nothing in the session can be opened Hint: project.recoverable.delete
- `recover.session_args`: session cannot be combined with files or track Hint: recover the session, then its recordings
- `recover.no_autosave`: no autosave is newer than the saved file Hint: open the project as saved
- `recover.keep`: a copy recovery must keep could not be made; nothing changed Hint: make the folder writable and retry
- `recover.autosave_args`: autosave cannot be combined with session, files or track Hint: recover the autosave, then the recordings

### Examples

**Recover everything** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recovered": [
    {
      "track": "tr_1",
      "item": "it_1",
      "take": "tk_1",
      "file": "<any>",
      "seconds": 1.0,
      "lost_frames": 0,
      "takes": ["tk_1"]
    }
  ]
}

```

**Recover an unsaved project** (`session_left_behind`)

```json
{
  "session": "20261114-090000-4242"
}

```

Result:

```json
{
  "recovered": [
    {
      "track": "tr_1",
      "item": "<any>",
      "take": "<any>",
      "file": "<any>",
      "seconds": 1.0,
      "lost_frames": 0,
      "takes": "<any>"
    }
  ],
  "autosave": "autosave-1.json"
}

```

## `project.recoverable@1`

List recordings and unsaved projects a crash left behind.

recordings: takes (audio WAVs, and MIDI takes' journals with midi_events) recorded but never committed (the program stopped mid-recording) or never saved, found in the media folder's rec/ (file: relative to the media folder, with / on every platform). start_seconds is where each was recorded; seconds includes audio written after the file's last header update. sessions: unsaved projects of earlier runs that ended without saving or discarding them (a crash), each with its newest autosave's project name, tracks and branches, its leftover recordings and its size on disk; project.recover {session} opens one, project.recoverable.delete deletes one.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "recoverable_list",
  "properties": {
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    },
    "recordings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recoverable_recording",
        "properties": {
          "file": {
            "type": "string"
          },
          "track": {
            "type": "string"
          },
          "start_seconds": {
            "type": "number"
          },
          "seconds": {
            "type": "number"
          },
          "channels": {
            "type": "integer"
          },
          "midi_events": {
            "type": "integer",
            "description": "A MIDI take's journal: the events it holds"
          }
        },
        "required": ["file", "track", "start_seconds", "seconds", "channels"],
        "additionalProperties": false
      }
    },
    "sessions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recoverable_session",
        "properties": {
          "session": {
            "type": "string"
          },
          "started_ms": {
            "type": "integer",
            "description": "When that run started (Unix ms)"
          },
          "autosaved_ms": {
            "type": "integer",
            "description": "When its newest readable autosave was written (Unix ms); absent: none, only recordings"
          },
          "name": {
            "type": "string"
          },
          "tracks": {
            "type": "integer",
            "description": "Channels and busses, not masters"
          },
          "branches": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "recordings": {
            "type": "integer",
            "description": "Takes no autosaved version uses"
          },
          "bytes": {
            "type": "integer",
            "description": "Its folder's size on disk"
          }
        },
        "required": ["session", "name", "tracks", "branches", "recordings", "bytes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["recordings", "sessions"],
  "additionalProperties": false
}

```

### Examples

**After a crash while recording** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recordings": [
    {
      "file": "rec/Vocal-1.wav",
      "track": "tr_1",
      "start_seconds": 1.0,
      "seconds": 1.0,
      "channels": 2
    }
  ],
  "sessions": []
}

```

**An unsaved project a crash left behind** (`session_left_behind`)

```json
{}

```

Result:

```json
{
  "recordings": [],
  "sessions": [
    {
      "session": "20261114-090000-4242",
      "started_ms": "<any>",
      "autosaved_ms": "<any>",
      "name": "Sketch",
      "tracks": 1,
      "branches": ["main"],
      "recordings": 1,
      "bytes": "<any>"
    }
  ]
}

```

**Nothing to recover** (`empty`)

```json
{}

```

Result:

```json
{
  "recordings": [],
  "sessions": []
}

```

## `project.recoverable.delete@1`

Delete an unsaved project a crash left behind, with its recordings.

session: as project.recoverable lists it. Its folder and everything in it are deleted for good (this is not an undo step).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `session` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "session_deleted",
  "properties": {
    "session": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["session"],
  "additionalProperties": false
}

```

### Errors

- `recover.unknown_session`: no unsaved project of that name was left behind Hint: project.recoverable lists them
- `recover.busy`: the session belongs to an app that is running Hint: quit that app
- `recover.delete`: the folder could not be deleted Hint: make it writable

### Examples

**Delete one** (`session_left_behind`)

```json
{
  "session": "20261114-090000-4242"
}

```

Result:

```json
{
  "session": "20261114-090000-4242"
}

```

## `project.save@1`

Save the project (canonical JSON) to its file or a new path.

path: where to save (becomes the project's file); omit to save in place. The file keeps every branch with its unmerged steps (undoable after opening) and which branch is current; history: true also keeps undo history. warnings: running plug-ins whose current state could not be read; the file keeps the state each had before (fx.restart, then save again, or replace the plug-in); project.history_backup names the copy kept of a file whose branches could not be restored when opened. project.recording: a first save, or a save into another folder, while recording; stop first. project.autosave_folder: the autosave folder beside the file (<file>-data) could not be read; nothing was written.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |
| `history` | boolean |  |  | false | Also keep undo and redo history (ADR-0076) |

### Result

```json
{
  "type": "object",
  "title": "save_result",
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

### Examples

**Save next to the work directory** (`empty`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `project.set@1`

Set project name, notes, Console 9, pan law, timebase or timecode.

timebase: what items follow when the tempo changes, for items and tracks that inherit it — beats (default), time or beats_position; they keep their place now. timecode: the frame rate (23.976, 24, 25, 29.97df, 29.97 or 30; default 30) and start, the timecode at project time 0 (default 00:00:00:00), which MTC and LTC sync and output use; either member alone keeps the other.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `title` | string |  |  |  |  |
| `author` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `console9` | boolean |  |  |  |  |
| `pan_law` | object |  |  |  | Channels' and busses' pan law unless a track sets its own: db is the centre's cut (0, -2.5, -3, -4.5 or -6; default 0); boost keeps the centre at 0 dB and raises the sides instead |
| `pan_law.db` | number or string | dB |  | 0.0 | Quantity strings are range-checked by the server. One of: 0.0, -2.5, -3.0, -4.5, -6.0. |
| `pan_law.boost` | boolean |  |  | false |  |
| `history_budget_bytes` | integer |  | 1.0.. |  | Estimated retained history byte budget; default 1 GiB. Leases and branch boundaries may exceed it. |
| `render` | object |  |  |  | Render settings (session-model §1.10); every member optional |
| `render.source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `render.masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `render.bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `render.start_beat` | number |  | 0.0..1e+09 |  |  |
| `render.end_beat` | number |  | ..1e+09 |  |  |
| `render.tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `render.pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `render.sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `render.channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `render.outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `render.normalize` | object |  |  |  |  |
| `render.limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `render.trim` | object |  |  |  |  |
| `render.seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `render.handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `render.stems` | object |  |  |  |  |
| `render.matrix` | object |  |  |  |  |
| `render.if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `render.metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `render.report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `render.add_to_project` | boolean |  |  |  |  |
| `timebase` | string |  |  |  | One of: time, beats, beats_position. |
| `timecode` | object |  |  |  |  |
| `timecode.rate` | string |  |  |  | One of: 23.976, 24, 25, 29.97df, 29.97, 30. |
| `timecode.start` | string |  |  |  | HH:MM:SS:FF, ";" before the frames in drop-frame |

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

**Name it** (`empty`)

```json
{
  "name": "Demo"
}

```

Result:

```json
{}

```

**EBU timecode starting at one hour** (`empty`)

```json
{
  "timecode": {
    "rate": "25",
    "start": "01:00:00:00"
  }
}

```

Result:

```json
{}

```

**Pan with the -3 dB law, boosting the sides** (`empty`)

```json
{
  "pan_law": {
    "db": -3,
    "boost": true
  }
}

```

Result:

```json
{}

```

## `project.template.delete@1`

Delete a saved project template by name.



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_delete_result",
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

- `template.unknown`: no template has that name Hint: use a name from project.template.list

### Examples

**Delete the band setup** (`song_with_project_template`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `project.template.list@1`

List saved project templates by name and path.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "project_template_list_result",
  "properties": {
    "templates": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "project_template_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          }
        },
        "required": ["name", "path"],
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
  "required": ["templates"],
  "additionalProperties": false
}

```

### Examples

**None yet** (`empty`)

```json
{}

```

Result:

```json
{
  "templates": []
}

```

## `project.template.load@1`

Start a new, unsaved project from a project template.

Replaces the current project and its history, like project.new: unsaved changes are lost, so save first. The new project has no file until project.save {path} and gets its own copy of the template's audio; the template itself is never changed.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_load_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Examples

**Start from the band setup** (`song_with_project_template`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "name": "<any>"
}

```

## `project.template.save@1`

Save the whole project as a named project template.

Writes the project as it is now (tracks, FX and their states, routing, items, settings; not the history) to a folder of its own in the user's templates directory, with a copy of every audio file it uses. The project keeps its own file. replace: true replaces a template of that name. warnings: running plug-ins whose current state could not be read; the template has the state each had before.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `replace` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "project_template_save_result",
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

- `template.exists`: the name already exists Hint: choose another name or set replace: true
- `media.missing`: an audio file the project uses is gone Hint: restore it, or remove the items that use it

### Examples

**Save the band setup** (`song`)

```json
{
  "name": "Band"
}

```

Result:

```json
{
  "path": "<any>"
}

```
