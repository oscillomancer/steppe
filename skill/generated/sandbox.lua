---@meta
-- Steppe 0.6.0 scripting reference, generated from the command registry.
-- Do not edit: `<slug>-engine registry --lua` writes it, tools/gen/update-generated.sh regenerates it.
-- Successful replies can include .warnings: command warnings then {code,message,path}
-- bus diagnostics. Non-object results with bus warnings become {result=...,warnings=...}.
-- The `q` table of a script (libs/lua-bus, ADR-0006): every command is a
-- function, `q.run(name, args)` reaches any of them by name, `q.batch(label, body)`
-- is one transaction, and `q.on(topic, handler)` subscribes to bus events.

---@type Steppe
q = nil

---@class Steppe.analyze_args
---@field source string A render id (rd_1) or an audio file path.
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")
---@field note_off_s? number

---@class Steppe.analyze_result
---@field report any
---@field summary string
---@field warnings? table[]

---@class Steppe.compare_args
---@field reference string A render id (rd_1) or an audio file path.
---@field test string A render id (rd_1) or an audio file path.
---@field reference_note_off_s? number The reference's note-off; defaults to a render's input end.
---@field test_note_off_s? number The test's note-off; defaults to a render's input end.

---@class Steppe.compare_result
---@field notable any
---@field ltas_rms_db number
---@field summary string
---@field loss any The score the optimizer minimizes: total, per-term contributions, and the note-off_s used per side.
---@field warnings? table[]

---@class Steppe.dsp_check_args
---@field name? string
---@field source? string

---@class Steppe.dsp_check_result
---@field ok boolean
---@field diagnostics table[]
---@field plugin? table
---@field warnings? table[]

---@class Steppe.dsp_edit_args
---@field name string
---@field edits table[]

---@class Steppe.dsp_edit_result
---@field name string
---@field path string
---@field ok boolean it compiled
---@field diagnostics table[]
---@field plugin? table
---@field listed boolean plugin.list shows it (the script player is on the plugin path)
---@field warnings? table[]

---@class Steppe.dsp_export_args
---@field script string a name from dsp.list
---@field format? "clap"|"source" clap (default): a plugin file; source: a C++ project
---@field folder? string an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new
---@field license? "MIT"|"GPL-3.0-or-later" source: the license of the project's own files (default MIT)

---@class Steppe.dsp_export_result
---@field path string the plugin file, or the project's folder
---@field id string
---@field name string
---@field format string clap or source
---@field player? string clap: the script player it was copied from
---@field bytes? integer
---@field notices? string clap: the licenses to ship with the plugin
---@field warnings? table[]

---@class Steppe.dsp_get_args
---@field name string

---@class Steppe.dsp_get_result
---@field name string
---@field path string
---@field source string
---@field warnings? table[]

---@class Steppe.dsp_list_result
---@field library string
---@field scripts table[]
---@field warnings? table[]

---@class Steppe.dsp_preview_args
---@field plugin? string
---@field script? string
---@field patch? any
---@field signal? "sine"|"sweep"|"impulse"|"noise"|"silence"|"notes"|"note"|"file"
---@field audio_file? string the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect
---@field level_db? number|string Quantity strings are range-checked by the server. (default: -12.0)
---@field frequency_hz? number|string Quantity strings are range-checked by the server. (default: 1000.0)
---@field seconds? number (default: 1.0)
---@field scale? number (default: 1.0)
---@field pointer? table[]

---@class Steppe.dsp_preview_result
---@field plugin string
---@field path string the PNG
---@field width integer pixels
---@field height integer pixels
---@field edits any what pointer changed, as a merge patch
---@field problems string[]
---@field log string[]
---@field warnings? table[]

---@class Steppe.dsp_test_args
---@field plugin? string
---@field script? string
---@field patch? any
---@field signals? ("sine"|"sweep"|"impulse"|"noise"|"silence"|"notes"|"note"|"file")[] default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes
---@field audio_file? string the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file signal plays through an effect
---@field automation? table[] discrete physical-value changes on every signal
---@field sidechain? table timed generated key input
---@field notes? table[] instrument sequence replacing the notes stimulus
---@field level_db? number|string Quantity strings are range-checked by the server. (default: -12.0)
---@field frequency_hz? number|string Quantity strings are range-checked by the server. (default: 1000.0)
---@field seconds? number (default: 1.0)
---@field sample_rate? number|string default: the session's or project's rate Quantity strings are range-checked by the server.
---@field save? boolean (default: false)
---@field trace_points? integer intervals the signal's traces are kept in (default: 100)

