# Spec: CLAP extension `com.suppleaudio.semantic-params/1`

**Status:** Implemented. The extension ID
carries the major version; incompatible changes get `/2`. Additions within
`/1` only append members after a version check (see §4).
**Header:** `plugins/common/include/supple/clap/semantic_params.h` (C ABI,
usable from C and C++, depends only on the CLAP headers).
**Implementations:** plugin side in `plugins/common` (C++ helper over
core-params/core-patch); host side in `host/clap-host`.

## 1. Purpose

Standard CLAP gives hosts anonymous parameters (a name, a range, value
text). This extension lets a host that understands it — the Steppe sandbox
and engine — see a stock plugin the way its designers do: parameters with
keys, physical units, curves and roles; a structured, versioned JSON patch
that can be read, replaced, merged and validated with actionable errors; and
a prose description of the current sound. Hosts that do not know the
extension use standard `clap.params` and `clap.state`, and lose nothing but
the semantics.

## 2. Plugin interface

```c
#include <clap/clap.h>

static const char CLAP_EXT_SUPPLE_SEMANTIC_PARAMS[] = "com.suppleaudio.semantic-params/1";

enum {
    CLAP_SUPPLE_PATCH_DETAIL_FULL = 0,      /* every non-readonly value */
    CLAP_SUPPLE_PATCH_DETAIL_CHANGED = 1,   /* only values differing from defaults */
};
enum {
    CLAP_SUPPLE_SET_REPLACE = 0,            /* the document is a complete patch */
    CLAP_SUPPLE_SET_MERGE = 1,              /* RFC 7396 merge patch into the current patch */
    CLAP_SUPPLE_SET_VALIDATE_ONLY = 2,      /* validate as MERGE would, change nothing */
};

typedef struct clap_plugin_supple_semantic_params {
    /* Size of this struct, for forward compatibility (see §4). */
    uint32_t struct_size;

    /* [main-thread] Writes the semantic schema document (JSON, §3.1) to `out`.
     * Returns false on stream failure. The schema may change only after the
     * plugin calls host->changed(CLAP_SUPPLE_CHANGED_SCHEMA). */
    bool(CLAP_ABI *get_schema)(const clap_plugin_t *plugin, const clap_ostream_t *out);

    /* [main-thread] Writes the current patch as canonical JSON (core-patch). */
    bool(CLAP_ABI *get_patch)(const clap_plugin_t *plugin, uint32_t detail, const clap_ostream_t *out);

    /* [main-thread] Applies `json` (UTF-8, `size` bytes, not NUL-terminated)
     * with the given mode. Transactional: either the whole document applies or
     * nothing changes. Returns true on success. On failure returns false and,
     * if `errors` is non-NULL, writes a JSON array of error objects (§3.3).
     * May be called while the plugin is active and processing (§5). */
    bool(CLAP_ABI *set_patch)(const clap_plugin_t *plugin, uint32_t mode,
                              const char *json, uint32_t size, const clap_ostream_t *errors);

    /* [main-thread] Writes a compact prose description of the current patch
     * (UTF-8 plain text, ≤ 600 characters for DETAIL_CHANGED). */
    bool(CLAP_ABI *describe)(const clap_plugin_t *plugin, uint32_t detail, const clap_ostream_t *out);
    /* [main-thread] Optional read-only supple.factory-patches/1.0 document.
     * Check struct_size through this member and a non-null pointer first. */
    bool(CLAP_ABI *get_factory_patches)(const clap_plugin_t *plugin, const clap_ostream_t *out);
} clap_plugin_supple_semantic_params_t;
```

## 3. Documents

### 3.1 Schema document

```json
{
  "schema": "supple.semantic-schema/1.0",
  "plugin": { "id": "com.suppleaudio.yarrow", "name": "…", "version": "1.0.0" },
  "patch_type": "supple.yarrow-patch/1.0",
  "params": { …core-params describeTable() "params" member… },
  "patch": { …JSON Schema (2020-12) of the patch document… },
  "docs": "One paragraph: what the plugin is and how its patch is organised."
}
```

