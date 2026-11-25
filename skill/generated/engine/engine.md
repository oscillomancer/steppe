# `engine` commands

The audio engine: devices, inputs, start, stop and status.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `engine.devices@1`

List audio devices: names, host APIs, rates, channel counts.

Enumerates PortAudio devices on macOS/Windows without opening a stream. backend defaults to portaudio; dummy returns an empty list without initializing hardware. Use exact host_api and name values with engine.start. Linux uses JACK ports and returns backend.unsupported; engine.inputs lists running JACK inputs.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  | One of: jack, dummy, portaudio. |

### Result

```json
{
  "type": "object",
  "title": "engine_devices_result",
  "properties": {
    "devices": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "audio_device",
        "properties": {
          "host_api": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "inputs": {
            "type": "integer"
          },
          "outputs": {
            "type": "integer"
          },
          "default_rate": {
            "type": "number"
          },
          "default_input": {
            "type": "boolean"
          },
          "default_output": {
            "type": "boolean"
          }
        },
        "required": [
          "host_api",
          "name",
          "inputs",
          "outputs",
          "default_rate",
          "default_input",
          "default_output"
        ],
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
  "required": ["devices"],
  "additionalProperties": false
}

```

### Errors

- `backend.unsupported`: native device enumeration is unavailable Hint: use engine.inputs with JACK

### Examples

**Without hardware** (`empty`)

```json
{
  "backend": "dummy"
}

```

Result:

```json
{
  "devices": []
}

```

## `engine.inputs@1`

The device's audio inputs, MIDI devices and master outputs.

audio: one entry per input channel (channel, the 1-based number track.set record_input takes, and the device port it is connected to). midi: the MIDI devices (names track.set record_midi matches; JACK ports or native CoreMIDI/WinMM inputs, fixed until restart). midi_outputs: destination names for the hardware output API (routing and clock generation are separate). outputs: one entry per output channel (channel, the number track.set hw_output takes for a master, and the port it is connected to). While the engine is stopped: running false and empty lists (engine.start opens the device).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Stopped** (`empty`)

```json
{}

```

Result:

```json
{
  "running": false,
  "audio": [],
  "midi": [],
  "outputs": [],
  "midi_outputs": []
}

```

## `engine.start@1`

Start playback through JACK, native PortAudio, or a device-free dummy.