---@class Steppe.dsp_test_result
---@field plugin string
---@field sample_rate number|string the rate rendered at Quantity strings are range-checked by the server.
---@field latency integer samples
---@field signals table[]
---@field cpu_percent number of real time, measured
---@field problems string[]
---@field log string[]
---@field warnings? table[]

---@class Steppe.dsp_write_args
---@field name string the script's folder and file name
---@field source string the whole C++ file

---@class Steppe.dsp_write_result
---@field name string
---@field path string
---@field ok boolean it compiled
---@field diagnostics table[]
---@field plugin? table
---@field listed boolean plugin.list shows it (the script player is on the plugin path)
---@field warnings? table[]

---@class Steppe.job_cancel_args
---@field job string

---@class Steppe.job_get_args
---@field job string

---@class Steppe.job_status_args
---@field job string

---@class Steppe.job_wait_args
---@field job string
---@field timeout_s? number Seconds to block (default 30). A larger value is clamped to 30 and reported as timeout_clamped_s.

---@class Steppe.masking_args
---@field masker string A render id (rd_1) or an audio file path.
---@field maskee string A render id (rd_1) or an audio file path.

---@class Steppe.masking_result
---@field result any
---@field warnings? table[]

---@class Steppe.optimize_start_args
---@field instance string
---@field params? any "auto" or a list of keys / {key, min, max}. (default: "auto")
---@field target any {reference?, profile?}
---@field phrase? string
---@field seconds? number
---@field tail_s? number
---@field evaluations? integer
---@field max_seconds? number
---@field seed? integer (default: 0)

---@class Steppe.optimize_start_result
---@field job string
---@field status string
---@field params string[]
---@field render string
---@field warnings? table[]

---@class Steppe.param_get_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field keys? string[]

---@class Steppe.param_get_result
---@field values any
---@field warnings? table[]

---@class Steppe.param_set_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field values any Key -> value.

---@class Steppe.param_set_result
---@field values any
---@field warnings? table[]

---@class Steppe.patch_get_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field detail? "changed"|"full" (default: "changed")

---@class Steppe.patch_get_result
---@field patch any
---@field warnings? table[]

---@class Steppe.patch_merge_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field patch any

---@class Steppe.patch_merge_result
---@field changed any
---@field description string
---@field inactive? string[] Params set that do not apply now
---@field warnings? table[]

---@class Steppe.patch_set_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field patch any

---@class Steppe.patch_set_result
---@field description string
---@field warnings? table[]

---@class Steppe.plugin_describe_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field detail? "summary"|"full" (default: "summary")

---@class Steppe.plugin_describe_result
---@field plugin string
---@field name string
---@field params any
---@field patch_type? string
---@field patch_schema? any
---@field description string
---@field docs? string
---@field status_available boolean The instance supplied its current problems
---@field problems? any Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable
---@field inactive? string[] Parameters that do not apply now
---@field warnings? table[]

---@class Steppe.plugin_list_args
---@field filter? string Text to match in the id or name.
---@field rescan? boolean (default: false)

---@class Steppe.plugin_list_result
---@field plugins table[]
---@field scan table
---@field warnings? table[]

---@class Steppe.plugin_load_args
---@field plugin string Plugin id or .clap path.
---@field position? integer Chain index (0 = first).

---@class Steppe.plugin_load_result
---@field instance string
---@field name string
---@field semantic boolean
---@field chain string[]
---@field warnings? table[]

---@class Steppe.plugin_remove_args
---@field instance string Instance id from plugin.load, e.g. "in_1".

---@class Steppe.plugin_remove_result
---@field chain string[]
---@field warnings? table[]

---@class Steppe.preset_list_args
---@field plugin? string
---@field source? "user"|"factory" (default: "user")

---@class Steppe.preset_list_result
---@field presets table[]
---@field warnings? table[]

