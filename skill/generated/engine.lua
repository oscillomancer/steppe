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

---@class Steppe.analyze_compare_args
---@field reference string
---@field test string

---@class Steppe.analyze_compare_result
---@field notable any
---@field ltas_rms_db number
---@field summary string
---@field warnings? table[]

---@class Steppe.analyze_masking_args
---@field masker_track string
---@field maskee_track string
---@field start_beat? number
---@field end_beat? number

---@class Steppe.analyze_render_args
---@field render string
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")

---@class Steppe.analyze_track_args
---@field track string
---@field start_beat? number
---@field end_beat? number
---@field tap? "post_fader"|"pre_fader"|"pre_fx" (default: "post_fader")
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")

---@class Steppe.arrangement_build_args
---@field sequence table[]
---@field at? number Start in beats, on a bar line.
---@field bar? integer Start bar, 1-based [1].
---@field pooled? boolean (default: false)
---@field follow? "none"|"key"|"chords" How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow.
---@field write_context? boolean Write the scene's clips' own key and chords onto the context track under each section that has none (shifted to the section, repeating with a loop clip), in the same undo step, before the clips are laid. Left alone: a section with points under it; not written: one whose clips disagree. The answer's context says which (default false). (default: false)

---@class Steppe.arrangement_build_result
---@field segments table[]
---@field warnings? table[]

---@class Steppe.audio_quantize_args
---@field items string[]
---@field grid number
---@field strength? number (default: 1.0)
---@field swing? number (default: 0.0)
---@field crossfade? number (default: 0.005)
---@field mode? "warp"|"slice" (default: "warp")
---@field reference? string (default: "")
---@field hits? "reference"|"merged" (default: "reference")

---@class Steppe.audio_warp_detach_args
---@field items string[]

---@class Steppe.audio_warp_reset_args
---@field items string[]

---@class Steppe.audition_get_result
---@field playing boolean
---@field decoding boolean
---@field path string
---@field seconds number
---@field position number
---@field warnings? table[]

---@class Steppe.audition_play_args
---@field path string

---@class Steppe.audition_play_result
---@field path string
---@field seconds number
---@field sample_rate number
---@field channels integer
---@field truncated boolean
---@field warnings? table[]

---@class Steppe.audition_stop_result
---@field playing boolean
---@field decoding boolean
---@field path string
---@field seconds number
---@field position number
---@field warnings? table[]

---@class Steppe.autoitem_add_args
---@field env string Envelope id, e.g. "en_1".
---@field position? number
---@field bar? integer
---@field length? number (default: 4.0)
---@field points? table[]
---@field source_length? number
---@field loop? boolean
---@field name? string

---@class Steppe.autoitem_duplicate_args
---@field item string Automation item id, e.g. "ai_1".
---@field position? number
---@field pooled? boolean (default: true)

---@class Steppe.autoitem_list_args
---@field env? string Envelope id, e.g. "en_1".

---@class Steppe.autoitem_remove_args
---@field item string Automation item id, e.g. "ai_1".

---@class Steppe.autoitem_remove_result
---@field warnings? table[]

---@class Steppe.autoitem_set_args
---@field item string Automation item id, e.g. "ai_1".
---@field position? number
---@field length? number
---@field source_length? number
---@field loop? boolean
---@field baseline? number
---@field amplitude? number
---@field points? table[]
---@field name? string

---@class Steppe.autoitem_unpool_args
---@field item string Automation item id, e.g. "ai_1".

---@class Steppe.autoitem_unpool_result
---@field warnings? table[]

---@class Steppe.branch_create_args
---@field name string
---@field from? string
---@field version? integer Start at this version of the current branch's history (history.list; a render result's version) instead of a head
---@field switch? boolean (default: true)

---@class Steppe.branch_create_result
---@field name string
---@field warnings? table[]

---@class Steppe.branch_delete_args
---@field name string

---@class Steppe.branch_delete_result
---@field warnings? table[]

---@class Steppe.branch_diff_args
---@field a string
---@field b string

---@class Steppe.branch_diff_result
---@field changes table[]
---@field warnings? table[]

---@class Steppe.branch_list_result
---@field branches string[]
---@field current string
---@field warnings? table[]

---@class Steppe.branch_merge_args
---@field from string
---@field into? string

---@class Steppe.branch_merge_result
---@field fast_forward boolean
---@field up_to_date boolean
---@field version integer
---@field remapped any
---@field warnings? table[]

---@class Steppe.branch_render_ab_args
---@field a string
---@field b string
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number

---@class Steppe.branch_switch_args
---@field name string

---@class Steppe.branch_switch_result
---@field name string
---@field warnings? table[]

---@class Steppe.cable_add_args
---@field from string
---@field to string
---@field amount? number
---@field color? string

---@class Steppe.cable_remove_args
---@field cable? string
---@field from? string
---@field to? string

---@class Steppe.cable_set_args
---@field cable string
---@field amount? number
---@field color? string

---@class Steppe.chord_remove_args
---@field at number

---@class Steppe.chord_remove_result
---@field warnings? table[]

---@class Steppe.chord_set_args
---@field at number
---@field symbol? string
---@field to? number

---@class Steppe.chord_set_result
---@field id string
---@field warnings? table[]