`params` is exactly the `params` member of `core-params::describeTable`;
`patch` is `PatchType::jsonSchema()`. Keys, ids and units are the same ones
the plugin exposes through `clap.params` (the CLAP values follow ADR-0010).
Curve exponents must be finite and positive and round-trip exactly: use
shortest round-trip text, retaining the trimmed fixed-three-decimal spelling
when it already round-trips. Invalid exponent text is rejected by
`core-params::fromDescription` with `params.description` and a corrective hint.

### 3.2 Patch documents

As specified in `docs/specs/core-patch.md`. `get_patch` output is canonical.
For flat plugins, the patch's `params` object uses the same keys as the
schema's `params`.

### 3.3 Errors

A JSON array of `supple::Error` objects:
`[{"code": "...", "message": "...", "path": "/params/band2.gain", "hint": "...", "details": [...]}]`
(`loc` is omitted; `path` is a JSON Pointer into the submitted document).
A `set_patch` failure caused by a stream write error returns false with an
empty array.

### 3.4 Factory patches

`get_factory_patches` writes canonical JSON:
`{"schema":"supple.factory-patches/1.0","patches":[<patch document>, ...]}`.
Every entry is a complete patch in the plugin's own schema, including a
unique `name` of 1..64 UTF-8 bytes. Values use the ordinary patch domains
(physical units, enum keys and booleans); omitted parameters take defaults.
Declaration order is preserved. The call is non-destructive, changes no
state and returns false on stream/report failure. An empty array means no
factory patches. The documents are read-only: applying one does not edit
the factory table; saving a user copy remains a separate operation.

The suite derives this view and the framework's `FactoryPreset` table
from one script declaration. The framework table uses its own plain
parameter domain (percent as a fraction); the extension converts back
through the same patch codec as `get_patch`.

## 4. Versioning within `/1`

Hosts read `struct_size` and must not call members beyond it. Plugins set it
to `sizeof(clap_plugin_supple_semantic_params_t)` of the header they were
built with. New members are only ever appended.

The original members (through `describe`) are usable when `struct_size >=
offsetof(type, get_factory_patches)`; a shorter struct is not used.

`get_factory_patches` was appended on 2026-10-13. Calling it requires
`struct_size >= offsetof(type, get_factory_patches) + sizeof(extension->get_factory_patches)`
and a non-null pointer. A shorter struct or null callback offers no factory
patches; its earlier methods remain usable: hosts must not require the full
struct size just to call them.

(2026-10-20) A `get_telemetry` member, appended for the analyzer Gauge on
2026-09-29 between `describe` and `get_factory_patches`, was removed before
any release, once Lichen replaced Gauge: nothing called it and no other
plugin set it. Its removal is the one exception to append-only; a plugin
built with it reads to a host as offering no factory patches until rebuilt.
Measurements reach hosts and agents as read-only parameters (`fx.meters`).

## 5. Threading, real-time safety, parameter sync

- Every function is main-thread only.
- `set_patch` on an active plugin must not block or allocate on the audio
  thread. The plugin validates and builds the new state on the main thread,
  then hands it to the audio thread with a wait-free mechanism (atomic
  values plus a generation counter, or an atomic pointer swap with deferred
  reclamation on the main thread). The change takes effect at the start of a
  process block; the parameter smoothers treat it like host automation
  (smoothed), except that a `set_patch` issued while the plugin is not
  processing snaps values (like state load).
- After a successful `set_patch` that changes parameter values, the plugin
  calls `clap_host_params.rescan(host, CLAP_PARAM_RESCAN_VALUES)` so generic
  views update, and calls the host extension's `changed(PATCH)` (§6). It does
  not emit param-value output events for these changes (they are state
  changes, not gestures).
- `get_patch` reflects every change applied so far, including host
  automation and GUI edits that have reached the main thread.
- The CLAP state (`clap.state`) is the canonical patch JSON framed as in
  core-patch §4; `state.load` applies it with REPLACE semantics.

