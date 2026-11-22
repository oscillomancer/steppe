# `audition` commands

Audition a file or item: play it, stop it, see what is playing.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `audition.get@1`

Whether an audition plays: path, length, position (seconds).



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "audition_status",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "decoding": {
      "type": "boolean"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "position": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "decoding", "path", "seconds", "position"],
  "additionalProperties": false
}

```

### Examples

**Nothing playing** (`empty`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "decoding": false,
  "path": "",
  "seconds": 0.0,
  "position": 0.0
}

```

## `audition.play@1`

Play an audio file once on outputs 1/2, outside the project.

A WAV, FLAC or MP3 file plays from its start, once, on the device's outputs 1 and 2 (mono on both; a wider file's first two channels), whether the transport is stopped or playing. It does not pass the project's tracks, masters or meters, and renders and recordings never contain it. The file's start, up to 28,800,000 frames at the higher of its rate and the device's (ten minutes at 48 kHz, 2.5 at 192 kHz), is read and resampled to the device's rate before it sounds (decoding: true meanwhile; truncated: true when the file is longer). path: absolute, or a name in the project's media folder. A second call replaces what plays; it ends at the file's end, with audition.stop, when the engine stops or another project is opened. Not an undo step, not saved.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "audition",
  "properties": {
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "sample_rate": {
      "type": "number"
    },
    "channels": {
      "type": "integer"
    },
    "truncated": {
      "type": "boolean"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["path", "seconds", "sample_rate", "channels", "truncated"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `audition.file`: the file is missing, unreadable or not WAV, FLAC or MP3 Hint: give a WAV, FLAC or MP3 file: an absolute path, or a name in the project's media folder

### Examples

**A file in the project's media folder** (`audition_file`)

```json
{
  "path": "tone.wav"
}

```

Result:

```json
{
  "path": "<any>",
  "seconds": 2.0,
  "sample_rate": 48000.0,
  "channels": 2,
  "truncated": false
}

```

## `audition.stop@1`

Stop the file audition (fading out over 5 ms); returns its state.

Stopping when nothing plays is not an error. Not an undo step.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "audition_status",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "decoding": {
      "type": "boolean"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "position": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["playing", "decoding", "path", "seconds", "position"],
  "additionalProperties": false
}

```

### Examples

**Nothing playing** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "decoding": false,
  "path": "",
  "seconds": 0.0,
  "position": 0.0
}

```