backend: jack (Linux default), portaudio (macOS/Windows native device), or dummy (default elsewhere, no device). For portaudio, host_api, input_device and output_device select exact names from engine.devices. inputs defaults to 0 (playback only); outputs (the device channels masters can play to, track.set hw_output) defaults to one per physical playback port with JACK (2 to 64) and 2 otherwise. rate defaults to the project rate; a rate given is asked of the device (CoreAudio switches the device's rate when it can, WASAPI converts) or, with JACK on PipeWire, suggested to the graph (node.rate: applied only while the devices are idle and the rate is allowed). force_graph (JACK on PipeWire, default false) instead forces block as the graph's quantum, and rate if given, while the engine runs. PortAudio opens up to 16 native MIDI inputs and outputs; engine.inputs lists their names. Dummy is paced like a device. block: the device block in frames (default 256); the device's period may differ and is reported. anticipative (default true): render ahead of the playhead on worker threads (edits are heard up to 200 ms later); false renders everything in the audio callback. The engine runs at the device's rate (media is resampled; renders stay at the project's rate). Background and live thread capacities follows the app's Audio processing threads preference (Automatic by default); live helpers join each callback with no added latency. memory_lock reports the default-off host-process memory policy: requested, active, and a structured OS error on failure. cpu_idle_latency reports the 20 microsecond PM QoS request: requested by the default-off option, or automatic, held on Linux while a bridged plug-in plays live; released on engine stop. thp_opt_out: on Linux the process is kept out of transparent huge pages while the engine runs (advised_only: regions a plug-in advised keep them). A protection failure does not prevent audio startup. MIDI output refusals are in midi_output_warnings; midi_output_latency reports signed audio-minus-MIDI frames per output. MIDI inputs that refuse to open are skipped: midi_warnings reports each device, numeric os_error, message and corrective hint; audio continues.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  | One of: jack, dummy, portaudio. |
| `host_api` | string |  |  |  |  |
| `input_device` | string |  |  |  |  |
| `output_device` | string |  |  |  |  |
| `rate` | number |  | 1.0..768000.0 |  |  |
| `inputs` | integer |  | 0.0..64.0 |  |  |
| `outputs` | integer |  | 2.0..64.0 |  |  |
| `block` | integer |  | 16.0..4096.0 |  |  |
| `anticipative` | boolean |  |  |  |  |
| `force_graph` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "preparing_audio": {
      "type": "boolean"
    },
    "prepared_version": {
      "type": "integer"
    },
    "preparation_version": {
      "type": "integer"
    },
    "preparation_error": {
      "type": "string"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "bridge_late": {
      "type": "integer"
    },
    "bridge_timing": {
      "type": "object",
      "title": "engine_bridge_timing",
      "properties": {
        "calls": {
          "type": "integer"
        },
        "late": {
          "type": "integer"
        },
        "settles": {
          "type": "integer"
        },
        "past_deadline": {
          "type": "integer"
        },
        "spun": {
          "type": "integer"
        },
        "wait_max_us": {
          "type": "number"
        },
        "wait_mean_us": {
          "type": "number"
        },
        "wake_max_us": {
          "type": "number"
        },
        "wake_mean_us": {
          "type": "number"
        },
        "work_max_us": {
          "type": "number"
        },
        "return_max_us": {
          "type": "number"
        },
        "wakes_over_200us": {
          "type": "integer"
        },
        "wakes_over_1ms": {
          "type": "integer"
        },
        "bridge_cpus": {
          "type": "array",
          "items": {
            "type": "integer"
          }
        },
        "migrations": {
          "type": "integer"
        },
        "class_mismatches": {
          "type": "integer"
        },
        "longest_cycle_us": {
          "type": "number"
        },
        "overruns_late_wait": {
          "type": "integer"
        },
        "overruns_slow_bridge": {
          "type": "integer"
        },
        "overruns_not_waiting": {
          "type": "integer"
        }
      },
      "required": [
        "calls",
        "late",
        "settles",
        "past_deadline",
        "spun",
        "wait_max_us",
        "wait_mean_us",
        "wake_max_us",
        "wake_mean_us",
        "work_max_us",
        "return_max_us",
        "wakes_over_200us",
        "wakes_over_1ms",
        "bridge_cpus",
        "migrations",
        "class_mismatches",
        "longest_cycle_us",
        "overruns_late_wait",
        "overruns_slow_bridge",
        "overruns_not_waiting"
      ],
      "additionalProperties": false
    },
    "midi_dropped": {
      "type": "integer"
    },
    "midi_inputs": {},
    "midi_outputs": {},
    "midi_lead_frames": {
      "type": "integer"
    },
    "midi_out_dropped": {
      "type": "integer"
    },
    "midi_output_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_output_latency": {
      "type": "array",
      "items": {
        "type": "integer"
      }
    },
    "midi_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_devices_changed": {
      "type": "boolean"
    },
    "midi_devices_message": {
      "type": "string"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    },
    "live_helpers": {
      "type": "integer"
    },
    "scheduling": {
      "type": "object",
      "title": "engine_scheduling",
      "properties": {
        "callback": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "live": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "renderer": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "background": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        }
      },
      "required": ["callback", "live", "renderer", "background"],
      "additionalProperties": false
    },
    "workgroup": {
      "type": "object",
      "title": "engine_workgroup",
      "properties": {
        "state": {
          "type": "string"
        },
        "joined": {
          "type": "integer"
        },
        "failed": {
          "type": "integer"
        },
        "os_error": {
          "type": "integer"
        },
        "message": {
          "type": "string"
        },
        "hint": {
          "type": "string"
        }
      },
      "required": ["state", "joined", "failed", "os_error", "message", "hint"],
      "additionalProperties": false
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
    },
    "memory_lock": {
      "type": "object",
      "title": "runtime_protection_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active"],
      "additionalProperties": false
    },
    "cpu_idle_latency": {
      "type": "object",
      "title": "cpu_idle_latency_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "automatic": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active", "automatic"],
      "additionalProperties": false
    },
    "thp_opt_out": {
      "type": "object",
      "title": "thp_opt_out_status",
      "properties": {
        "active": {
          "type": "boolean"
        },
        "advised_only": {
          "type": "boolean"
        },
        "inherited": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["active", "advised_only", "inherited"],
      "additionalProperties": false
    },
    "audio_page_faults": {
      "type": "object",
      "title": "audio_page_faults",
      "properties": {
        "scope": {
          "type": "string",
          "pattern": "^(audio_threads|process|none)$"
        },
        "minor": {
          "type": "integer"
        },
        "major": {
          "type": "integer"
        },
        "threads": {
          "type": "integer"
        },
        "saturated": {
          "type": "boolean"
        }
      },
      "required": ["scope", "minor", "threads", "saturated"],
      "additionalProperties": false
    },
    "meter_drops": {
      "type": "object",
      "title": "meter_drops",
      "properties": {
        "renderer": {
          "type": "integer"
        },
        "callback": {
          "type": "integer"
        }
      },
      "required": ["renderer", "callback"],
      "additionalProperties": false
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "disabled_fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "fx whose plug-in this run has disabled (plugin.override): the strip passes audio through"
    },
    "bridge_threads_refused": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "modulation_deferred": {
      "type": "integer",
      "description": "Modulation events that waited past their grid point for room in a block (ADR-0096)"
    },
    "failed_errors": {},
    "sync": {
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
    }
  },
  "required": [
    "running",
    "preparing_audio",
    "prepared_version",
    "preparation_version",
    "preparation_error",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "bridge_late",
    "bridge_timing",
    "midi_dropped",
    "midi_inputs",
    "midi_outputs",
    "midi_lead_frames",
    "midi_out_dropped",
    "midi_output_warnings",
    "midi_output_latency",
    "midi_warnings",
    "midi_devices_changed",
    "midi_devices_message",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses",
    "live_helpers",
    "scheduling",
    "workgroup",
    "warnings",
    "memory_lock",
    "cpu_idle_latency",
    "thp_opt_out",
    "audio_page_faults",
    "meter_drops",
    "failed_fx",
    "disabled_fx",
    "bridge_threads_refused",
    "dropped",
    "voice_dropped",
    "modulation_deferred",
    "failed_errors",
    "sync"
  ],
  "additionalProperties": false
}

