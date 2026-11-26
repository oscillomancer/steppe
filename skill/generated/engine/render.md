# `render` commands

Render the mix or stems: presets, queue, dry run, regions.

Generated from the command registry; `index.md` lists the groups.

Successful replies may include `warnings`: command warnings followed by bus diagnostics `{code: string, message: string, path: string}`, with `reason: string`, a fixed word for the cause, where the code names its causes. Paths are retained as supplied by the decoder or handler. Batch steps carry their own warnings in `results[i].warnings`. A non-object result with bus warnings is `{result: <value>, warnings: [...]}`. Replies without bus warnings keep their original shape.

In an argument table, a member of an object argument is listed under it as `argument.member` (`argument[].member` for a list of objects).

## `render.dry-run@1`

List the files a render would write, with lengths, without rendering.

Each file: path (where if_exists sends it), exists (its destination is already there), skip (if_exists skip leaves it out), seconds and outputs. operation: mix, stems or regions (default: tracks or regions given, else the settings' source, mix with a performance where that is regions). With performance it also answers performance, range, as_heard and not_heard_because, compiling no plan.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `performance` | value |  |  |  | Mix, stems: play a launcher performance: "captured", a captured_performance, or {from: "launcher", start_beat, end_beat}; its range replaces bounds |
| `job` | boolean |  |  | false |  |
| `as_heard` | boolean |  |  | false | With performance: refuse unless it renders as heard |

### Result

```json
{}

```

### Errors

- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `region.unknown`: no such region Hint: region.list
- `launch.capture`: "captured" with no capture to play Hint: launch.capture {on: true}, play, then render
- `render.performance.sample_rate`: the performance is at another rate Hint: launch.commit it and render the timeline
- `render.performance.launches`: launches out of order or past frames Hint: give the launches in frame order, each within frames
- `render.performance.too_many`: more than 100,000 launches Hint: launch.commit it and render the timeline
- `render.performance.no_clip`: a clip launch names no clip Hint: give the launch's clip, or action arrangement or stop
- `render.performance.clip_track`: a clip on another track Hint: give the clip's own track
- `cl.unknown`: a launch's clip is not in the version Hint: render a version that has it
- `track.frozen`: a launch's track is frozen Hint: track.unfreeze it
- `launch.playrate`: a clip refused under the playrate Hint: launch.commit it and render the timeline
- `render.not_as_heard`: as_heard: true for one not as heard Hint: act on the first reason, or leave as_heard out

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `render.mix@1`

Render the mix offline with the project's settings (default: whole song).

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary. skipped lists files left out: skip_silent, if_exists skip, and (stems without tracks) a channel that can produce no audio, reason no_audio: it has a rack whose Out modules have no audio patched to them (as modulate's Modulators channel) and nothing else feeds it; naming it in tracks renders it. performance plays a launcher performance instead of the arrangement alone: "captured" (the capture, on the version it began on unless version is given), a captured_performance (launch.commit's), or {from: "launcher", start_beat, end_beat}. Its range replaces bounds (bounds in the call is refused). The result adds performance (the object played), range, as_heard and not_heard_because; as_heard: true refuses one that is not as heard (render.not_as_heard). Refusals name the launch: /performance/launches/N. The mix answers with its file's entry at the top level (path, secondary_path, outputs, peak_dbfs, …); analyze: summary or full adds the report inline. masters renders several masters, a file each (the first on top, all in files); a pattern without {track} then gets -{track}, the master's name.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `performance` | value |  |  |  | Mix, stems: play a launcher performance: "captured", a captured_performance, or {from: "launcher", start_beat, end_beat}; its range replaces bounds |
| `job` | boolean |  |  | false |  |
| `as_heard` | boolean |  |  | false | With performance: refuse unless it renders as heard |

### Result

```json
{}

```

### Errors

- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `render.stale_target`: the project changed while it rendered Hint: render again
- `engine.start_failed`: a plug-in refused to start processing Hint: fx.restart it
- `job.cancelled`: the render job was cancelled Hint: render again
- `launch.capture`: "captured" with no capture to play Hint: launch.capture {on: true}, play, then render
- `render.performance.sample_rate`: the performance is at another rate Hint: launch.commit it and render the timeline
- `render.performance.launches`: launches out of order or past frames Hint: give the launches in frame order, each within frames
- `render.performance.too_many`: more than 100,000 launches Hint: launch.commit it and render the timeline
- `render.performance.no_clip`: a clip launch names no clip Hint: give the launch's clip, or action arrangement or stop
- `render.performance.clip_track`: a clip on another track Hint: give the clip's own track
- `cl.unknown`: a launch's clip is not in the version Hint: render a version that has it
- `track.frozen`: a launch's track is frozen Hint: track.unfreeze it
- `launch.playrate`: a clip refused under the playrate Hint: launch.commit it and render the timeline
- `render.not_as_heard`: as_heard: true for one not as heard Hint: act on the first reason, or leave as_heard out
- `launch.memory`: a launch's clip is past the memory budget Hint: launch.commit it and render the timeline

### Examples

**Render** (`song_with_fx`)

```json
{
  "name": "mix-v1"
}

```

Result:

```json
{
  "render": "rd_1",
  "path": "<any>",
  "outputs": "<any>",
  "branch": "main",
  "version": "<any>",
  "start_beat": 0,
  "seconds": "<any>",
  "peak_dbfs": "<any>",
  "output_latency": 0
}

```

## `render.preset.delete@1`

Delete one of the user's render presets, or the user's render default.

It never reads the file, so a damaged preset (render.preset.list: damaged) is removed too.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.preset_name`: neither or both of name and default, or a bad name Hint: name a preset or pass default: true
- `render.preset_factory`: factory presets cannot be deleted Hint: delete one of your own
- `render.preset_write`: the preset file cannot be removed Hint: check the user data folder
- `render.preset_unknown`: no preset with that name, or no default Hint: render.preset.list shows them

### Examples

**Delete a preset** (`render_preset`)

```json
{
  "name": "Archive"
}

```

Result:

```json
{
  "deleted": true
}

```

## `render.preset.list@1`

Render presets, factory then user's, with settings.

default: the user's render default, or null. Presets live in the user's data folder (render-presets/), shared by every project. A user preset that cannot be read is {name, factory: false, damaged: {code, path}}, as render.preset.load answers it; render.preset.delete removes it.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Errors

- `file.open`: a preset file cannot be opened Hint: check the folder
- `file.read`: a preset file cannot be read Hint: check the file
- `file.too_large`: a preset file is too large Hint: delete it
- `json.parse.*`: a preset file is not JSON Hint: fix or delete it

### Examples

**List** (`empty`)

```json
{}

```

Result:

```json
{
  "presets": "<any>",
  "default": null
}

```

## `render.preset.load@1`

Replace the project's render settings with a preset (one undo step).



Kind: `edit`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.preset_name`: neither or both of name and default, or a bad name Hint: name a preset or pass default: true
- `file.open`: a preset file cannot be opened Hint: check the folder
- `file.read`: a preset file cannot be read Hint: check the file
- `file.too_large`: a preset file is too large Hint: delete it
- `json.parse.*`: a preset file is not JSON Hint: fix or delete it
- `render.preset_unknown`: no preset with that name Hint: render.preset.list shows them

### Examples

**A factory preset** (`empty`)

```json
{
  "name": "WAV 24-bit"
}

```

Result:

```json
{
  "render": "<any>"
}

```

## `render.preset.save@1`

Save render settings as a named preset or the user's default.

settings: what to save (default: the effective settings now). Never saved: stems.tracks, matrix and custom bounds, which belong to one project. A file per preset in the user's data folder; factory presets cannot be overwritten.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  | A preset name (render.preset.list) |
| `default` | boolean |  |  | false |  |
| `settings` | object |  |  |  | Render settings (session-model §1.10); every member optional |
| `settings.source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `settings.masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `settings.bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `settings.start_beat` | number |  | 0.0..1e+09 |  |  |
| `settings.end_beat` | number |  | ..1e+09 |  |  |
| `settings.tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `settings.pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `settings.sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `settings.channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `settings.outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `settings.normalize` | object |  |  |  |  |
| `settings.limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `settings.trim` | object |  |  |  |  |
| `settings.seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `settings.handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `settings.stems` | object |  |  |  |  |
| `settings.matrix` | object |  |  |  |  |
| `settings.if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `settings.metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `settings.report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `settings.add_to_project` | boolean |  |  |  |  |

### Result

```json
{}

```

### Errors

- `render.preset_name`: neither or both of name and default, or a bad name Hint: name a preset or pass default: true
- `file.open`: a preset file cannot be opened Hint: check the folder
- `file.read`: a preset file cannot be read Hint: check the file
- `file.too_large`: a preset file is too large Hint: delete it
- `json.parse.*`: a preset file is not JSON Hint: fix or delete it
- `render.preset_write`: the presets folder cannot be made Hint: check the user data folder
- `file.write`: a preset file cannot be written Hint: check the disk
- `file.sync`: a preset file cannot be synced Hint: check the disk
- `file.sync_directory`: the presets folder cannot be synced Hint: check the disk
- `file.sync_published`: a preset was written but not synced Hint: check the disk
- `render.preset_factory`: a factory preset has that name Hint: save under another name

### Examples

**A preset of FLAC files** (`empty`)

```json
{
  "name": "Archive",
  "settings": {
    "outputs": [
      {
        "format": "flac"
      }
    ]
  }
}

```

Result:

```json
{
  "name": "Archive"
}

```

## `render.queue.add@1`

Queue a render (mix, stems, regions) for render.queue.run.

args: the render command's arguments (as for render.mix/stems/regions). The queued render is pinned to the session and settings as they are now (its version, a branch's head when branch is given, and the effective render settings, a loop range as beats), so later edits do not change it; a dry run lists its files now. A performance is stored as the captured_performance it resolved to. The queue is transport-like state: not an undo step, not saved with the project.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `command` | string |  |  |  |  |
| `args` | value |  |  |  |  |
| `label` | string |  |  |  |  |

