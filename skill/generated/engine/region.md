# `region` commands

Named regions in beats: add, list, set, remove.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `region.add@1`

Add a named region in beats.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `section` | boolean |  |  |  | Whether the region is a section of the song, which context.at and session.describe report [true] |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "object"
      }
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**Add** (`empty`)

```json
{
  "start": 0,
  "end": 4,
  "name": "Verse"
}

```

Result:

```json
{
  "id": "rg_1"
}

```

## `region.list@1`

List rulers in this project.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "ruler_list",
  "properties": {
    "regions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "ruler_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "timebase": {
            "type": "string",
            "enum": ["time", "beats", "beats_position"]
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "pos": {
            "type": "number"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          },
          "section": {
            "type": "boolean"
          }
        },
        "required": ["id", "timebase", "name", "color"],
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
  "required": ["regions"],
  "additionalProperties": false
}

```

### Examples

**List empty** (`empty`)

```json
{}

```

Result:

```json
{
  "regions": []
}

```

## `region.remove@1`

Remove a ruler.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Region id, e.g. "rg_1". |

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

**Remove** (`song_with_region`)

```json
{
  "id": "rg_1"
}

```

Result:

```json
{}

```

## `region.set@1`

Edit a ruler's position, name, or color.



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  | Region id, e.g. "rg_1". |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `section` | boolean |  |  |  | Whether the region is a section of the song, which context.at and session.describe report [true] |

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

**Rename** (`song_with_region`)

```json
{
  "id": "rg_1",
  "name": "Bridge"
}

```

Result:

```json
{}

```