---@class Steppe.clip_add_args
---@field track string Track id, e.g. "tr_1".
---@field scene string Scene id, e.g. "sc_1".
---@field notation? string
---@field audio_file? string
---@field midi_file? string
---@field item? string Item id, e.g. "it_1".
---@field pooled? boolean (default: false)
---@field name? string
---@field context? string The key and chords the clip was written against, as a lead sheet in clip bars (bar 1 is the clip's start): "1: key D minor {Dm}:w". "" is none.
---@field context_at? table A place in the project, by bar or in beats: the clip takes the key and chords holding there, and the changes up to its length after it.
---@field follow? "none"|"key"|"chords" How the clip's notes are mapped when it is laid on the timeline (ADR-0098): none (default), key or chords. Needs a MIDI clip with a key at its start.

---@class Steppe.clip_add_result
---@field clip string
---@field takes string[]
---@field source? string
---@field kind string
---@field length number
---@field warnings? (string|table)[]

---@class Steppe.clip_duplicate_args
---@field clip string Clip id, e.g. "cl_1".
---@field track? string Track id, e.g. "tr_1".
---@field scene? string Scene id, e.g. "sc_1".
---@field pooled? boolean (default: false)

---@class Steppe.clip_duplicate_result
---@field id string
---@field warnings? table[]

---@class Steppe.clip_move_args
---@field clip string Clip id, e.g. "cl_1".
---@field track? string Track id, e.g. "tr_1".
---@field scene? string Scene id, e.g. "sc_1".

---@class Steppe.clip_move_result
---@field warnings? table[]

---@class Steppe.clip_remove_args
---@field clip string Clip id, e.g. "cl_1".

---@class Steppe.clip_remove_result
---@field warnings? table[]

---@class Steppe.clip_set_args
---@field clip string Clip id, e.g. "cl_1".
---@field name? string
---@field color? string
---@field mute? boolean
---@field gain_db? number
---@field length? number
---@field loop? table
---@field mode? "loop"|"once"
---@field quantize? any
---@field context? string As clip.add's: the lead sheet in clip bars; "" clears the context.
---@field context_at? table A place in the project, by bar or in beats: the clip takes the key and chords holding there, and the changes up to its length after it.
---@field follow? "none"|"key"|"chords" How the clip's notes are mapped when it is laid on the timeline (ADR-0098): none (default), key or chords. Needs a MIDI clip with a key at its start.

---@class Steppe.clip_set_result
---@field warnings? table[]

---@class Steppe.context_at_args
---@field at? number
---@field bar? integer

---@class Steppe.context_at_result
---@field beat number
---@field bar integer
---@field beat_in_bar number
---@field key? table
---@field chord? table
---@field section? table
---@field warnings? table[]

---@class Steppe.context_get_args
---@field from? number
---@field to? number

---@class Steppe.context_get_result
---@field text? string
---@field points table[]
---@field key? table
---@field chord? table
---@field warnings? table[]

---@class Steppe.context_write_args
---@field text string

---@class Steppe.context_write_result
---@field first_bar integer
---@field last_bar integer
---@field added integer
---@field replaced integer
---@field removed integer
---@field unchanged integer
---@field warnings? table[]

---@class Steppe.crash_acknowledge_args
---@field incident string A report's incident id, from crash.list.

---@class Steppe.crash_delete_args
---@field incident string A report's incident id, from crash.list.

---@class Steppe.crash_list_args
---@field limit? integer

---@class Steppe.crash_read_args
---@field incident string A report's incident id, from crash.list.

---@class Steppe.crash_settle_args
---@field incident string A report's incident id, from crash.list.

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

---@class Steppe.engine_devices_args
---@field backend? "jack"|"dummy"|"portaudio"

---@class Steppe.engine_devices_result
---@field devices table[]
---@field warnings? table[]

---@class Steppe.engine_start_args
---@field backend? "jack"|"dummy"|"portaudio"
---@field host_api? string
---@field input_device? string
---@field output_device? string
---@field rate? number
---@field inputs? integer
---@field outputs? integer
---@field block? integer
---@field anticipative? boolean
---@field force_graph? boolean

---@class Steppe.engine_start_result
---@field running boolean
---@field preparing_audio boolean
---@field prepared_version integer
---@field preparation_version integer
---@field preparation_error string
---@field backend string
---@field sample_rate number
---@field block integer
---@field period integer
---@field plan_latency integer
---@field device_latency integer
---@field xruns integer
---@field bridge_late integer
---@field bridge_timing table
---@field midi_dropped integer
---@field midi_inputs any
---@field midi_outputs any
---@field midi_lead_frames integer
---@field midi_out_dropped integer
---@field midi_output_warnings table[]
---@field midi_output_latency integer[]
---@field midi_warnings table[]
---@field midi_devices_changed boolean
---@field midi_devices_message string
---@field cycles integer
---@field process_errors integer
---@field restart_needed boolean
---@field anticipative boolean
---@field anticipation_misses integer
---@field live_helpers integer
---@field scheduling table
---@field workgroup table
---@field warnings (string|table)[]
---@field memory_lock table
---@field cpu_idle_latency table
---@field thp_opt_out table
---@field audio_page_faults table
---@field meter_drops table
---@field failed_fx string[]
---@field disabled_fx string[] fx whose plug-in this run has disabled (plugin.override): the strip passes audio through
---@field bridge_threads_refused string[]
---@field dropped integer
---@field voice_dropped integer
---@field modulation_deferred integer Modulation events that waited past their grid point for room in a block (ADR-0096)
---@field failed_errors any
---@field sync table

---@class Steppe.engine_status_result
---@field running boolean
---@field preparing_audio boolean
---@field prepared_version integer
---@field preparation_version integer
---@field preparation_error string
---@field backend string
---@field sample_rate number
---@field block integer
---@field period integer
---@field plan_latency integer
---@field device_latency integer
---@field xruns integer
---@field bridge_late integer
---@field bridge_timing table
---@field midi_dropped integer
---@field midi_inputs any
---@field midi_outputs any
---@field midi_lead_frames integer
---@field midi_out_dropped integer
---@field midi_output_warnings table[]
---@field midi_output_latency integer[]
---@field midi_warnings table[]
---@field midi_devices_changed boolean
---@field midi_devices_message string
---@field cycles integer
---@field process_errors integer
---@field restart_needed boolean
---@field anticipative boolean
---@field anticipation_misses integer
---@field live_helpers integer
---@field scheduling table
---@field workgroup table
---@field warnings (string|table)[]
---@field memory_lock table
---@field cpu_idle_latency table
---@field thp_opt_out table
---@field audio_page_faults table
---@field meter_drops table
---@field failed_fx string[]
---@field disabled_fx string[] fx whose plug-in this run has disabled (plugin.override): the strip passes audio through
---@field bridge_threads_refused string[]
---@field dropped integer
---@field voice_dropped integer
---@field modulation_deferred integer Modulation events that waited past their grid point for room in a block (ADR-0096)
---@field failed_errors any
---@field sync table

---@class Steppe.engine_stop_result
---@field warnings? table[]

---@class Steppe.env_add_args
---@field track? string Track id, e.g. "tr_1".
---@field module? string Rack module id, e.g. "mo_1".
---@field take? string Take id, e.g. "tk_1".
---@field group? string VCA group id, e.g. "gr_1".
---@field fx? string Plugin instance id, e.g. "fx_1".
---@field route? string Route id, e.g. "sd_1".
---@field param string
---@field points? table[]

---@class Steppe.env_add_result
---@field env string
---@field points integer
---@field param? string A plug-in parameter's canonical reference: its semantic key or decimal id.
---@field param_name? string A plug-in parameter's display name.
---@field converted? boolean True when a semantic plugin's parameter was named by id or name: the points were plain values, stored converted to the key's units (env.list shows those).
---@field warnings? table[]

---@class Steppe.env_list_args
---@field object? string

---@class Steppe.env_list_result
---@field envelopes table[]
---@field warnings? table[]

---@class Steppe.env_points_set_args
---@field env string Envelope id, e.g. "en_1".
---@field points table[]
---@field from_beat? number
---@field to_beat? number

---@class Steppe.env_points_set_result
---@field points integer
---@field warnings? table[]

---@class Steppe.env_remove_args
---@field env string Envelope id, e.g. "en_1".

---@class Steppe.env_remove_result
---@field warnings? table[]

---@class Steppe.env_set_args
---@field env string Envelope id, e.g. "en_1".
---@field active? boolean
---@field visible? boolean
---@field mode? "trim"|"read"|"touch"|"latch"|"latch_preview"|"write"

---@class Steppe.env_set_result
---@field warnings? table[]

---@class Steppe.env_write_commit_args
---@field env string Envelope id, e.g. "en_1".
---@field after number
---@field through number
---@field points table[]

---@class Steppe.env_write_commit_result
---@field warnings? table[]

---@class Steppe.fx_add_args
---@field track? string Track id, e.g. "tr_1".
---@field take? string Take id, e.g. "tk_1".
---@field plugin string
---@field index? integer
---@field patch? any
---@field chain? string

---@class Steppe.fx_add_result
---@field fx string
---@field name string
---@field semantic boolean
---@field chain string[]
---@field warnings? table[]

---@class Steppe.fx_chain_delete_args
---@field name string

---@class Steppe.fx_chain_delete_result
---@field path string
---@field warnings? table[]

---@class Steppe.fx_chain_list_result
---@field chains table[]
---@field warnings? table[]

---@class Steppe.fx_chain_load_args
---@field track string
---@field name? string
---@field path? string
---@field chain? string (default: "main")
---@field mode? string (default: "append")

---@class Steppe.fx_chain_load_result
---@field effects string[]
---@field warnings? table[]

---@class Steppe.fx_chain_save_args
---@field track string
---@field name string
---@field chain? string (default: "main")
---@field replace? boolean (default: false)

---@class Steppe.fx_chain_save_result
---@field path string
---@field effects integer
---@field warnings? table[]

---@class Steppe.fx_copy_args
---@field fx string
---@field track string
---@field index? integer
---@field chain? string (default: "main")

---@class Steppe.fx_copy_result
---@field fx string
---@field chain string[]
---@field warnings? table[]

---@class Steppe.fx_describe_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field full? boolean (default: false)
---@field filter? string Plain parameters only: list those whose name contains this text (case-insensitive).

---@class Steppe.fx_describe_result
---@field plugin string
---@field name string
---@field description string
---@field params? any With full: each parameter; modulatable_by_envelope: true when a route reaches it through its envelope (ADR-0096 M4); modulatable: true when a modulation route can take it (modulation.add)
---@field patch? any
---@field status_available boolean A live instance supplied its current problems
---@field problems? any Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable
---@field sidechain_channels? integer The plug-in's key input: route.add {fx} feeds it
---@field latency_samples? integer The instance's reported latency, in samples
---@field isolation_effective? string Where the instance runs: in_process, shared_bridge or dedicated; inherit resolves to the plug-in isolation preference (stock plug-ins in process), as the instance is made
---@field override? string This run's plugin.override for the plug-in, when it is not normal: bridge (it runs on a dedicated bridge) or disable (it is not loaded: the strip passes audio through, a rack module is silent, and there is no isolation_effective or parameter section)
---@field outputs_notes? boolean The loaded plug-in sends notes or MIDI: fx.set midi_output chooses replace or merge
---@field params_total? integer Plain parameters matching; at most 512 are listed.
---@field warnings? table[]

---@class Steppe.fx_learn_list_args
---@field fx? string

---@class Steppe.fx_learn_list_result
---@field learn any
---@field warnings? table[]

---@class Steppe.fx_learn_remove_args
---@field fx string
---@field param string

---@class Steppe.fx_learn_remove_result
---@field warnings? table[]

---@class Steppe.fx_learn_set_args
---@field fx string
---@field param string A parameter's semantic key, id or name (fx.describe); a semantic plugin's is stored as its key
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean
---@field when_selected? boolean
---@field when_focused? boolean
---@field when_visible? boolean

---@class Steppe.fx_learn_set_result
---@field param string
---@field control string
---@field warnings? table[]

---@class Steppe.fx_meters_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field from? "live"|"render" live: the playing instance; render: the last render.mix (default: "live")

---@class Steppe.fx_meters_result
---@field source string live, render, or none (nothing to read yet)
---@field meters any Read-only values by patch key (physical) or name
---@field warnings? table[]

---@class Steppe.fx_move_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field track? string Track id, e.g. "tr_1".
---@field index integer

---@class Steppe.fx_move_result
---@field warnings? table[]

---@class Steppe.fx_param_get_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field param string

---@class Steppe.fx_param_get_result
---@field value any
---@field warnings? table[]

---@class Steppe.fx_param_set_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field values any

---@class Steppe.fx_param_set_result
---@field changed any
---@field description string
---@field inactive? string[] Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it
---@field warnings? table[]

---@class Steppe.fx_patch_get_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field changed_only? boolean (default: false)

---@class Steppe.fx_patch_get_result
---@field patch any
---@field warnings? table[]

---@class Steppe.fx_patch_list_args
---@field fx string Plugin instance id, e.g. "fx_1".

---@class Steppe.fx_patch_list_result
---@field patches any
---@field warnings? table[]

---@class Steppe.fx_patch_merge_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field patch any

---@class Steppe.fx_patch_merge_result
---@field changed any
---@field description string
---@field inactive? string[] Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it
---@field warnings? table[]

---@class Steppe.fx_patch_set_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field patch? any
---@field name? string

---@class Steppe.fx_patch_set_result
---@field patch any
---@field warnings? table[]

---@class Steppe.fx_preset_delete_args
---@field fx string
---@field name string

---@class Steppe.fx_preset_delete_result
---@field path string
---@field warnings? table[]

---@class Steppe.fx_preset_list_args
---@field fx string

---@class Steppe.fx_preset_list_result
---@field presets table[]
---@field warnings? table[]

---@class Steppe.fx_preset_load_args
---@field fx string
---@field name string
---@field source? string

---@class Steppe.fx_preset_load_result
---@field name string
---@field source string
---@field warnings? table[]

---@class Steppe.fx_preset_save_args
---@field fx string
---@field name string
---@field replace? boolean (default: false)

---@class Steppe.fx_preset_save_result
---@field path string
---@field warnings? table[]

---@class Steppe.fx_remove_args
---@field fx string Plugin instance id, e.g. "fx_1".

---@class Steppe.fx_remove_result
---@field warnings? table[]

---@class Steppe.fx_restart_args
---@field fx string Plugin instance id, e.g. "fx_1".

---@class Steppe.fx_restart_result
---@field warnings? table[]

---@class Steppe.fx_set_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field bypass? boolean
---@field offline? boolean
---@field wet? number|string Quantity strings are range-checked by the server.
---@field label? string
---@field isolation? "inherit"|"in_process"|"shared_bridge"|"dedicated"
---@field pins? table
---@field midi_output? "replace"|"merge"

---@class Steppe.fx_set_result
---@field warnings? table[]

---@class Steppe.fx_state_capture_args
---@field fx string Plug-in id, e.g. "fx_1".
---@field state string The state's blob hash, as the plug-in's state field holds it

---@class Steppe.fx_state_capture_result
---@field warnings? table[]

---@class Steppe.group_add_args
---@field name string
---@field color? string
---@field active? boolean
---@field members? string[]
---@field links? string[]

---@class Steppe.group_add_result
---@field group string
---@field warnings? table[]

---@class Steppe.group_describe_args
---@field group string

---@class Steppe.group_describe_result
---@field group string
---@field state table
---@field controllers string[]
---@field warnings? table[]

---@class Steppe.group_list_result
---@field groups table[]
---@field warnings? table[]

---@class Steppe.group_members_set_args
---@field group string
---@field members string[]

---@class Steppe.group_members_set_result
---@field warnings? table[]

---@class Steppe.group_remove_args
---@field group string

---@class Steppe.group_remove_result
---@field warnings? table[]

---@class Steppe.group_set_args
---@field group string
---@field name? string
---@field color? string
---@field active? boolean
---@field links? string[]

---@class Steppe.group_set_result
---@field warnings? table[]

---@class Steppe.group_targets_args
---@field track? string
---@field item? string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field attribute string

---@class Steppe.group_targets_result
---@field tracks string[]
---@field items string[]
---@field warnings? table[]

---@class Steppe.history_list_result
---@field versions table[]
---@field can_undo boolean
---@field can_redo boolean
---@field truncated boolean
---@field retained_bytes integer
---@field budget_bytes integer
---@field warnings? table[]

---@class Steppe.history_redo_result
---@field version integer
---@field label string
---@field warnings? table[]

---@class Steppe.history_undo_result
---@field version integer
---@field label string
---@field warnings? table[]

---@class Steppe.item_add_args
---@field track string Track id, e.g. "tr_1".
---@field position? number Beats.
---@field bar? integer Start at this bar (1-based).
---@field length? number Beats.
---@field notation? string
---@field audio_file? string
---@field midi_file? string
---@field clip? string Lay a copy of this clip's takes here.
---@field follow? "none"|"key"|"chords" How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow.
---@field name? string

---@class Steppe.item_add_result
---@field item string
---@field take string
---@field source? string
---@field position number
---@field length number
---@field warnings? (string|table)[]
---@field followed? table

---@class Steppe.item_apply_fx_args
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field mode? string (default: "take")

---@class Steppe.item_apply_fx_result
---@field take string
---@field source string
---@field path string
---@field warnings? table[]

---@class Steppe.item_consolidate_args
---@field items string[]

---@class Steppe.item_consolidate_commit_args
---@field items string[]
---@field expected string
---@field media table[]

---@class Steppe.item_duplicate_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field to? number

---@class Steppe.item_duplicate_result
---@field id string
---@field linked? string[] (default: [])
---@field warnings? table[]

---@class Steppe.item_explode_takes_args
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field mode? string (default: "tracks")

---@class Steppe.item_explode_takes_result
---@field items string[]
---@field tracks string[]
---@field warnings? table[]

---@class Steppe.item_export_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field path string

---@class Steppe.item_export_result
---@field path string
---@field format string
---@field sample_format? string
---@field sample_rate? number
---@field channels? integer
---@field frames? integer
---@field notes? integer
---@field warnings? table[]

---@class Steppe.item_heal_args
---@field items string[]

---@class Steppe.item_mix_to_track_args
---@field items string[]

---@class Steppe.item_mix_to_track_commit_args
---@field items string[]
---@field expected string
---@field start number
---@field end number
---@field media table

---@class Steppe.item_move_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field position? number
---@field bar? integer
---@field track? string Track id, e.g. "tr_1".

---@class Steppe.item_move_result
---@field warnings? table[]

---@class Steppe.item_paste_args
---@field version? integer
---@field items table[]

---@class Steppe.item_paste_result
---@field items string[]
---@field warnings? table[]

---@class Steppe.item_remove_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).

---@class Steppe.item_remove_result
---@field warnings? table[]

---@class Steppe.item_set_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field position? number
---@field bar? integer
---@field length? number
---@field mute? boolean
---@field loop? boolean
---@field locked? boolean The arrange view marks locked items.
---@field gain_db? number|string Quantity strings are range-checked by the server.
---@field fade_in? number
---@field fade_in_shape? "linear"|"slow"|"fast"|"slow_fast"|"fast_slow"|"cos"|"exp"
---@field fade_out_shape? "linear"|"slow"|"fast"|"slow_fast"|"fast_slow"|"cos"|"exp"
---@field fade_out? number
---@field name? string
---@field color? string
---@field timebase? "track"|"time"|"beats"|"beats_position"

---@class Steppe.item_set_result
---@field warnings? table[]

---@class Steppe.item_split_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field at number
---@field snap? "none"|"zero_crossing" (default: "none")

---@class Steppe.item_split_result
---@field left string
---@field right string
---@field linked_left? string[] (default: [])
---@field linked_right? string[] (default: [])
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

---@class Steppe.key_remove_args
---@field at number

---@class Steppe.key_remove_result
---@field warnings? table[]

---@class Steppe.key_set_args
---@field at number
---@field tonic? string
---@field scale? string
---@field to? number

---@class Steppe.key_set_result
---@field id string
---@field warnings? table[]

---@class Steppe.launch_arrangement_args
---@field track? string Track id, e.g. "tr_1".

---@class Steppe.launch_arrangement_result
---@field action string
---@field tracks table[]
---@field beat? number
---@field seconds? number
---@field started boolean
---@field warnings? table[]

---@class Steppe.launch_capture_args
---@field on boolean

---@class Steppe.launch_capture_result
---@field on boolean
---@field from? number
---@field to? number
---@field warnings? table[]

---@class Steppe.launch_clip_args
---@field clip string Clip id, e.g. "cl_1".

---@class Steppe.launch_clip_result
---@field action string
---@field tracks table[]
---@field beat? number
---@field seconds? number
---@field started boolean
---@field warnings? table[]

---@class Steppe.launch_commit_args
---@field pooled? boolean Share the clips' MIDI with the items (pooled) instead of copying it (default: false)
---@field performance? table What history records; launch.commit fills it from the capture

---@class Steppe.launch_commit_result
---@field tracks string[]
---@field items string[]
---@field sections table[]
---@field mismatches? table[] Each launched stretch of a MIDI clip copied under a key that is not the clip's, as its context.mismatch warning says it.
---@field from number
---@field to number
---@field warnings? table[]

---@class Steppe.launch_scene_args
---@field scene string Scene id, e.g. "sc_1".

---@class Steppe.launch_scene_result
---@field action string
---@field tracks table[]
---@field beat? number
---@field seconds? number
---@field started boolean
---@field warnings? table[]

---@class Steppe.launch_state_result
---@field playing boolean
---@field capturing boolean
---@field tracks table[]
---@field warnings? table[]

---@class Steppe.launch_stop_args
---@field track? string Track id, e.g. "tr_1".

---@class Steppe.launch_stop_result
---@field action string
---@field tracks table[]
---@field beat? number
---@field seconds? number
---@field started boolean
---@field warnings? table[]

