# Command reference

Generated from the command registry.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code, message, path}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

## `analyze@1`

Analyze a render or audio file: loudness, spectrum, pitch, envelope, stereo.

source: rd_<n> or a file path. Report supple.analysis-report/1.0: loudness (LUFS, LRA, true peak), spectral (centroid, rolloff, flatness, tilt, named band levels relative to the total), harmonic (f0, note, harmonics in dB relative to the fundamental), envelope (attack, decay to -20 dB, release after note-off, onsets), stereo (width, correlation). note_off_s defaults to the end of a render's input. Envelope values (attack, decay, release) describe single notes: render one note to measure them.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. |
| `note_off_s` | number |  | 0.0..3600.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "analysis",
  "properties": {
    "report": {},
    "summary": {
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
  "required": ["report", "summary"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_render`: no such render Hint: session.get lists the renders
- `audio.*`: the file cannot be read Hint: WAV, FLAC or MP3

### Examples

**Analyze the last render** (`synth_and_render`)

```json
{
  "source": "rd_1"
}

```

Result:

```json
{
  "report": "<any>",
  "summary": "<any>"
}

```

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

## `compare@1`

Compare two renders or files: notable descriptor changes and timbre distance.

Notable changes use fixed thresholds (loudness 1 LU, centroid 10 %, band levels 1.5 dB, harmonics 3 dB, f0 10 cents, ...). ltas_rms_db is the RMS difference of the 1/3-octave spectra after removing the level offset: a pure gain change reads 0. loss is the score the optimizer minimizes and the evaluation scorer judges: the test is loudness-matched to the reference, both are analyzed at their note-off times (note_off_s reports them), and loss.total is the number to get down. A render's note-off is its own input end; give reference_note_off_s / test_note_off_s for files, and one unknown side borrows the other's (note_off_assumed).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `reference` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `test` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `reference_note_off_s` | number |  | 0.0..3600.0 |  | The reference's note-off; defaults to a render's input end. |
| `test_note_off_s` | number |  | 0.0..3600.0 |  | The test's note-off; defaults to a render's input end. |

### Result

```json
{
  "type": "object",
  "title": "comparison",
  "properties": {
    "notable": {},
    "ltas_rms_db": {
      "type": "number"
    },
    "summary": {
      "type": "string"
    },
    "loss": {
      "description": "The score the optimizer minimizes: total, per-term contributions, and the note-off_s used per side."
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
  "required": ["notable", "ltas_rms_db", "summary", "loss"],
  "additionalProperties": false
}

```

### Examples

**A render against itself** (`synth_and_render`)

```json
{
  "reference": "rd_1",
  "test": "rd_1"
}

```

Result:

```json
{
  "notable": [],
  "ltas_rms_db": 0.0,
  "summary": "<any>",
  "loss": {
    "total": "<any>",
    "terms": "<any>",
    "note_off_s": {
      "reference": "<any>",
      "test": "<any>"
    }
  }
}

```

## `dsp.check@1`

Compile a DSP script without saving it; return its diagnostics.

Give source (a whole script) or name (one in the library). ok is true when it compiles; plugin then lists its id, name and parameters as the patch shows them.

Kind: `query`. Exposure: `mcp-tool`.

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

Change part of a library script by replacing text; compile it, as dsp.write.

For a change to part of a script, instead of sending all of it to dsp.write. Each edit replaces old, which must be in the script exactly once, with new (empty deletes it). The edits apply in order, each to the script as the ones before left it, and all or none: when one fails, nothing is saved and the error says where old and the script part. Then as dsp.write: it saves the script, compiles it and returns the diagnostics; instances playing it follow within half a second.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `edits` | array |  |  |  |  |

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

Export a DSP script as a plugin for other DAWs: a CLAP file, or C++ source.

format clap (the default): copies the platform's script player and embeds the script in its trailer (Linux/Windows) or bundle resource (macOS, re-signed ad hoc with the system codesign tool). The copy is a plugin of its own, under the script's id and name, that plays this version of the script in any CLAP host (it reads no library), written to <folder>/<plugin name>.clap (default: the script's folder, under export/) with the licenses to ship beside it (notices). It runs on this platform, as fast as the player it copies (a debug build's is slow). format source: a C++ project of the plugin template that builds the same plugin (same id, parameters and state) as CLAP and VST3, plus AU on macOS, with the script compiled as C++, written to the empty <folder> (default: the script's folder, under export/<script>-source); its README says how to build it, and license (MIT unless GPL-3.0-or-later) is the license of its own files. Export again after changing the script; an export does not follow the library.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `script` | string |  |  |  | a name from dsp.list |
| `format` | string |  |  |  | clap (default): a plugin file; source: a C++ project |
| `folder` | string |  |  |  | an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new |
| `license` | string |  |  |  | source: the license of the project's own files (default MIT) |

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



Kind: `query`. Exposure: `mcp-tool`.

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



Kind: `query`. Exposure: `mcp-tool`.

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

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signal` | string |  |  |  |  |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `level_db` | value | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | value | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.0..10.0 | 1.0 |  |
| `scale` | number |  | 0.5..3.0 | 1.0 |  |
| `pointer` | array |  |  |  |  |

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

Play test signals or notes through a plugin and measure it: levels, THD, pitch.

Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in the library). A fresh instance, set by patch (a merge patch) when given, plays each signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at level_db; and file, audio_file played as it is (resampled to the render's rate, mono on both channels, at most 60 s), the only signal when signals is not given. An instrument, at velocity 0.8: notes (a note, then a chord), note (A4; pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from silence, no output, a note out of tune, sound that never dies away, non-finite samples the framework silenced, a script stopped by its loop budget or indexing outside an array). cpu_percent is measured, so it varies between runs. save: true keeps each output as a WAV (path) for analyze and compare. automation is a list of {param, at_s, value}: semantic patch keys and physical values, as discrete sample-accurate changes applied to every signal (no implicit ramps). automation.max_step is the largest adjacent-sample change in full-scale amplitude; baseline_max_step uses the same render without automation; excess_step is their positive difference. A step above both four times baseline and 0.05 suggests clicks or zipper noise: a screening heuristic, not proof of an artifact or of inaudibility. sidechain supplies a timed key; its middle half is measured against a connected silent key with identical automation: gain_change_db is negative for ducking, null if either output is below -120 dBFS. Slow attack may not settle in that window. Each pass has a fresh instance and initial patch. THD and impulse response are omitted during automation or a non-silent key; gain_db then describes the changing render, not static gain. notes supplies an instrument's sequence ({key, velocity, start_s, length_s}), replacing the notes signal; defaults become notes and silence. Do not combine it with the A4 note signal or select only silence. release_ms starts at its last note-off. Times round to samples at the render rate; spans must fit within seconds. sample_rate renders at that rate instead of the session's (sandbox) or the project's (DAW); latency is in samples at the rate used, which sample_rate in the result reports. meters gives a semantic plugin's read-only parameters (its meters and readouts) by patch key in physical units, as they stood when the signal ended (the tail after it does not move them). traces gives what a script's trace(name, value) calls recorded over the signal, in trace_points intervals of trace_interval_s: for each name, the last value in each interval and the lowest and highest, null where none was given.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `script` | string |  |  |  |  |
| `patch` | value |  |  |  |  |
| `signals` | array |  |  |  | default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes |
| `audio_file` | string |  |  |  | the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect |
| `automation` | array |  |  |  | discrete physical-value changes on every signal |
| `sidechain` | object |  |  |  | timed generated key input |
| `notes` | array |  |  |  | instrument sequence replacing the notes stimulus |
| `level_db` | value | dB | -60.0..0.0 | -12.0 | Quantity strings are range-checked by the server. |
| `frequency_hz` | value | Hz | 10.0..20000.0 | 1000.0 | Quantity strings are range-checked by the server. |
| `seconds` | number |  | 0.1..10.0 | 1.0 |  |
| `sample_rate` | value | Hz | 8000.0..384000.0 |  | default: the session's or project's rate Quantity strings are range-checked by the server. |
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

Kind: `action`. Exposure: `mcp-tool`.

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

## `masking@1`

Estimate where one sound masks another (critical bands).

masker (e.g. the kick) and maskee (e.g. the bass): renders or files at the same sample rate. Per Bark band: % of the maskee's active frames masked and by how much; the three worst bands. An estimate for corrective mixing, not a psychoacoustic model of record.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `masker` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `maskee` | string |  |  |  | A render id (rd_1) or an audio file path. |

### Result

```json
{
  "type": "object",
  "title": "masking",
  "properties": {
    "result": {},
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
  "required": ["result"],
  "additionalProperties": false
}

```

### Examples

**A render masking itself** (`synth_and_render`)

```json
{
  "masker": "rd_1",
  "maskee": "rd_1"
}

```

Result:

```json
{
  "result": "<any>"
}

```

## `optimize.start@1`

Tune continuous parameters toward a reference sound or a profile (a job).

You choose the structure (patch) and which knobs matter; CMA-ES tunes the numbers. params: "auto" (continuous parameters of the modules present, max 32) or keys, optionally {key, min, max} to narrow the search. "auto" searches around each parameter's current value (±2 octaves for frequency, time and pitch-interval quantities, ±25 % of the range otherwise): name keys with min/max when the answer may be far from the current patch. target: {"reference": rd_<n> or file} and/or {"profile": supple.analysis-profile document}; a reference target starts from a smaller initial step than a profile, because the patch is already close. phrase: notation rendered per candidate (default "1: C3:w"; use the reference's phrase when matching a reference). Returns a job: poll job.wait; the result has the best loss per term, the failed-candidate count, changes, a merge patch ({"params": {...}}) for patch.merge, and the best candidate's render.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  |  |
| `params` | value |  |  | "auto" | "auto" or a list of keys / {key, min, max}. |
| `target` | value |  |  |  | {reference?, profile?} |
| `phrase` | string |  |  |  |  |
| `seconds` | number |  | ..30.0 |  |  |
| `tail_s` | number |  | 0.0..10.0 |  |  |
| `evaluations` | integer |  | 10.0..5000.0 |  |  |
| `max_seconds` | number |  | 1.0..600.0 |  |  |
| `seed` | integer |  | 0.0..1e+09 | 0 |  |

### Result

```json
{
  "type": "object",
  "title": "optimize_started",
  "properties": {
    "job": {
      "type": "string"
    },
    "status": {
      "type": "string"
    },
    "params": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "render": {
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
  "required": ["job", "status", "params", "render"],
  "additionalProperties": false
}

```

### Errors

- `optimize.not_semantic`: the plugin has no semantic schema Hint: use a stock plugin
- `optimize.target`: no reference or profile Hint: {"reference": "rd_1"}
- `optimize.too_many`: more than 32 parameters Hint: name the ones that matter

### Examples

**Tune toward a profile** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "evaluations": 20,
  "max_seconds": 5,
  "target": {
    "profile": {
      "schema": "supple.analysis-profile/1.0",
      "targets": {
        "spectral.centroid_hz.median": {
          "target": 800,
          "tolerance": 100
        }
      }
    }
  }
}

```

Result:

```json
{
  "job": "<any>",
  "status": "running",
  "params": "<any>",
  "render": "<any>"
}

```

## `param.get@1`

Read parameter values (physical value and display text).

keys: semantic keys (e.g. "filter.cutoff"); omit for all. Third-party plugins: CLAP ids or names; values are CLAP values.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `keys` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "param_values",
  "properties": {
    "values": {},
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
  "required": ["values"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_param`: no parameter with that key Hint: closest key in the hint; see plugin.describe

### Examples

**Read one value** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "keys": ["gain"]
}

```

Result:

```json
{
  "values": {
    "gain": {
      "value": 0.0,
      "text": "<any>"
    }
  }
}

```

## `param.set@1`

Set parameters in physical units; returns the values as applied.

values: {key: number | "unit string" | enum key | bool}, e.g. {"filter.cutoff": "1.2 kHz", "filter.mode": "lowpass"}. All values are validated first; nothing changes if any is invalid. Third-party plugins: CLAP id or name keys with CLAP values or {"text": "..."}. A value that does not apply with the new values says "active": false (plugin.describe detail full says what makes it apply). Use patch.merge to change several sections at once.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `values` | value |  |  |  | Key -> value. |

### Result

```json
{
  "type": "object",
  "title": "param_values",
  "properties": {
    "values": {},
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
  "required": ["values"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_param`: no parameter with that key Hint: closest key in the hint
- `params.out_of_range`: a value is outside its range Hint: the hint gives the valid range
- `params.parse`: a unit string could not be read Hint: the hint shows the expected form

### Examples

**Set a level with a unit string** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "values": {
    "gain": "-6 dB"
  }
}

```

Result:

```json
{
  "values": {
    "gain": {
      "value": -6.0,
      "text": "<any>"
    }
  }
}

```

## `patch.get@1`

Get an instance's patch document (changed values or all).

detail changed (default): only values that differ from the defaults, still a complete patch. detail full: every value. Semantic plugins only.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `detail` | string |  |  | "changed" |  |

### Result

```json
{
  "type": "object",
  "title": "patch",
  "properties": {
    "patch": {},
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
  "required": ["patch"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use param.get / presets

### Examples

**Get the changed values** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "patch": "<any>"
}

```

## `patch.merge@1`

Merge a partial patch (RFC 7396) into an instance.

patch: only the values to change, e.g. {"params": {"filter.cutoff": "800 Hz"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "patch_merge",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Params set that do not apply now"
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
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use param.set
- `params.out_of_range`: a value is outside its range Hint: the hint gives the valid range

### Examples

**Turn it down** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "patch": {
    "params": {
      "gain": "-12 dB"
    }
  }
}

```

Result:

```json
{
  "changed": {
    "params": {
      "gain": -12.0
    }
  },
  "description": "<any>"
}

```

## `patch.set@1`

Replace an instance's whole patch; returns a description of the result.

patch: a complete patch document (see plugin.describe detail full for its schema); missing values take their defaults. Validated fully before anything changes. For partial edits use patch.merge.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "description",
  "properties": {
    "description": {
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
  "required": ["description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.not_semantic`: the plugin has no semantic patch Hint: use presets
- `schema.invalid`: the patch has problems Hint: details list each with its path

### Examples

**Reset to defaults** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "patch": {
    "schema": "supple.test-gain-patch/1.0"
  }
}

```

Result:

```json
{
  "description": "<any>"
}

```

## `plugin.describe@1`

Describe an instance: parameters, patch format, and current problems.

detail summary: parameters grouped as compact 'key = value (range)' lines plus a prose description of the current sound. detail full: every parameter's schema entry with current value and text, and the patch JSON Schema. inactive lists the parameters that do not apply with the current values (summary leaves them out): their entries carry "active": false, and active_when says what makes them apply (another parameter's values), or their module is not in the patch (add it with patch.merge). Semantic plugins are programmed with param.set (physical units) or patch.merge.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `detail` | string |  |  | "summary" |  |

### Result

```json
{
  "type": "object",
  "title": "plugin_description",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "params": {},
    "patch_type": {
      "type": "string"
    },
    "patch_schema": {},
    "description": {
      "type": "string"
    },
    "docs": {
      "type": "string"
    },
    "status_available": {
      "type": "boolean",
      "description": "The instance supplied its current problems"
    },
    "problems": {
      "description": "Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable"
    },
    "inactive": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "Parameters that do not apply now"
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
  "required": ["plugin", "name", "params", "description", "status_available"],
  "additionalProperties": false
}

```

### Examples

**Summarize the effect** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "params": "<any>",
  "patch_type": "<any>",
  "description": "<any>",
  "status_available": false
}

```

## `plugin.list@1`

List the CLAP plugins the sandbox can load.

Scans the plugin path once (rescan: true to scan again). filter matches the id or name, case-insensitively. Load one with plugin.load.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `filter` | string |  |  |  | Text to match in the id or name. |
| `rescan` | boolean |  |  | false |  |

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
        "title": "plugin",
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
            }
          },
          "path": {
            "type": "string"
          },
          "version": {
            "type": "string"
          },
          "shadowed": {
            "type": "boolean",
            "description": "Not loadable by id: an earlier location on the plugin path has the same id (load it by path)"
          }
        },
        "required": [
          "id",
          "name",
          "vendor",
          "instrument",
          "semantic",
          "features",
          "path",
          "version"
        ],
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
  "required": ["plugins", "scan"],
  "additionalProperties": false
}

```

### Examples

**List everything** (`empty`)

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

## `plugin.load@1`

Load a plugin into the chain and return its instance id.

plugin is an id from plugin.list or a .clap path. An instrument goes first in the chain, anything else last, unless position says otherwise (instruments must stay first). Then describe it with plugin.describe.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  | Plugin id or .clap path. |
| `position` | integer |  | 0.0.. |  | Chain index (0 = first). |

### Result

```json
{
  "type": "object",
  "title": "plugin_load",
  "properties": {
    "instance": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "semantic": {
      "type": "boolean"
    },
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
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
  "required": ["instance", "name", "semantic", "chain"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_plugin`: no plugin with that id or path Hint: closest ids in the hint; see plugin.list
- `sandbox.chain_order`: an instrument would not be first, or a second instrument Hint: remove the existing instrument or load an effect
- `sandbox.position`: position past the end of the chain Hint: 0 .. chain length

### Examples

**Load an instrument** (`empty`)

```json
{
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "instance": "in_1",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["in_1"]
}

```

**Add an effect after the instrument** (`synth_loaded`)

```json
{
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "instance": "in_3",
  "name": "<any>",
  "semantic": "<any>",
  "chain": ["in_1", "in_2", "in_3"]
}

```

## `plugin.remove@1`

Remove a plugin instance from the chain.

Unloads the instance; its id is not reused.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |

### Result

```json
{
  "type": "object",
  "title": "chain",
  "properties": {
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
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
  "required": ["chain"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_instance`: no such instance Hint: the hint lists loaded instances

### Examples

**Remove the effect** (`synth_loaded`)

```json
{
  "instance": "in_2"
}

```

Result:

```json
{
  "chain": ["in_1"]
}

```

## `preset.list@1`

List user presets, or a plugin's read-only factory patches.

source: user (default) lists saved presets. source: factory requires plugin and lists its immutable factory patches in declaration order.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `plugin` | string |  |  |  |  |
| `source` | string |  |  | "user" |  |

### Result

```json
{
  "type": "object",
  "title": "preset_list",
  "properties": {
    "presets": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "preset",
        "properties": {
          "plugin": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "tags": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "read_only": {
            "type": "boolean"
          }
        },
        "required": ["plugin", "name", "tags"],
        "additionalProperties": false
      }
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
  "required": ["presets"],
  "additionalProperties": false
}

```

### Examples

**List the effect's presets** (`synth_with_preset`)

```json
{
  "plugin": "com.suppleaudio.test.gain"
}

```

Result:

```json
{
  "presets": [
    {
      "plugin": "com.suppleaudio.test.gain",
      "name": "Quiet",
      "tags": ["soft"]
    }
  ]
}

```

## `preset.load@1`

Load a named preset into an instance.

The preset must belong to the instance's plugin (see preset.list). source: factory applies a read-only factory patch; user (default) loads a saved preset.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `name` | string |  |  |  |  |
| `source` | string |  |  | "user" |  |

### Result

```json
{
  "type": "object",
  "title": "description",
  "properties": {
    "description": {
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
  "required": ["description"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.unknown_preset`: no preset with that name for this plugin Hint: closest names in the hint

### Examples

**Load** (`synth_with_preset`)

```json
{
  "instance": "in_2",
  "name": "Quiet"
}

```

Result:

```json
{
  "description": "<any>"
}

```

## `preset.save@1`

Save an instance's current settings as a named preset.

Stores the patch (semantic plugins) or the plugin state. Overwrites a preset of the same name for the same plugin. Names: 1-64 of A-Z a-z 0-9 space _ . + -.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `instance` | string |  |  |  | Instance id from plugin.load, e.g. "in_1". |
| `name` | string |  |  |  |  |
| `tags` | array |  |  |  |  |
| `notes` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "path",
  "properties": {
    "path": {
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
  "required": ["path"],
  "additionalProperties": false
}

```

### Errors

- `sandbox.preset_name`: the name has characters outside the allowed set Hint: letters, digits, space _ . + -

### Examples

**Save** (`synth_loaded`)

```json
{
  "instance": "in_2",
  "name": "Quiet",
  "tags": ["soft"]
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `profile.evaluate@1`

Score a render against a target descriptor profile.

profile: {"schema": "supple.analysis-profile/1.0", "targets": {"<report key>": {"target": x, "tolerance": t, "weight"?: w} | {"min"?: a, "max"?: b}}, "ltas"?: [31 relative dB]}. Keys are dotted report paths such as spectral.centroid_hz.median or harmonic.harmonics_db.3. Returns per-target pass and normalized distance, and a weighted loss (0 = on target).

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | A render id (rd_1) or an audio file path. |
| `profile` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "profile_result",
  "properties": {
    "result": {},
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
  "required": ["result"],
  "additionalProperties": false
}

```

### Examples

**A brightness target** (`synth_and_render`)

```json
{
  "source": "rd_1",
  "profile": {
    "schema": "supple.analysis-profile/1.0",
    "targets": {
      "spectral.centroid_hz.median": {
        "target": 1500,
        "tolerance": 500
      }
    }
  }
}

```

Result:

```json
{
  "result": "<any>"
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

## `render@1`

Render notes, a MIDI file or an audio file through the chain to a WAV.

notation: a phrase in the notation (see the notation guide), e.g. "voice lead; 1: C4:q E4:q G4:h | C5:w". midi_file / audio_file: paths (audio at the session rate). bars/seconds override the length; the tail after it is rendered until silence (tail_s caps it). Returns the render id for analyze/compare and the WAV path.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `notation` | string |  |  |  | Notation text. |
| `midi_file` | string |  |  |  |  |
| `audio_file` | string |  |  |  |  |
| `bars` | number |  | ..1000.0 |  |  |
| `seconds` | number |  | ..600.0 |  |  |
| `tempo` | value | bpm | 1.0..999.0 |  | Quantity strings are range-checked by the server. |
| `tail_s` | number |  | 0.0..60.0 |  |  |
| `name` | string |  |  |  |  |
| `analyze` | string |  |  | "none" | Also analyze the render (see the analyze command); summary is compact (≤ 400 tokens). |

### Result

```json
{
  "type": "object",
  "title": "render_result",
  "properties": {
    "render": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "peak_dbfs": {
      "type": "number"
    },
    "tail_s": {
      "type": "number"
    },
    "tail_capped": {
      "type": "boolean"
    },
    "latency_samples": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "anyOf": [
          {
            "type": "string"
          },
          {
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
          }
        ]
      }
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "analysis": {}
  },
  "required": ["render", "path", "seconds", "peak_dbfs", "tail_s", "tail_capped", "latency_samples"],
  "additionalProperties": false
}

```

### Errors

- `notation.*`: the notation does not compile Hint: the error has line:col and a hint
- `render.empty_chain`: nothing is loaded Hint: plugin.load first
- `render.not_instrument`: notes given but the first plugin is an effect Hint: load an instrument first
- `render.input_rate`: audio_file at another sample rate Hint: session.set sample_rate

### Examples

**Render a phrase** (`synth_loaded`)

```json
{
  "notation": "voice lead; 1: A4:q E5:q r:h"
}

```

Result:

```json
{
  "render": "rd_1",
  "path": "<any>",
  "seconds": "<any>",
  "peak_dbfs": "<any>",
  "tail_s": "<any>",
  "tail_capped": false,
  "latency_samples": 0
}

```

## `script.run@1`

Run a Lua script on the command bus.

Every call the script makes goes through the command registry, so it is validated, transactional and in the undo history like a call from the GUI or an agent. Use it for loops, conditionals and arithmetic that the command set cannot express; use batch for a fixed list of steps. Not transactional itself: undo the edits it made with history.undo.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `code` | string |  |  |  | The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays). |
| `name` | string |  |  |  | The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name. |
| `budget_ms` | number |  | 1.0..10000.0 |  | Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count. |
| `args` | value |  |  |  | Converted to the global table `args`. |

### Result

```json
{
  "type": "object",
  "title": "script_run_result",
  "properties": {
    "result": {
      "description": "What the script returned (null if it returned nothing)."
    },
    "log": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "What the script printed."
    },
    "log_dropped": {
      "type": "integer",
      "description": "Cumulative lines evicted from this script's retained log."
    },
    "script": {
      "type": "string",
      "description": "The name it ran under."
    },
    "instructions": {
      "type": "integer",
      "description": "Lua instructions the run executed."
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
  "required": ["result", "log", "log_dropped", "script", "instructions"],
  "additionalProperties": false
}

```

### Errors

- `script.syntax`: the source does not compile Hint: fix the Lua syntax at the reported line
- `script.budget`: the run spent its instruction budget Hint: raise budget_ms or make the script do less between waits
- `script.memory`: the run hit the script's memory cap Hint: raise memory_limit_bytes or hold less data
- `script.error`: the script raised a Lua error Hint: read the message and the traceback
- `script.arguments`: a command was called with something other than a table Hint: call it with a table of arguments, e.g. q.track.add{name = "Bass"}
- `script.unavailable`: no command dispatcher is attached to the script Hint: run the script through a host that provides a Runtime

### Examples

**A script that computes, with no commands** (`empty`)

```json
{
  "code": "local total = 0\nfor i = 1, 4 do total = total + i end\nreturn {total = total, steps = {1, 2, 3, 4}}"
}

```

Result:

```json
{
  "result": {
    "total": 10,
    "steps": [1, 2, 3, 4]
  },
  "log": [],
  "log_dropped": 0,
  "script": "<any>",
  "instructions": "<any>"
}

```

## `session.get@1`

Show the session: settings, chain, instances and renders.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "session",
  "properties": {
    "settings": {},
    "chain": {},
    "instances": {},
    "renders": {},
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
  "required": ["settings", "chain", "instances", "renders"],
  "additionalProperties": false
}

```

### Examples

**An empty session** (`empty`)

```json
{}

```

Result:

```json
{
  "settings": {
    "sample_rate": 48000.0,
    "block_size": 256,
    "tempo": 120.0,
    "meter": "4/4",
    "tail_limit_s": 5.0
  },
  "chain": [],
  "instances": {},
  "renders": {}
}

```

## `session.reset@1`

Unload every plugin, delete renders and restore default settings.

Not undoable. Saved presets are kept.

Kind: `action`. Exposure: `mcp-tool`.

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
  "additionalProperties": false
}

```

### Examples

**Start over** (`synth_loaded`)

```json
{}

```

Result:

```json
{}

```

## `session.set@1`

Change session settings (rate, block size, tempo, meter, tail limit).

Only the given fields change. A rate or block size change reactivates plugins at the next render.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `sample_rate` | value | Hz | 8000.0..384000.0 |  | Quantity strings are range-checked by the server. |
| `block_size` | integer |  | 1.0..8192.0 |  |  |
| `tempo` | value | bpm | 1.0..999.0 |  | Quantity strings are range-checked by the server. |
| `meter` | string |  |  |  |  |
| `tail_limit_s` | number |  | 0.0..60.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "settings",
  "properties": {
    "settings": {},
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
  "required": ["settings"],
  "additionalProperties": false
}

```

### Examples

**Slow down, 3/4** (`empty`)

```json
{
  "tempo": "90 bpm",
  "meter": "3/4"
}

```

Result:

```json
{
  "settings": {
    "sample_rate": 48000.0,
    "block_size": 256,
    "tempo": 90.0,
    "meter": "3/4",
    "tail_limit_s": 5.0
  }
}

```