---@class Steppe.preset_load_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field name string
---@field source? "user"|"factory" (default: "user")

---@class Steppe.preset_load_result
---@field description string
---@field warnings? table[]

---@class Steppe.preset_save_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field name string
---@field tags? string[]
---@field notes? string

---@class Steppe.preset_save_result
---@field path string
---@field warnings? table[]

---@class Steppe.profile_evaluate_args
---@field source string A render id (rd_1) or an audio file path.
---@field profile any

---@class Steppe.profile_evaluate_result
---@field result any
---@field warnings? table[]

---@class Steppe.registry_describe_args
---@field command string

---@class Steppe.registry_list_args
---@field tag? string
---@field kind? string
---@field text? string

---@class Steppe.render_args
---@field notation? string Notation text.
---@field midi_file? string
---@field audio_file? string
---@field bars? number
---@field seconds? number
---@field tempo? number|string Quantity strings are range-checked by the server.
---@field tail_s? number
---@field name? string
---@field analyze? "none"|"summary"|"standard"|"full" Also analyze the render (see the analyze command); summary is compact (≤ 400 tokens). (default: "none")

---@class Steppe.render_result
---@field render string
---@field path string
---@field seconds number
---@field peak_dbfs number
---@field tail_s number
---@field tail_capped boolean
---@field latency_samples integer
---@field warnings? (string|table)[]
---@field log? string[]
---@field analysis? any

---@class Steppe.script_run_args
---@field code string The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays).
---@field name? string The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name.
---@field budget_ms? number Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count.
---@field args? any Converted to the global table `args`.

---@class Steppe.script_run_result
---@field result any What the script returned (null if it returned nothing).
---@field log string[] What the script printed.
---@field log_dropped integer Cumulative lines evicted from this script's retained log.
---@field script string The name it ran under.
---@field instructions integer Lua instructions the run executed.
---@field warnings? table[]

---@class Steppe.session_get_result
---@field settings any
---@field chain any
---@field instances any
---@field renders any
---@field warnings? table[]

---@class Steppe.session_reset_result
---@field warnings? table[]

---@class Steppe.session_set_args
---@field sample_rate? number|string Quantity strings are range-checked by the server.
---@field block_size? integer
---@field tempo? number|string Quantity strings are range-checked by the server.
---@field meter? string
---@field tail_limit_s? number

---@class Steppe.session_set_result
---@field settings any
---@field warnings? table[]

---@class Steppe.dsp
---@field check fun(args?: Steppe.dsp_check_args): Steppe.dsp_check_result Compile a DSP script without saving it; return its diagnostics.
---@field edit fun(args: Steppe.dsp_edit_args): Steppe.dsp_edit_result Change part of a library script by replacing text; compile it, as dsp.write.
---@field export fun(args: Steppe.dsp_export_args): Steppe.dsp_export_result Export a DSP script as a plugin for other DAWs: a CLAP file, or C++ source.
---@field get fun(args: Steppe.dsp_get_args): Steppe.dsp_get_result Read a DSP script from the library.
---@field list fun(): Steppe.dsp_list_result List the DSP scripts in the library, compiled, with their plugin ids.
---@field preview fun(args?: Steppe.dsp_preview_args): Steppe.dsp_preview_result Draw a plugin's editor to a PNG, after a test signal plays through it.
---@field test fun(args?: Steppe.dsp_test_args): Steppe.dsp_test_result Play test signals or notes through a plugin and measure it: levels, THD, pitch.
---@field write fun(args: Steppe.dsp_write_args): Steppe.dsp_write_result Save a DSP script to the library and compile it, with diagnostics.

---@class Steppe.job
---@field cancel fun(args: Steppe.job_cancel_args): table Cancel a job.
---@field get fun(args: Steppe.job_get_args): table Get a job's status and progress.
---@field list fun(): table List session jobs.
---@field status fun(args: Steppe.job_status_args): table Get a job's status.
---@field wait fun(args: Steppe.job_wait_args): table Wait for a job.

---@class Steppe.optimize
---@field start fun(args: Steppe.optimize_start_args): Steppe.optimize_start_result Tune continuous parameters toward a reference sound or a profile (a job).