---@class Steppe.log_list_args
---@field limit? integer
---@field since? integer
---@field queries? boolean (default: false)
---@field origin? string

---@class Steppe.marker_add_args
---@field pos? number
---@field name? string
---@field color? string

---@class Steppe.marker_add_result
---@field id string
---@field warnings? table[]

---@class Steppe.marker_list_result
---@field markers table[]
---@field warnings? table[]

---@class Steppe.marker_remove_args
---@field id string Marker id, e.g. "mk_1".

---@class Steppe.marker_remove_result
---@field warnings? table[]

---@class Steppe.marker_set_args
---@field id string Marker id, e.g. "mk_1".
---@field pos? number
---@field name? string
---@field color? string

---@class Steppe.marker_set_result
---@field warnings? table[]

---@class Steppe.midi_edit_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field ops table[]

---@class Steppe.midi_get_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field sounding? boolean Answer what sounds (a following take's notes as they play), in item time, not what is written. A clip is refused: midi.sounding_clip. (default: false)
---@field from? number With sounding: the first song beat; the heard notes of from..to answer, passes as they fall.
---@field to? number With sounding: the end song beat.

---@class Steppe.midi_get_result
---@field text string
---@field context? table
---@field follow? string
---@field take_context? string
---@field sounding_differs? integer
---@field differs_by_pass? boolean
---@field warnings? table[]

---@class Steppe.midi_inputs_set_args
---@field port string
---@field enabled? boolean
---@field offset_ms? number

---@class Steppe.midi_notes_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field sounding? boolean Add each note's sounding pitch as `sounds` (a following take's notes as they play; the first pass of a looped item, and differs_by_pass when later passes differ). `pitch` stays the written one. A clip is refused: midi.sounding_clip. (default: false)

---@class Steppe.midi_outputs_set_args
---@field port string
---@field enabled? boolean
---@field offset_ms? number
---@field reset_on_stop? boolean
---@field clock? boolean
---@field transport? boolean
---@field spp? boolean
---@field mtc? boolean
---@field clock_while_stopped? boolean

---@class Steppe.midi_write_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field notation string

---@class Steppe.midi_write_result
---@field follows? string
---@field warnings? table[]

---@class Steppe.modulate_args
---@field target table
---@field source string|table
---@field depth? number
---@field from? string The track a follower listens to.
---@field rack? string Rack id, e.g. "rk_1".

---@class Steppe.modulate_result
---@field modulation string
---@field param? string A plug-in target's canonical reference.
---@field param_name? string A plug-in target's display name.
---@field rack? string
---@field module? string
---@field mod_out? string
---@field input? string
---@field send? string
---@field channel? string The channel made for a new rack.
---@field knob? string A macro's knob to move, by key.
---@field warnings? table[]

---@class Steppe.modulation_add_args
---@field source table
---@field target table
---@field depth number
---@field enabled? boolean
---@field lane? boolean Show the target's automation lane in the timeline while the route exists (default true). It never changes the sound.

---@class Steppe.modulation_add_result
---@field modulation string
---@field param? string A plug-in target's canonical reference: its semantic key or decimal id.
---@field param_name? string A plug-in target's display name.
---@field warnings? table[]

---@class Steppe.modulation_list_args
---@field track? string Target track id, e.g. "tr_1".
---@field fx? string Target plug-in instance id, e.g. "fx_1".
---@field rack? string Source rack id, e.g. "rk_1".

---@class Steppe.modulation_list_result
---@field modulations table[]
---@field warnings? table[]

---@class Steppe.modulation_remove_args
---@field modulation string Modulation route id, e.g. "mr_1".

---@class Steppe.modulation_remove_result
---@field warnings? table[]

---@class Steppe.modulation_set_args
---@field modulation string Modulation route id, e.g. "mr_1".
---@field source? table
---@field target? table
---@field depth? number
---@field enabled? boolean
---@field lane? boolean Show or hide the target's automation lane for this route. It never changes the sound.

---@class Steppe.modulation_set_result
---@field warnings? table[]

---@class Steppe.modulation_telemetry_args
---@field track? string Target track id, e.g. "tr_1".

---@class Steppe.modulation_telemetry_result
---@field modulations table[]
---@field warnings? table[]

---@class Steppe.module_add_args
---@field rack string
---@field type? string
---@field label? string
---@field preset? string
---@field x? number
---@field y? number
---@field params? any
---@field source? string|table
---@field device? string
---@field plugin? string

---@class Steppe.module_check_args
---@field name? string
---@field source? string

---@class Steppe.module_copy_args
---@field modules string[]

---@class Steppe.module_describe_args
---@field type? string
---@field module? string

---@class Steppe.module_duplicate_args
---@field modules string[]
---@field dx? number
---@field dy? number
---@field cables? boolean

---@class Steppe.module_edit_args
---@field name? string
---@field type? string
---@field edits table[]

---@class Steppe.module_fork_args
---@field type string
---@field id string
---@field name? string

---@class Steppe.module_get_args
---@field type? string
---@field name? string

---@class Steppe.module_learn_list_args
---@field module? string

---@class Steppe.module_learn_remove_args
---@field module string
---@field param string

---@class Steppe.module_learn_set_args
---@field module string
---@field param string
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean

---@class Steppe.module_list_args
---@field query? string
---@field tag? string

---@class Steppe.module_randomize_args
---@field module string
---@field amount? number
---@field seed? integer

---@class Steppe.module_remove_args
---@field module string

---@class Steppe.module_set_args
---@field module string
---@field params? any
---@field label? string
---@field preset? string
---@field x? number
---@field y? number
---@field bypass? boolean
---@field source? any
---@field device? any
---@field source_sends? boolean

---@class Steppe.module_write_args
---@field name string
---@field source string

---@class Steppe.plugin_list_args
---@field filter? string

---@class Steppe.plugin_list_result
---@field plugins table[]
---@field scan table
---@field warnings? table[]

---@class Steppe.plugin_override_args
---@field plugin string id or name, as plugin.list shows it
---@field mode "normal"|"bridge"|"disable"

---@class Steppe.plugin_override_result
---@field plugin string
---@field name string
---@field format string
---@field mode "normal"|"bridge"|"disable"
---@field changed boolean false: it was set that way already
---@field fx string[] the fx of this plug-in in the project now
---@field warnings? table[]

---@class Steppe.plugin_overrides_result
---@field overrides table[]
---@field bridge_available boolean
---@field warnings? table[]

---@class Steppe.plugin_scan_args
---@field wait? boolean false: scan in the background (default true)

---@class Steppe.plugin_scan_result
---@field plugins table[]
---@field scan table
---@field job? string the background scan (wait: false)
---@field warnings? table[]

---@class Steppe.project_get_result
---@field name string
---@field path string
---@field branch string
---@field branches string[] Every branch the project holds; the file keeps them all
---@field sample_rate number
---@field tempo number
---@field meter string
---@field timecode table The timecode frame rate and the label at project time 0
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field recoverable_sessions integer Unsaved projects earlier runs left behind (project.recoverable lists them)
---@field warnings? table[]

---@class Steppe.project_new_args
---@field name? string
---@field sample_rate? number Hz; default 48000.

---@class Steppe.project_new_result
---@field name string
---@field path string
---@field branch string
---@field branches string[] Every branch the project holds; the file keeps them all
---@field sample_rate number
---@field tempo number
---@field meter string
---@field timecode table The timecode frame rate and the label at project time 0
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field recoverable_sessions integer Unsaved projects earlier runs left behind (project.recoverable lists them)
---@field warnings? table[]

---@class Steppe.project_open_args
---@field path? string
---@field expect_hash? string The hash project.plugins returned: the file is opened only if its bytes are those it checked (project.changed otherwise)

---@class Steppe.project_open_result
---@field name string
---@field path string
---@field branch string
---@field branches string[] Every branch the project holds; the file keeps them all
---@field sample_rate number
---@field tempo number
---@field meter string
---@field timecode table The timecode frame rate and the label at project time 0
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field recoverable_sessions integer Unsaved projects earlier runs left behind (project.recoverable lists them)
---@field warnings? table[]

---@class Steppe.project_plugins_args
---@field path? string
---@field template? boolean
---@field session? string A session project.recoverable lists
---@field autosave? boolean The open project's newer autosave
---@field autosave_file? string With autosave: which one, as offered

---@class Steppe.project_recover_args
---@field files? string[]
---@field track? string Track id, e.g. "tr_1".
---@field session? string A session project.recoverable lists
---@field autosave? boolean The saved project's newer autosave (default: false)
---@field autosave_file? string With autosave: which one, e.g. autosave-3.json, as offered

---@class Steppe.project_recover_result
---@field recovered table[]
---@field autosave? string The autosave it opened
---@field warnings? table[]

---@class Steppe.project_recoverable_result
---@field warnings? table[]
---@field recordings table[]
---@field sessions table[]

---@class Steppe.project_recoverable_delete_args
---@field session string

---@class Steppe.project_recoverable_delete_result
---@field session string
---@field warnings? table[]

---@class Steppe.project_save_args
---@field path? string
---@field history? boolean Also keep undo and redo history (ADR-0076) (default: false)

---@class Steppe.project_save_result
---@field path string
---@field warnings? table[]

---@class Steppe.project_set_args
---@field name? string
---@field title? string
---@field author? string
---@field notes? string
---@field console9? boolean
---@field pan_law? table Channels' and busses' pan law unless a track sets its own: db is the centre's cut (0, -2.5, -3, -4.5 or -6; default 0); boost keeps the centre at 0 dB and raises the sides instead
---@field history_budget_bytes? integer Estimated retained history byte budget; default 1 GiB. Leases and branch boundaries may exceed it.
---@field render? table Render settings (session-model §1.10); every member optional
---@field timebase? "time"|"beats"|"beats_position"
---@field timecode? table

---@class Steppe.project_set_result
---@field warnings? table[]

---@class Steppe.project_template_delete_args
---@field name string

---@class Steppe.project_template_delete_result
---@field path string
---@field warnings? table[]

---@class Steppe.project_template_list_result
---@field templates table[]
---@field warnings? table[]

---@class Steppe.project_template_load_args
---@field name string

---@class Steppe.project_template_load_result
---@field name string
---@field warnings? table[]

---@class Steppe.project_template_save_args
---@field name string
---@field replace? boolean (default: false)

---@class Steppe.project_template_save_result
---@field path string
---@field warnings? table[]

---@class Steppe.rack_add_args
---@field track? string
---@field name? string

---@class Steppe.rack_arrange_args
---@field rack string
---@field modules? string[]
---@field x? number
---@field y? number

---@class Steppe.rack_capture_args
---@field async? boolean (default: false)
---@field rack string Rack id
---@field pitch string
---@field gate string
---@field velocity? string
---@field track string Track id
---@field beats? number
---@field from_beat? number
---@field at? number
---@field quantize? number
---@field notes? string
---@field name? string

---@class Steppe.rack_capture_commit_args
---@field track string
---@field source table
---@field name string
---@field at number
---@field beats number
---@field voices integer
---@field warnings string[]

---@class Steppe.rack_get_args
---@field rack string
---@field detail? string

---@class Steppe.rack_levels_args
---@field rack string

---@class Steppe.rack_paste_args
---@field rack string
---@field x? number
---@field y? number

---@class Steppe.rack_patch_args
---@field rack string
---@field modules any
---@field cables? any[]
---@field x? number
---@field y? number

---@class Steppe.rack_preset_delete_args
---@field name string

---@class Steppe.rack_preset_list_args
---@field query? string
---@field factory? boolean

---@class Steppe.rack_preset_load_args
---@field rack string
---@field name? string
---@field path? string
---@field x? number
---@field y? number
---@field io? string

---@class Steppe.rack_preset_save_args
---@field rack string
---@field name string
---@field modules? string[]
---@field description? string
---@field replace? boolean

---@class Steppe.rack_probe_args
---@field rack string Rack id
---@field ports string[]
---@field seconds? number
---@field from_beat? number
---@field notes? string

---@class Steppe.rack_remove_args
---@field rack string

---@class Steppe.rack_set_args
---@field rack string
---@field name? string
---@field track? string

---@class Steppe.record_commit_args
---@field takes table[]
---@field punch table
---@field stack? boolean A pass over an item becomes a take of it (ADR-0103) (default: false)

---@class Steppe.record_get_result
---@field stack_takes boolean
---@field warnings? table[]

---@class Steppe.record_set_args
---@field stack_takes? boolean

---@class Steppe.record_set_result
---@field stack_takes boolean
---@field warnings? table[]

---@class Steppe.region_add_args
---@field start? number
---@field end? number
---@field name? string
---@field color? string
---@field section? boolean Whether the region is a section of the song, which context.at and session.describe report [true]

---@class Steppe.region_add_result
---@field id string
---@field warnings? table[]

---@class Steppe.region_list_result
---@field regions table[]
---@field warnings? table[]

---@class Steppe.region_remove_args
---@field id string Region id, e.g. "rg_1".

---@class Steppe.region_remove_result
---@field warnings? table[]

---@class Steppe.region_set_args
---@field id string Region id, e.g. "rg_1".
---@field start? number
---@field end? number
---@field name? string
---@field color? string
---@field section? boolean Whether the region is a section of the song, which context.at and session.describe report [true]

---@class Steppe.region_set_result
---@field warnings? table[]

---@class Steppe.registry_describe_args
---@field command string

---@class Steppe.registry_groups_args
---@field group? string

---@class Steppe.registry_list_args
---@field tag? string
---@field kind? string
---@field text? string
---@field group? string

