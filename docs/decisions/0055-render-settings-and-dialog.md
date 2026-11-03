# ADR-0055: Render settings, presets and the render dialog

**Status:** Accepted, 2026-11-03. Contracts: session-model.md §1.10, §5 and
§7.3, engine.md §3, gui.md §4.8 and §4.11. Extends ADR-0033's dependency rule
with a named LGPL exception (LAME).

## Context

The render dialog was a column of text fields, one per schema field: format,
bit depth, normalize mode, stem mode and tap were all typed, with no list of
the accepted values. Its grey placeholders did not always show what the
engine would do: Limit showed `-1` but an empty field meant no limiting, and a
limit was dropped unless Normalize was set. The Target placeholder read a key
the engine never reads, and Bit depth showed 32 for FLAC, which renders at 24.
Nothing the dialog held was saved: the project's render settings
(`project.render`) could only be changed by an agent through `project.set`. So
File → Mix, which runs `render.mix` with no arguments, ignored the dialog
completely. The render folder was the one setting that persisted, as an
app-wide preference.

Two separate implementations existed: `render.mix` in `commands.cpp` and
stems/regions/dry-run in `render_commands.cpp`, with different tail limits
(60 s and 600 s).

## Decision

1. **One set of render settings per project, in `project.render`.** They are
   project data: saved in the file, versioned like any other edit (so
   undoable), belonging to the branch, and read by every `render.*` command,
   whether it comes from the dialog, File → Mix or an agent. The dialog writes
   them only when the user presses Render or Add to queue, and only if they
   differ from the stored settings. A change is one history step,
   `project.set` labelled "Render settings". Arguments given to a `render.*`
   command override the settings for that call only and are never written
   back. Agents change the stored settings through `project.set` or
   `render.preset.load`.

2. **Values resolve per top-level field.** In priority order: command
   arguments, then `project.render`, then the user's default, then the
   built-in defaults. Object-valued fields (`outputs`, `normalize`, `trim`,
   `metadata`, `stems`) are replaced whole, never merged member by member.
   `render.settings` (a query) returns the resolved settings and the layer
   each field came from. The dialog and agents use it, so the dialog shows
   the values that will actually apply instead of guessing at placeholders.

3. **Presets and the user default are per-user files.** They live under the
   user's data folder (`render-presets/`), in canonical JSON
   (`supple.render-preset/1.0`). `render.preset.save` writes either a named
   preset or the default; "Save as default" in the dialog writes the
   default. Presets never contain project-specific references (track and
   region ids, custom start and end beats). Factory presets are compiled in
   and read-only. Loading a preset replaces `project.render` as one edit.
   Projects with no settings, or with gaps, take the missing fields from the
   default, so an old project's File → Mix follows the default until the
   user renders from the dialog.

4. **The render folder stays app-wide.** It remains the `render_directory`
   preference, set only by the GUI's folder dialog; no command can set it.
   No project field names a folder. Every output path is checked to stay
   inside the folder, as before: no absolute names or `..`, and the resolved
   real path must lie inside it. This keeps agents confined to the folder
   the user chose, gives files from elsewhere no way to name one, and leaves
   nothing to fix when a project moves between machines. Per-project
   organisation uses subfolders in the file-name pattern (for example
   `{project}/{project}-v{version}`). The dialog labels the folder as shared
   by all projects.

5. **Outputs are a list.** `outputs` holds one to four encodings of the same
   rendered audio: WAV, AIFF, FLAC, MP3 or Ogg Vorbis. Each has its own bit
   depth, dither, optional sample rate and codec options. This replaces
   `format` / `bit_depth` / `secondary`, which are still accepted as
   arguments and migrated in files.

6. **The processing chain is fixed, ordered and deterministic.** In order:
   offline render, seamless-loop fold, mono fold, normalize, true-peak limit,
   trim, then for each output: resample, dither and encode. The limiter runs
   whenever a ceiling is set, with or without normalization. Nothing reads
   the wall clock: identical settings and session give identical files,
   metadata included.

7. **Delivery checks.** When a loudness target or a peak ceiling is set,
   every output is measured as written (lossy files are decoded first, so an
   MP3's peak overshoot is caught) and reports pass or fail. A plain-text
   loudness report can be written beside each file.

8. **MP3 uses LAME, an approved LGPL exception.** The maintainer approved it
   on 2026-11-03. It is linked only into the GPL DAW and never into
   `plugins/` or the MIT plugin-linked libraries. The DAW is
   GPL-3.0-or-later, so LGPL code links into it statically with no further
   obligation beyond the DAW's own source offer. `tools/license-check`
   allows it by name, and AGENTS.md rule 1 lists it beside the ASIO SDK.
   Ogg Vorbis uses libogg and libvorbis (BSD-3). MP3 patents have expired.

9. **Distinctive extras.**
   - Render a branch or an earlier version, with `{version}` and `{branch}`
     tokens in file names.
   - Every result records the branch and version it came from;
     `branch.create` accepts a `version`, so the dialog's results can start a
     branch at the exact mix behind an old bounce.
   - Stems can share one normalization gain, taken from the master mix.
   - Seamless loops for game audio.
   - Region handles.
   - Embedded BWF timecode and markers.

## Consequences

- Project schema 2.3. A 2.2 file's `format`, `bit_depth`, `secondary`,
  `tail_ms` and `normalize.limit_true_peak_db` migrate to the new shape on
  load (`outputs`, `tail_s`, top-level `limit_true_peak_db`). An older build
  opening a 2.3 file ignores the new members with warnings, as it does
  today.
- The two render implementations become one.
- The render result keeps `path` (the first output) and `secondary_path`
  (the second, if any) for existing clients, and adds `outputs`.
- A render from the dialog can add a history step. The user accepted this.
- One render folder for all projects. Sending different projects' renders to
  unrelated places means changing the folder. An approved-folder list can be
  added later without undoing this decision.
- Vendored LAME, libogg and libvorbis add build time on all three platforms,
  plus entries in `third_party/manifest.json` and `THIRD_PARTY.md`.

## Alternatives considered

- **Last-used settings in the UI sidecar** (not undoable). Rejected: two
  sources of truth. File → Mix and an agent's `render.mix` would quietly
  differ, and agents could not see the user's delivery settings.
- **A folder per project in the project file.** Rejected: an agent through
  `project.set`, or a file from elsewhere, could direct renders anywhere,
  and absolute paths break when a project moves.
- **Opus instead of Vorbis.** Vorbis was chosen because game engines (Godot,
  Unity, FMOD, Wwise) import it directly. Opus can be added as another
  output later.
- **A per-file limiter on stems with a shared gain.** Rejected: limiting each
  stem separately breaks their sum. A shared gain with a ceiling is refused.