```

### Errors

- `engine.running`: the engine is already running Hint: engine.stop first
- `backend.device`: device or host API was not found Hint: choose names from engine.devices
- `backend.config`: device format is invalid Hint: check rate and channel capacities
- `backend.unavailable`: JACK is not installed Hint: install pipewire-jack
- `backend.open`: no JACK server answered Hint: is PipeWire running?
- `backend.unsupported`: backend is unavailable on this platform Hint: select dummy

### Examples

**Without a device** (`empty`)

```json
{
  "backend": "dummy",
  "block": 128
}

```

Result:

```json
{
  "running": true,
  "backend": "dummy",
  "sample_rate": 48000.0,
  "block": 128,
  "period": 128,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "bridge_late": 0,
  "bridge_timing": {
    "calls": 0,
    "late": 0,
    "settles": 0,
    "past_deadline": 0,
    "spun": 0,
    "wait_max_us": 0,
    "wait_mean_us": 0,
    "wake_max_us": 0,
    "wake_mean_us": 0,
    "work_max_us": 0,
    "return_max_us": 0,
    "wakes_over_200us": 0,
    "wakes_over_1ms": 0,
    "bridge_cpus": [],
    "migrations": 0,
    "class_mismatches": 0,
    "longest_cycle_us": 0,
    "overruns_late_wait": 0,
    "overruns_slow_bridge": 0,
    "overruns_not_waiting": 0
  },
  "midi_dropped": 0,
  "midi_warnings": [],
  "midi_out_dropped": 0,
  "midi_output_warnings": [],
  "midi_output_latency": [],
  "midi_inputs": [],
  "midi_outputs": [],
  "midi_lead_frames": 0,
  "midi_devices_changed": false,
  "midi_devices_message": "",
  "preparing_audio": false,
  "preparation_error": "",
  "prepared_version": "<any>",
  "preparation_version": 0,
  "cycles": "<any>",
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": true,
  "anticipation_misses": 0,
  "live_helpers": "<any>",
  "scheduling": {
    "callback": {
      "policy": "normal",
      "priority": 0
    },
    "live": "<any>",
    "renderer": {
      "policy": "normal",
      "priority": 0
    },
    "background": "<any>"
  },
  "workgroup": {
    "state": "none",
    "joined": 0,
    "failed": 0,
    "os_error": 0,
    "message": "",
    "hint": ""
  },
  "warnings": [],
  "memory_lock": {
    "requested": false,
    "active": false
  },
  "cpu_idle_latency": {
    "requested": false,
    "active": false,
    "automatic": false
  },
  "thp_opt_out": "<any>",
  "audio_page_faults": "<any>",
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "disabled_fx": [],
  "bridge_threads_refused": [],
  "dropped": 0,
  "voice_dropped": 0,
  "modulation_deferred": 0,
  "failed_errors": [],
  "sync": "<any>"
}

