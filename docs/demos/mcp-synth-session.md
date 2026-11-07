# The stock synth over MCP

This is a scripted sound-design session with Marram, the stock synth. The
same commands run directly in the sandbox and through its Unix socket proxy.
The client checks every step's structured result before recording the exchange.
Arrays longer than 12 entries show their count and names; temporary paths,
render IDs, and elapsed job times use stable placeholders. MCP text content
that exactly duplicates structuredContent is shown once.

## stdio

### request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {
      "name": "synth-demo",
      "version": "1.0"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "steppe",
      "version": "0.6.0"
    },
    "instructions": "Successful command results may include warnings: command warnings followed by bus diagnostics {code: string, message: string, path: string}. Paths are retained as emitted. Non-object results with bus warnings become {result: value, warnings: [...]}. Batch warnings stay on each step's result."
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {}
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": {
      "_elided_array_count": 32,
      "names": [
        "analyze",
        "batch",
        "compare",
        "dsp_check",
        "dsp_edit",
        "dsp_export",
        "dsp_get",
        "dsp_list",
        "dsp_preview",
        "dsp_test",
        "dsp_write",
        "masking",
        "optimize_start",
        "param_get",
        "param_set",
        "patch_get",
        "patch_merge",
        "patch_set",
        "plugin_describe",
        "plugin_list",
        "plugin_load",
        "plugin_remove",
        "preset_list",
        "preset_load",
        "preset_save",
        "profile_evaluate",
        "render",
        "session_get",
        "session_reset",
        "session_set",
        "describe",
        "exec"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "describe",
    "arguments": {
      "command": "patch.merge"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "name": "patch.merge",
      "version": 1,
      "kind": "edit",
      "exposure": "mcp-tool",
      "summary": "Merge a partial patch (RFC 7396) into an instance.",
      "description": "patch: only the values to change, e.g. {\"params\": {\"filter.cutoff\": \"800 Hz\"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).",
      "tags": [
        "patch"
      ],
      "args": {
        "type": "object",
        "title": "patch_merge_args",
        "properties": {
          "instance": {
            "type": "string",
            "pattern": "^in_[0-9]+$",
            "description": "Instance id from plugin.load, e.g. \"in_1\"."
          },
          "patch": {}
        },
        "required": [
          "instance",
          "patch"
        ],
        "additionalProperties": false
      },
      "result": {
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
              "type": "object"
            }
          }
        },
        "required": [
          "changed",
          "description"
        ],
        "additionalProperties": false
      },
      "errors": [
        {
          "code": "sandbox.not_semantic",
          "when": "the plugin has no semantic patch",
          "hint": "use param.set"
        },
        {
          "code": "params.out_of_range",
          "when": "a value is outside its range",
          "hint": "the hint gives the valid range"
        }
      ],
      "examples": [
        {
          "title": "Turn it down",
          "args": {
            "instance": "in_2",
            "patch": {
              "params": {
                "gain": "-12 dB"
              }
            }
          },
          "result": {
            "changed": {
              "params": {
                "gain": -12.0
              }
            },
            "description": "<any>"
          },
          "fixture": "synth_loaded"
        }
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "plugin_load",
    "arguments": {
      "plugin": "<synth.clap>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "instance": "in_1",
      "name": "Marram",
      "semantic": true,
      "chain": [
        "in_1"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "plugin_describe",
    "arguments": {
      "instance": "in_1",
      "detail": "summary"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "plugin": "com.suppleaudio.marram",
      "name": "Marram",
      "params": {
        "Voice": [
          "voice.mode = Poly  (poly|mono|legato)",
          "voice.polyphony = 16  (1..16)",
          "voice.glide = 0.00 ms  (0.0..5000.0 ms, pow:3)",
          "voice.bend_range = 2.0 st  (0.0..48.0 st)",
          "voice.tune = 0 ct  (-100.0..100.0 ct)",
          "voice.unison = 1  (1..8)",
          "voice.drift = 0 %  (0.0..100.0 %)",
          "voice.oversampling = 2x  (x1|x2|x4)"
        ],
        "Osc 1": [
          "osc1.type = Analog  (off|analog|wavetable|fm|noise|sub)",
          "osc1.level = -6.0 dB  (-60.0..6.0 dB)",
          "osc1.pan = 0 %  (-100.0..100.0 %)",
          "osc1.coarse = 0  (-48..48)",
          "osc1.fine = 0 ct  (-100.0..100.0 ct)",
          "osc1.wave = Saw  (saw|square|pulse|triangle|sine)",
          "osc1.fm = 0.00  (0.0..10.0)",
          "osc1.to = Filter 1  (filter1|filter2|both|direct)",
          "osc1.retrigger = Off  (on|off)"
        ],
        "Osc 2": [
          "osc2.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Osc 3": [
          "osc3.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Osc 4": [
          "osc4.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Filter 1": [
          "filter1.type = Ladder  (off|ladder|svf|comb|formant)",
          "filter1.cutoff = 2.00 kHz  (20.0..20000.0 Hz, log)",
          "filter1.resonance = 10 %  (0.0..100.0 %)",
          "filter1.slope = 24 dB/oct  (db12|db24)",
          "filter1.keytrack = 0 %  (0.0..200.0 %)",
          "filter1.env = +2.00 oct  (-8.0..8.0 oct)",
          "filter1.velocity = 0.00 oct  (0.0..4.0 oct)",
          "filter1.drive = 0.0 dB  (0.0..24.0 dB)"
        ],
        "Filter 2": [
          "filter2.type = Off  (off|ladder|svf|comb|formant)"
        ],
        "Amp": [
          "amp.attack = 2.00 ms  (0.0..30000.0 ms, pow:3)",
          "amp.decay = 600 ms  (1.0..30000.0 ms, log)",
          "amp.sustain = 80 %  (0.0..100.0 %)",
          "amp.release = 250 ms  (1.0..30000.0 ms, log)",
          "amp.velocity = 50 %  (0.0..100.0 %)",
          "amp.level = 0.0 dB  (-48.0..12.0 dB)",
          "amp.pan = 0 %  (-100.0..100.0 %)",
          "output = 0.0 dB  (-36.0..12.0 dB)"
        ],
        "Env 2": [
          "env2.attack = 1.00 ms  (0.0..30000.0 ms, pow:3)",
          "env2.decay = 400 ms  (1.0..30000.0 ms, log)",
          "env2.sustain = 20 %  (0.0..100.0 %)",
          "env2.release = 300 ms  (1.0..30000.0 ms, log)",
          "env2.pitch = 0.0 st  (-48.0..48.0 st)",
          "env2.fm = 0 %  (0.0..100.0 %)"
        ],
        "Env 3": [
          "env3.attack = 10.0 ms  (0.0..30000.0 ms, pow:3)",
          "env3.decay = 800 ms  (1.0..30000.0 ms, log)",
          "env3.sustain = 0 %  (0.0..100.0 %)",
          "env3.release = 300 ms  (1.0..30000.0 ms, log)"
        ],
        "LFO 1": [
          "lfo1.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo1.sync = Off  (on|off)",
          "lfo1.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo1.mode = Note  (note|free)",
          "lfo1.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo1.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "LFO 2": [
          "lfo2.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo2.sync = Off  (on|off)",
          "lfo2.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo2.mode = Note  (note|free)",
          "lfo2.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo2.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "LFO 3": [
          "lfo3.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo3.sync = Off  (on|off)",
          "lfo3.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo3.mode = Note  (note|free)",
          "lfo3.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo3.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "Macros": [
          "macro1 = 0 %  (0.0..100.0 %)",
          "macro2 = 0 %  (0.0..100.0 %)",
          "macro3 = 0 %  (0.0..100.0 %)",
          "macro4 = 0 %  (0.0..100.0 %)"
        ]
      },
      "patch_type": "supple.marram-patch/1.0",
      "description": "Marram: default settings",
      "docs": "Synthesizer: four oscillators (analog, wavetable, FM, noise, sub) with unison, two filters (ladder, state-variable, comb, formant), three envelopes, three LFOs and four macros, connected by modulation routes.",
      "status_available": true,
      "problems": [],
      "inactive": {
        "_elided_array_count": 82
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "tools/call",
  "params": {
    "name": "patch_set",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "schema": "supple.marram-patch/1.0",
        "name": "MCP demo pad",
        "params": {
          "filter1.type": "svf",
          "filter1.cutoff": "900 Hz",
          "filter1.env": "+0 oct",
          "amp.attack": "80 ms",
          "amp.release": "200 ms",
          "env2.attack": "100 ms",
          "env2.decay": "400 ms",
          "env2.sustain": "0 %"
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "description": "Marram: Filter 1: Type SVF, Cutoff 900 Hz, Env 0.00 oct; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "base pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-1>",
      "path": "<workdir>/renders/<render-1>.wav",
      "seconds": 2.22,
      "peak_dbfs": -16.45,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.1,
          "lra_lu": null,
          "max_momentary_lufs": -21.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.4,
          "sample_peak_dbfs": -16.4,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": 571.0,
          "rolloff_hz": 352.0,
          "flatness": 0.0,
          "flux": 0.005,
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": -46.8,
            "low": -1.4,
            "low_mid": -7.1,
            "mid": -11.0,
            "high_mid": -34.4,
            "presence": -49.4,
            "air": -57.7
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+10c",
          "stability_cents": 510.5,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.3,
            -13.2,
            -16.7,
            -19.6,
            -22.3,
            -24.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.1
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 7.9,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-22.1 LUFS, true peak -16.4 dBTP. Pitched D3+10c (148 Hz), 2nd/3rd harmonics -6.2/-10.3 dB. Centroid 571 Hz, tilt -9.5 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-1>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.1,
          "lra_lu": null,
          "max_momentary_lufs": -21.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.4,
          "sample_peak_dbfs": -16.4,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": {
            "mean": 566.0,
            "median": 571.0,
            "p10": 531.0,
            "p90": 611.0
          },
          "rolloff_hz": {
            "mean": 334.0,
            "median": 352.0,
            "p10": 281.0,
            "p90": 398.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.0
          },
          "flux": {
            "mean": 0.023,
            "median": 0.005,
            "p10": 0.0,
            "p90": 0.07
          },
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": {
              "level_db": -46.8,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.4,
              "share_pct": 72.6,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.1,
              "share_pct": 19.4,
              "width": 0.0
            },
            "mid": {
              "level_db": -11.0,
              "share_pct": 7.9,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -34.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "presence": {
              "level_db": -49.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "air": {
              "level_db": -57.7,
              "share_pct": 0.0,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+10c",
          "stability_cents": 510.5,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.3,
            -13.2,
            -16.7,
            -19.6,
            -22.3,
            -24.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.1
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 7.9,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-22.1 LUFS, true peak -16.4 dBTP. Pitched D3+10c (148 Hz), 2nd/3rd harmonics -6.2/-10.3 dB. Centroid 571 Hz, tilt -9.5 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": "1.4 kHz",
          "filter1.env": "+2 oct"
        },
        "routes": {
          "velocity>filter1.cutoff": {
            "amount": "+1 oct"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "params": {
          "filter1.cutoff": 1400.0,
          "filter1.env": 2.0
        },
        "routes": {
          "velocity>filter1.cutoff": {
            "amount": 1.0
          }
        }
      },
      "description": "Marram: Filter 1: Type SVF, Cutoff 1.40 kHz; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %; Routes: velocity>filter1.cutoff +1 oct"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "opened pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-2>",
      "path": "<workdir>/renders/<render-2>.wav",
      "seconds": 2.22,
      "peak_dbfs": -15.610000000000001,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.3,
          "lra_lu": null,
          "max_momentary_lufs": -20.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.6,
          "sample_peak_dbfs": -15.6,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": 1150.0,
          "rolloff_hz": 492.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.6,
          "bands": {
            "sub": -47.3,
            "low": -1.9,
            "low_mid": -7.2,
            "mid": -8.4,
            "high_mid": -19.1,
            "presence": -27.3,
            "air": -31.5
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 47.9
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.2,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-21.3 LUFS, true peak -15.6 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1152 Hz, tilt -4.6 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "compare",
    "arguments": {
      "reference": "<render-1>",
      "test": "<render-2>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "notable": {
        "_elided_array_count": 18
      },
      "ltas_rms_db": 11.1,
      "summary": "spectral.centroid_hz.p90 +1375.9 Hz; spectral.bands.air.level_db +26.2 dB; spectral.bands.presence.level_db +22.1 dB; spectral.centroid_hz.mean +760.9 Hz; spectral.bands.high_mid.level_db +15.3 dB. Timbre difference 11.1 dB (LTAS RMS, level removed).",
      "loss": {
        "total": 200.53297224924086,
        "terms": {
          "bands": 25.30695267912023,
          "centroid": 8.181635231661867,
          "envelope.attack": 0.0,
          "envelope.decay20": 0.0,
          "envelope.release": 0.0,
          "flatness": 9.78437558611631e-08,
          "flux": 0.00017161899295100268,
          "harmonics": 0.41621189245660134,
          "inharmonicity": 7.231940417484903e-07,
          "ltas": 13.710232422179855,
          "stereo": 0.0,
          "trajectory": 152.91776758379157
        },
        "note_off_s": {
          "reference": 2.0,
          "test": 2.0
        }
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "optimize_start",
    "arguments": {
      "instance": "in_1",
      "params": [
        "filter1.cutoff"
      ],
      "target": {
        "profile": {
          "schema": "supple.analysis-profile/1.0",
          "targets": {
            "spectral.centroid_hz.median": {
              "target": 1500,
              "tolerance": 100
            }
          }
        }
      },
      "phrase": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "evaluations": 20,
      "max_seconds": 120,
      "seed": 2
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "running",
      "params": [
        "filter1.cutoff"
      ],
      "render": "<render-3>"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "exec",
    "arguments": {
      "command": "job.wait",
      "args": {
        "job": "jb_1",
        "timeout_s": 30
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "done",
      "progress": {
        "best_loss": 0.482,
        "evaluations": 17,
        "fraction": 1.0,
        "message": "generation 4"
      },
      "result": {
        "loss": {
          "initial": 12.1426,
          "best": 0.482,
          "terms": {
            "profile.spectral.centroid_hz.median": 0.482
          }
        },
        "evaluations": 17,
        "failed": 0,
        "seconds": "<elapsed-job-seconds>",
        "stopped": "evaluations",
        "patch": {
          "params": {
            "filter1.cutoff": 1959.411
          }
        },
        "changes": [
          {
            "key": "filter1.cutoff",
            "from": 1400.0,
            "to": 1959.411,
            "unit": "Hz"
          }
        ],
        "render": "<render-3>",
        "applied": false
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": 1959.411
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "params": {
          "filter1.cutoff": 1959.411
        }
      },
      "description": "Marram: Filter 1: Type SVF, Cutoff 1.96 kHz; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %; Routes: velocity>filter1.cutoff +1 oct"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "optimized pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-4>",
      "path": "<workdir>/renders/<render-4>.wav",
      "seconds": 2.22,
      "peak_dbfs": -15.33,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.2,
          "lra_lu": null,
          "max_momentary_lufs": -20.4,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.3,
          "sample_peak_dbfs": -15.3,
          "plr_lu": 5.9
        },
        "spectral": {
          "centroid_hz": 1430.0,
          "rolloff_hz": 516.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.1,
          "bands": {
            "sub": -47.4,
            "low": -1.9,
            "low_mid": -7.3,
            "mid": -8.3,
            "high_mid": -17.8,
            "presence": -25.1,
            "air": -28.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.0,
            -9.6,
            -12.1,
            -14.1,
            -15.7,
            -17.1,
            -18.4
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.4,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-21.2 LUFS, true peak -15.3 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.0/-9.6 dB. Centroid 1431 Hz, tilt -4.1 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-4>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.2,
          "lra_lu": null,
          "max_momentary_lufs": -20.4,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.3,
          "sample_peak_dbfs": -15.3,
          "plr_lu": 5.9
        },
        "spectral": {
          "centroid_hz": {
            "mean": 1640.0,
            "median": 1430.0,
            "p10": 1260.0,
            "p90": 2410.0
          },
          "rolloff_hz": {
            "mean": 518.0,
            "median": 516.0,
            "p10": 398.0,
            "p90": 647.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.001
          },
          "flux": {
            "mean": 0.024,
            "median": 0.006,
            "p10": 0.001,
            "p90": 0.07
          },
          "tilt_db_per_oct": -4.1,
          "bands": {
            "sub": {
              "level_db": -47.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.9,
              "share_pct": 64.4,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.3,
              "share_pct": 18.7,
              "width": 0.0
            },
            "mid": {
              "level_db": -8.3,
              "share_pct": 14.8,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -17.8,
              "share_pct": 1.6,
              "width": 0.0
            },
            "presence": {
              "level_db": -25.1,
              "share_pct": 0.3,
              "width": 0.0
            },
            "air": {
              "level_db": -28.4,
              "share_pct": 0.1,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.0,
            -9.6,
            -12.1,
            -14.1,
            -15.7,
            -17.1,
            -18.4
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.4,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-21.2 LUFS, true peak -15.3 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.0/-9.6 dB. Centroid 1431 Hz, tilt -4.1 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

## proxy

### request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {
      "name": "synth-demo",
      "version": "1.0"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "steppe",
      "version": "0.6.0"
    },
    "instructions": "Successful command results may include warnings: command warnings followed by bus diagnostics {code: string, message: string, path: string}. Paths are retained as emitted. Non-object results with bus warnings become {result: value, warnings: [...]}. Batch warnings stay on each step's result."
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {}
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": {
      "_elided_array_count": 32,
      "names": [
        "analyze",
        "batch",
        "compare",
        "dsp_check",
        "dsp_edit",
        "dsp_export",
        "dsp_get",
        "dsp_list",
        "dsp_preview",
        "dsp_test",
        "dsp_write",
        "masking",
        "optimize_start",
        "param_get",
        "param_set",
        "patch_get",
        "patch_merge",
        "patch_set",
        "plugin_describe",
        "plugin_list",
        "plugin_load",
        "plugin_remove",
        "preset_list",
        "preset_load",
        "preset_save",
        "profile_evaluate",
        "render",
        "session_get",
        "session_reset",
        "session_set",
        "describe",
        "exec"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "describe",
    "arguments": {
      "command": "patch.merge"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "name": "patch.merge",
      "version": 1,
      "kind": "edit",
      "exposure": "mcp-tool",
      "summary": "Merge a partial patch (RFC 7396) into an instance.",
      "description": "patch: only the values to change, e.g. {\"params\": {\"filter.cutoff\": \"800 Hz\"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).",
      "tags": [
        "patch"
      ],
      "args": {
        "type": "object",
        "title": "patch_merge_args",
        "properties": {
          "instance": {
            "type": "string",
            "pattern": "^in_[0-9]+$",
            "description": "Instance id from plugin.load, e.g. \"in_1\"."
          },
          "patch": {}
        },
        "required": [
          "instance",
          "patch"
        ],
        "additionalProperties": false
      },
      "result": {
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
              "type": "object"
            }
          }
        },
        "required": [
          "changed",
          "description"
        ],
        "additionalProperties": false
      },
      "errors": [
        {
          "code": "sandbox.not_semantic",
          "when": "the plugin has no semantic patch",
          "hint": "use param.set"
        },
        {
          "code": "params.out_of_range",
          "when": "a value is outside its range",
          "hint": "the hint gives the valid range"
        }
      ],
      "examples": [
        {
          "title": "Turn it down",
          "args": {
            "instance": "in_2",
            "patch": {
              "params": {
                "gain": "-12 dB"
              }
            }
          },
          "result": {
            "changed": {
              "params": {
                "gain": -12.0
              }
            },
            "description": "<any>"
          },
          "fixture": "synth_loaded"
        }
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "plugin_load",
    "arguments": {
      "plugin": "<synth.clap>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "instance": "in_1",
      "name": "Marram",
      "semantic": true,
      "chain": [
        "in_1"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "plugin_describe",
    "arguments": {
      "instance": "in_1",
      "detail": "summary"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "plugin": "com.suppleaudio.marram",
      "name": "Marram",
      "params": {
        "Voice": [
          "voice.mode = Poly  (poly|mono|legato)",
          "voice.polyphony = 16  (1..16)",
          "voice.glide = 0.00 ms  (0.0..5000.0 ms, pow:3)",
          "voice.bend_range = 2.0 st  (0.0..48.0 st)",
          "voice.tune = 0 ct  (-100.0..100.0 ct)",
          "voice.unison = 1  (1..8)",
          "voice.drift = 0 %  (0.0..100.0 %)",
          "voice.oversampling = 2x  (x1|x2|x4)"
        ],
        "Osc 1": [
          "osc1.type = Analog  (off|analog|wavetable|fm|noise|sub)",
          "osc1.level = -6.0 dB  (-60.0..6.0 dB)",
          "osc1.pan = 0 %  (-100.0..100.0 %)",
          "osc1.coarse = 0  (-48..48)",
          "osc1.fine = 0 ct  (-100.0..100.0 ct)",
          "osc1.wave = Saw  (saw|square|pulse|triangle|sine)",
          "osc1.fm = 0.00  (0.0..10.0)",
          "osc1.to = Filter 1  (filter1|filter2|both|direct)",
          "osc1.retrigger = Off  (on|off)"
        ],
        "Osc 2": [
          "osc2.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Osc 3": [
          "osc3.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Osc 4": [
          "osc4.type = Off  (off|analog|wavetable|fm|noise|sub)"
        ],
        "Filter 1": [
          "filter1.type = Ladder  (off|ladder|svf|comb|formant)",
          "filter1.cutoff = 2.00 kHz  (20.0..20000.0 Hz, log)",
          "filter1.resonance = 10 %  (0.0..100.0 %)",
          "filter1.slope = 24 dB/oct  (db12|db24)",
          "filter1.keytrack = 0 %  (0.0..200.0 %)",
          "filter1.env = +2.00 oct  (-8.0..8.0 oct)",
          "filter1.velocity = 0.00 oct  (0.0..4.0 oct)",
          "filter1.drive = 0.0 dB  (0.0..24.0 dB)"
        ],
        "Filter 2": [
          "filter2.type = Off  (off|ladder|svf|comb|formant)"
        ],
        "Amp": [
          "amp.attack = 2.00 ms  (0.0..30000.0 ms, pow:3)",
          "amp.decay = 600 ms  (1.0..30000.0 ms, log)",
          "amp.sustain = 80 %  (0.0..100.0 %)",
          "amp.release = 250 ms  (1.0..30000.0 ms, log)",
          "amp.velocity = 50 %  (0.0..100.0 %)",
          "amp.level = 0.0 dB  (-48.0..12.0 dB)",
          "amp.pan = 0 %  (-100.0..100.0 %)",
          "output = 0.0 dB  (-36.0..12.0 dB)"
        ],
        "Env 2": [
          "env2.attack = 1.00 ms  (0.0..30000.0 ms, pow:3)",
          "env2.decay = 400 ms  (1.0..30000.0 ms, log)",
          "env2.sustain = 20 %  (0.0..100.0 %)",
          "env2.release = 300 ms  (1.0..30000.0 ms, log)",
          "env2.pitch = 0.0 st  (-48.0..48.0 st)",
          "env2.fm = 0 %  (0.0..100.0 %)"
        ],
        "Env 3": [
          "env3.attack = 10.0 ms  (0.0..30000.0 ms, pow:3)",
          "env3.decay = 800 ms  (1.0..30000.0 ms, log)",
          "env3.sustain = 0 %  (0.0..100.0 %)",
          "env3.release = 300 ms  (1.0..30000.0 ms, log)"
        ],
        "LFO 1": [
          "lfo1.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo1.sync = Off  (on|off)",
          "lfo1.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo1.mode = Note  (note|free)",
          "lfo1.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo1.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "LFO 2": [
          "lfo2.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo2.sync = Off  (on|off)",
          "lfo2.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo2.mode = Note  (note|free)",
          "lfo2.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo2.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "LFO 3": [
          "lfo3.shape = Sine  (sine|triangle|saw_up|saw_down|square|random|drift)",
          "lfo3.sync = Off  (on|off)",
          "lfo3.rate = 2.0 Hz  (0.01..50.0 Hz, log)",
          "lfo3.mode = Note  (note|free)",
          "lfo3.phase = 0.0 deg  (0.0..360.0 deg)",
          "lfo3.delay = 0.00 ms  (0.0..10000.0 ms, pow:3)"
        ],
        "Macros": [
          "macro1 = 0 %  (0.0..100.0 %)",
          "macro2 = 0 %  (0.0..100.0 %)",
          "macro3 = 0 %  (0.0..100.0 %)",
          "macro4 = 0 %  (0.0..100.0 %)"
        ]
      },
      "patch_type": "supple.marram-patch/1.0",
      "description": "Marram: default settings",
      "docs": "Synthesizer: four oscillators (analog, wavetable, FM, noise, sub) with unison, two filters (ladder, state-variable, comb, formant), three envelopes, three LFOs and four macros, connected by modulation routes.",
      "status_available": true,
      "problems": [],
      "inactive": {
        "_elided_array_count": 82
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "tools/call",
  "params": {
    "name": "patch_set",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "schema": "supple.marram-patch/1.0",
        "name": "MCP demo pad",
        "params": {
          "filter1.type": "svf",
          "filter1.cutoff": "900 Hz",
          "filter1.env": "+0 oct",
          "amp.attack": "80 ms",
          "amp.release": "200 ms",
          "env2.attack": "100 ms",
          "env2.decay": "400 ms",
          "env2.sustain": "0 %"
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "description": "Marram: Filter 1: Type SVF, Cutoff 900 Hz, Env 0.00 oct; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "base pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-1>",
      "path": "<workdir>/renders/<render-1>.wav",
      "seconds": 2.22,
      "peak_dbfs": -16.45,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.1,
          "lra_lu": null,
          "max_momentary_lufs": -21.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.4,
          "sample_peak_dbfs": -16.4,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": 571.0,
          "rolloff_hz": 352.0,
          "flatness": 0.0,
          "flux": 0.005,
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": -46.8,
            "low": -1.4,
            "low_mid": -7.1,
            "mid": -11.0,
            "high_mid": -34.4,
            "presence": -49.4,
            "air": -57.7
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+10c",
          "stability_cents": 510.5,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.3,
            -13.2,
            -16.7,
            -19.6,
            -22.3,
            -24.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.1
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 7.9,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-22.1 LUFS, true peak -16.4 dBTP. Pitched D3+10c (148 Hz), 2nd/3rd harmonics -6.2/-10.3 dB. Centroid 571 Hz, tilt -9.5 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-1>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.1,
          "lra_lu": null,
          "max_momentary_lufs": -21.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.4,
          "sample_peak_dbfs": -16.4,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": {
            "mean": 566.0,
            "median": 571.0,
            "p10": 531.0,
            "p90": 611.0
          },
          "rolloff_hz": {
            "mean": 334.0,
            "median": 352.0,
            "p10": 281.0,
            "p90": 398.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.0
          },
          "flux": {
            "mean": 0.023,
            "median": 0.005,
            "p10": 0.0,
            "p90": 0.07
          },
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": {
              "level_db": -46.8,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.4,
              "share_pct": 72.6,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.1,
              "share_pct": 19.4,
              "width": 0.0
            },
            "mid": {
              "level_db": -11.0,
              "share_pct": 7.9,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -34.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "presence": {
              "level_db": -49.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "air": {
              "level_db": -57.7,
              "share_pct": 0.0,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+10c",
          "stability_cents": 510.5,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.3,
            -13.2,
            -16.7,
            -19.6,
            -22.3,
            -24.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.1
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 7.9,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-22.1 LUFS, true peak -16.4 dBTP. Pitched D3+10c (148 Hz), 2nd/3rd harmonics -6.2/-10.3 dB. Centroid 571 Hz, tilt -9.5 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": "1.4 kHz",
          "filter1.env": "+2 oct"
        },
        "routes": {
          "velocity>filter1.cutoff": {
            "amount": "+1 oct"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "params": {
          "filter1.cutoff": 1400.0,
          "filter1.env": 2.0
        },
        "routes": {
          "velocity>filter1.cutoff": {
            "amount": 1.0
          }
        }
      },
      "description": "Marram: Filter 1: Type SVF, Cutoff 1.40 kHz; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %; Routes: velocity>filter1.cutoff +1 oct"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "opened pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-2>",
      "path": "<workdir>/renders/<render-2>.wav",
      "seconds": 2.22,
      "peak_dbfs": -15.610000000000001,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.3,
          "lra_lu": null,
          "max_momentary_lufs": -20.5,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.6,
          "sample_peak_dbfs": -15.6,
          "plr_lu": 5.7
        },
        "spectral": {
          "centroid_hz": 1150.0,
          "rolloff_hz": 492.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.6,
          "bands": {
            "sub": -47.3,
            "low": -1.9,
            "low_mid": -7.2,
            "mid": -8.4,
            "high_mid": -19.1,
            "presence": -27.3,
            "air": -31.5
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 47.9
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.2,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-21.3 LUFS, true peak -15.6 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1152 Hz, tilt -4.6 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "compare",
    "arguments": {
      "reference": "<render-1>",
      "test": "<render-2>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "notable": {
        "_elided_array_count": 18
      },
      "ltas_rms_db": 11.1,
      "summary": "spectral.centroid_hz.p90 +1375.9 Hz; spectral.bands.air.level_db +26.2 dB; spectral.bands.presence.level_db +22.1 dB; spectral.centroid_hz.mean +760.9 Hz; spectral.bands.high_mid.level_db +15.3 dB. Timbre difference 11.1 dB (LTAS RMS, level removed).",
      "loss": {
        "total": 200.53297224924086,
        "terms": {
          "bands": 25.30695267912023,
          "centroid": 8.181635231661867,
          "envelope.attack": 0.0,
          "envelope.decay20": 0.0,
          "envelope.release": 0.0,
          "flatness": 9.78437558611631e-08,
          "flux": 0.00017161899295100268,
          "harmonics": 0.41621189245660134,
          "inharmonicity": 7.231940417484903e-07,
          "ltas": 13.710232422179855,
          "stereo": 0.0,
          "trajectory": 152.91776758379157
        },
        "note_off_s": {
          "reference": 2.0,
          "test": 2.0
        }
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "optimize_start",
    "arguments": {
      "instance": "in_1",
      "params": [
        "filter1.cutoff"
      ],
      "target": {
        "profile": {
          "schema": "supple.analysis-profile/1.0",
          "targets": {
            "spectral.centroid_hz.median": {
              "target": 1500,
              "tolerance": 100
            }
          }
        }
      },
      "phrase": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "evaluations": 20,
      "max_seconds": 120,
      "seed": 2
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "running",
      "params": [
        "filter1.cutoff"
      ],
      "render": "<render-3>"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "exec",
    "arguments": {
      "command": "job.wait",
      "args": {
        "job": "jb_1",
        "timeout_s": 30
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "done",
      "progress": {
        "best_loss": 0.482,
        "evaluations": 17,
        "fraction": 1.0,
        "message": "generation 4"
      },
      "result": {
        "loss": {
          "initial": 12.1426,
          "best": 0.482,
          "terms": {
            "profile.spectral.centroid_hz.median": 0.482
          }
        },
        "evaluations": 17,
        "failed": 0,
        "seconds": "<elapsed-job-seconds>",
        "stopped": "evaluations",
        "patch": {
          "params": {
            "filter1.cutoff": 1959.411
          }
        },
        "changes": [
          {
            "key": "filter1.cutoff",
            "from": 1400.0,
            "to": 1959.411,
            "unit": "Hz"
          }
        ],
        "render": "<render-3>",
        "applied": false
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": 1959.411
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "params": {
          "filter1.cutoff": 1959.411
        }
      },
      "description": "Marram: Filter 1: Type SVF, Cutoff 1.96 kHz; Amp: Attack 80.0 ms, Release 200 ms; Env 2: Attack 100 ms, Sustain 0 %; Routes: velocity>filter1.cutoff +1 oct"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "optimized pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-4>",
      "path": "<workdir>/renders/<render-4>.wav",
      "seconds": 2.22,
      "peak_dbfs": -15.33,
      "tail_s": 0.22,
      "tail_capped": false,
      "latency_samples": 1,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.2,
          "lra_lu": null,
          "max_momentary_lufs": -20.4,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.3,
          "sample_peak_dbfs": -15.3,
          "plr_lu": 5.9
        },
        "spectral": {
          "centroid_hz": 1430.0,
          "rolloff_hz": 516.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.1,
          "bands": {
            "sub": -47.4,
            "low": -1.9,
            "low_mid": -7.3,
            "mid": -8.3,
            "high_mid": -17.8,
            "presence": -25.1,
            "air": -28.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.0,
            -9.6,
            -12.1,
            -14.1,
            -15.7,
            -17.1,
            -18.4
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.4,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-21.2 LUFS, true peak -15.3 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.0/-9.6 dB. Centroid 1431 Hz, tilt -4.1 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-4>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.22,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -21.2,
          "lra_lu": null,
          "max_momentary_lufs": -20.4,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.3,
          "sample_peak_dbfs": -15.3,
          "plr_lu": 5.9
        },
        "spectral": {
          "centroid_hz": {
            "mean": 1640.0,
            "median": 1430.0,
            "p10": 1260.0,
            "p90": 2410.0
          },
          "rolloff_hz": {
            "mean": 518.0,
            "median": 516.0,
            "p10": 398.0,
            "p90": 647.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.001
          },
          "flux": {
            "mean": 0.024,
            "median": 0.006,
            "p10": 0.001,
            "p90": 0.07
          },
          "tilt_db_per_oct": -4.1,
          "bands": {
            "sub": {
              "level_db": -47.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.9,
              "share_pct": 64.4,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.3,
              "share_pct": 18.7,
              "width": 0.0
            },
            "mid": {
              "level_db": -8.3,
              "share_pct": 14.8,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -17.8,
              "share_pct": 1.6,
              "width": 0.0
            },
            "presence": {
              "level_db": -25.1,
              "share_pct": 0.3,
              "width": 0.0
            },
            "air": {
              "level_db": -28.4,
              "share_pct": 0.1,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 148.0,
          "note": "D3+12c",
          "stability_cents": 510.4,
          "harmonics_db": [
            0.0,
            -6.0,
            -9.6,
            -12.1,
            -14.1,
            -15.7,
            -17.1,
            -18.4
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 69.0,
          "decay20_ms": 1010.0,
          "release_ms": 198.0,
          "sustain_db": -3.5,
          "crest_db": 8.4,
          "onsets_per_s": 0.5
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-21.2 LUFS, true peak -15.3 dBTP. Pitched D3+12c (148 Hz), 2nd/3rd harmonics -6.0/-9.6 dB. Centroid 1431 Hz, tilt -4.1 dB/oct, most energy in low. Attack 69 ms, -20 dB after 1014 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