---@class Steppe.param
---@field get fun(args: Steppe.param_get_args): Steppe.param_get_result Read parameter values (physical value and display text).
---@field set fun(args: Steppe.param_set_args): Steppe.param_set_result Set parameters in physical units; returns the values as applied.

---@class Steppe.patch
---@field get fun(args: Steppe.patch_get_args): Steppe.patch_get_result Get an instance's patch document (changed values or all).
---@field merge fun(args: Steppe.patch_merge_args): Steppe.patch_merge_result Merge a partial patch (RFC 7396) into an instance.
---@field set fun(args: Steppe.patch_set_args): Steppe.patch_set_result Replace an instance's whole patch; returns a description of the result.

---@class Steppe.plugin
---@field describe fun(args: Steppe.plugin_describe_args): Steppe.plugin_describe_result Describe an instance: parameters, patch format, and current problems.
---@field list fun(args?: Steppe.plugin_list_args): Steppe.plugin_list_result List the CLAP plugins the sandbox can load.
---@field load fun(args: Steppe.plugin_load_args): Steppe.plugin_load_result Load a plugin into the chain and return its instance id.
---@field remove fun(args: Steppe.plugin_remove_args): Steppe.plugin_remove_result Remove a plugin instance from the chain.

---@class Steppe.preset
---@field list fun(args?: Steppe.preset_list_args): Steppe.preset_list_result List user presets, or a plugin's read-only factory patches.
---@field load fun(args: Steppe.preset_load_args): Steppe.preset_load_result Load a named preset into an instance.
---@field save fun(args: Steppe.preset_save_args): Steppe.preset_save_result Save an instance's current settings as a named preset.

---@class Steppe.profile
---@field evaluate fun(args: Steppe.profile_evaluate_args): Steppe.profile_evaluate_result Score a render against a target descriptor profile.

---@class Steppe.registry
---@field describe fun(args: Steppe.registry_describe_args): table Describe a command.
---@field list fun(args?: Steppe.registry_list_args): table List available commands.

---@class Steppe.script
---@field run fun(args: Steppe.script_run_args): Steppe.script_run_result Run a Lua script on the command bus.

---@class Steppe.session
---@field get fun(): Steppe.session_get_result Show the session: settings, chain, instances and renders.
---@field reset fun(): Steppe.session_reset_result Unload every plugin, delete renders and restore default settings.
---@field set fun(args?: Steppe.session_set_args): Steppe.session_set_result Change session settings (rate, block size, tempo, meter, tail limit).

---@class Steppe
---@field dsp Steppe.dsp Commands named dsp.*
---@field job Steppe.job Commands named job.*
---@field optimize Steppe.optimize Commands named optimize.*
---@field param Steppe.param Commands named param.*
---@field patch Steppe.patch Commands named patch.*
---@field plugin Steppe.plugin Commands named plugin.*
---@field preset Steppe.preset Commands named preset.*
---@field profile Steppe.profile Commands named profile.*
---@field registry Steppe.registry Commands named registry.*
---@field script Steppe.script Commands named script.*
---@field session Steppe.session Commands named session.*
---@field analyze fun(args: Steppe.analyze_args): Steppe.analyze_result Analyze a render or audio file: loudness, spectrum, pitch, envelope, stereo.
---@field compare fun(args: Steppe.compare_args): Steppe.compare_result Compare two renders or files: notable descriptor changes and timbre distance.
---@field masking fun(args: Steppe.masking_args): Steppe.masking_result Estimate where one sound masks another (critical bands).
---@field render fun(args?: Steppe.render_args): Steppe.render_result Render notes, a MIDI file or an audio file through the chain to a WAV.
---@field run fun(name: string, args?: table): any Run any registered command by name, e.g. the ones whose names are not Lua identifiers.
---@field batch fun(label: string, body: fun()) Run the calls inside body as one transaction: one undo step, rolled back when one of them fails.
---@field on fun(topic: string, handler: fun(payload: table, topic: string), filter?: table): integer Subscribe to bus events; returns a handle for q.off.
---@field off fun(handle: integer) Drop a subscription.
---@field log fun(...) Print a line to the script's log (the same as print).
---@field null any The JSON null: Lua tables cannot hold nil.
---@field name string The name this script runs under (its origin is `lua:<name>`).
