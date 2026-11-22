# `audio` commands

Audio items: quantize hits, detach or reset warping.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `audio.quantize@1`

Quantize detected audio hits together using stretch markers or slices.

Consumes stored source hits. Detect absent/stale hits with take.hits.detect. Aligned items share one timing map; warp mode jointly processes their channels. Slice mode requires unwarped input; strength 0 splits without moving hits.

Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |
| `grid` | number |  | 0.0009765625..1024.0 |  |  |
| `strength` | number |  | 0.0..1.0 | 1.0 |  |
| `swing` | number |  | 0.0..1.0 | 0.0 |  |
| `crossfade` | number |  | 0.0..0.05 | 0.005 |  |
| `mode` | string |  |  | "warp" | One of: warp, slice. |
| `reference` | string |  |  | "" |  |
| `hits` | string |  |  | "reference" | One of: reference, merged. |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Warp to sixteenths** (`song_with_hits`)

```json
{
  "items": ["it_1"],
  "grid": 0.25
}

```

Result:

```json
"<any>"

```

**Split without moving** (`song_with_hits`)

```json
{
  "items": ["it_1"],
  "grid": 0.25,
  "mode": "slice",
  "strength": 0
}

```

Result:

```json
"<any>"

```

## `audio.warp.detach@1`

Detach selected items from jointly processed warp bundles.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Clear warp** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{}

```

## `audio.warp.reset@1`

Remove selected items' warping while keeping detected hits.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `items` | array |  |  |  |  |

### Result

```json
{}

```

### Errors

- `audio.*`: invalid, absent or stale source hits or incompatible items Hint: detect current audio hits; align item spans and use a finer grid or lower strength
- `warp.*`: unsupported map, mode, bundle or preparation budget Hint: use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or detach
- `item.locked`: a target item or warp peer is locked Hint: unlock the affected item with item.set
- `take.unknown`: the take no longer exists Hint: choose a current take from take.list

### Examples

**Clear warp** (`song_with_audio`)

```json
{
  "items": ["it_1"]
}

```

Result:

```json
{}

```
