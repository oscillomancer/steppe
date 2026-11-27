# `cable` commands

Cables inside a rack: connect outputs to inputs or knobs.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `cable.add@1`

Connect an output to an input or a knob.

from: "mo_N.<output>"; to: "mo_N.<input>" or "mo_N.<knob>" (a modulation cable: amount is the knob's movement at a signal of 1, in octaves for a frequency, the knob's unit otherwise; default an octave or half the range), or a plugin module's plug-in parameter "fx_N.<key>" (CLAP modulation, amount a fraction of its range, default 0.5; it must be modulatable). A numeric CLAP id is also accepted (fx_3.12), with no leading zeros except 0; a numeric semantic key takes precedence. Several cables into one input sum; a mono cable reaches every voice of a poly module. feedback: it closes a loop, so it carries the previous frame (a one-sample delay).

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |
| `amount` | number |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `rack.port`: no such output, input or knob Hint: rack.get detail full
- `cable.exists`: the two ends are joined already Hint: cable.set
- `cable.rack`: the ends are in different racks Hint: use In and Out modules

### Examples

**Notes' gate opens the VCA** (`song_with_rack`)

```json
{
  "from": "mo_2.gate",
  "to": "mo_3.cv"
}

```

Result:

```json
{
  "cable": "cb_2",
  "feedback": false
}

```

**Modulate a knob** (`song_with_rack`)

```json
{
  "from": "mo_2.modwheel",
  "to": "mo_3.level",
  "amount": 12
}

```

Result:

```json
{
  "cable": "cb_2",
  "feedback": false
}

```

## `cable.remove@1`

Remove a cable, by id or by its two ends.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `cable` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Unplug** (`song_with_rack`)

```json
{
  "cable": "cb_1"
}

```

Result:

```json
{}

```

**Unplug by ends** (`song_with_rack`)

```json
{
  "from": "mo_3.out",
  "to": "mo_1.l"
}

```

Result:

```json
{}

```

## `cable.set@1`

Set a modulation cable's amount or a cable's colour.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `cable` | string |  |  |  |  |
| `amount` | number |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{}

```

### Examples

**Colour it** (`song_with_rack`)

```json
{
  "cable": "cb_1",
  "color": "violet"
}

```

Result:

```json
{}

```