### Result

```json
{}

```

### Errors

- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `region.unknown`: no such region Hint: region.list
- `launch.capture`: "captured" with no capture to play Hint: launch.capture {on: true}, play, then render
- `render.performance.sample_rate`: the performance is at another rate Hint: launch.commit it and render the timeline
- `render.performance.launches`: launches out of order or past frames Hint: give the launches in frame order, each within frames
- `render.performance.too_many`: more than 100,000 launches Hint: launch.commit it and render the timeline
- `render.performance.no_clip`: a clip launch names no clip Hint: give the launch's clip, or action arrangement or stop
- `render.performance.clip_track`: a clip on another track Hint: give the clip's own track
- `cl.unknown`: a launch's clip is not in the version Hint: render a version that has it
- `track.frozen`: a launch's track is frozen Hint: track.unfreeze it
- `launch.playrate`: a clip refused under the playrate Hint: launch.commit it and render the timeline
- `render.not_as_heard`: as_heard: true for one not as heard Hint: act on the first reason, or leave as_heard out

### Examples

**Queue the mix as it is now** (`song`)

```json
{
  "command": "render.mix",
  "args": {
    "end_beat": 1,
    "name": "queued"
  }
}

```

Result:

```json
{
  "id": "q1",
  "command": "render.mix",
  "label": "<any>",
  "version": "<any>",
  "files": "<any>",
  "args": "<any>"
}

```