```

## `engine.status@1`

Engine state: backend, rate, block, device period, latency, xruns.

preparing_audio: source preparation is pending; the previous ready plan stays audible. prepared_version is the last compiled version, preparation_version the pending target (zero when idle); preparation_error explains a failed preparation. plan_latency: plugin delay plus prepared MIDI audio lead in frames; midi_lead_frames reports the latter. midi_inputs/midi_outputs give per-port enabled/timing/loss/warnings, and for outputs what each sends (clock, transport, spp, mtc), as midi.inputs.get/midi.outputs.get. device_latency: frames from the engine's outputs to the device. restart_needed: the device changed rate or went away; engine.stop and engine.start again. midi_devices_changed is advisory: MIDI topology changed but audio keeps running. midi_devices_message asks for an explicit engine restart to pick up the new set; existing port names stay fixed. midi_out_dropped counts hardware output rejections, queue/device loss and pending events discarded at shutdown. midi_output_warnings lists outputs skipped at start; midi_output_latency is signed audio-minus-MIDI latency per output, in frames. sync: external sync (sync.get has the same status with the settings). midi_dropped: MIDI input messages/packets rejected or lost to bounded queues (JACK and native; zero for the dummy backend). midi_warnings lists inputs skipped at start, with device, numeric os_error, message and corrective hint. meter_drops: entries a full ring dropped (the renderer's, the audio callback's) — a track's peak/RMS missed a block, never audio. dropped/voice_dropped count lost plug-in output/voice records. memory_lock reports requested/active and an optional OS error with errno and a corrective hint, including while stopped. Bridges are excluded. cpu_idle_latency reports the owned 20 microsecond request with the same status/error fields, and automatic: held without the option because a bridged plug-in plays live (Linux; an automatic failure is no warning). It affects system idle while held and leaves CPU frequency policy unchanged. thp_opt_out: active, advised_only, inherited (the process was already kept out when the engine started; left as it was, not undone at stop) and an optional error, Linux only (khugepaged collapsing memory would stall audio threads). audio_page_faults: page faults since the engine started; scope audio_threads counts the callback's and live helpers' (Linux), process the whole process's (macOS, Windows, where major is omitted), none while stopped; saturated: the platform's counter stopped at its limit (macOS 32-bit counts), so minor is 0 and major omitted, not counts. live_helpers is the enabled live helper capacity. scheduling: each thread class's policy (none, normal, fifo, rr, high) and priority as applied at start: the device callback, live helpers, renderer and background helpers (real-time below a real-time callback when permitted). workgroup: whether the live helpers joined the device's audio workgroup (macOS CoreAudio; state none, ready, joined, partial or failed, with the helper counts and the last refusal's OS error, message and hint). warnings includes scheduling fallbacks and plan warnings with corrective actions. failed_errors adds structured code, message, path and hint to each failure, and `incident`, the id of the local crash report the bridge's death may have caused (crash.read; the report may not exist), when it died of a signal or an exception. failed_fx: isolated plugins whose process crashed or hung (silent now; fx.restart re-creates them). bridge_late: callback cycles in which a bridged plugin had not answered by the cycle's bridge deadline (its period or 2 ms, whichever is larger); its output was silent for that cycle, and xruns counts each of these cycles too. bridge_threads_refused: isolated plugins whose bridge audio thread was refused the scheduling it asked for (it runs at what it had and can fall behind under load; grant real-time scheduling to the user and restart the engine).

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "preparing_audio": {
      "type": "boolean"
    },
    "prepared_version": {
      "type": "integer"
    },
    "preparation_version": {
      "type": "integer"
    },
    "preparation_error": {
      "type": "string"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "bridge_late": {
      "type": "integer"
    },
    "bridge_timing": {
      "type": "object",
      "title": "engine_bridge_timing",
      "properties": {
        "calls": {
          "type": "integer"
        },
        "late": {
          "type": "integer"
        },
        "settles": {
          "type": "integer"
        },
        "past_deadline": {
          "type": "integer"
        },
        "spun": {
          "type": "integer"
        },
        "wait_max_us": {
          "type": "number"
        },
        "wait_mean_us": {
          "type": "number"
        },
        "wake_max_us": {
          "type": "number"
        },
        "wake_mean_us": {
          "type": "number"
        },
        "work_max_us": {
          "type": "number"
        },
        "return_max_us": {
          "type": "number"
        },
        "wakes_over_200us": {
          "type": "integer"
        },
        "wakes_over_1ms": {
          "type": "integer"
        },
        "bridge_cpus": {
          "type": "array",
          "items": {
            "type": "integer"
          }
        },
        "migrations": {
          "type": "integer"
        },
        "class_mismatches": {
          "type": "integer"
        },
        "longest_cycle_us": {
          "type": "number"
        },
        "overruns_late_wait": {
          "type": "integer"
        },
        "overruns_slow_bridge": {
          "type": "integer"
        },
        "overruns_not_waiting": {
          "type": "integer"
        }
      },
      "required": [
        "calls",
        "late",
        "settles",
        "past_deadline",
        "spun",
        "wait_max_us",
        "wait_mean_us",
        "wake_max_us",
        "wake_mean_us",
        "work_max_us",
        "return_max_us",
        "wakes_over_200us",
        "wakes_over_1ms",
        "bridge_cpus",
        "migrations",
        "class_mismatches",
        "longest_cycle_us",
        "overruns_late_wait",
        "overruns_slow_bridge",
        "overruns_not_waiting"
      ],
      "additionalProperties": false
    },
    "midi_dropped": {
      "type": "integer"
    },
    "midi_inputs": {},
    "midi_outputs": {},
    "midi_lead_frames": {
      "type": "integer"
    },
    "midi_out_dropped": {
      "type": "integer"
    },
    "midi_output_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_output_latency": {
      "type": "array",
      "items": {
        "type": "integer"
      }
    },
    "midi_warnings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "midi_input_warning",
        "properties": {
          "device": {
            "type": "string"
          },
          "os_error": {
            "type": "integer"
          },
          "message": {
            "type": "string"
          },
          "hint": {
            "type": "string"
          }
        },
        "required": ["device", "os_error", "message", "hint"],
        "additionalProperties": false
      }
    },
    "midi_devices_changed": {
      "type": "boolean"
    },
    "midi_devices_message": {
      "type": "string"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    },
    "live_helpers": {
      "type": "integer"
    },
    "scheduling": {
      "type": "object",
      "title": "engine_scheduling",
      "properties": {
        "callback": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "live": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "renderer": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        },
        "background": {
          "type": "object",
          "title": "thread_class",
          "properties": {
            "policy": {
              "type": "string",
              "enum": ["none", "normal", "fifo", "rr", "high"]
            },
            "priority": {
              "type": "integer",
              "minimum": 0.0,
              "maximum": 99.0
            }
          },
          "required": ["policy", "priority"],
          "additionalProperties": false
        }
      },
      "required": ["callback", "live", "renderer", "background"],
      "additionalProperties": false
    },
    "workgroup": {
      "type": "object",
      "title": "engine_workgroup",
      "properties": {
        "state": {
          "type": "string"
        },
        "joined": {
          "type": "integer"
        },
        "failed": {
          "type": "integer"
        },
        "os_error": {
          "type": "integer"
        },
        "message": {
          "type": "string"
        },
        "hint": {
          "type": "string"
        }
      },
      "required": ["state", "joined", "failed", "os_error", "message", "hint"],
      "additionalProperties": false
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
    },
    "memory_lock": {
      "type": "object",
      "title": "runtime_protection_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active"],
      "additionalProperties": false
    },
    "cpu_idle_latency": {
      "type": "object",
      "title": "cpu_idle_latency_status",
      "properties": {
        "requested": {
          "type": "boolean"
        },
        "active": {
          "type": "boolean"
        },
        "automatic": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["requested", "active", "automatic"],
      "additionalProperties": false
    },
    "thp_opt_out": {
      "type": "object",
      "title": "thp_opt_out_status",
      "properties": {
        "active": {
          "type": "boolean"
        },
        "advised_only": {
          "type": "boolean"
        },
        "inherited": {
          "type": "boolean"
        },
        "error": {
          "type": "object",
          "title": "runtime_protection_error",
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
            "errno": {
              "type": "integer"
            }
          },
          "required": ["code", "message", "path", "hint", "errno"],
          "additionalProperties": false
        }
      },
      "required": ["active", "advised_only", "inherited"],
      "additionalProperties": false
    },
    "audio_page_faults": {
      "type": "object",
      "title": "audio_page_faults",
      "properties": {
        "scope": {
          "type": "string",
          "pattern": "^(audio_threads|process|none)$"
        },
        "minor": {
          "type": "integer"
        },
        "major": {
          "type": "integer"
        },
        "threads": {
          "type": "integer"
        },
        "saturated": {
          "type": "boolean"
        }
      },
      "required": ["scope", "minor", "threads", "saturated"],
      "additionalProperties": false
    },
    "meter_drops": {
      "type": "object",
      "title": "meter_drops",
      "properties": {
        "renderer": {
          "type": "integer"
        },
        "callback": {
          "type": "integer"
        }
      },
      "required": ["renderer", "callback"],
      "additionalProperties": false
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "disabled_fx": {
      "type": "array",
      "items": {
        "type": "string"
      },
      "description": "fx whose plug-in this run has disabled (plugin.override): the strip passes audio through"
    },
    "bridge_threads_refused": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "dropped": {
      "type": "integer"
    },
    "voice_dropped": {
      "type": "integer"
    },
    "modulation_deferred": {
      "type": "integer",
      "description": "Modulation events that waited past their grid point for room in a block (ADR-0096)"
    },
    "failed_errors": {},
    "sync": {
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
    }
  },
  "required": [
    "running",
    "preparing_audio",
    "prepared_version",
    "preparation_version",
    "preparation_error",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "bridge_late",
    "bridge_timing",
    "midi_dropped",
    "midi_inputs",
    "midi_outputs",
    "midi_lead_frames",
    "midi_out_dropped",
    "midi_output_warnings",
    "midi_output_latency",
    "midi_warnings",
    "midi_devices_changed",
    "midi_devices_message",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses",
    "live_helpers",
    "scheduling",
    "workgroup",
    "warnings",
    "memory_lock",
    "cpu_idle_latency",
    "thp_opt_out",
    "audio_page_faults",
    "meter_drops",
    "failed_fx",
    "disabled_fx",
    "bridge_threads_refused",
    "dropped",
    "voice_dropped",
    "modulation_deferred",
    "failed_errors",
    "sync"
  ],
  "additionalProperties": false
}

```