---@class Steppe.render_dry_run_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? string|table File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? string|string|"track" 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
---@field outputs? table[] 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]
---@field normalize? table
---@field limit_true_peak_db? number A true-peak ceiling (dBTP)
---@field trim? table
---@field seamless_loop? boolean Sum the tail into the start (needs tail_s)
---@field handles_s? number Regions: seconds added at both ends
---@field stems? table
---@field matrix? table<string, string[]>
---@field if_exists? "overwrite"|"number"|"skip"|"error"
---@field metadata? table Tags written into each file; text members accept the file-name tokens
---@field report? boolean Write <file>.loudness.txt beside each file
---@field add_to_project? boolean
---@field master? string Mix: one master for this call (default: the settings' masters, else tr_0)
---@field branch? string Render this branch's head (default: the current one)
---@field version? integer Render the session as of this version (history.list); the render queue pins its renders
---@field name? string A file name instead of the pattern, for this call
---@field operation? "mix"|"stems"|"regions" Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source)
---@field regions? string[] Regions: region ids (default: all, with the render matrix)
---@field tracks? string[] Stems: track ids (sets stems.tracks for this call)
---@field format? "wav"|"aiff"|"flac"|"mp3"|"vorbis" Sets outputs[0].format for this call
---@field bit_depth? integer Sets outputs[0].bit_depth for this call
---@field secondary? any Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call
---@field mode? "isolated"|"through_master" Sets stems.mode for this call
---@field tap? "post_fader"|"pre_fader"|"pre_fx" Sets stems.tap for this call
---@field target? number Sets normalize.target for this call
---@field analyze? "summary"|"full" Mix: a supple.analysis-report/1.0 inline
---@field job? boolean (default: false)

---@class Steppe.render_mix_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? string|table File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? string|string|"track" 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
---@field outputs? table[] 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]
---@field normalize? table
---@field limit_true_peak_db? number A true-peak ceiling (dBTP)
---@field trim? table
---@field seamless_loop? boolean Sum the tail into the start (needs tail_s)
---@field handles_s? number Regions: seconds added at both ends
---@field stems? table
---@field matrix? table<string, string[]>
---@field if_exists? "overwrite"|"number"|"skip"|"error"
---@field metadata? table Tags written into each file; text members accept the file-name tokens
---@field report? boolean Write <file>.loudness.txt beside each file
---@field add_to_project? boolean
---@field master? string Mix: one master for this call (default: the settings' masters, else tr_0)
---@field branch? string Render this branch's head (default: the current one)
---@field version? integer Render the session as of this version (history.list); the render queue pins its renders
---@field name? string A file name instead of the pattern, for this call
---@field operation? "mix"|"stems"|"regions" Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source)
---@field regions? string[] Regions: region ids (default: all, with the render matrix)
---@field tracks? string[] Stems: track ids (sets stems.tracks for this call)
---@field format? "wav"|"aiff"|"flac"|"mp3"|"vorbis" Sets outputs[0].format for this call
---@field bit_depth? integer Sets outputs[0].bit_depth for this call
---@field secondary? any Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call
---@field mode? "isolated"|"through_master" Sets stems.mode for this call
---@field tap? "post_fader"|"pre_fader"|"pre_fx" Sets stems.tap for this call
---@field target? number Sets normalize.target for this call
---@field analyze? "summary"|"full" Mix: a supple.analysis-report/1.0 inline
---@field job? boolean (default: false)

---@class Steppe.render_preset_delete_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)

---@class Steppe.render_preset_load_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)

---@class Steppe.render_preset_save_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)
---@field settings? table Render settings (session-model §1.10); every member optional

---@class Steppe.render_queue_add_args
---@field command string
---@field args any
---@field label? string

---@class Steppe.render_queue_remove_args
---@field id? string
---@field all? boolean (default: false)

---@class Steppe.render_queue_run_args
---@field job? boolean (default: false)

---@class Steppe.render_regions_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? string|table File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? string|string|"track" 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
---@field outputs? table[] 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]
---@field normalize? table
---@field limit_true_peak_db? number A true-peak ceiling (dBTP)
---@field trim? table
---@field seamless_loop? boolean Sum the tail into the start (needs tail_s)
---@field handles_s? number Regions: seconds added at both ends
---@field stems? table
---@field matrix? table<string, string[]>
---@field if_exists? "overwrite"|"number"|"skip"|"error"
---@field metadata? table Tags written into each file; text members accept the file-name tokens
---@field report? boolean Write <file>.loudness.txt beside each file
---@field add_to_project? boolean
---@field master? string Mix: one master for this call (default: the settings' masters, else tr_0)
---@field branch? string Render this branch's head (default: the current one)
---@field version? integer Render the session as of this version (history.list); the render queue pins its renders
---@field name? string A file name instead of the pattern, for this call
---@field operation? "mix"|"stems"|"regions" Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source)
---@field regions? string[] Regions: region ids (default: all, with the render matrix)
---@field tracks? string[] Stems: track ids (sets stems.tracks for this call)
---@field format? "wav"|"aiff"|"flac"|"mp3"|"vorbis" Sets outputs[0].format for this call
---@field bit_depth? integer Sets outputs[0].bit_depth for this call
---@field secondary? any Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call
---@field mode? "isolated"|"through_master" Sets stems.mode for this call
---@field tap? "post_fader"|"pre_fader"|"pre_fx" Sets stems.tap for this call
---@field target? number Sets normalize.target for this call
---@field analyze? "summary"|"full" Mix: a supple.analysis-report/1.0 inline
---@field job? boolean (default: false)

---@class Steppe.render_settings_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? string|table File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? string|string|"track" 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
---@field outputs? table[] 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]
---@field normalize? table
---@field limit_true_peak_db? number A true-peak ceiling (dBTP)
---@field trim? table
---@field seamless_loop? boolean Sum the tail into the start (needs tail_s)
---@field handles_s? number Regions: seconds added at both ends
---@field stems? table
---@field matrix? table<string, string[]>
---@field if_exists? "overwrite"|"number"|"skip"|"error"
---@field metadata? table Tags written into each file; text members accept the file-name tokens
---@field report? boolean Write <file>.loudness.txt beside each file
---@field add_to_project? boolean
---@field master? string Mix: one master for this call (default: the settings' masters, else tr_0)
---@field branch? string Render this branch's head (default: the current one)
---@field version? integer Render the session as of this version (history.list); the render queue pins its renders
---@field name? string A file name instead of the pattern, for this call
---@field operation? "mix"|"stems"|"regions" Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source)
---@field regions? string[] Regions: region ids (default: all, with the render matrix)
---@field tracks? string[] Stems: track ids (sets stems.tracks for this call)
---@field format? "wav"|"aiff"|"flac"|"mp3"|"vorbis" Sets outputs[0].format for this call
---@field bit_depth? integer Sets outputs[0].bit_depth for this call
---@field secondary? any Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call
---@field mode? "isolated"|"through_master" Sets stems.mode for this call
---@field tap? "post_fader"|"pre_fader"|"pre_fx" Sets stems.tap for this call
---@field target? number Sets normalize.target for this call
---@field analyze? "summary"|"full" Mix: a supple.analysis-report/1.0 inline
---@field job? boolean (default: false)