## `render.queue.list@1`

The queued renders, in the order render.queue.run runs them.



Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{}

```

### Examples

**Empty** (`empty`)

```json
{}

```

Result:

```json
{
  "queue": []
}

```

## `render.queue.remove@1`

Take a render off the queue (id), or clear it (all: true).



Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `all` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.queue_unknown`: no queued render with that id Hint: render.queue.list shows them

### Examples

**Clear** (`empty`)

```json
{
  "all": true
}

```

Result:

```json
{
  "removed": 0
}

```

## `render.queue.run@1`

Run every queued render in order, removing each when done.

Each render renders the version it was queued at. job: true runs them as one job (job.get for progress). The result lists each render's files under its id.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Errors

- `render.queue_empty`: nothing is queued Hint: render.queue.add first
- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `render.stale_target`: the project changed while it rendered Hint: render again
- `engine.start_failed`: a plug-in refused to start processing Hint: fx.restart it
- `job.cancelled`: the render job was cancelled Hint: render again
- `launch.capture`: "captured" with no capture to play Hint: launch.capture {on: true}, play, then render
- `render.performance.sample_rate`: the performance is at another rate Hint: launch.commit it and render the timeline
- `render.performance.launches`: launches out of order or past frames Hint: give the launches in frame order, each within frames
- `render.performance.too_many`: more than 100,000 launches Hint: launch.commit it and render the timeline
- `render.performance.no_clip`: a clip launch names no clip Hint: give the launch's clip, or action arrangement or stop
- `render.performance.clip_track`: a clip on another track Hint: give the clip's own track
- `cl.unknown`: a launch's clip is not in the version Hint: render a version that has it
- `track.frozen`: a launch's track is frozen Hint: track.unfreeze it
- `launch.playrate`: a clip refused under the playrate Hint: launch.commit it and render the timeline
- `render.not_as_heard`: as_heard: true for one not as heard Hint: act on the first reason, or leave as_heard out
- `launch.memory`: a launch's clip is past the memory budget Hint: launch.commit it and render the timeline