## 6. Host interface (optional for plugins to use)

```c
enum {
    CLAP_SUPPLE_CHANGED_PATCH = 1u << 0,   /* patch values changed (GUI edit, preset load in plugin UI) */
    CLAP_SUPPLE_CHANGED_SCHEMA = 1u << 1,  /* schema changed (rare: e.g. module slot types changed availability) */
};
typedef struct clap_host_supple_semantic_params {
    uint32_t struct_size;
    /* [main-thread] Notifies the host that it should re-read patch and/or schema. */
    void(CLAP_ABI *changed)(const clap_host_t *host, uint32_t flags);
} clap_host_supple_semantic_params_t;
```

Plugins call `changed(PATCH)` when their own GUI changes structure or loads
a preset (in addition to the standard `clap_host_state.mark_dirty`), so the
host can capture an undo step (ADR-0008).

Steppe validates semantic schemas on the main thread before accepting them.
Invalid required entry fields or an ID set that differs from `clap.params`
disable the semantic view for that instance, log a warning, and fall back to
plain CLAP parameters. The sandbox returns `sandbox.plugin_contract` with a value path
and a restart/replace hint when a parameter query has no current value in
either the patch or CLAP, rather than returning an incomplete entry.

## 7. Conformance tests (both sides)

A test host in `host/clap-host/tests` loads each stock plugin and checks:
schema parses and its `params` match `clap.params` (ids, names, CLAP ranges
derived per ADR-0010); `get_patch` → `set_patch(REPLACE)` round trip is
identity; `CHANGED` detail re-applies to the same state; merge of a single
value changes only that value (compared through `get_patch`); invalid
documents fail with errors whose paths point at the problem and leave
`get_patch` unchanged; `VALIDATE_ONLY` never changes state; `set_patch`
during processing (another thread calling process with rtsan active) is
RT-clean and takes effect within one block; state save/load equals patch
round trip; `describe` is non-empty and ≤ 600 chars.

## Implementation notes (2026-09-26)

- `plugins/common` `ParamStore` delivers whole patches to the audio thread
  with a seqlock: `assign()` (set_patch, state load, reset) is one write;
  the audio thread calls `readAll(values)` when `generation()` moved and keeps
  its previous values if a write is in progress (wait-free; retried next
  block). Single CLAP param events still update one value at a time.
- Legacy state: `FlatSemantic::legacyLoad` is only offered streams that do not
  claim the framed form (`patch::isFramedState`); a malformed v2 frame is
  rejected, never reinterpreted. (2026-10-20: `FlatSemantic`, the flat-patch
  implementation in `plugins/common`, was removed with its last user, the
  probe synth. A plugin implements the extension through `SemanticAdapter`
  below, or as a DSP script through the template framework's semantic
  layer.)
- The conformance tool's step 7 runs 200 merges against a thread that
  processes blocks continuously and checks for process errors and that the
  last patch is in effect; RT cleanliness is checked when the plugin and tool
  run under the rtsan preset.

## Amendments (2026-09-26, structured patches)

- **Flat `params` in merge patches.** A plugin whose patch is structured
  (slots, routes — e.g. Orrery) also accepts a top-level `"params"` member in
  `CLAP_SUPPLE_SET_MERGE` / `VALIDATE_ONLY` documents: `{"params": {"<key>":
  value}}` with keys as in the schema's `params`. The plugin translates each
  key to its place in the nested document. Keys of slots the patch does not
  have are errors (`/params/<key>`). Hosts can therefore set values by key on
  any semantic plugin (the sandbox's `param.set`, the optimizer).
- **`"active": false`** on a schema `params` entry marks a parameter the
  current structure does not use (its slot is absent, or its slot's type has
  no such field). Plugins whose schema changes this way call
  `changed(PATCH | SCHEMA)` after a structural `set_patch`.
- Structured plugins implement the extension through
  `plugins/common`'s `SemanticModel` / `SemanticAdapter` (same wire formats as
  `FlatSemantic`); `semantic-conformance` exercises them with whole-document
  replaces during processing. (2026-10-21: `plugins/common`'s plugin support,
  `ParamStore`, `ClapParams` and `SemanticModel` / `SemanticAdapter`, was
  removed with its last user, Orrery. Every stock plugin implements the
  extension as a DSP script through the framework's semantic layer, whose
  patches are flat `params` plus `routes`.)