---@class Steppe.render_stems_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? string|table File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? string|string|"track" 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
---@field outputs? table[] 1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]
---@field normalize? table
---@field limit_true_peak_db? number A true-peak ceiling (dBTP)
---@field trim? table
---@field seamless_loop? boolean Sum the tail into the start (needs tail_s)
---@field handles_s? number Regions: seconds added at both ends
---@field stems? table
---@field matrix? table<string, string[]>
---@field if_exists? "overwrite"|"number"|"skip"|"error"
---@field metadata? table Tags written into each file; text members accept the file-name tokens
---@field report? boolean Write <file>.loudness.txt beside each file
---@field add_to_project? boolean
---@field master? string Mix: one master for this call (default: the settings' masters, else tr_0)
---@field branch? string Render this branch's head (default: the current one)
---@field version? integer Render the session as of this version (history.list); the render queue pins its renders
---@field name? string A file name instead of the pattern, for this call
---@field operation? "mix"|"stems"|"regions" Dry run and render.settings: mix, stems or regions (default: tracks or regions given, else the settings' source)
---@field regions? string[] Regions: region ids (default: all, with the render matrix)
---@field tracks? string[] Stems: track ids (sets stems.tracks for this call)
---@field format? "wav"|"aiff"|"flac"|"mp3"|"vorbis" Sets outputs[0].format for this call
---@field bit_depth? integer Sets outputs[0].bit_depth for this call
---@field secondary? any Replaces outputs[1] ({format, bit_depth}), or "none" removes it, for this call
---@field mode? "isolated"|"through_master" Sets stems.mode for this call
---@field tap? "post_fader"|"pre_fader"|"pre_fx" Sets stems.tap for this call
---@field target? number Sets normalize.target for this call
---@field analyze? "summary"|"full" Mix: a supple.analysis-report/1.0 inline
---@field job? boolean (default: false)

---@class Steppe.route_add_args
---@field from string|table
---@field to? string|table
---@field to_midi? string Exact hardware MIDI output name; requires midi: {} and replaces to/fx.
---@field midi? table MIDI send routing; {} means every channel unchanged. Channel values 0..16: from 0 = all, to 0 = original.
---@field fx? string Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out
---@field tap? "post_fader"|"pre_fader"|"pre_fx"
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? string|table
---@field mute? boolean
---@field mono? boolean Sum the source to mono before the pan
---@field phase_invert? boolean
---@field channels? table Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity

---@class Steppe.route_add_result
---@field id string
---@field warnings? table[]

---@class Steppe.route_learn_list_args
---@field route? string

---@class Steppe.route_learn_list_result
---@field learn any
---@field warnings? table[]

---@class Steppe.route_learn_remove_args
---@field route string
---@field param string

---@class Steppe.route_learn_remove_result
---@field warnings? table[]

---@class Steppe.route_learn_set_args
---@field route string
---@field param string The control: volume, pan, mute, phase_invert, mono, tap:post_fader, tap:pre_fader, tap:pre_fx
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean

---@class Steppe.route_learn_set_result
---@field param string
---@field control string
---@field warnings? table[]

---@class Steppe.route_list_result
---@field routes table[]
---@field warnings? table[]

---@class Steppe.route_matrix_args
---@field mode? string (default: "audio")

---@class Steppe.route_remove_args
---@field route string Route id, e.g. "sd_1".

---@class Steppe.route_remove_result
---@field warnings? table[]

---@class Steppe.route_set_args
---@field route string Route id, e.g. "sd_1".
---@field from? string|table
---@field to? string|table
---@field to_midi? string Exact hardware MIDI output name; requires midi: {} and replaces to/fx.
---@field midi? table MIDI send routing; {} means every channel unchanged. Channel values 0..16: from 0 = all, to 0 = original.
---@field fx? string Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out
---@field tap? "post_fader"|"pre_fader"|"pre_fx"
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? string|table
---@field mute? boolean
---@field mono? boolean Sum the source to mono before the pan
---@field phase_invert? boolean
---@field channels? table Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity

---@class Steppe.route_set_result
---@field warnings? table[]

---@class Steppe.scene_add_args
---@field name? string
---@field color? string
---@field at? integer Column, 0-based.

---@class Steppe.scene_add_result
---@field id string
---@field warnings? table[]

---@class Steppe.scene_capture_args
---@field region string Region id, e.g. "rg_1".
---@field name? string
---@field pooled? boolean (default: false)

---@class Steppe.scene_capture_result
---@field scene string
---@field clips table[]
---@field warnings? table[]

---@class Steppe.scene_insert_args
---@field scene string Scene id, e.g. "sc_1".
---@field at? number Start in beats, on a bar line.
---@field bar? integer Start bar, 1-based.
---@field bars integer
---@field name? string
---@field pooled? boolean (default: false)
---@field follow? "none"|"key"|"chords" How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): none, key or chords. Default: the clip's own follow.
---@field write_context? boolean Write the scene's clips' own key and chords onto the context track under each section that has none (shifted to the section, repeating with a loop clip), in the same undo step, before the clips are laid. Left alone: a section with points under it; not written: one whose clips disagree. The answer's context says which (default false). (default: false)

---@class Steppe.scene_insert_result
---@field items table[]
---@field region string
---@field start number The span's start, in beats.
---@field end number The span's end, in beats.
---@field context? table
---@field warnings? table[]

---@class Steppe.scene_list_result
---@field scenes table[]
---@field warnings? table[]

---@class Steppe.scene_move_args
---@field scene string Scene id, e.g. "sc_1".
---@field to integer Column.

---@class Steppe.scene_move_result
---@field warnings? table[]

---@class Steppe.scene_remove_args
---@field scene string Scene id, e.g. "sc_1".

---@class Steppe.scene_remove_result
---@field warnings? table[]

---@class Steppe.scene_set_args
---@field scene string Scene id, e.g. "sc_1".
---@field name? string
---@field color? string

---@class Steppe.scene_set_result
---@field warnings? table[]

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

---@class Steppe.session_describe_result
---@field text string
---@field warnings? table[]

---@class Steppe.sync_get_result
---@field settings any
---@field status table
---@field warnings? table[]

---@class Steppe.sync_set_args
---@field source? "off"|"midi_clock"|"mtc"|"ltc"
---@field midi_input? string
---@field ltc_input? integer
---@field follow_tempo? boolean
---@field offset_ms? number
---@field on_loss? "stop"|"freewheel"
---@field freewheel_frames? integer
---@field ltc_output? boolean
---@field ltc_output_channel? integer
---@field ltc_level_db? number

---@class Steppe.sync_set_result
---@field settings any
---@field status table
---@field warnings? table[]

---@class Steppe.take_activate_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field take? string Take id, e.g. "tk_1".
---@field index? integer

---@class Steppe.take_activate_result
---@field warnings? table[]

---@class Steppe.take_add_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".
---@field from? string Take id, e.g. "tk_1".

---@class Steppe.take_add_result
---@field id string
---@field warnings? table[]

---@class Steppe.take_bake_args
---@field take string Take id, e.g. "tk_1".
---@field pitches? table[] Recorded by history, not given: each heard note's written pitch, where it is heard and the pitch it sounded as, so a replay writes what was heard. A replay that finds other notes is take.bake_replay.

---@class Steppe.take_bake_result
---@field followed table
---@field unrolled boolean
---@field warnings? table[]

---@class Steppe.take_comp_args
---@field item? string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field items? string[]
---@field spans table[]
---@field crossfade? number Seconds. (default: 0.01)
---@field crossfade_shape? "cos"|"linear" (default: "cos")
---@field bypass_groups? boolean (default: false)

---@class Steppe.take_comp_result
---@field pieces table[]
---@field crossfade number Seconds.
---@field crossfade_shape "cos"|"linear"
---@field uniform boolean
---@field differing_seams table[]
---@field members table[]
---@field differing_members string[]
---@field warnings? table[]

---@class Steppe.take_comp_get_args
---@field item? string Item id, e.g. "it_1" (a clip's id is refused with item.kind).
---@field items? string[]

---@class Steppe.take_comp_get_result
---@field pieces table[]
---@field crossfade number Seconds.
---@field crossfade_shape "cos"|"linear"
---@field uniform boolean
---@field differing_seams table[]
---@field members table[]
---@field differing_members string[]
---@field warnings? table[]

---@class Steppe.take_extract_args
---@field take string Take id, e.g. "tk_1".
---@field track? string Default: the item's or clip's track.
---@field position? number Beats.

---@class Steppe.take_extract_result
---@field item string
---@field warnings? table[]

---@class Steppe.take_hits_detect_args
---@field takes string[]
---@field sensitivity? number (default: 0.5)

---@class Steppe.take_hits_detect_result
---@field job string
---@field warnings? table[]

---@class Steppe.take_hits_set_args
---@field take string
---@field positions number[]
---@field source_hash? string (default: "")
---@field algorithm? string (default: "manual")
---@field sensitivity? number (default: 0.5)

---@class Steppe.take_list_args
---@field item string Item or clip id, e.g. "it_1" or "cl_1".

---@class Steppe.take_list_result
---@field takes table[]
---@field warnings? table[]

---@class Steppe.take_remove_args
---@field take string Take id, e.g. "tk_1".

---@class Steppe.take_remove_result
---@field item string
---@field item_removed boolean
---@field warnings? table[]

---@class Steppe.take_set_args
---@field take string Take id, e.g. "tk_1".
---@field gain_db? number|string Quantity strings are range-checked by the server.
---@field pan? number|string Quantity strings are range-checked by the server.
---@field channel_mode? "normal"|"reverse_stereo"|"mono_mix"|"left"|"right"
---@field name? string
---@field rebase_seconds? number Rebase an audio take after trimming its item start; preserves the warp map.
---@field start_offset? number Source position at the item start; negative for a late first pass.
---@field playrate? number|string Playback speed: 2 plays the source twice as fast. Its length in seconds is the item's, so the take runs out sooner or loops. Quantity strings are range-checked by the server.
---@field preserve_pitch? boolean Keep the source's pitch when the playrate changes (the default). With pitch_mode "resample" the pitch follows the speed.
---@field pitch? number|string Pitch offset in semitones. Stretched when the pitch is preserved, otherwise a further speed factor of 2^(pitch/12). Quantity strings are range-checked by the server.
---@field pitch_mode? "resample"|"signalsmith" Time-stretch algorithm: "signalsmith" (pitch-preserving, the default) or "resample" (varispeed).
---@field context? string A MIDI take's lead sheet, as context.write's text: bars of the meter at its item's position, bar 1 at context beat 0, in context beats (the take's playrate does not scale them); "" clears it. A point at or past the context's period is take.context_range.
---@field follow? "none"|"key"|"chords" How the take's notes sound against the project's context (ADR-0098, following is playback): none plays them as written, key or chords maps them as they play. Needs a MIDI take with a readable key in its context. The notes stay as written; take.bake writes them.

---@class Steppe.take_set_result
---@field warnings? table[]

---@class Steppe.take_stretch_add_args
---@field take string
---@field source_pos? number
---@field item_pos number

---@class Steppe.take_stretch_move_args
---@field take string
---@field index integer
---@field item_pos number

---@class Steppe.take_stretch_remove_args
---@field take string
---@field index integer

---@class Steppe.tempo_set_args
---@field points table[]

---@class Steppe.tempo_set_result
---@field count integer
---@field warnings? table[]

---@class Steppe.time_convert_args
---@field seconds? number
---@field beat? number
---@field bar? integer

---@class Steppe.time_convert_result
---@field seconds number
---@field beat number
---@field bar integer
---@field beat_in_bar integer
---@field fraction number
---@field warnings? table[]

---@class Steppe.timesig_remove_args
---@field bar integer

---@class Steppe.timesig_remove_result
---@field warnings? table[]

---@class Steppe.timesig_set_args
---@field bar integer
---@field meter string

---@class Steppe.timesig_set_result
---@field warnings? table[]

---@class Steppe.track_add_args
---@field type? "channel"|"buss"|"master" (default: "channel")
---@field name? string
---@field color? string
---@field parent? string Parent track id, e.g. "tr_1".
---@field index? integer
---@field channels? integer

---@class Steppe.track_add_result
---@field track string
---@field warnings? table[]

---@class Steppe.track_describe_args
---@field track string
---@field at? number

---@class Steppe.track_describe_result
---@field track string
---@field name string
---@field type "channel"|"buss"|"master"
---@field groups string[]
---@field vcas string[]
---@field volume_db number
---@field requested_effective_volume_db number
---@field effective_volume_db number
---@field at number
---@field effective_mute boolean
---@field solo_silenced boolean
---@field warnings? table[]

---@class Steppe.track_duplicate_args
---@field tracks string[]

---@class Steppe.track_duplicate_result
---@field tracks string[]
---@field warnings? table[]

---@class Steppe.track_freeze_args
---@field track string
---@field job? boolean (default: false)

---@class Steppe.track_freeze_commit_args
---@field track string
---@field expected string
---@field media table
---@field freeze table
---@field states table[]

---@class Steppe.track_learn_list_args
---@field track? string

---@class Steppe.track_learn_list_result
---@field learn any
---@field warnings? table[]

---@class Steppe.track_learn_remove_args
---@field track string
---@field param string

---@class Steppe.track_learn_remove_result
---@field warnings? table[]

---@class Steppe.track_learn_set_args
---@field track string
---@field param string The control: volume, pan, width, pan_left, pan_right, pan_boost, mute, solo, phase_invert, mono, armed, monitor, lane_mute
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean

---@class Steppe.track_learn_set_result
---@field param string
---@field control string
---@field warnings? table[]

---@class Steppe.track_list_args
---@field at? number

---@class Steppe.track_list_result
---@field tracks table[]
---@field warnings? table[]

---@class Steppe.track_move_args
---@field track string Track id, e.g. "tr_1".
---@field parent? string Track id, e.g. "tr_1".
---@field index? integer

---@class Steppe.track_move_result
---@field warnings? table[]

---@class Steppe.track_remove_args
---@field track string Track id, e.g. "tr_1".

---@class Steppe.track_remove_result
---@field warnings? table[]

---@class Steppe.track_set_args
---@field bypass_groups? boolean (default: false)
---@field track string Track id, e.g. "tr_1".
---@field type? "channel"|"buss"|"master"
---@field name? string
---@field color? string
---@field notes? string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? number|string Quantity strings are range-checked by the server.
---@field pan_mode? "pan"|"dual"|"width" Channels and busses: pan (balance a stereo image, pan a mono one), dual (pan_left and pan_right place each input on its own) or width (width narrows, widens or swaps the image, pan places it)
---@field width? number|string Width mode: 100 as recorded, 0 mono, -100 sides swapped Quantity strings are range-checked by the server.
---@field pan_left? number|string Dual mode: where the left input goes (default -100) Quantity strings are range-checked by the server.
---@field pan_right? number|string Dual mode: where the right input goes (default 100) Quantity strings are range-checked by the server.
---@field pan_law? any
---@field mute? boolean
---@field phase_invert? boolean Flip the polarity of the main pair (after the FX chain, before the fader)
---@field mono? boolean Sum the main pair to mono (after the FX chain, before the fader and pan): a mono check on the master
---@field solo? string
---@field armed? boolean
---@field record_input? any Hardware audio input; replaces MIDI or rack input. null clears hardware audio only
---@field record_midi? any MIDI device; replaces hardware audio or rack input. null clears MIDI only
---@field record_rack? any
---@field midi_channel? integer
---@field record_mode? "input"|"midi_overdub"|"midi_replace"
---@field monitor? "off"|"on"|"auto"
---@field parent_send? boolean The implicit send to the parent buss/master (off: only explicit sends leave the track)
---@field timebase? "project"|"time"|"beats"|"beats_position" What this channel's items follow when the tempo changes (those that inherit it): project, beats, time or beats_position; they keep their place now
---@field channels? integer 1-2: the track's main pair; up to 128 adds auxiliary channels 3-128 for routes and plug-in pins (a detector on 3/4, a multi-out instrument's outputs), outside the fader, pan and parent send
---@field hw_output? integer[] Masters: the device outputs the main pair plays to: [l, r], [mono] or [] for none (engine.inputs lists them). Default: tr_0 [1, 2], other masters []
---@field lane_mute? boolean Masters: silence the master's own items (its prints) and nothing else
---@field chain_midi? "bus"|"instrument" What the FX chain does with MIDI: bus (default; plug-ins hear and feed it in order) or instrument (only the first plug-in with a note input hears the track's MIDI; plug-ins' note output goes nowhere)

---@class Steppe.track_set_result
---@field warnings? table[]

---@class Steppe.track_template_delete_args
---@field name string

---@class Steppe.track_template_delete_result
---@field path string
---@field warnings? table[]

---@class Steppe.track_template_list_result
---@field templates table[]
---@field warnings? table[]

---@class Steppe.track_template_load_args
---@field name? string
---@field path? string
---@field parent? string

---@class Steppe.track_template_load_result
---@field track string
---@field effects string[]
---@field warnings? table[]

---@class Steppe.track_template_save_args
---@field track string
---@field name string
---@field replace? boolean (default: false)

---@class Steppe.track_template_save_result
---@field path string
---@field effects integer
---@field warnings? table[]

---@class Steppe.track_unfreeze_args
---@field track string

---@class Steppe.transport_get_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.transport_loop_args
---@field enabled? boolean
---@field clear? boolean
---@field start_seconds? number
---@field start_beat? number
---@field start_bar? integer
---@field end_seconds? number
---@field end_beat? number
---@field end_bar? integer

---@class Steppe.transport_loop_result
---@field enabled boolean
---@field start_seconds number
---@field end_seconds number
---@field warnings? table[]

---@class Steppe.transport_metronome_args
---@field enabled? boolean
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field count_in_bars? integer
---@field count_in_playback? boolean

---@class Steppe.transport_metronome_result
---@field enabled boolean
---@field volume_db number
---@field count_in_bars integer
---@field count_in_playback boolean
---@field warnings? table[]

---@class Steppe.transport_pause_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.transport_play_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.transport_playrate_apply_to_tempo_result
---@field warnings? table[]

---@class Steppe.transport_playrate_get_result
---@field requested_rate number
---@field effective_rate number
---@field preserve_pitch boolean
---@field envelope? string
---@field warnings? table[]

---@class Steppe.transport_playrate_set_args
---@field rate? number
---@field preserve_pitch? boolean

---@class Steppe.transport_playrate_set_result
---@field requested_rate number
---@field effective_rate number
---@field preserve_pitch boolean
---@field envelope? string
---@field warnings? table[]

---@class Steppe.transport_punch_args
---@field enabled? boolean
---@field clear? boolean
---@field start_seconds? number
---@field start_beat? number
---@field start_bar? integer
---@field end_seconds? number
---@field end_beat? number
---@field end_bar? integer

---@class Steppe.transport_punch_result
---@field enabled boolean
---@field start_seconds number
---@field end_seconds number
---@field warnings? table[]

---@class Steppe.transport_record_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.transport_seek_args
---@field seconds? number
---@field beat? number
---@field bar? integer

---@class Steppe.transport_seek_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.transport_stop_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]
---@field warnings? table[]

---@class Steppe.vca_add_args
---@field group string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field mute? boolean
---@field solo? boolean
---@field followers? string[]

---@class Steppe.vca_add_result
---@field warnings? table[]

---@class Steppe.vca_learn_list_args
---@field group? string

---@class Steppe.vca_learn_list_result
---@field learn any
---@field warnings? table[]

---@class Steppe.vca_learn_remove_args
---@field group string
---@field param string

---@class Steppe.vca_learn_remove_result
---@field warnings? table[]

---@class Steppe.vca_learn_set_args
---@field group string
---@field param string The control: volume, mute, solo
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean

---@class Steppe.vca_learn_set_result
---@field param string
---@field control string
---@field warnings? table[]

---@class Steppe.vca_members_set_args
---@field group string
---@field tracks? string[]
---@field vcas? string[]

---@class Steppe.vca_members_set_result
---@field warnings? table[]

---@class Steppe.vca_remove_args
---@field group string