### Examples

**Render the queue** (`render_queued`)

```json
{}

```

Result:

```json
{
  "renders": "<any>"
}

```

## `render.regions@1`

Render each region (or the render matrix's tracks per region) to files.

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary. skipped lists files left out: skip_silent, if_exists skip, and (stems without tracks) a channel that can produce no audio, reason no_audio: it has a rack whose Out modules have no audio patched to them (as modulate's Modulators channel) and nothing else feeds it; naming it in tracks renders it.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `performance` | value |  |  |  | Mix, stems: play a launcher performance: "captured", a captured_performance, or {from: "launcher", start_beat, end_beat}; its range replaces bounds |
| `job` | boolean |  |  | false |  |
| `as_heard` | boolean |  |  | false | With performance: refuse unless it renders as heard |

### Result

```json
{}

```

### Errors

- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `region.unknown`: no such region Hint: region.list
- `render.stale_target`: the project changed while it rendered Hint: render again
- `engine.start_failed`: a plug-in refused to start processing Hint: fx.restart it
- `job.cancelled`: the render job was cancelled Hint: render again

### Examples

**Render project** (`song_with_region`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `render.settings@1`

The render settings that apply, and the layer each one comes from.

effective: what a render with these arguments would use (every member filled in); project: project.render as stored; default: the user's render default (null if none); from: per member, args, project, default or built-in (ADR-0055). Change the project's settings with project.set {render} or render.preset.load.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `performance` | value |  |  |  | Mix, stems: play a launcher performance: "captured", a captured_performance, or {from: "launcher", start_beat, end_beat}; its range replaces bounds |
| `job` | boolean |  |  | false |  |
| `as_heard` | boolean |  |  | false | With performance: refuse unless it renders as heard |

### Result

```json
{}

```

### Errors

- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list shows the versions
- `branch.unknown`: no such branch Hint: branch.list shows the branches
- `render.args`: an argument does not apply to this call Hint: drop the argument the path names
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.preset_name`: neither or both of name and default, or a bad name Hint: name a preset or pass default: true
- `file.open`: a preset file cannot be opened Hint: check the folder
- `file.read`: a preset file cannot be read Hint: check the file
- `file.too_large`: a preset file is too large Hint: delete it
- `json.parse.*`: a preset file is not JSON Hint: fix or delete it

### Examples

**What File → Mix would do** (`song`)

```json
{}

```

Result:

```json
{
  "effective": "<any>",
  "project": "<any>",
  "default": null,
  "from": "<any>"
}

```

## `render.stems@1`

Render each selected track to its own file (stems).

Arguments override project.render (then the user's default, then built-ins) for this call only; render.settings shows what applies. Files go to the renders folder, named by pattern (subfolders allowed). job: true runs the render as a job (job.get for progress, job.cancel to stop). Each file lists its outputs (with delivery checks when a loudness target or a ceiling is set), branch, version, start_beat, seconds, a render id (analyze.render, analyze.compare) and an analysis summary. skipped lists files left out: skip_silent, if_exists skip, and (stems without tracks) a channel that can produce no audio, reason no_audio: it has a rack whose Out modules have no audio patched to them (as modulate's Modulators channel) and nothing else feeds it; naming it in tracks renders it. performance plays a launcher performance instead of the arrangement alone: "captured" (the capture, on the version it began on unless version is given), a captured_performance (launch.commit's), or {from: "launcher", start_beat, end_beat}. Its range replaces bounds (bounds in the call is refused). The result adds performance (the object played), range, as_heard and not_heard_because; as_heard: true refuses one that is not as heard (render.not_as_heard). Refusals name the launch: /performance/launches/N.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `source` | string |  |  |  | The dialog's source; render.mix always renders the mix One of: mix, stems, regions. |
| `masters` | array |  |  |  | The mix: the masters to render, a file each (default: tr_0) |
| `bounds` | string |  |  |  | project (default), loop or custom (start_beat, end_beat) One of: project, loop, custom. |
| `start_beat` | number |  | 0.0..1e+09 |  |  |
| `end_beat` | number |  | ..1e+09 |  |  |
| `tail_s` | number |  | 0.0..600.0 |  | Seconds rendered past the end |
| `pattern` | string or object |  |  |  | File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions} |
| `pattern.mix` | string |  |  |  |  |
| `pattern.stems` | string |  |  |  |  |
| `pattern.regions` | string |  |  |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  | Default: the project rate |
| `channels` | integer or string |  |  |  | 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks) |
| `outputs` | array |  |  |  | 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]] |
| `outputs[].format` | string |  |  |  | One of: wav, aiff, flac, mp3, vorbis. |
| `outputs[].bit_depth` | integer |  |  |  | WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and Vorbis One of: 16, 24, 32. |
| `outputs[].dither` | string |  |  |  | 16- and 24-bit output: none, tpdf (default) or shaped One of: none, tpdf, shaped. |
| `outputs[].sample_rate` | integer |  | 8000.0..384000.0 |  | This output's own rate |
| `outputs[].mp3` | object |  |  |  |  |
| `outputs[].vorbis_quality` | number |  | -1.0..10.0 |  | Ogg Vorbis quality, -1 to 10 [6] |
| `outputs[].suffix` | string |  |  |  | Inserted before the extension of a second or later output, e.g. "-16bit" |
| `normalize` | object |  |  |  |  |
| `normalize.mode` | string |  |  |  | One of: none, lufs_i, lufs_s, lufs_m, peak, true_peak, rms. |
| `normalize.target` | number |  | -100.0..0.0 |  | LUFS or dB [-14] |
| `normalize.only_reduce` | boolean |  |  |  | Never raise the level |
| `normalize.scope` | string |  |  |  | file (default), or master: stems share the master mix's gain One of: file, master. |
| `limit_true_peak_db` | number |  | -60.0..0.0 |  | A true-peak ceiling (dBTP) |
| `trim` | object |  |  |  |  |
| `trim.start` | boolean |  |  |  |  |
| `trim.end` | boolean |  |  |  |  |
| `trim.threshold_db` | number |  | -144.0..0.0 |  | [-60] |
| `seamless_loop` | boolean |  |  |  | Sum the tail into the start (needs tail_s) |
| `handles_s` | number |  | 0.0..60.0 |  | Regions: seconds added at both ends |
| `stems` | object |  |  |  |  |
| `stems.mode` | string |  |  |  | isolated (default; no master processing) or through_master One of: isolated, through_master. |
| `stems.tap` | string |  |  |  | One of: post_fader, pre_fader, pre_fx. |
| `stems.tracks` | array |  |  |  | Track ids (default: every top-level track) |
| `stems.skip_silent` | boolean |  |  |  | Leave out stems whose peak is below -90 dBFS |
| `matrix` | object |  |  |  |  |
| `if_exists` | string |  |  |  | One of: overwrite, number, skip, error. |
| `metadata` | object |  |  |  | Tags written into each file; text members accept the file-name tokens |
| `metadata.title` | string |  |  |  |  |
| `metadata.artist` | string |  |  |  |  |
| `metadata.album` | string |  |  |  |  |
| `metadata.track_number` | integer |  | 1.0..9999.0 |  |  |
| `metadata.date` | string |  |  |  |  |
| `metadata.genre` | string |  |  |  |  |
| `metadata.comment` | string |  |  |  |  |
| `metadata.isrc` | string |  |  |  |  |
| `metadata.cover` | string |  |  |  | A PNG or JPEG of at most 10 MiB, relative to the project folder |
| `metadata.bwf` | boolean |  |  |  | WAV: a BWF bext chunk (v2) with time reference and loudness |
| `metadata.markers` | boolean |  |  |  | WAV cue points / AIFF markers for the markers and regions in the file's range |
| `report` | boolean |  |  |  | Write <file>.loudness.txt beside each file |
| `add_to_project` | boolean |  |  |  |  |
| `master` | string |  |  |  | Mix: one master for this call (default: the settings' masters, else tr_0) |
| `branch` | string |  |  |  | Render this branch's head (default: the current one) |
| `version` | integer |  | 0.0.. |  | Render the session as of this version (history.list); the render queue pins its renders |
| `name` | string |  |  |  | A file name instead of the pattern, for this call |
| `operation` | string |  |  |  | Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source) One of: mix, stems, regions. |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the render matrix) |
| `tracks` | array |  |  |  | Stems: track ids (sets stems.tracks for this call) |
| `format` | string |  |  |  | Sets outputs[0].format for this call One of: wav, aiff, flac, mp3, vorbis. |
| `bit_depth` | integer |  | 16.0..32.0 |  | Sets outputs[0].bit_depth for this call |
| `secondary` | value |  |  |  | Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call |
| `mode` | string |  |  |  | Sets stems.mode for this call One of: isolated, through_master. |
| `tap` | string |  |  |  | Sets stems.tap for this call One of: post_fader, pre_fader, pre_fx. |
| `target` | number |  |  |  | Sets normalize.target for this call |
| `analyze` | string |  |  |  | Mix: a supple.analysis-report/1.0 inline One of: summary, full. |
| `performance` | value |  |  |  | Mix, stems: play a launcher performance: "captured", a captured_performance, or {from: "launcher", start_beat, end_beat}; its range replaces bounds |
| `job` | boolean |  |  | false |  |
| `as_heard` | boolean |  |  | false | With performance: refuse unless it renders as heard |

### Result

```json
{}

```

### Errors

- `render.args`: an argument does not apply Hint: drop it
- `render.empty`: the render would write nothing Hint: add items or a range
- `render.source`: branch and version are both given Hint: give one of them
- `history.version`: no such version Hint: history.list
- `branch.unknown`: no such branch Hint: branch.list
- `engine.range`: the range is empty or out of bounds Hint: fix the range
- `engine.block_size`: the block size is out of range Hint: 1 .. 8192
- `engine.fx_failed`: a plug-in could not be loaded Hint: fx.restart it
- `engine.bridge_failed`: a bridged plug-in failed Hint: fx.restart it
- `engine.process_error`: a plug-in failed Hint: fx.restart it
- `engine.media_missing`: an audio file is missing Hint: restore the file
- `engine.media_changed`: an item's audio file changed Hint: restore the file
- `engine.media_too_long`: an audio file is too long Hint: trim it
- `engine.blob_missing`: a plug-in state blob is missing Hint: re-save
- `engine.patch_unpatchable`: a rack patch cannot be compiled Hint: fix the patch
- `engine.stretch`: a source cannot be stretched Hint: see the message
- `media.budget`: sources exceed the sample budget Hint: use shorter sources
- `warp.peers`: a warp peer is missing Hint: detach the bundle
- `warp.budget`: a warp bundle exceeds its budget Hint: shorten its items
- `warp.rate`: a warp segment's rate is out of range Hint: rates 0.01 .. 100
- `json.parse.*`: the render default is not JSON Hint: fix or delete it
- `plugin.unknown`: a plug-in is not available Hint: plugin.list
- `file.open`: a file cannot be opened Hint: check the path
- `file.read`: a file cannot be read Hint: check it exists and is readable
- `file.too_large`: a file is too large to read Hint: use a smaller file
- `file.write`: a file cannot be written Hint: check the disk
- `file.sync`: a written file cannot be synced Hint: check the disk
- `file.sync_directory`: a folder cannot be synced Hint: check the disk
- `file.sync_published`: written but not synced Hint: check the disk
- `audio.open`: an audio file cannot be opened Hint: check the path
- `audio.read`: an audio file cannot be read Hint: check the file
- `audio.decode`: an audio file cannot be decoded Hint: check the file
- `audio.format`: a format or depth is unsupported Hint: change outputs
- `audio.write`: an audio file cannot be written Hint: check the disk
- `audio.encode`: an encoder or tag writer failed Hint: change outputs
- `audio.cancelled`: an encoder was cancelled Hint: render again
- `render.pattern`: the name pattern does not parse Hint: fix the token named
- `render.path`: a destination is outside renders or unwritable Hint: change pattern
- `render.exists`: a destination exists Hint: if_exists overwrite, number or skip
- `render.secondary`: secondary is neither {format, bit_depth} nor "none" Hint: e.g. {"format": "flac", "bit_depth": 16}
- `render.format`: an output format is unknown Hint: render.settings
- `render.normalize`: normalization cannot apply as set Hint: fix normalize as the path names
- `render.masters`: masters names a non-master Hint: track.list
- `render.budget`: the render exceeds the in-memory frame budget Hint: render a shorter range
- `render.loop`: a loop it cannot follow Hint: drop the loop
- `track.unknown`: a named track is not in the version Hint: track.list
- `track.strip_type`: the mix output is not a master Hint: name a master
- `engine.too_long`: too long, or so at the playrate Hint: render a shorter range
- `engine.tail`: tail_s is out of range Hint: 0 .. 600 s
- `render.stale_target`: the project changed while it rendered Hint: render again
- `engine.start_failed`: a plug-in refused to start processing Hint: fx.restart it
- `job.cancelled`: the render job was cancelled Hint: render again
- `launch.capture`: "captured" with no capture to play Hint: launch.capture {on: true}, play, then render
- `render.performance.sample_rate`: the performance is at another rate Hint: launch.commit it and render the timeline
- `render.performance.launches`: launches out of order or past frames Hint: give the launches in frame order, each within frames
- `render.performance.too_many`: more than 100,000 launches Hint: launch.commit it and render the timeline
- `render.performance.no_clip`: a clip launch names no clip Hint: give the launch's clip, or action arrangement or stop
- `render.performance.clip_track`: a clip on another track Hint: give the clip's own track
- `cl.unknown`: a launch's clip is not in the version Hint: render a version that has it
- `track.frozen`: a launch's track is frozen Hint: track.unfreeze it
- `launch.playrate`: a clip refused under the playrate Hint: launch.commit it and render the timeline
- `render.not_as_heard`: as_heard: true for one not as heard Hint: act on the first reason, or leave as_heard out
- `launch.memory`: a launch's clip is past the memory budget Hint: launch.commit it and render the timeline

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```
