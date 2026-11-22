# `sync` commands

External sync settings (clock, MTC, LTC) and their status.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `sync.get@1`

Show external sync settings and what the engine hears from the master.

source: off, midi_clock, mtc or ltc. midi_input: the MIDI input port the master's clock or MTC arrives on (its name, or a part of it). ltc_input: the device input channel carrying LTC (1-based). follow_tempo (midi_clock): chase the master's tempo with the live playrate (pitch preserved); off follows Start, Continue, Stop and Song Position only. offset_ms: positive plays Steppe that much behind the master. on_loss: stop, or freewheel for freewheel_frames timecode frames, when the signal stops. ltc_output: write LTC of the transport (the project's timecode) to ltc_output_channel, replacing whatever a master plays there, at ltc_level_db. status: present (the signal arrives), running, locked, state (idle, acquiring, relocating, chasing, locked), the master's bpm (midi_clock) or timecode and rate (mtc, ltc), chase_rate (the live playrate the chase applies on top of the saved one) and error_ms.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "sync_info",
  "properties": {
    "settings": {},
    "status": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings", "status"],
  "additionalProperties": false
}

```

### Examples

**Off** (`empty`)

```json
{}

```

Result:

```json
{
  "settings": {
    "source": "off",
    "midi_input": "",
    "ltc_input": 1,
    "follow_tempo": true,
    "offset_ms": 0.0,
    "on_loss": "stop",
    "freewheel_frames": 25,
    "ltc_output": false,
    "ltc_output_channel": 3,
    "ltc_level_db": -18.0
  },
  "status": "<any>"
}

```

## `sync.set@1`

Set external sync (MIDI clock, MTC, LTC) and LTC output; returns them.

source: off, midi_clock, mtc or ltc. midi_input: the MIDI input port the master's clock or MTC arrives on (its name, or a part of it). ltc_input: the device input channel carrying LTC (1-based). follow_tempo (midi_clock): chase the master's tempo with the live playrate (pitch preserved); off follows Start, Continue, Stop and Song Position only. offset_ms: positive plays Steppe that much behind the master. on_loss: stop, or freewheel for freewheel_frames timecode frames, when the signal stops. ltc_output: write LTC of the transport (the project's timecode) to ltc_output_channel, replacing whatever a master plays there, at ltc_level_db. Omitted members keep their values. Machine settings: not an undo step, not saved in the project (the app keeps them with its preferences).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | One of: off, midi_clock, mtc, ltc. |
| `midi_input` | string |  |  |  |  |
| `ltc_input` | integer |  | 1..64 |  |  |
| `follow_tempo` | boolean |  |  |  |  |
| `offset_ms` | number |  | -10000..10000 |  |  |
| `on_loss` | string |  |  |  | One of: stop, freewheel. |
| `freewheel_frames` | integer |  | 1..300 |  |  |
| `ltc_output` | boolean |  |  |  |  |
| `ltc_output_channel` | integer |  | 1..64 |  |  |
| `ltc_level_db` | number |  | -60..0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "sync_info",
  "properties": {
    "settings": {},
    "status": {
      "type": "object",
      "title": "sync_status",
      "properties": {
        "source": {
          "type": "string"
        },
        "input": {
          "type": "string"
        },
        "present": {
          "type": "boolean"
        },
        "running": {
          "type": "boolean"
        },
        "locked": {
          "type": "boolean"
        },
        "state": {
          "type": "string"
        },
        "bpm": {
          "type": "number"
        },
        "timecode": {
          "type": "string"
        },
        "rate": {
          "type": "string"
        },
        "chase_rate": {
          "type": "number"
        },
        "error_ms": {
          "type": "number"
        },
        "locate_ms": {
          "type": "number"
        },
        "ltc_output": {
          "type": "boolean"
        },
        "warnings": {
          "type": "array",
          "items": {
            "type": "string"
          }
        }
      },
      "required": [
        "source",
        "input",
        "present",
        "running",
        "locked",
        "state",
        "bpm",
        "timecode",
        "rate",
        "chase_rate",
        "error_ms",
        "locate_ms",
        "ltc_output",
        "warnings"
      ],
      "additionalProperties": false
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["settings", "status"],
  "additionalProperties": false
}

```

### Examples

**Chase MTC from a MIDI interface** (`empty`)

```json
{
  "source": "mtc",
  "midi_input": "Interface"
}

```

Result:

```json
{
  "settings": {
    "source": "mtc",
    "midi_input": "Interface",
    "ltc_input": 1,
    "follow_tempo": true,
    "offset_ms": 0.0,
    "on_loss": "stop",
    "freewheel_frames": 25,
    "ltc_output": false,
    "ltc_output_channel": 3,
    "ltc_level_db": -18.0
  },
  "status": "<any>"
}

```