---@class Steppe.vca_remove_result
---@field warnings? table[]

---@class Steppe.vca_set_args
---@field group string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field mute? boolean
---@field solo? boolean
---@field followers? string[]

---@class Steppe.vca_set_result
---@field warnings? table[]

---@class Steppe.analyze
---@field compare fun(args: Steppe.analyze_compare_args): Steppe.analyze_compare_result Compare two renders or track outputs by perceptual descriptors.
---@field masking fun(args: Steppe.analyze_masking_args): table Estimate masking between two tracks over the same range.
---@field render fun(args: Steppe.analyze_render_args): table Analyze a render or audio file: loudness, spectrum, envelope, stereo.
---@field track fun(args: Steppe.analyze_track_args): table Render one track's output offline and analyze it.

---@class Steppe.arrangement
---@field build fun(args: Steppe.arrangement_build_args): Steppe.arrangement_build_result Lay a song out from scenes, each for some bars, in order.

---@class Steppe.audio
---@field quantize fun(args: Steppe.audio_quantize_args): table Quantize detected audio hits together using stretch markers or slices.

---@class Steppe.audio.warp
---@field detach fun(args: Steppe.audio_warp_detach_args): table Detach selected items from jointly processed warp bundles.
---@field reset fun(args: Steppe.audio_warp_reset_args): table Remove selected items' warping while keeping detected hits.

---@class Steppe.audition
---@field get fun(): Steppe.audition_get_result Whether an audition plays: path, length, position (seconds).
---@field play fun(args: Steppe.audition_play_args): Steppe.audition_play_result Play an audio file once on outputs 1/2, outside the project.
---@field stop fun(): Steppe.audition_stop_result Stop the file audition (fading out over 5 ms); returns its state.

---@class Steppe.autoitem
---@field add fun(args: Steppe.autoitem_add_args): table Put an automation item on an envelope: a block that replaces it there.
---@field duplicate fun(args: Steppe.autoitem_duplicate_args): table Copy an automation item (pooled by default: edits to either reach both).
---@field list fun(args?: Steppe.autoitem_list_args): table List automation items (all, or one envelope's), with their points.
---@field remove fun(args: Steppe.autoitem_remove_args): Steppe.autoitem_remove_result Remove an automation item (the envelope under it plays again).
---@field set fun(args: Steppe.autoitem_set_args): table Move, resize, loop, scale or rewrite an automation item.
---@field unpool fun(args: Steppe.autoitem_unpool_args): Steppe.autoitem_unpool_result Take an automation item out of its pool (its points stay).

---@class Steppe.branch
---@field create fun(args: Steppe.branch_create_args): Steppe.branch_create_result Start a branch from a head or an earlier version and switch to it.
---@field delete fun(args: Steppe.branch_delete_args): Steppe.branch_delete_result Delete a branch (not main, not the current one).
---@field diff fun(args: Steppe.branch_diff_args): Steppe.branch_diff_result What differs between two branch heads: added, removed, changed objects.
---@field list fun(): Steppe.branch_list_result List branches and the current one.
---@field merge fun(args: Steppe.branch_merge_args): Steppe.branch_merge_result Merge a branch into another (default: main) by replaying its edits.
---@field ["render-ab"] fun(args: Steppe.branch_render_ab_args): table Render two branch heads over the same range and compare them.
---@field switch fun(args: Steppe.branch_switch_args): Steppe.branch_switch_result Make another branch current (edits, undo and playback follow it).

---@class Steppe.cable
---@field add fun(args: Steppe.cable_add_args): table Connect an output to an input or a knob.
---@field remove fun(args?: Steppe.cable_remove_args): table Remove a cable, by id or by its two ends.
---@field set fun(args: Steppe.cable_set_args): table Set a modulation cable's amount or a cable's colour.

---@class Steppe.chord
---@field remove fun(args: Steppe.chord_remove_args): Steppe.chord_remove_result Remove the chord point at a beat.
---@field set fun(args: Steppe.chord_set_args): Steppe.chord_set_result Set the chord from a beat; "" ends it.

---@class Steppe.clip
---@field add fun(args: Steppe.clip_add_args): Steppe.clip_add_result Add a clip to a scene's cell: from notation, a file or an item.
---@field duplicate fun(args: Steppe.clip_duplicate_args): Steppe.clip_duplicate_result Copy a clip and its takes to an empty cell.
---@field move fun(args: Steppe.clip_move_args): Steppe.clip_move_result Move a clip to an empty cell in another scene, channel or both.
---@field remove fun(args: Steppe.clip_remove_args): Steppe.clip_remove_result Remove a clip with its takes (unpooled sources go too).
---@field set fun(args: Steppe.clip_set_args): Steppe.clip_set_result Set a clip's name, colour, mute, gain, length, loop, mode or quantize.

---@class Steppe.context
---@field at fun(args?: Steppe.context_at_args): Steppe.context_at_result What key, chord and section hold at a beat or bar.
---@field get fun(args?: Steppe.context_get_args): Steppe.context_get_result List the key and chord points in a range of beats.
---@field write fun(args: Steppe.context_write_args): Steppe.context_write_result Write key and chord points from a lead sheet.

---@class Steppe.crash
---@field acknowledge fun(args: Steppe.crash_acknowledge_args): table Mark a crash report, or a run that left none, as seen.
---@field clear fun(): table Delete every finished crash report and record of an ended run.
---@field delete fun(args: Steppe.crash_delete_args): table Delete one crash report, or the record of a run that left none.
---@field list fun(args?: Steppe.crash_list_args): table The crash reports earlier runs left, newest first.
---@field read fun(args: Steppe.crash_read_args): table One crash report in full.
---@field settle fun(args: Steppe.crash_settle_args): table Mark the suspicion a crash report raised as resolved.

---@class Steppe.dsp
---@field check fun(args?: Steppe.dsp_check_args): Steppe.dsp_check_result Compile a DSP script without saving it; return its diagnostics.
---@field edit fun(args: Steppe.dsp_edit_args): Steppe.dsp_edit_result Replace text in a library script and compile it, as dsp.write.
---@field export fun(args: Steppe.dsp_export_args): Steppe.dsp_export_result Export a DSP script as a plugin: a CLAP file, or C++ source.
---@field get fun(args: Steppe.dsp_get_args): Steppe.dsp_get_result Read a DSP script from the library.
---@field list fun(): Steppe.dsp_list_result List the DSP scripts in the library, compiled, with their plugin ids.
---@field preview fun(args?: Steppe.dsp_preview_args): Steppe.dsp_preview_result Draw a plugin's editor to a PNG, after a test signal plays through it.
---@field test fun(args?: Steppe.dsp_test_args): Steppe.dsp_test_result Play test signals or notes through a plugin; measure levels, THD, pitch.
---@field write fun(args: Steppe.dsp_write_args): Steppe.dsp_write_result Save a DSP script to the library and compile it, with diagnostics.

---@class Steppe.engine
---@field devices fun(args?: Steppe.engine_devices_args): Steppe.engine_devices_result List audio devices: names, host APIs, rates, channel counts.
---@field inputs fun(): table The device's audio inputs, MIDI devices and master outputs.
---@field start fun(args?: Steppe.engine_start_args): Steppe.engine_start_result Start playback through JACK, native PortAudio, or a device-free dummy.
---@field status fun(): Steppe.engine_status_result Engine state: backend, rate, block, device period, latency, xruns.
---@field stop fun(): Steppe.engine_stop_result Stop real-time playback and release the audio device.

---@class Steppe.env
---@field add fun(args: Steppe.env_add_args): Steppe.env_add_result Automate a parameter: a track's or route's mixer control, or a plugin's.
---@field list fun(args?: Steppe.env_list_args): Steppe.env_list_result List envelopes (optionally of one track, route or plugin).
---@field remove fun(args: Steppe.env_remove_args): Steppe.env_remove_result Remove an envelope (the parameter returns to its set value).
---@field set fun(args: Steppe.env_set_args): Steppe.env_set_result Set an envelope's active flag and mode (read, touch, latch, write).

---@class Steppe.env.points
---@field set fun(args: Steppe.env_points_set_args): Steppe.env_points_set_result Replace an envelope's points (all of them, or those in a beat range).

---@class Steppe.env.write
---@field commit fun(args: Steppe.env_write_commit_args): Steppe.env_write_commit_result Record what a write pass wrote into an envelope as a replayable edit.

---@class Steppe.fx
---@field add fun(args: Steppe.fx_add_args): Steppe.fx_add_result Insert a plugin into a track's (or a take's) FX chain and return its id.
---@field copy fun(args: Steppe.fx_copy_args): Steppe.fx_copy_result Copy a plugin and its current settings into a track's FX chain.
---@field describe fun(args: Steppe.fx_describe_args): Steppe.fx_describe_result Describe an instance and its live problems; full adds parameters and patch.
---@field meters fun(args: Steppe.fx_meters_args): Steppe.fx_meters_result Read a plugin's meters and readouts, playing now or as a render ended.
---@field move fun(args: Steppe.fx_move_args): Steppe.fx_move_result Move a plugin within a track chain or to another track.
---@field remove fun(args: Steppe.fx_remove_args): Steppe.fx_remove_result Remove a plugin instance (and its envelopes).
---@field restart fun(args: Steppe.fx_restart_args): Steppe.fx_restart_result Re-create a plugin instance from its saved patch/state (after a crash).
---@field set fun(args: Steppe.fx_set_args): Steppe.fx_set_result Set instance options: bypass, offline, wet, label, isolation, pins, MIDI.

---@class Steppe.fx.chain
---@field delete fun(args: Steppe.fx_chain_delete_args): Steppe.fx_chain_delete_result Delete a saved FX chain preset by name.
---@field list fun(): Steppe.fx_chain_list_result List saved FX chain presets by name, path and effect count.
---@field load fun(args: Steppe.fx_chain_load_args): Steppe.fx_chain_load_result Append or replace a track's FX chain from a preset in one undo step.
---@field save fun(args: Steppe.fx_chain_save_args): Steppe.fx_chain_save_result Save an ordered FX chain as a named preset.

---@class Steppe.fx.learn
---@field list fun(args?: Steppe.fx_learn_list_args): Steppe.fx_learn_list_result List the MIDI controller bindings of plugin parameters.
---@field remove fun(args: Steppe.fx_learn_remove_args): Steppe.fx_learn_remove_result Remove a plugin parameter's MIDI controller binding.
---@field set fun(args: Steppe.fx_learn_set_args): Steppe.fx_learn_set_result Let a MIDI controller (CC, note, pitch bend) move a plugin parameter.

---@class Steppe.fx.param
---@field get fun(args: Steppe.fx_param_get_args): Steppe.fx_param_get_result Read one saved plugin parameter by semantic key or numeric CLAP id.
---@field set fun(args: Steppe.fx_param_set_args): Steppe.fx_param_set_result Set semantic parameters in units, or plain plugin parameters by name/id.

---@class Steppe.fx.patch
---@field get fun(args: Steppe.fx_patch_get_args): Steppe.fx_patch_get_result Read a semantic plugin's saved patch.
---@field list fun(args: Steppe.fx_patch_list_args): Steppe.fx_patch_list_result List an instance's read-only factory patches, by name.
---@field merge fun(args: Steppe.fx_patch_merge_args): Steppe.fx_patch_merge_result Merge a partial patch into a plugin instance; returns the effective change.
---@field set fun(args: Steppe.fx_patch_set_args): Steppe.fx_patch_set_result Replace a semantic patch, or apply a factory patch by name.

---@class Steppe.fx.preset
---@field delete fun(args: Steppe.fx_preset_delete_args): Steppe.fx_preset_delete_result Delete one of the user's presets for a plugin; factory presets stay.
---@field list fun(args: Steppe.fx_preset_list_args): Steppe.fx_preset_list_result List a plugin's presets: its factory ones, then the user's.
---@field load fun(args: Steppe.fx_preset_load_args): Steppe.fx_preset_load_result Apply a factory or user preset to a plugin instance in one undo step.
---@field save fun(args: Steppe.fx_preset_save_args): Steppe.fx_preset_save_result Save a plugin instance's current settings as a named user preset.

---@class Steppe.fx.state
---@field capture fun(args: Steppe.fx_state_capture_args): Steppe.fx_state_capture_result Record a plug-in's own state change as a replayable edit.

---@class Steppe.group
---@field add fun(args: Steppe.group_add_args): Steppe.group_add_result Create a named track group with optional links and members.
---@field describe fun(args: Steppe.group_describe_args): Steppe.group_describe_result Describe a track group's membership, links and VCA ancestry.
---@field list fun(): Steppe.group_list_result List track groups, linked attributes and optional VCA controls.
---@field remove fun(args: Steppe.group_remove_args): Steppe.group_remove_result Remove a track group and its VCA, keeping its tracks.
---@field set fun(args: Steppe.group_set_args): Steppe.group_set_result Change a group's name, color, links or active state.
---@field targets fun(args: Steppe.group_targets_args): Steppe.group_targets_result Resolve linked tracks or matching items for an edit or selection.

---@class Steppe.group.members
---@field set fun(args: Steppe.group_members_set_args): Steppe.group_members_set_result Replace a group's track membership; an empty array clears it.

---@class Steppe.history
---@field list fun(): Steppe.history_list_result List the current branch's recent versions (newest first).
---@field redo fun(): Steppe.history_redo_result Redo the last undone change.
---@field undo fun(): Steppe.history_undo_result Undo the last change on the current branch.