## Amendments (2026-10-13, the patch as a state chunk)

ADR-0032 §7 allows a second state layout; §5's last bullet now reads:
the CLAP state is either the canonical patch JSON framed as in core-patch
§4 (the stock plugins), or a plugin framework's own state followed by the
patch as a chunk.

- **The chunk.** Plugins of the template with the suite's layer (the
  script player, its plugin-file exports and source exports;
  `plugins/suite/semantic/include/supple/suite/semantic_layer.h`) write the
  framework's state, then a chunk tagged `PTCH` holding the canonical patch
  framed as in core-patch §4.
- **Loading** reads the framework's values, then applies the chunk's
  values by key with REPLACE semantics, so a session survives a parameter's
  CLAP id changing and keeps the patch's name, author, tags and notes. A key
  the plugin no longer has is skipped there, not refused: a session must
  load. A state without the chunk loads by id alone.
- **Hosts** read and set a plugin's patch with `get_patch` and `set_patch`,
  never by parsing state bytes, so they need not know which layout a plugin
  uses. §7's check that a state save and load equals the patch round trip
  applies to both layouts.

## Amendments (2026-10-13, properties and files)

- **Properties.** A plugin of the template with the suite's layer may have
  properties (the framework's `core/Property.h`): state that is not a
  parameter, such as a file's path. Its patch has a top-level
  `"properties"` object of text by key beside `"params"`, and the schema's
  `patch` lists each as a string with its `maxLength` and, when it has
  one, a `description`. A DSP script's file member (`AudioFile`,
  `Convolver`) is a file property keyed by the member's name in
  snake_case, or by its `withKey()`.
- **Files.** A file property holds a path, and its description says so
  (with the file types it takes). A path a `set_patch` document changes,
  `VALIDATE_ONLY` included, must be absolute and name an existing file of
  one of those types that the plugin can read (the script player opens it
  with its decoder); otherwise the call fails with `properties.file` at
  `/properties/<key>`, saying why, and nothing changes. `""` means none.
  A file path containing NUL is invalid, including a path already held.
  Any other path the plugin already holds passes as it is, found or not, so a patch
  read and set again still applies; loading state never refuses a missing
  file (the framework relocates a moved one, or reports it missing).
  `set_patch` returns once the file is loaded, so the next block, offline
  renders included, plays it.