### Examples

**Not running** (`empty`)

```json
{}

```

Result:

```json
{
  "running": false,
  "backend": "",
  "sample_rate": 0.0,
  "block": 0,
  "period": 0,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "bridge_late": 0,
  "bridge_timing": {
    "calls": 0,
    "late": 0,
    "settles": 0,
    "past_deadline": 0,
    "spun": 0,
    "wait_max_us": 0,
    "wait_mean_us": 0,
    "wake_max_us": 0,
    "wake_mean_us": 0,
    "work_max_us": 0,
    "return_max_us": 0,
    "wakes_over_200us": 0,
    "wakes_over_1ms": 0,
    "bridge_cpus": [],
    "migrations": 0,
    "class_mismatches": 0,
    "longest_cycle_us": 0,
    "overruns_late_wait": 0,
    "overruns_slow_bridge": 0,
    "overruns_not_waiting": 0
  },
  "midi_dropped": 0,
  "midi_warnings": [],
  "midi_out_dropped": 0,
  "midi_output_warnings": [],
  "midi_output_latency": [],
  "midi_inputs": [],
  "midi_outputs": [],
  "midi_lead_frames": 0,
  "midi_devices_changed": false,
  "midi_devices_message": "",
  "preparing_audio": false,
  "preparation_error": "",
  "prepared_version": 0,
  "preparation_version": 0,
  "cycles": 0,
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": false,
  "anticipation_misses": 0,
  "live_helpers": 0,
  "scheduling": {
    "callback": {
      "policy": "none",
      "priority": 0
    },
    "live": {
      "policy": "none",
      "priority": 0
    },
    "renderer": {
      "policy": "none",
      "priority": 0
    },
    "background": {
      "policy": "none",
      "priority": 0
    }
  },
  "workgroup": {
    "state": "none",
    "joined": 0,
    "failed": 0,
    "os_error": 0,
    "message": "",
    "hint": ""
  },
  "warnings": [],
  "memory_lock": {
    "requested": false,
    "active": false
  },
  "cpu_idle_latency": {
    "requested": false,
    "active": false,
    "automatic": false
  },
  "thp_opt_out": {
    "active": false,
    "advised_only": false,
    "inherited": false
  },
  "audio_page_faults": {
    "scope": "none",
    "minor": 0,
    "major": 0,
    "threads": 0,
    "saturated": false
  },
  "meter_drops": {
    "renderer": 0,
    "callback": 0
  },
  "failed_fx": [],
  "disabled_fx": [],
  "bridge_threads_refused": [],
  "dropped": 0,
  "voice_dropped": 0,
  "modulation_deferred": 0,
  "failed_errors": [],
  "sync": "<any>"
}

```

## `engine.stop@1`

Stop real-time playback and release the audio device.

A clip launcher capture is kept, not lost: one running ends here, at what was heard. warnings names a capture launch.commit has not written; it stays committable after the engine starts again (launch.state's capture_kept).

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "engine_stop_done",
  "properties": {
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
  "additionalProperties": false
}

```

### Examples

**Stop** (`engine_running`)

```json
{}

```

Result:

```json
{}

```