---@class Steppe.item
---@field add fun(args: Steppe.item_add_args): Steppe.item_add_result Add an item on a track: notation, a MIDI or audio file, or a clip's copy.
---@field ["apply-fx"] fun(args: Steppe.item_apply_fx_args): Steppe.item_apply_fx_result Render an item through its take and track FX into a new audio take.
---@field consolidate fun(args: Steppe.item_consolidate_args): table Consolidate selected audio and MIDI per track in a background job.
---@field duplicate fun(args: Steppe.item_duplicate_args): Steppe.item_duplicate_result Copy an item and its takes to a new position (default: right after).
---@field ["explode-takes"] fun(args: Steppe.item_explode_takes_args): Steppe.item_explode_takes_result Spread an item's takes onto new tracks, or in turn on its track.
---@field export fun(args: Steppe.item_export_args): Steppe.item_export_result Write an item's raw audio (WAV) or its MIDI (SMF) to a file.
---@field heal fun(args: Steppe.item_heal_args): table Heal compatible consecutive audio pieces without rendering.
---@field ["mix-to-track"] fun(args: Steppe.item_mix_to_track_args): table Mix the selected items through their tracks onto a new muted channel.
---@field move fun(args: Steppe.item_move_args): Steppe.item_move_result Move an item in time (position in beats or bar) and/or to another track.
---@field paste fun(args: Steppe.item_paste_args): Steppe.item_paste_result Copy items as they were in a version to tracks and beats.
---@field remove fun(args: Steppe.item_remove_args): Steppe.item_remove_result Remove an item with its takes (unpooled sources go too).
---@field set fun(args: Steppe.item_set_args): Steppe.item_set_result Set an item's position, length, playback, fades, name, color, or lock.
---@field split fun(args: Steppe.item_split_args): Steppe.item_split_result Split an item at a beat, optionally at the prior audio zero crossing.

---@class Steppe.item.consolidate
---@field commit fun(args: Steppe.item_consolidate_commit_args): table Attach consolidated audio and merge MIDI as one replayable edit.

---@class Steppe.item.mix-to-track
---@field commit fun(args: Steppe.item_mix_to_track_commit_args): table Put a finished item mix on a new muted channel as one replayable edit.

---@class Steppe.job
---@field cancel fun(args: Steppe.job_cancel_args): table Cancel a job.
---@field get fun(args: Steppe.job_get_args): table Get a job's status and progress.
---@field list fun(): table List session jobs.
---@field status fun(args: Steppe.job_status_args): table Get a job's status.
---@field wait fun(args: Steppe.job_wait_args): table Wait for a job.

---@class Steppe.key
---@field remove fun(args: Steppe.key_remove_args): Steppe.key_remove_result Remove the key point at a beat.
---@field set fun(args: Steppe.key_set_args): Steppe.key_set_result Set the key from a beat: tonic and scale.

---@class Steppe.launch
---@field arrangement fun(args?: Steppe.launch_arrangement_args): Steppe.launch_arrangement_result Return tracks to the arrangement at the next boundary.
---@field capture fun(args: Steppe.launch_capture_args): Steppe.launch_capture_result Start or end capturing launches for the timeline.
---@field clip fun(args: Steppe.launch_clip_args): Steppe.launch_clip_result Play a clip on its track from the next quantize boundary.
---@field commit fun(args?: Steppe.launch_commit_args): Steppe.launch_commit_result Write the captured performance onto the timeline as items.
---@field scene fun(args: Steppe.launch_scene_args): Steppe.launch_scene_result Play every clip of a scene together at one boundary.
---@field state fun(): Steppe.launch_state_result What each track with clips plays, and what it has queued.
---@field stop fun(args?: Steppe.launch_stop_args): Steppe.launch_stop_result Stop a track's clip, or all clips, at the next boundary.

---@class Steppe.log
---@field list fun(args?: Steppe.log_list_args): table Recent commands with origin, arguments and outcome (the command log).

---@class Steppe.marker
---@field add fun(args?: Steppe.marker_add_args): Steppe.marker_add_result Add a named marker in beats.
---@field list fun(): Steppe.marker_list_result List rulers in this project.
---@field remove fun(args: Steppe.marker_remove_args): Steppe.marker_remove_result Remove a ruler.
---@field set fun(args: Steppe.marker_set_args): Steppe.marker_set_result Edit a ruler's position, name, or color.

---@class Steppe.midi
---@field edit fun(args: Steppe.midi_edit_args): table Edit a MIDI item's notes and CCs: move, resize, quantize, fit to key or chord.
---@field get fun(args: Steppe.midi_get_args): Steppe.midi_get_result Read an item's or clip's MIDI as notation, with its key and chords.
---@field notes fun(args: Steppe.midi_notes_args): table List a MIDI item's or clip's notes and CC points (beats from its start).
---@field write fun(args: Steppe.midi_write_args): Steppe.midi_write_result Replace an item's or clip's MIDI with notation (keeping its place and length).

---@class Steppe.midi.inputs
---@field get fun(): table Hardware MIDI inputs, timing, settings, loss and startup warnings.
---@field set fun(args: Steppe.midi_inputs_set_args): table Change one machine MIDI port's enabled state and timing offset.

---@class Steppe.midi.outputs
---@field get fun(): table Hardware MIDI outputs: settings, clock/MTC, timing, loss, warnings.
---@field set fun(args: Steppe.midi_outputs_set_args): table Change one MIDI output: enabled, offset, reset, clock, SPP, MTC.

---@class Steppe.modulation
---@field add fun(args: Steppe.modulation_add_args): Steppe.modulation_add_result Route a rack's Mod Out to a plug-in parameter or mixer level.
---@field list fun(args?: Steppe.modulation_list_args): Steppe.modulation_list_result List modulation routes by target or source rack.
---@field remove fun(args: Steppe.modulation_remove_args): Steppe.modulation_remove_result Remove a modulation route.
---@field set fun(args: Steppe.modulation_set_args): Steppe.modulation_set_result Change a modulation route's source, target, depth, enabled or lane.
---@field telemetry fun(args?: Steppe.modulation_telemetry_args): Steppe.modulation_telemetry_result Read each modulation route's live offset and span.

---@class Steppe.module
---@field add fun(args: Steppe.module_add_args): table Add a module to a rack: a type with knob values.
---@field check fun(args?: Steppe.module_check_args): table Compile a module without saving; return diagnostics and description.
---@field copy fun(args: Steppe.module_copy_args): table Copy modules, their current plug-in settings and internal cables.
---@field describe fun(args?: Steppe.module_describe_args): table A module type's knobs, inputs and outputs, with units and help.
---@field duplicate fun(args: Steppe.module_duplicate_args): table Duplicate modules, with the cables between them.
---@field edit fun(args: Steppe.module_edit_args): table Edit a library module in place: replace text, then save and compile it.
---@field fork fun(args: Steppe.module_fork_args): table Copy a module type's source into the library under a new id.
---@field get fun(args?: Steppe.module_get_args): table A module type's source, not a placed module's knobs (rack.get reads those).
---@field list fun(args?: Steppe.module_list_args): table List module types: id, name, tags, what they do.
---@field randomize fun(args: Steppe.module_randomize_args): table Move a module's knobs at random, by an amount.
---@field remove fun(args: Steppe.module_remove_args): table Remove a module with its cables.
---@field set fun(args: Steppe.module_set_args): table Set a module's knobs, label, place or bypass.
---@field write fun(args: Steppe.module_write_args): table Write a module to the library and compile it; racks use it at once.

---@class Steppe.module.learn
---@field list fun(args?: Steppe.module_learn_list_args): table List the MIDI controllers of module knobs.
---@field remove fun(args: Steppe.module_learn_remove_args): table Remove a module knob's MIDI controller.
---@field set fun(args: Steppe.module_learn_set_args): table Let a MIDI controller turn a module's knob.

---@class Steppe.plugin
---@field list fun(args?: Steppe.plugin_list_args): Steppe.plugin_list_result List scanned plugins; filter matches id, name, or vendor.
---@field override fun(args: Steppe.plugin_override_args): Steppe.plugin_override_result Load a plug-in normally, on a bridge, or not at all for this run.
---@field overrides fun(): Steppe.plugin_overrides_result List the plug-ins this run bridges or has disabled.
---@field scan fun(args?: Steppe.plugin_scan_args): Steppe.plugin_scan_result Rescan configured plugin paths and return the available plugins.

---@class Steppe.project
---@field get fun(): Steppe.project_get_result Show the project: name, rate, tempo, meter, file, version and branch.
---@field new fun(args?: Steppe.project_new_args): Steppe.project_new_result Start a new, empty project (replaces the current one and its history).
---@field open fun(args?: Steppe.project_open_args): Steppe.project_open_result Open a project file and its branches (replaces the current project).
---@field plugins fun(args?: Steppe.project_plugins_args): table Plug-ins a project or recovery names, and any a crash report suspects.
---@field recover fun(args?: Steppe.project_recover_args): Steppe.project_recover_result Put back what a crash left behind: recordings, or an unsaved project.
---@field recoverable fun(): Steppe.project_recoverable_result List recordings and unsaved projects a crash left behind.
---@field save fun(args?: Steppe.project_save_args): Steppe.project_save_result Save the project (canonical JSON) to its file or a new path.
---@field set fun(args?: Steppe.project_set_args): Steppe.project_set_result Set project name, notes, Console 9, pan law, timebase or timecode.

---@class Steppe.project.recoverable
---@field delete fun(args: Steppe.project_recoverable_delete_args): Steppe.project_recoverable_delete_result Delete an unsaved project a crash left behind, with its recordings.

---@class Steppe.project.template
---@field delete fun(args: Steppe.project_template_delete_args): Steppe.project_template_delete_result Delete a saved project template by name.
---@field list fun(): Steppe.project_template_list_result List saved project templates by name and path.
---@field load fun(args: Steppe.project_template_load_args): Steppe.project_template_load_result Start a new, unsaved project from a project template.
---@field save fun(args: Steppe.project_template_save_args): Steppe.project_template_save_result Save the whole project as a named project template.

---@class Steppe.rack
---@field add fun(args?: Steppe.rack_add_args): table Add a modular rack on a channel, its Out the input.
---@field arrange fun(args: Steppe.rack_arrange_args): table Lay out a rack's modules by signal flow.
---@field capture fun(args: Steppe.rack_capture_args): table Record a rack's pitch and gate as notes in a MIDI item on a track.
---@field get fun(args: Steppe.rack_get_args): table A rack's modules with their changed knobs, cables, Outs and problems.
---@field levels fun(args: Steppe.rack_levels_args): table Read every module output's linear peak, without subscribing to telemetry.
---@field list fun(): table List racks: home channels, module counts, Outs.
---@field paste fun(args: Steppe.rack_paste_args): table Paste the module clipboard with its plug-in settings as one undo step.
---@field patch fun(args: Steppe.rack_patch_args): table Add named modules and the cables among them in one step.
---@field probe fun(args: Steppe.rack_probe_args): table Listen to a rack's ports: level, pitch, gate rate, notes, a CV's range.
---@field remove fun(args: Steppe.rack_remove_args): table Remove a rack with its modules and cables.
---@field set fun(args: Steppe.rack_set_args): table Rename a rack or move it to another channel.

---@class Steppe.rack.capture
---@field commit fun(args: Steppe.rack_capture_commit_args): table Attach a rack capture's rendered MIDI notes.

---@class Steppe.rack.preset
---@field delete fun(args: Steppe.rack_preset_delete_args): table Delete a saved rack preset.
---@field list fun(args?: Steppe.rack_preset_list_args): table List rack presets, saved and factory: names and contents.
---@field load fun(args: Steppe.rack_preset_load_args): table Add a saved preset's modules and cables to a rack.
---@field save fun(args: Steppe.rack_preset_save_args): table Save a rack's modules and cables as a preset.

---@class Steppe.record
---@field commit fun(args: Steppe.record_commit_args): table Record a recording's commit (or a recovery's) as a replayable edit.
---@field get fun(): Steppe.record_get_result How a recording pass over an item is kept (stack_takes).
---@field set fun(args?: Steppe.record_set_args): Steppe.record_set_result Choose whether recording over an item makes a take of it; returns it.

---@class Steppe.region
---@field add fun(args?: Steppe.region_add_args): Steppe.region_add_result Add a named region in beats.
---@field list fun(): Steppe.region_list_result List rulers in this project.
---@field remove fun(args: Steppe.region_remove_args): Steppe.region_remove_result Remove a ruler.
---@field set fun(args: Steppe.region_set_args): Steppe.region_set_result Edit a ruler's position, name, or color.

---@class Steppe.registry
---@field describe fun(args: Steppe.registry_describe_args): table Describe a command.
---@field groups fun(args?: Steppe.registry_groups_args): table List the command groups, with one line each.
---@field list fun(args?: Steppe.registry_list_args): table List available commands.