- **Factory files** (2026-10-29). A file property may also hold
  `"factory:<name>"`, one of the plugin's own audio files inside its binary
  (the template's `PRODUCT_ASSETS`, framework 1.32.0). The schema's
  description of the property lists those the plugin has; a `set_patch`
  path of that form must be one of them, read by the plugin's decoder, and
  fails with `properties.file` otherwise. It needs no file system and is
  saved as it is, so a session refers to it the same way on every machine.
  A script declares a default (`.withDefault()`) or a factory patch's file
  (`Patches::file()`) as one. The script player has no factory files: there
  a factory default is reported missing and loads nothing, and a
  `factory:` path is refused. A factory
  file's name is a permanent identifier once shipped.

## Amendments (2026-10-21, parameters that apply only sometimes)

- **`active_when`** on a schema `params` entry says when the parameter
  applies: an array of conditions that must all hold,
  `[{"param": "band1.enabled", "in": [true]}, {"param": "band1.type", "in":
  ["bell", "low_shelf"]}]`. Each names a parameter that comes before the
  entry in `params`: a toggle or a choice with the values (patch values:
  booleans, choice keys) that satisfy it, or a number (`float` or `int`)
  with `above` and/or `below`, in its unit, that its value lies strictly
  between (`{"param": "voices", "above": 1}`). The parameter it names must
  apply too. An entry without `active_when` applies whenever its module is
  present.
- **`"active": false`** now also marks a flat plugin's parameter whose
  conditions do not hold with the current values, so `get_schema` follows
  the values. Hosts read the schema when they need activity; a plugin calls
  `changed(PATCH | SCHEMA)` after a `set_patch` that changes it, and does
  not report changes that arrive through `clap.params` (automation, the
  editor).
- An inactive parameter keeps its value, saves it in the patch and state,
  and accepts writes; it only does nothing until its conditions hold.
  `describe` leaves it out. The DSP scripts' `.activeWhen()` and
  `.activeUnless()`, `.activeAbove()` and `.activeBelow()` declare these
  conditions (`skill/guides/dsp-language.md`); the suite's
  `SemanticDefinition` carries them as `ParamMeaning::activeWhen` and answers
  the framework's `Definition::applies()` (1.28.0) from them, so the
  automatic editor dims what does not apply, as a script's own editor does.
- Hosts report them back: the sandbox's `plugin.describe` lists the keys
  under `inactive` and leaves them out of its summary, its `param.set`
  marks such a value `"active": false`, and `patch.merge`, the engine's
  `fx.patch.merge` and `fx.param.set` name the `params` they set that do not
  apply under `inactive`.

## Amendments (2026-10-21, modulation routes)

- **`routes`**, a patch member beside `params`, for a plugin whose meaning
  has sources: an object keyed `"<source>><parameter>"`, each value
  `{"amount": ..., "via": "<source>"}` (`via` optional; a bare number or
  string stands for `{"amount": ...}` on input). The amount is in the
  parameter's physical unit at the source's full value, except a
  frequency's, in octaves; text may carry a unit that converts (`"12 st"`,
  `"+1.5 oct"`, `"-6 dB"`). `via` scales the amount by a second source. A
  parameter's routes sum, and the result is kept to its route range (its
  own range unless the plugin gives a wider one). At most 64 routes; a
  route's amount at most the width of its parameter's route range.
  Canonical form: sorted by source, then parameter, amounts as numbers.
  Merge patches add, change (field by field) and remove (`null`) routes one
  by one.
- **Errors**: `routes.key` (not `source>parameter`), `routes.source` and
  `routes.via` (no such source, with the closest and the list),
  `routes.destination` (no such parameter, or one routes do not move, with
  the list of those they do), `routes.unit` (the hint names the amount's
  unit), `routes.amount` (missing, or more than the parameter can move),
  `routes.voice` (a source with a value per voice, directly or through
  `via`, routed to a parameter all voices share), `routes.count`,
  `routes.type`, `routes.unknown_member`.
- **The schema** gains `"sources"`: by key, `name`, `per_voice`, `range`
  (`[-1, 1]` or `[0, 1]`) and `description`. A parameter routes move has
  `"route": {"unit": "oct", "per_voice": true}` (and `min`/`max` when its
  route range is wider than its own). The patch's JSON Schema has `routes`.
- `describe` ends with `Routes: lfo1>filter1.cutoff +1.5 oct via modwheel,
  ...`.
- The routes live in a text property of the plugin's (a DSP script's:
  `routes`, one route a line), so state and presets carry them; a patch
  never shows that property among its `properties`, nor the automatic
  editor as a text field (framework 1.29.0's `.hidden()`). Restoring a saved patch
  keeps the routes whose source and parameter still exist.
- The editor shows them as a list, `RouteList` (`supple/suite/route_list.h`):
  a panel of the automatic editor (framework 1.30.0's `AutoPanel`) for a
  script with routes and no editor of its own, and `e.routes(x, y, w, h)` in
  a script's own. A row a route, with menus for its source, parameter and
  via source (what a patch would refuse is shown but cannot be chosen) and
  its amount as a bar, in the parameter's unit. Every change is written
  through `PatchCodec::routeText`, the patch member's own checks and
  canonical text, so the list makes only what a patch can and the host
  records it as one property edit.
