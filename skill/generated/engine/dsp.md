# `dsp` commands

DSP scripts: write, check, test, preview and export as plug-ins.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `dsp.check@1`

Compile a DSP script without saving it; return its diagnostics.

Give source (a whole script) or name (one in the library). ok is true when it compiles; plugin then lists its id, name and parameters as the patch shows them.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `source` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_check_result",
  "properties": {
    "ok": {
      "type": "boolean"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["ok", "diagnostics"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of name and source Hint: give exactly one
- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts

### Examples

**Check a script in the library** (`dsp_script`)

```json
{
  "name": "gain"
}

```

Result:

```json
{
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>"
}

```

**A mistake, with its hint** (`empty`)

```json
{
  "source": "struct X : Effect {};"
}

```

Result:

```json
{
  "ok": false,
  "diagnostics": "<any>"
}

```

## `dsp.edit@1`

Replace text in a library script and compile it, as dsp.write.

For a change to part of a script, instead of sending all of it to dsp.write. Each edit replaces old, which must be in the script exactly once, with new (empty deletes it). The edits apply in order, each to the script as the ones before left it, and all or none: when one fails, nothing is saved and the error says where old and the script part. Then as dsp.write: it saves the script, compiles it and returns the diagnostics; instances playing it follow within half a second.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `edits` | array |  |  |  |  |
| `edits[].old` | string |  |  |  | text the script has in one place |
| `edits[].new` | string |  |  |  | what replaces it |

### Result

```json
{
  "type": "object",
  "title": "dsp_write_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "ok": {
      "type": "boolean",
      "description": "it compiled"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "listed": {
      "type": "boolean",
      "description": "plugin.list shows it (the script player is on the plugin path)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "ok", "diagnostics", "listed"],
  "additionalProperties": false
}

```

### Errors

- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts
- `dsp.edit`: an edit's old text is not in the script, or in several places Hint: copy old exactly from dsp.get's source, with enough around it to be unique
- `dsp.size`: the script would be longer than 1 MiB Hint: split it up
- `dsp.id_taken`: another script in the library has its id; not saved Hint: give Info.id a value no other script has

### Examples

**Widen a parameter's range** (`dsp_script`)

```json
{
  "name": "gain",
  "edits": [
    {
      "old": "-24, 24, 0",
      "new": "-36, 36, 0"
    }
  ]
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>",
  "listed": "<any>"
}

```

## `dsp.export@1`

Export a DSP script as a plugin: a CLAP file, or C++ source.

format clap (the default): copies the platform's script player and embeds the script in its trailer (Linux/Windows) or bundle resource (macOS, re-signed ad hoc with the system codesign tool). The copy is a plugin of its own, under the script's id and name, that plays this version of the script in any CLAP host (it reads no library), written to <folder>/<plugin name>.clap (default: the script's folder, under export/) with the licenses to ship beside it (notices). It runs on this platform, as fast as the player it copies (a debug build's is slow). format source: a C++ project of the plugin template that builds the same plugin (same id, parameters and state) as CLAP and VST3, plus AU on macOS, with the script compiled as C++, written to the empty <folder> (default: the script's folder, under export/<script>-source); its README says how to build it, and license (MIT unless GPL-3.0-or-later) is the license of its own files. Export again after changing the script; an export does not follow the library.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `script` | string |  |  |  | a name from dsp.list |
| `format` | string |  |  |  | clap (default): a plugin file; source: a C++ project One of: clap, source. |
| `folder` | string |  |  |  | an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new |
| `license` | string |  |  |  | source: the license of the project's own files (default MIT) One of: MIT, GPL-3.0-or-later. |

### Result

```json
{
  "type": "object",
  "title": "dsp_export_result",
  "properties": {
    "path": {
      "type": "string",
      "description": "the plugin file, or the project's folder"
    },
    "id": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "format": {
      "type": "string",
      "description": "clap or source"
    },
    "player": {
      "type": "string",
      "description": "clap: the script player it was copied from"
    },
    "bytes": {
      "type": "integer",
      "minimum": 0.0
    },
    "notices": {
      "type": "string",
      "description": "clap: the licenses to ship with the plugin"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "id", "name", "format"],
  "additionalProperties": false
}

```

### Errors

- `dsp.sign`: macOS: ad-hoc bundle signing failed or codesign is unavailable Hint: restore the player and system codesign tool, or use format source
- `dsp.compile`: the script does not compile Hint: dsp.check gives its diagnostics
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.player`: the plugin with the script's id is not the script player Hint: give the script an id no other plugin has
- `dsp.notices`: the player's license or notices cannot be read Hint: restore the license and notices beside the player before exporting
- `dsp.path`: the folder is relative, or (source) not empty Hint: give an absolute folder; for source, a new one
- `dsp.plugin_name`: source: the plugin's name cannot name a project Hint: rename it in Info.name
- `dsp.no_parameter`: source: the project's tests need a parameter a host can automate, plainly named Hint: declare one
- `dsp.kit`: source: no export kit beside the stock plugins Hint: build the script-export-kit target

### Examples

**Export a script from the library as a plugin file** (`dsp_player_export`)

```json
{
  "script": "gain"
}

```

Result:

```json
{
  "path": "<any>",
  "id": "com.example.gain",
  "name": "Gain",
  "format": "clap",
  "player": "<any>",
  "bytes": "<any>",
  "notices": "<any>"
}

```

**Export it as a C++ project** (`dsp_script`)

```json
{
  "script": "gain",
  "format": "source"
}

```

Result:

```json
{
  "path": "<any>",
  "id": "com.example.gain",
  "name": "Gain",
  "format": "source"
}

```

## `dsp.get@1`

Read a DSP script from the library.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_get_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "source"],
  "additionalProperties": false
}

```

### Errors

- `dsp.unknown`: no such script Hint: dsp.list names the library's scripts

### Examples

**Read a script** (`dsp_script`)

```json
{
  "name": "gain"
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "source": "<any>"
}

```

## `dsp.list@1`

List the DSP scripts in the library, compiled, with their plugin ids.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "dsp_list_result",
  "properties": {
    "library": {
      "type": "string"
    },
    "scripts": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_list_entry",
        "properties": {
          "name": {
            "type": "string"
          },
          "path": {
            "type": "string"
          },
          "ok": {
            "type": "boolean",
            "description": "it compiles"
          },
          "errors": {
            "type": "integer",
            "minimum": 0.0
          },
          "id": {
            "type": "string",
            "description": "its plugin's id"
          },
          "plugin_name": {
            "type": "string"
          }
        },
        "required": ["name", "path", "ok", "errors"],
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
  "required": ["library", "scripts"],
  "additionalProperties": false
}

```

### Examples

**An empty library** (`empty`)

```json
{}

```

Result:

```json
{
  "library": "<any>",
  "scripts": []
}

```

## `dsp.preview@1`

Draw a plugin's editor to a PNG, after a test signal plays through it.

Give plugin (an id) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays signal -- an effect: sine (the default, at frequency_hz), sweep, impulse, noise or silence, peaking at level_db, for seconds, or file (the default with audio_file), the file played whole as dsp.test plays it; an instrument: notes (the default), note or silence, for seconds -- while its editor watches, so meters and the script's draw() show what played. Then the editor is drawn without a window, scale times its design size, and saved as a PNG (path; width and height in pixels): read the file to see it. A script with neither editor() nor draw() shows the automatic editor. pointer, if given, acts on the editor first, in its design units (the picture's pixels at scale 1): move (a hover; a drag after a press), press, release, click, double_click, drag (from x, y to to_x, to_y) and wheel (steps notches, up positive), with shift, ctrl or alt held; edits is a merge patch of what they changed, as a user's edits would. problems says what went wrong in the editor's code (a loop stopped by its budget, an index out of range), with the line. Headless pictures use EGL on Linux, Metal on macOS and WGL on Windows; an unavailable renderer returns dsp.no_picture.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signal` | string |  |  |  | One of: sine, sweep, impulse, noise, silence, notes, note, file. |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `level_db` | number or string | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | number or string | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.0..10.0 | 1.0 |  |
| `scale` | number |  | 0.5..3.0 | 1.0 |  |
| `pointer` | array |  |  |  |  |
| `pointer[].action` | string |  |  |  | One of: move, press, release, click, double_click, drag, wheel. |
| `pointer[].x` | number |  | -1000.0..5000.0 |  | design units: the picture's pixels at scale 1 |
| `pointer[].y` | number |  | -1000.0..5000.0 |  | design units: the picture's pixels at scale 1 |
| `pointer[].to_x` | number |  | -1000.0..5000.0 |  | drag: where it ends |
| `pointer[].to_y` | number |  | -1000.0..5000.0 |  | drag: where it ends |
| `pointer[].steps` | number |  | -20.0..20.0 | 1.0 | wheel: notches turned, up positive |
| `pointer[].shift` | boolean |  |  | false |  |
| `pointer[].ctrl` | boolean |  |  | false |  |
| `pointer[].alt` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "dsp_preview_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "path": {
      "type": "string",
      "description": "the PNG"
    },
    "width": {
      "type": "integer",
      "description": "pixels"
    },
    "height": {
      "type": "integer",
      "description": "pixels"
    },
    "edits": {
      "description": "what pointer changed, as a merge patch"
    },
    "problems": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "log": {
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
  "required": ["plugin", "path", "width", "height", "edits", "problems", "log"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of plugin and script Hint: give exactly one
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.signal`: a signal the plugin cannot play Hint: an effect plays audio signals, an instrument notes
- `dsp.file`: the file signal without audio_file, or a file that is not absolute, is over 10 minutes or holds no audio Hint: give audio_file a WAV, FLAC or MP3, or another signal
- `dsp.pointer`: a drag without its end, or too many steps Hint: give a drag to_x and to_y; at most 64 steps
- `dsp.no_picture`: the plugin cannot draw its editor without a window Hint: scripts can, through the script player; other plugins need the editor-picture extension

### Examples

**See a script's editor** (`dsp_player_editor`)

```json
{
  "script": "gain"
}

```

Result:

```json
{
  "plugin": "com.example.gain",
  "path": "<any>",
  "width": "<any>",
  "height": "<any>",
  "edits": {},
  "problems": [],
  "log": "<any>"
}

```

**Drag a knob, then see the editor** (`dsp_player_editor`)

```json
{
  "script": "gain",
  "pointer": [
    {
      "action": "drag",
      "x": 86,
      "y": 132,
      "to_x": 86,
      "to_y": 92
    }
  ]
}

```

Result:

```json
{
  "plugin": "com.example.gain",
  "path": "<any>",
  "width": "<any>",
  "height": "<any>",
  "edits": {
    "params": {
      "gain": "<any>"
    }
  },
  "problems": [],
  "log": "<any>"
}

```

## `dsp.test@1`

Play test signals or notes through a plugin; measure levels, THD, pitch.

Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays each signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at level_db; and file, audio_file played as it is (resampled to the render's rate, mono on both channels, at most 60 s), the only signal when signals is not given. An instrument, at velocity 0.8: notes (a note, then a chord), note (A4; pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from silence, no output, a note out of tune, sound that never dies away, non-finite samples the framework silenced, a script stopped by its loop budget or indexing outside an array). cpu_percent is measured, so it varies between runs. save: true keeps each output as a WAV (path) for analyze and compare. automation is a list of {param, at_s, value}: semantic patch keys and physical values, as discrete sample-accurate changes applied to every signal (no implicit ramps). automation.max_step is the largest adjacent-sample change in full-scale amplitude; baseline_max_step uses the same render without automation; excess_step is their positive difference. A step above both four times baseline and 0.05 suggests clicks or zipper noise: a screening heuristic, not proof of an artifact or of inaudibility. sidechain supplies a timed key; its middle half is measured against a connected silent key with identical automation: gain_change_db is negative for ducking, null if either output is below -120 dBFS. Slow attack may not settle in that window. Each pass has a fresh instance and initial patch. THD and impulse response are omitted during automation or a non-silent key; gain_db then describes the changing render, not static gain. notes supplies an instrument's sequence ({key, velocity, start_s, length_s}), replacing the notes signal; defaults become notes and silence. Do not combine it with the A4 note signal or select only silence. release_ms starts at its last note-off. Times round to samples at the render rate; spans must fit within seconds. sample_rate renders at that rate instead of the session's (sandbox) or the project's (DAW); latency is in samples at the rate used, which sample_rate in the result reports. meters gives a semantic plugin's read-only parameters (its meters and readouts) by patch key in physical units, as they stood when the signal ended (the tail after it does not move them). traces gives what a script's trace(name, value) calls recorded over the signal, in trace_points intervals of trace_interval_s: for each name, the last value in each interval and the lowest and highest, null where none was given.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signals` | array |  |  |  | default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `automation` | array |  |  |  | discrete physical-value changes on every signal |
| `automation[].param` | string |  |  |  | semantic patch key |
| `automation[].at_s` | number |  | 0.0.. |  |  |
| `automation[].value` | value |  |  |  | physical value, enum key or boolean |
| `sidechain` | object |  |  |  | timed generated key input |
| `sidechain.signal` | string |  |  | "sine" | One of: sine, sweep, impulse, noise, silence. |
| `sidechain.level_db` | number or string | dB | -60.0..0.0 | -6.0 | Quantity strings are range-checked by the server. |
| `sidechain.frequency_hz` | number or string | Hz | 10.0..20000.0 | 100.0 | Quantity strings are range-checked by the server. |
| `sidechain.start_s` | number |  | 0.0.. |  | default: 0.25 * seconds |
| `sidechain.length_s` | number |  |  |  | default: 0.5 * seconds |
| `notes` | array |  |  |  | instrument sequence replacing the notes stimulus |
| `notes[].key` | integer |  | 0.0..127.0 |  |  |
| `notes[].velocity` | number |  | ..1.0 | 0.8 |  |
| `notes[].start_s` | number |  | 0.0.. |  |  |
| `notes[].length_s` | number |  |  |  |  |
| `level_db` | number or string | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | number or string | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.1..10.0 | 1.0 |  |
| `sample_rate` | number or string | Hz | 8000.0..384000.0 |  | default: the session's or project's rate Quantity strings are range-checked by the server. |
| `save` | boolean |  |  | false |  |
| `trace_points` | integer |  | 1.0..4096.0 | 100 | intervals the signal's traces are kept in |

### Result

```json
{
  "type": "object",
  "title": "dsp_test_result",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "sample_rate": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "string",
          "pattern": "^\\s*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?(?:\\s*(?:[hH][zZ]|[kK][hH][zZ]|[kK]))?\\s*$"
        }
      ],
      "x-unit": "Hz",
      "x-unit-strings": true,
      "description": "the rate rendered at Quantity strings are range-checked by the server."
    },
    "latency": {
      "type": "integer",
      "description": "samples"
    },
    "signals": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_signal_result",
        "properties": {
          "signal": {
            "type": "string"
          },
          "peak_dbfs": {
            "type": "number"
          },
          "rms_dbfs": {
            "type": "number"
          },
          "dc": {
            "type": "number"
          },
          "clipped": {
            "type": "integer",
            "minimum": 0.0
          },
          "denormals": {
            "type": "integer",
            "minimum": 0.0
          },
          "gain_db": {
            "type": "number",
            "description": "sine: the fundamental out against in"
          },
          "thd_percent": {
            "type": "number"
          },
          "response_db": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "dsp_response_point",
              "properties": {
                "hz": {
                  "type": "number"
                },
                "db": {
                  "type": "number"
                }
              },
              "required": ["hz", "db"],
              "additionalProperties": false
            }
          },
          "pitch_hz": {
            "type": "number",
            "description": "note: the pitch heard (A4 is 440 Hz)"
          },
          "release_ms": {
            "type": "number",
            "description": "an instrument's: sound after the last release"
          },
          "path": {
            "type": "string"
          },
          "automation": {
            "type": "object",
            "title": "dsp_automation_result",
            "properties": {
              "max_step": {
                "type": "number"
              },
              "baseline_max_step": {
                "type": "number"
              },
              "excess_step": {
                "type": "number"
              }
            },
            "required": ["max_step", "baseline_max_step", "excess_step"],
            "additionalProperties": false
          },
          "sidechain": {
            "type": "object",
            "title": "dsp_sidechain_result",
            "properties": {
              "rms_dbfs": {
                "type": "number"
              },
              "reference_rms_dbfs": {
                "type": "number"
              },
              "gain_change_db": {
                "type": ["number", "null"]
              }
            },
            "required": ["rms_dbfs", "reference_rms_dbfs", "gain_change_db"],
            "additionalProperties": false
          },
          "meters": {
            "description": "read-only parameters (meters) by patch key, physical, as of the signal's end"
          },
          "trace_interval_s": {
            "type": "number",
            "description": "each trace interval's length"
          },
          "traces": {
            "type": "array",
            "items": {
              "type": "object",
              "title": "dsp_trace_result",
              "properties": {
                "name": {
                  "type": "string"
                },
                "last": {
                  "description": "each interval's last value, or null"
                },
                "low": {
                  "description": "each interval's lowest"
                },
                "high": {
                  "description": "each interval's highest"
                },
                "non_finite": {
                  "type": "integer",
                  "minimum": 0.0,
                  "description": "NaN or infinity given, not kept"
                }
              },
              "required": ["name", "last", "low", "high", "non_finite"],
              "additionalProperties": false
            },
            "description": "what trace() recorded, by name"
          }
        },
        "required": ["signal", "peak_dbfs", "rms_dbfs", "dc", "clipped", "denormals"],
        "additionalProperties": false
      }
    },
    "cpu_percent": {
      "type": "number",
      "description": "of real time, measured"
    },
    "problems": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "log": {
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
  "required": ["plugin", "sample_rate", "latency", "signals", "cpu_percent", "problems", "log"],
  "additionalProperties": false
}

```

### Errors

- `dsp.args`: neither or both of plugin and script Hint: give exactly one
- `dsp.id_taken`: another script in the library has the script's id Hint: give each script its own Info.id
- `dsp.not_listed`: the script compiles but no plugin has its id Hint: put the script player on the plugin path
- `dsp.not_effect`: the plugin takes no audio and no notes Hint: test an effect or an instrument
- `dsp.signal`: a signal the plugin cannot play Hint: an effect plays audio signals, an instrument notes
- `dsp.automation`: unavailable parameter or automation time outside the render Hint: choose a writable semantic parameter and a time before seconds
- `dsp.sidechain`: no key input or invalid key timing Hint: choose a plugin with a key and a nonempty span within seconds
- `dsp.notes`: invalid custom sequence or signal selection Hint: use an instrument, notes or notes and silence, and spans within seconds
- `dsp.frequency`: a sine at or above half the sample rate Hint: lower frequency_hz or raise sample_rate
- `dsp.file`: the file signal without audio_file, or a file that is not absolute, is over 10 minutes or holds no audio Hint: give audio_file a WAV, FLAC or MP3, or leave file out of signals

### Examples

**Measure an effect with a sine** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Automate an effect in dB** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"],
  "automation": [
    {
      "param": "gain",
      "at_s": 0.253,
      "value": -12
    },
    {
      "param": "gain",
      "at_s": 0.753,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Measure a timed sidechain** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.keymix",
  "signals": ["sine"],
  "sidechain": {
    "level_db": -6
  }
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Measure an effect at 96 kHz** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "signals": ["sine"],
  "sample_rate": 96000
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

**Play a custom note sequence** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.sine",
  "notes": [
    {
      "key": 60,
      "start_s": 0.013,
      "length_s": 0.2
    },
    {
      "key": 67,
      "velocity": 0.6,
      "start_s": 0.303,
      "length_s": 0.4
    }
  ]
}

```

Result:

```json
{
  "plugin": "<any>",
  "sample_rate": "<any>",
  "latency": "<any>",
  "signals": "<any>",
  "cpu_percent": "<any>",
  "problems": "<any>",
  "log": "<any>"
}

```

## `dsp.write@1`

Save a DSP script to the library and compile it, with diagnostics.

Writes <library>/<name>/<name>.cpp (replacing it), compiles it and returns the compiler's diagnostics, each with a hint. A script that compiles becomes a plugin of the script player under the id its Info gives: plugin.list shows it (listed) and fx.add loads it. Instances already playing it pick up the change within half a second, crossfaded; one that fails to compile leaves them playing the old code. Language: the dsp-language guide.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | the script's folder and file name |
| `source` | string |  |  |  | the whole C++ file |

### Result

```json
{
  "type": "object",
  "title": "dsp_write_result",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "ok": {
      "type": "boolean",
      "description": "it compiled"
    },
    "diagnostics": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "dsp_diagnostic",
        "properties": {
          "severity": {
            "type": "string",
            "description": "error or warning"
          },
          "code": {
            "type": "string",
            "description": "script.<area>.<what>"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string",
            "description": "what to write instead"
          },
          "line": {
            "type": "integer"
          },
          "column": {
            "type": "integer"
          }
        },
        "required": ["severity", "code", "message", "hint", "line", "column"],
        "additionalProperties": false
      }
    },
    "plugin": {
      "type": "object",
      "title": "dsp_plugin",
      "properties": {
        "id": {
          "type": "string"
        },
        "name": {
          "type": "string"
        },
        "kind": {
          "type": "string",
          "description": "effect (audio in, audio out) or instrument (notes in)"
        },
        "params": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_param",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "continuous, integer, choice or toggle"
              },
              "unit": {
                "type": "string",
                "description": "dB, Hz, ms, % ..."
              },
              "min": {
                "type": "number"
              },
              "max": {
                "type": "number"
              },
              "default": {
                "type": "number"
              },
              "choices": {
                "type": "array",
                "items": {
                  "type": "string"
                }
              },
              "meter": {
                "type": "boolean",
                "description": "an output the plugin reports"
              }
            },
            "required": ["key", "name", "kind", "min", "max", "default", "choices", "meter"],
            "additionalProperties": false
          }
        },
        "files": {
          "type": "array",
          "items": {
            "type": "object",
            "title": "dsp_file",
            "properties": {
              "key": {
                "type": "string",
                "description": "the patch's properties key"
              },
              "name": {
                "type": "string"
              },
              "kind": {
                "type": "string",
                "description": "audio (a sample) or impulse (a Convolver's response)"
              },
              "description": {
                "type": "string"
              }
            },
            "required": ["key", "name", "kind"],
            "additionalProperties": false
          },
          "description": "the script's files (AudioFile, Convolver), when it has any: give each an absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"
        }
      },
      "required": ["id", "name", "kind", "params"],
      "additionalProperties": false
    },
    "listed": {
      "type": "boolean",
      "description": "plugin.list shows it (the script player is on the plugin path)"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["name", "path", "ok", "diagnostics", "listed"],
  "additionalProperties": false
}

```

### Errors

- `dsp.name`: not a script name Hint: use lower-case letters, digits, _ or -
- `dsp.size`: the script is longer than 1 MiB Hint: split it up
- `dsp.id_taken`: another script in the library has its id; not saved Hint: give Info.id a value no other script has

### Examples

**Save a gain script** (`empty`)

```json
{
  "name": "gain",
  "source": "#include <supple/script.h>\n\nstruct Gain : Effect {\n    static constexpr Info info = {.name = \"Gain\", .id = \"com.example.gain\"};\n    Param gain = decibels(\"Gain\", -24, 24, 0).smoothed(20).help(\"The level change.\");\n\n    void process(Block& io) {\n        for (int c = 0; c < io.channels; ++c)\n            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));\n    }\n};\n"
}

```

Result:

```json
{
  "name": "gain",
  "path": "<any>",
  "ok": true,
  "diagnostics": [],
  "plugin": "<any>",
  "listed": "<any>"
}

```