---@class Steppe.render
---@field ["dry-run"] fun(args?: Steppe.render_dry_run_args): table List the files a render would write, with lengths, without rendering.
---@field mix fun(args?: Steppe.render_mix_args): table Render the mix offline with the project's settings (default: whole song).
---@field regions fun(args?: Steppe.render_regions_args): table Render each region (or the render matrix's tracks per region) to files.
---@field settings fun(args?: Steppe.render_settings_args): table The render settings that apply, and the layer each one comes from.
---@field stems fun(args?: Steppe.render_stems_args): table Render each selected track to its own file (stems).

---@class Steppe.render.preset
---@field delete fun(args?: Steppe.render_preset_delete_args): table Delete one of the user's render presets, or the user's render default.
---@field list fun(): table Render presets, factory then user's, with settings.
---@field load fun(args?: Steppe.render_preset_load_args): table Replace the project's render settings with a preset (one undo step).
---@field save fun(args?: Steppe.render_preset_save_args): table Save render settings as a named preset or the user's default.

---@class Steppe.render.queue
---@field add fun(args: Steppe.render_queue_add_args): table Queue a render (mix, stems, regions) for render.queue.run.
---@field list fun(): table The queued renders, in the order render.queue.run runs them.
---@field remove fun(args?: Steppe.render_queue_remove_args): table Take a render off the queue (id), or clear it (all: true).
---@field run fun(args?: Steppe.render_queue_run_args): table Run every queued render in order, removing each when done.

---@class Steppe.route
---@field add fun(args: Steppe.route_add_args): Steppe.route_add_result Send audio between tracks, or MIDI between channels and rack modules.
---@field list fun(): Steppe.route_list_result List sends with endpoints and mixer settings.
---@field matrix fun(args?: Steppe.route_matrix_args): table Read audio or MIDI endpoints, existing sends and blocked pairs.
---@field remove fun(args: Steppe.route_remove_args): Steppe.route_remove_result Remove a send.
---@field set fun(args: Steppe.route_set_args): Steppe.route_set_result Change a route's endpoints, tap, level, pan, mute, or channel map.

---@class Steppe.route.learn
---@field list fun(args?: Steppe.route_learn_list_args): Steppe.route_learn_list_result List the MIDI controller bindings of send controls.
---@field remove fun(args: Steppe.route_learn_remove_args): Steppe.route_learn_remove_result Remove a send control's MIDI controller binding.
---@field set fun(args: Steppe.route_learn_set_args): Steppe.route_learn_set_result Let a MIDI controller move a send's level, pan, mute or tap.

---@class Steppe.scene
---@field add fun(args?: Steppe.scene_add_args): Steppe.scene_add_result Add a scene (launcher column) at the end or an index.
---@field capture fun(args: Steppe.scene_capture_args): Steppe.scene_capture_result Make a scene from a section's items, one clip per track.
---@field insert fun(args: Steppe.scene_insert_args): Steppe.scene_insert_result Lay a scene's clips on the timeline as items and a section.
---@field list fun(): Steppe.scene_list_result List scenes in column order with clip counts.
---@field move fun(args: Steppe.scene_move_args): Steppe.scene_move_result Move a scene to another column.
---@field remove fun(args: Steppe.scene_remove_args): Steppe.scene_remove_result Remove a scene with its clips and their takes.
---@field set fun(args: Steppe.scene_set_args): Steppe.scene_set_result Rename or recolour a scene.

---@class Steppe.script
---@field run fun(args: Steppe.script_run_args): Steppe.script_run_result Run a Lua script on the command bus.

---@class Steppe.session
---@field describe fun(): Steppe.session_describe_result Describe the session in compact text: tracks, items, fx, routes, key.

---@class Steppe.sync
---@field get fun(): Steppe.sync_get_result Show external sync settings and what the engine hears from the master.
---@field set fun(args?: Steppe.sync_set_args): Steppe.sync_set_result Set external sync (MIDI clock, MTC, LTC) and LTC output; returns them.

---@class Steppe.take
---@field activate fun(args: Steppe.take_activate_args): Steppe.take_activate_result Choose the take heard from an item or clip.
---@field add fun(args: Steppe.take_add_args): Steppe.take_add_result Add a take to an item or clip, optionally copied from one of its takes.
---@field bake fun(args: Steppe.take_bake_args): Steppe.take_bake_result Write a following take's sounding pitches into its notes, and stop following.
---@field comp fun(args: Steppe.take_comp_args): Steppe.take_comp_result Comp an item's takes: each span plays one take, crossfaded at the seams.
---@field extract fun(args: Steppe.take_extract_args): Steppe.take_extract_result Take a take out of its item or clip into an item of its own on any channel.
---@field list fun(args: Steppe.take_list_args): Steppe.take_list_result List the takes of an item or clip and identify its active take.
---@field remove fun(args: Steppe.take_remove_args): Steppe.take_remove_result Delete a take from its item or clip (which goes with its last take).
---@field set fun(args: Steppe.take_set_args): Steppe.take_set_result Set a take's gain, pan, name, rate, pitch, context, follow and more.

---@class Steppe.take.comp
---@field get fun(args?: Steppe.take_comp_get_args): Steppe.take_comp_get_result List the pieces of a comp: spans, takes and fades.

---@class Steppe.take.hits
---@field detect fun(args: Steppe.take_hits_detect_args): Steppe.take_hits_detect_result Detect transient candidates in audio takes, then commit one undoable edit.
---@field set fun(args: Steppe.take_hits_set_args): table Replace a take's transient candidates in original source seconds.

---@class Steppe.take.stretch
---@field add fun(args: Steppe.take_stretch_add_args): table Add a pitch-preserving audio stretch marker.
---@field move fun(args: Steppe.take_stretch_move_args): table Move an audio stretch marker's item position.
---@field remove fun(args: Steppe.take_stretch_remove_args): table Remove an interior audio stretch marker.

---@class Steppe.tempo
---@field set fun(args: Steppe.tempo_set_args): Steppe.tempo_set_result Replace the tempo map: points {beat, bpm, ramp?}, the first at beat 0.

---@class Steppe.time
---@field convert fun(args?: Steppe.time_convert_args): Steppe.time_convert_result Convert a seconds, beat, or bar position to all three time domains.

---@class Steppe.timesig
---@field remove fun(args: Steppe.timesig_remove_args): Steppe.timesig_remove_result Remove the meter change at a bar: the meter before it continues.
---@field set fun(args: Steppe.timesig_set_args): Steppe.timesig_set_result Set the meter from a bar on, e.g. {"bar": 9, "meter": "3/4"}.

---@class Steppe.track
---@field add fun(args?: Steppe.track_add_args): Steppe.track_add_result Add a track (a channel, buss or master) and return its id.
---@field describe fun(args: Steppe.track_describe_args): Steppe.track_describe_result Describe track groups and requested/applied effective gain at a time.
---@field duplicate fun(args: Steppe.track_duplicate_args): Steppe.track_duplicate_result Duplicate channels or busses with their FX, items, sends and automation.
---@field freeze fun(args: Steppe.track_freeze_args): table Freeze a channel's items and FX to pre-fader audio and release its plugins.
---@field list fun(args?: Steppe.track_list_args): Steppe.track_list_result List tracks in tree order with their level, mute/solo, fx and items.
---@field move fun(args: Steppe.track_move_args): Steppe.track_move_result Move a track under another track or reorder it among its siblings.
---@field remove fun(args: Steppe.track_remove_args): Steppe.track_remove_result Remove a track with its items, clips, fx, routes and envelopes (not a folder).
---@field set fun(args: Steppe.track_set_args): Steppe.track_set_result Set track fields; choosing audio, MIDI or rack input replaces the others.
---@field unfreeze fun(args: Steppe.track_unfreeze_args): table Restore a frozen channel's original items and plugin state.

---@class Steppe.track.freeze
---@field commit fun(args: Steppe.track_freeze_commit_args): table Attach a verified freeze artifact as one replayable edit.

---@class Steppe.track.learn
---@field list fun(args?: Steppe.track_learn_list_args): Steppe.track_learn_list_result List the MIDI controller bindings of track controls.
---@field remove fun(args: Steppe.track_learn_remove_args): Steppe.track_learn_remove_result Remove a track control's MIDI controller binding.
---@field set fun(args: Steppe.track_learn_set_args): Steppe.track_learn_set_result Let a MIDI controller move a track's fader, pan, mute, solo or arm.

---@class Steppe.track.template
---@field delete fun(args: Steppe.track_template_delete_args): Steppe.track_template_delete_result Delete a saved track template by name.
---@field list fun(): Steppe.track_template_list_result List saved track templates by name, path and effect count.
---@field load fun(args?: Steppe.track_template_load_args): Steppe.track_template_load_result Add a track from a track template (settings and FX) in one undo step.
---@field save fun(args: Steppe.track_template_save_args): Steppe.track_template_save_result Save a track (settings and FX chains) as a named track template.

---@class Steppe.transport
---@field get fun(): Steppe.transport_get_result The play position and whether it is playing.
---@field loop fun(args?: Steppe.transport_loop_args): Steppe.transport_loop_result Set or toggle the loop range: playback reaching its end repeats it.
---@field metronome fun(args?: Steppe.transport_metronome_args): Steppe.transport_metronome_result Turn the metronome on or off and set its level; returns the settings.
---@field pause fun(): Steppe.transport_pause_result Pause playback where it is (ends a recording).
---@field play fun(): Steppe.transport_play_result Start playback from the current position.
---@field punch fun(args?: Steppe.transport_punch_args): Steppe.transport_punch_result Set or toggle the punch range: recordings keep only that range.
---@field record fun(): Steppe.transport_record_result Play and record every armed track's input or master's output.
---@field seek fun(args?: Steppe.transport_seek_args): Steppe.transport_seek_result Move the play position (one of seconds, beat or bar).
---@field stop fun(): Steppe.transport_stop_result Stop playback and return to where it started (ends a recording).

---@class Steppe.transport.playrate
---@field ["apply-to-tempo"] fun(): Steppe.transport_playrate_apply_to_tempo_result Scale every tempo BPM by the master playrate, then reset it to 1.
---@field get fun(): Steppe.transport_playrate_get_result Get requested/effective master playrate, pitch mode and active envelope.
---@field set fun(args?: Steppe.transport_playrate_set_args): Steppe.transport_playrate_set_result Set the project's master playrate ratio and preserve-pitch flag.

---@class Steppe.vca
---@field add fun(args: Steppe.vca_add_args): Steppe.vca_add_result Add an audio-free VCA fader to a track group.
---@field remove fun(args: Steppe.vca_remove_args): Steppe.vca_remove_result Remove a VCA and its automation, keeping the track group.
---@field set fun(args: Steppe.vca_set_args): Steppe.vca_set_result Set a VCA's level, mute or solo without moving member faders.

---@class Steppe.vca.learn
---@field list fun(args?: Steppe.vca_learn_list_args): Steppe.vca_learn_list_result List the MIDI controller bindings of VCA controls.
---@field remove fun(args: Steppe.vca_learn_remove_args): Steppe.vca_learn_remove_result Remove a VCA control's MIDI controller binding.
---@field set fun(args: Steppe.vca_learn_set_args): Steppe.vca_learn_set_result Let a MIDI controller move a VCA's fader, mute or solo.

---@class Steppe.vca.members
---@field set fun(args: Steppe.vca_members_set_args): Steppe.vca_members_set_result Replace the tracks or nested VCAs controlled by a VCA.

---@class Steppe
---@field analyze Steppe.analyze Commands named analyze.*
---@field arrangement Steppe.arrangement Commands named arrangement.*
---@field audio Steppe.audio Commands named audio.*
---@field audition Steppe.audition Commands named audition.*
---@field autoitem Steppe.autoitem Commands named autoitem.*
---@field branch Steppe.branch Commands named branch.*
---@field cable Steppe.cable Commands named cable.*
---@field chord Steppe.chord Commands named chord.*
---@field clip Steppe.clip Commands named clip.*
---@field context Steppe.context Commands named context.*
---@field crash Steppe.crash Commands named crash.*
---@field dsp Steppe.dsp Commands named dsp.*
---@field engine Steppe.engine Commands named engine.*
---@field env Steppe.env Commands named env.*
---@field fx Steppe.fx Commands named fx.*
---@field group Steppe.group Commands named group.*
---@field history Steppe.history Commands named history.*
---@field item Steppe.item Commands named item.*
---@field job Steppe.job Commands named job.*
---@field key Steppe.key Commands named key.*
---@field launch Steppe.launch Commands named launch.*
---@field log Steppe.log Commands named log.*
---@field marker Steppe.marker Commands named marker.*
---@field midi Steppe.midi Commands named midi.*
---@field modulation Steppe.modulation Commands named modulation.*
---@field module Steppe.module Commands named module.*
---@field plugin Steppe.plugin Commands named plugin.*
---@field project Steppe.project Commands named project.*
---@field rack Steppe.rack Commands named rack.*
---@field record Steppe.record Commands named record.*
---@field region Steppe.region Commands named region.*
---@field registry Steppe.registry Commands named registry.*
---@field render Steppe.render Commands named render.*
---@field route Steppe.route Commands named route.*
---@field scene Steppe.scene Commands named scene.*
---@field script Steppe.script Commands named script.*
---@field session Steppe.session Commands named session.*
---@field sync Steppe.sync Commands named sync.*
---@field take Steppe.take Commands named take.*
---@field tempo Steppe.tempo Commands named tempo.*
---@field time Steppe.time Commands named time.*
---@field timesig Steppe.timesig Commands named timesig.*
---@field track Steppe.track Commands named track.*
---@field transport Steppe.transport Commands named transport.*
---@field vca Steppe.vca Commands named vca.*
---@field modulate fun(args: Steppe.modulate_args): Steppe.modulate_result Modulate a parameter or mixer level: LFO, follower, random, macro.
---@field run fun(name: string, args?: table): any Run any registered command by name, e.g. the ones whose names are not Lua identifiers.
---@field batch fun(label: string, body: fun()) Run the calls inside body as one transaction: one undo step, rolled back when one of them fails.
---@field on fun(topic: string, handler: fun(payload: table, topic: string), filter?: table): integer Subscribe to bus events; returns a handle for q.off.
---@field off fun(handle: integer) Drop a subscription.
---@field log fun(...) Print a line to the script's log (the same as print).
---@field null any The JSON null: Lua tables cannot hold nil.
---@field name string The name this script runs under (its origin is `lua:<name>`).
