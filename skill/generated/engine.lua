---@meta
-- Quire 0.5.0 scripting reference, generated from the command registry.
-- Do not edit: `<slug>-engine registry --lua` writes it, tools/gen/update-generated.sh regenerates it.
-- The `q` table of a script (libs/lua-bus, ADR-0006): every command is a
-- function, `q.run(name, args)` reaches any of them by name, `q.batch(label, body)`
-- is one transaction, and `q.on(topic, handler)` subscribes to bus events.

---@type Quire
q = nil

---@class Quire.analyze_compare_args
---@field reference string
---@field test string

---@class Quire.analyze_compare_result
---@field notable any
---@field ltas_rms_db number
---@field summary string

---@class Quire.analyze_masking_args
---@field masker_track string
---@field maskee_track string
---@field start_beat? number
---@field end_beat? number

---@class Quire.analyze_render_args
---@field render string
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")

---@class Quire.analyze_track_args
---@field track string
---@field start_beat? number
---@field end_beat? number
---@field tap? "post_fader"|"pre_fader"|"pre_fx" (default: "post_fader")
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")

---@class Quire.audio_quantize_args
---@field items string[]
---@field grid number
---@field strength? number (default: 1.0)
---@field swing? number (default: 0.0)
---@field crossfade? number (default: 0.005)
---@field mode? "warp"|"slice" (default: "warp")
---@field reference? string (default: "")
---@field hits? "reference"|"merged" (default: "reference")

---@class Quire.audio_warp_detach_args
---@field items string[]

---@class Quire.audio_warp_reset_args
---@field items string[]

---@class Quire.autoitem_add_args
---@field env string Envelope id, e.g. "en_1".
---@field position? number
---@field bar? integer
---@field length? number (default: 4.0)
---@field points? table[]
---@field source_length? number
---@field loop? boolean
---@field name? string

---@class Quire.autoitem_duplicate_args
---@field item string Automation item id, e.g. "ai_1".
---@field position? number
---@field pooled? boolean (default: true)

---@class Quire.autoitem_list_args
---@field env? string Envelope id, e.g. "en_1".

---@class Quire.autoitem_remove_args
---@field item string Automation item id, e.g. "ai_1".

---@class Quire.autoitem_set_args
---@field item string Automation item id, e.g. "ai_1".
---@field position? number
---@field length? number
---@field source_length? number
---@field loop? boolean
---@field baseline? number
---@field amplitude? number
---@field points? table[]
---@field name? string

---@class Quire.autoitem_unpool_args
---@field item string Automation item id, e.g. "ai_1".

---@class Quire.branch_create_args
---@field name string
---@field from? string
---@field version? integer Start at this version of the current branch's history (history.list; a render result's version) instead of a head
---@field switch? boolean (default: true)

---@class Quire.branch_create_result
---@field name string

---@class Quire.branch_delete_args
---@field name string

---@class Quire.branch_diff_args
---@field a string
---@field b string

---@class Quire.branch_diff_result
---@field changes table[]

---@class Quire.branch_list_result
---@field branches string[]
---@field current string

---@class Quire.branch_merge_args
---@field from string
---@field into? string

---@class Quire.branch_merge_result
---@field fast_forward boolean
---@field up_to_date boolean
---@field version integer
---@field remapped any

---@class Quire.branch_render_ab_args
---@field a string
---@field b string
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number

---@class Quire.branch_switch_args
---@field name string

---@class Quire.branch_switch_result
---@field name string

---@class Quire.dsp_check_args
---@field name? string
---@field source? string

---@class Quire.dsp_check_result
---@field ok boolean
---@field diagnostics table[]
---@field plugin? table

---@class Quire.dsp_edit_args
---@field name string
---@field edits table[]

---@class Quire.dsp_edit_result
---@field name string
---@field path string
---@field ok boolean it compiled
---@field diagnostics table[]
---@field plugin? table
---@field listed boolean plugin.list shows it (the script player is on the plugin path)

---@class Quire.dsp_export_args
---@field script string a name from dsp.list
---@field format? "clap"|"source" clap (default): a plugin file; source: a C++ project
---@field folder? string an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new
---@field license? "MIT"|"GPL-3.0-or-later" source: the license of the project's own files (default MIT)

---@class Quire.dsp_export_result
---@field path string the plugin file, or the project's folder
---@field id string
---@field name string
---@field format string clap or source
---@field player? string clap: the script player it was copied from
---@field bytes? integer
---@field notices? string clap: the licenses to ship with the plugin

---@class Quire.dsp_get_args
---@field name string

---@class Quire.dsp_get_result
---@field name string
---@field path string
---@field source string

---@class Quire.dsp_list_result
---@field library string
---@field scripts table[]

---@class Quire.dsp_preview_args
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

---@class Quire.dsp_preview_result
---@field plugin string
---@field path string the PNG
---@field width integer pixels
---@field height integer pixels
---@field edits any what pointer changed, as a merge patch
---@field problems string[]
---@field log string[]

---@class Quire.dsp_test_args
---@field plugin? string
---@field script? string
---@field patch? any
---@field signals? "sine"|"sweep"|"impulse"|"noise"|"silence"|"notes"|"note"|"file"[] default: effects five, or file with audio_file; instruments three, or notes and silence with custom notes
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

---@class Quire.dsp_test_result
---@field plugin string
---@field sample_rate number|string the rate rendered at Quantity strings are range-checked by the server.
---@field latency integer samples
---@field signals table[]
---@field cpu_percent number of real time, measured
---@field problems string[]
---@field log string[]

---@class Quire.dsp_write_args
---@field name string the script's folder and file name
---@field source string the whole C++ file

---@class Quire.dsp_write_result
---@field name string
---@field path string
---@field ok boolean it compiled
---@field diagnostics table[]
---@field plugin? table
---@field listed boolean plugin.list shows it (the script player is on the plugin path)

---@class Quire.engine_devices_args
---@field backend? "jack"|"dummy"|"portaudio"

---@class Quire.engine_devices_result
---@field devices table[]

---@class Quire.engine_start_args
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

---@class Quire.engine_start_result
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
---@field midi_dropped integer
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
---@field warnings string[]
---@field memory_lock table
---@field cpu_idle_latency table
---@field meter_drops table
---@field failed_fx string[]
---@field dropped integer
---@field voice_dropped integer
---@field failed_errors any

---@class Quire.engine_status_result
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
---@field midi_dropped integer
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
---@field warnings string[]
---@field memory_lock table
---@field cpu_idle_latency table
---@field meter_drops table
---@field failed_fx string[]
---@field dropped integer
---@field voice_dropped integer
---@field failed_errors any

---@class Quire.env_add_args
---@field track? string Track id, e.g. "tr_1".
---@field take? string Take id, e.g. "tk_1".
---@field group? string VCA group id, e.g. "gr_1".
---@field fx? string Plugin instance id, e.g. "fx_1".
---@field route? string Route id, e.g. "sd_1".
---@field param string
---@field points? table[]

---@class Quire.env_add_result
---@field env string
---@field points integer

---@class Quire.env_list_args
---@field object? string

---@class Quire.env_list_result
---@field envelopes table[]

---@class Quire.env_points_set_args
---@field env string Envelope id, e.g. "en_1".
---@field points table[]
---@field from_beat? number
---@field to_beat? number

---@class Quire.env_points_set_result
---@field points integer

---@class Quire.env_remove_args
---@field env string Envelope id, e.g. "en_1".

---@class Quire.env_set_args
---@field env string Envelope id, e.g. "en_1".
---@field active? boolean
---@field visible? boolean
---@field mode? "trim"|"read"|"touch"|"latch"|"latch_preview"|"write"

---@class Quire.fx_add_args
---@field track? string Track id, e.g. "tr_1".
---@field take? string Take id, e.g. "tk_1".
---@field plugin string
---@field index? integer
---@field patch? any
---@field chain? string

---@class Quire.fx_add_result
---@field fx string
---@field name string
---@field semantic boolean
---@field chain string[]

---@class Quire.fx_chain_delete_args
---@field name string

---@class Quire.fx_chain_delete_result
---@field path string

---@class Quire.fx_chain_list_result
---@field chains table[]

---@class Quire.fx_chain_load_args
---@field track string
---@field name? string
---@field path? string
---@field chain? string (default: "main")
---@field mode? string (default: "append")

---@class Quire.fx_chain_load_result
---@field effects string[]

---@class Quire.fx_chain_save_args
---@field track string
---@field name string
---@field chain? string (default: "main")
---@field replace? boolean (default: false)

---@class Quire.fx_chain_save_result
---@field path string
---@field effects integer

---@class Quire.fx_copy_args
---@field fx string
---@field track string
---@field index? integer
---@field chain? string (default: "main")

---@class Quire.fx_copy_result
---@field fx string
---@field chain string[]

---@class Quire.fx_describe_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field full? boolean (default: false)
---@field filter? string Plain parameters only: list those whose name contains this text (case-insensitive).

---@class Quire.fx_describe_result
---@field plugin string
---@field name string
---@field description string
---@field params? any
---@field patch? any
---@field status_available boolean A live instance supplied its current problems
---@field problems? any Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable
---@field sidechain_channels? integer The plug-in's key input: route.add {fx} feeds it
---@field latency_samples? integer The instance's reported latency, in samples
---@field params_total? integer Plain parameters matching; at most 512 are listed.

---@class Quire.fx_learn_list_args
---@field fx? string

---@class Quire.fx_learn_list_result
---@field learn any

---@class Quire.fx_learn_remove_args
---@field fx string
---@field param string

---@class Quire.fx_learn_set_args
---@field fx string
---@field param string A semantic key, or a plain parameter's id or name (fx.describe)
---@field midi table
---@field mode? "absolute"|"relative1"|"relative2"|"relative3"|"toggle"
---@field soft_takeover? boolean
---@field when_selected? boolean
---@field when_focused? boolean
---@field when_visible? boolean

---@class Quire.fx_learn_set_result
---@field param string
---@field control string

---@class Quire.fx_meters_args
---@field fx string
---@field from? "live"|"render" live: the playing instance; render: the last render.mix (default: "live")

---@class Quire.fx_meters_result
---@field source string live, render, or none (nothing to read yet)
---@field meters any Read-only values by patch key (physical) or name

---@class Quire.fx_move_args
---@field fx string
---@field track? string
---@field index integer

---@class Quire.fx_param_get_args
---@field fx string
---@field param string

---@class Quire.fx_param_get_result
---@field value any

---@class Quire.fx_param_set_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field values any

---@class Quire.fx_param_set_result
---@field changed any
---@field description string
---@field inactive? string[] Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it

---@class Quire.fx_patch_get_args
---@field fx string
---@field changed_only? boolean (default: false)

---@class Quire.fx_patch_get_result
---@field patch any

---@class Quire.fx_patch_list_args
---@field fx string

---@class Quire.fx_patch_list_result
---@field patches any

---@class Quire.fx_patch_merge_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field patch any

---@class Quire.fx_patch_merge_result
---@field changed any
---@field description string
---@field inactive? string[] Params set that do not apply with the new values: kept, and heard once what fx.describe full's active_when names allows it

---@class Quire.fx_patch_set_args
---@field fx string
---@field patch? any
---@field name? string

---@class Quire.fx_patch_set_result
---@field patch any

---@class Quire.fx_preset_delete_args
---@field fx string
---@field name string

---@class Quire.fx_preset_delete_result
---@field path string

---@class Quire.fx_preset_list_args
---@field fx string

---@class Quire.fx_preset_list_result
---@field presets table[]

---@class Quire.fx_preset_load_args
---@field fx string
---@field name string
---@field source? string

---@class Quire.fx_preset_load_result
---@field name string
---@field source string

---@class Quire.fx_preset_save_args
---@field fx string
---@field name string
---@field replace? boolean (default: false)

---@class Quire.fx_preset_save_result
---@field path string

---@class Quire.fx_remove_args
---@field fx string Plugin instance id, e.g. "fx_1".

---@class Quire.fx_restart_args
---@field fx string Plugin instance id, e.g. "fx_1".

---@class Quire.fx_set_args
---@field fx string Plugin instance id, e.g. "fx_1".
---@field bypass? boolean
---@field offline? boolean
---@field wet? number|string Quantity strings are range-checked by the server.
---@field label? string
---@field isolation? "inherit"|"in_process"|"shared_bridge"|"dedicated"
---@field pins? table

---@class Quire.group_add_args
---@field name string
---@field color? string
---@field active? boolean
---@field members? string[]
---@field links? string[]

---@class Quire.group_add_result
---@field group string

---@class Quire.group_describe_args
---@field group string

---@class Quire.group_describe_result
---@field group string
---@field state table
---@field controllers string[]

---@class Quire.group_list_result
---@field groups table[]

---@class Quire.group_members_set_args
---@field group string
---@field members string[]

---@class Quire.group_remove_args
---@field group string

---@class Quire.group_set_args
---@field group string
---@field name? string
---@field color? string
---@field active? boolean
---@field links? string[]

---@class Quire.group_targets_args
---@field track? string
---@field item? string
---@field attribute string

---@class Quire.group_targets_result
---@field tracks string[]
---@field items string[]

---@class Quire.history_list_result
---@field versions table[]
---@field can_undo boolean
---@field can_redo boolean
---@field truncated boolean
---@field retained_bytes integer
---@field budget_bytes integer

---@class Quire.history_redo_result
---@field version integer
---@field label string

---@class Quire.history_undo_result
---@field version integer
---@field label string

---@class Quire.item_add_args
---@field track string Track id, e.g. "tr_1".
---@field position? number Beats.
---@field bar? integer Start at this bar (1-based).
---@field length? number Beats.
---@field notation? string
---@field audio_file? string
---@field midi_file? string
---@field name? string

---@class Quire.item_add_result
---@field item string
---@field take string
---@field source? string
---@field position number
---@field length number
---@field warnings? string[]

---@class Quire.item_apply_fx_args
---@field item string
---@field mode? string (default: "take")

---@class Quire.item_apply_fx_result
---@field take string
---@field source string
---@field path string

---@class Quire.item_consolidate_args
---@field items string[]

---@class Quire.item_consolidate_commit_args
---@field items string[]
---@field expected string
---@field media table[]

---@class Quire.item_duplicate_args
---@field bypass_groups? boolean (default: false)
---@field item string
---@field to? number

---@class Quire.item_duplicate_result
---@field id string
---@field linked? string[] (default: [])

---@class Quire.item_explode_takes_args
---@field item string
---@field mode? string (default: "tracks")

---@class Quire.item_explode_takes_result
---@field items string[]
---@field tracks string[]

---@class Quire.item_export_args
---@field item string
---@field path string

---@class Quire.item_export_result
---@field path string
---@field format string
---@field sample_format? string
---@field sample_rate? number
---@field channels? integer
---@field frames? integer
---@field notes? integer

---@class Quire.item_heal_args
---@field items string[]

---@class Quire.item_mix_to_track_args
---@field items string[]

---@class Quire.item_mix_to_track_commit_args
---@field items string[]
---@field expected string
---@field start number
---@field end number
---@field media table

---@class Quire.item_move_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1".
---@field position? number
---@field bar? integer
---@field track? string Track id, e.g. "tr_1".

---@class Quire.item_paste_args
---@field version? integer
---@field items table[]

---@class Quire.item_paste_result
---@field items string[]

---@class Quire.item_remove_args
---@field bypass_groups? boolean (default: false)
---@field item string Item id, e.g. "it_1".

---@class Quire.item_set_args
---@field bypass_groups? boolean (default: false)
---@field item string
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

---@class Quire.item_split_args
---@field bypass_groups? boolean (default: false)
---@field item string
---@field at number
---@field snap? "none"|"zero_crossing" (default: "none")

---@class Quire.item_split_result
---@field left string
---@field right string
---@field linked_left? string[] (default: [])
---@field linked_right? string[] (default: [])

---@class Quire.job_cancel_args
---@field job string

---@class Quire.job_get_args
---@field job string

---@class Quire.job_status_args
---@field job string

---@class Quire.job_wait_args
---@field job string
---@field timeout_s? number Seconds to block (default 30). A larger value is clamped to 30 and reported as timeout_clamped_s.

---@class Quire.log_list_args
---@field limit? integer
---@field since? integer
---@field queries? boolean (default: false)
---@field origin? string

---@class Quire.marker_add_args
---@field pos? number
---@field name? string
---@field color? string

---@class Quire.marker_add_result
---@field id string

---@class Quire.marker_list_result
---@field markers table[]

---@class Quire.marker_remove_args
---@field id string

---@class Quire.marker_set_args
---@field id string
---@field pos? number
---@field name? string
---@field color? string

---@class Quire.midi_edit_args
---@field item string
---@field ops table[]

---@class Quire.midi_get_args
---@field item string Item id, e.g. "it_1".

---@class Quire.midi_get_result
---@field text string

---@class Quire.midi_notes_args
---@field item string

---@class Quire.midi_write_args
---@field item string Item id, e.g. "it_1".
---@field notation string

---@class Quire.plugin_list_args
---@field filter? string

---@class Quire.plugin_list_result
---@field plugins table[]
---@field scan table

---@class Quire.plugin_scan_args
---@field wait? boolean false: scan in the background (default true)

---@class Quire.plugin_scan_result
---@field plugins table[]
---@field scan table
---@field job? string the background scan (wait: false)

---@class Quire.project_get_result
---@field name string
---@field path string
---@field branch string
---@field sample_rate number
---@field tempo number
---@field meter string
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field warnings? table[]

---@class Quire.project_new_args
---@field name? string
---@field sample_rate? number Hz; default 48000.

---@class Quire.project_new_result
---@field name string
---@field path string
---@field branch string
---@field sample_rate number
---@field tempo number
---@field meter string
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field warnings? table[]

---@class Quire.project_open_args
---@field path? string

---@class Quire.project_open_result
---@field name string
---@field path string
---@field branch string
---@field sample_rate number
---@field tempo number
---@field meter string
---@field version integer
---@field generation integer
---@field recoverable integer
---@field autosave_available boolean
---@field warnings? table[]

---@class Quire.project_recover_args
---@field files? string[]
---@field track? string Track id, e.g. "tr_1".

---@class Quire.project_recover_result
---@field recovered table[]
---@field warnings? table[]

---@class Quire.project_recoverable_result
---@field warnings? table[]
---@field recordings table[]

---@class Quire.project_save_args
---@field path? string

---@class Quire.project_save_result
---@field path string
---@field warnings? table[]

---@class Quire.project_set_args
---@field name? string
---@field title? string
---@field author? string
---@field notes? string
---@field console9? boolean
---@field history_budget_bytes? integer Estimated retained history byte budget; default 1 GiB. Leases and branch boundaries may exceed it.
---@field render? table Render settings (session-model §1.10); every member optional
---@field timebase? "time"|"beats"|"beats_position"

---@class Quire.project_template_delete_args
---@field name string

---@class Quire.project_template_delete_result
---@field path string

---@class Quire.project_template_list_result
---@field templates table[]

---@class Quire.project_template_load_args
---@field name string

---@class Quire.project_template_load_result
---@field name string

---@class Quire.project_template_save_args
---@field name string
---@field replace? boolean (default: false)

---@class Quire.project_template_save_result
---@field path string
---@field warnings? table[]

---@class Quire.region_add_args
---@field start? number
---@field end? number
---@field name? string
---@field color? string

---@class Quire.region_add_result
---@field id string

---@class Quire.region_list_result
---@field regions table[]

---@class Quire.region_remove_args
---@field id string

---@class Quire.region_set_args
---@field id string
---@field start? number
---@field end? number
---@field name? string
---@field color? string

---@class Quire.registry_describe_args
---@field command string

---@class Quire.registry_list_args
---@field tag? string
---@field kind? string
---@field text? string

---@class Quire.render_dry_run_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? number|string File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? number|string 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
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

---@class Quire.render_mix_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? number|string File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? number|string 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
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

---@class Quire.render_preset_delete_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)

---@class Quire.render_preset_load_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)

---@class Quire.render_preset_save_args
---@field name? string A preset name (render.preset.list)
---@field default? boolean (default: false)
---@field settings? table Render settings (session-model §1.10); every member optional

---@class Quire.render_queue_add_args
---@field command string
---@field args any
---@field label? string

---@class Quire.render_queue_remove_args
---@field id? string
---@field all? boolean (default: false)

---@class Quire.render_queue_run_args
---@field job? boolean (default: false)

---@class Quire.render_regions_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? number|string File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? number|string 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
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

---@class Quire.render_settings_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? number|string File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? number|string 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
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

---@class Quire.render_stems_args
---@field source? "mix"|"stems"|"regions" The dialog's source; render.mix always renders the mix
---@field masters? string[] The mix: the masters to render, a file each (default: tr_0)
---@field bounds? "project"|"loop"|"custom" project (default), loop or custom (start_beat, end_beat)
---@field start_beat? number
---@field end_beat? number
---@field tail_s? number Seconds rendered past the end
---@field pattern? number|string File name pattern (session-model §7.3; may contain / for subfolders), for every source or as {mix, stems, regions}
---@field sample_rate? integer Default: the project rate
---@field channels? number|string 1 (mono, (L+R)/2), 2 (stereo) or "track" (stems: mono files for 1-channel tracks)
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

---@class Quire.route_add_args
---@field from string
---@field to? string
---@field fx? string Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out
---@field tap? "post_fader"|"pre_fader"|"pre_fx"
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? string|table
---@field mute? boolean
---@field mono? boolean Sum the source to mono before the pan
---@field phase_invert? boolean
---@field channels? table Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity

---@class Quire.route_add_result
---@field id string

---@class Quire.route_list_result
---@field routes table[]

---@class Quire.route_remove_args
---@field route string

---@class Quire.route_set_args
---@field route string
---@field from? string
---@field to? string
---@field fx? string Feed this plug-in's sidechain (key) input instead of a track's input; `to` may be left out
---@field tap? "post_fader"|"pre_fader"|"pre_fx"
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? string|table
---@field mute? boolean
---@field mono? boolean Sum the source to mono before the pan
---@field phase_invert? boolean
---@field channels? table Which source channel feeds which destination channel, pair by pair: {from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left channel to both, {from: [], to: []} restores the identity

---@class Quire.script_run_args
---@field code string The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays).
---@field name? string The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name.
---@field budget_ms? number Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count.
---@field args? any Converted to the global table `args`.

---@class Quire.script_run_result
---@field result any What the script returned (null if it returned nothing).
---@field log string[] What the script printed.
---@field log_dropped integer Cumulative lines evicted from this script's retained log.
---@field script string The name it ran under.
---@field instructions integer Lua instructions the run executed.

---@class Quire.session_describe_result
---@field text string

---@class Quire.take_activate_args
---@field item string
---@field take? string
---@field index? integer

---@class Quire.take_add_args
---@field item string
---@field from? string

---@class Quire.take_add_result
---@field id string

---@class Quire.take_extract_args
---@field take string
---@field track string
---@field position? number Beats.

---@class Quire.take_extract_result
---@field item string

---@class Quire.take_hits_detect_args
---@field takes string[]
---@field sensitivity? number (default: 0.5)

---@class Quire.take_hits_detect_result
---@field job string

---@class Quire.take_hits_set_args
---@field take string
---@field positions number[]
---@field source_hash? string (default: "")
---@field algorithm? string (default: "manual")
---@field sensitivity? number (default: 0.5)

---@class Quire.take_list_args
---@field item string

---@class Quire.take_list_result
---@field takes table[]

---@class Quire.take_remove_args
---@field take string

---@class Quire.take_remove_result
---@field item string
---@field item_removed boolean

---@class Quire.take_set_args
---@field take string
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

---@class Quire.take_stretch_add_args
---@field take string
---@field source_pos? number
---@field item_pos number

---@class Quire.take_stretch_move_args
---@field take string
---@field index integer
---@field item_pos number

---@class Quire.take_stretch_remove_args
---@field take string
---@field index integer

---@class Quire.tempo_set_args
---@field points table[]

---@class Quire.tempo_set_result
---@field count integer

---@class Quire.time_convert_args
---@field seconds? number
---@field beat? number
---@field bar? integer

---@class Quire.time_convert_result
---@field seconds number
---@field beat number
---@field bar integer
---@field beat_in_bar integer
---@field fraction number

---@class Quire.timesig_remove_args
---@field bar integer

---@class Quire.timesig_set_args
---@field bar integer
---@field meter string

---@class Quire.track_add_args
---@field type? "channel"|"buss"|"master" (default: "channel")
---@field name? string
---@field parent? string Parent track id, e.g. "tr_1".
---@field index? integer
---@field channels? integer

---@class Quire.track_add_result
---@field track string

---@class Quire.track_describe_args
---@field track string
---@field at? number

---@class Quire.track_describe_result
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

---@class Quire.track_duplicate_args
---@field tracks string[]

---@class Quire.track_duplicate_result
---@field tracks string[]

---@class Quire.track_freeze_args
---@field track string
---@field job? boolean (default: false)

---@class Quire.track_freeze_commit_args
---@field track string
---@field expected string
---@field media table
---@field freeze table
---@field states table[]

---@class Quire.track_list_args
---@field at? number

---@class Quire.track_list_result
---@field tracks table[]

---@class Quire.track_move_args
---@field track string
---@field parent? string
---@field index? integer

---@class Quire.track_remove_args
---@field track string Track id, e.g. "tr_1".

---@class Quire.track_set_args
---@field bypass_groups? boolean (default: false)
---@field track string Track id, e.g. "tr_1".
---@field type? "channel"|"buss"|"master"
---@field name? string
---@field color? string
---@field notes? string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field pan? number|string Quantity strings are range-checked by the server.
---@field mute? boolean
---@field phase_invert? boolean Flip the polarity of the main pair (after the FX chain, before the fader)
---@field mono? boolean Sum the main pair to mono (after the FX chain, before the fader and pan): a mono check on the master
---@field solo? string
---@field armed? boolean
---@field record_input? integer[]
---@field record_midi? string
---@field midi_channel? integer
---@field record_mode? "input"|"midi_overdub"|"midi_replace"
---@field monitor? "off"|"on"|"auto"
---@field parent_send? boolean The implicit send to the parent buss/master (off: only explicit sends leave the strip)
---@field timebase? "project"|"time"|"beats"|"beats_position" What this channel's items follow when the tempo changes (those that inherit it): project, beats, time or beats_position; they keep their place now
---@field channels? integer 1-2: the strip's main pair; up to 128 adds auxiliary channels 3-128 for routes and plug-in pins (a detector on 3/4, a multi-out instrument's outputs), outside the fader, pan and parent send
---@field hw_output? integer[] Masters: the device outputs the main pair plays to: [l, r], [mono] or [] for none (engine.inputs lists them). Default: tr_0 [1, 2], other masters []
---@field lane_mute? boolean Masters: silence the master's own items (its prints) and nothing else

---@class Quire.track_template_delete_args
---@field name string

---@class Quire.track_template_delete_result
---@field path string

---@class Quire.track_template_list_result
---@field templates table[]

---@class Quire.track_template_load_args
---@field name? string
---@field path? string
---@field parent? string

---@class Quire.track_template_load_result
---@field track string
---@field effects string[]

---@class Quire.track_template_save_args
---@field track string
---@field name string
---@field replace? boolean (default: false)

---@class Quire.track_template_save_result
---@field path string
---@field effects integer

---@class Quire.track_unfreeze_args
---@field track string

---@class Quire.transport_get_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.transport_loop_args
---@field enabled? boolean
---@field clear? boolean
---@field start_seconds? number
---@field start_beat? number
---@field start_bar? integer
---@field end_seconds? number
---@field end_beat? number
---@field end_bar? integer

---@class Quire.transport_loop_result
---@field enabled boolean
---@field start_seconds number
---@field end_seconds number

---@class Quire.transport_metronome_args
---@field enabled? boolean
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field count_in_bars? integer
---@field count_in_playback? boolean

---@class Quire.transport_metronome_result
---@field enabled boolean
---@field volume_db number
---@field count_in_bars integer
---@field count_in_playback boolean

---@class Quire.transport_pause_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.transport_play_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.transport_playrate_get_result
---@field requested_rate number
---@field effective_rate number
---@field preserve_pitch boolean
---@field envelope? string

---@class Quire.transport_playrate_set_args
---@field rate? number
---@field preserve_pitch? boolean

---@class Quire.transport_playrate_set_result
---@field requested_rate number
---@field effective_rate number
---@field preserve_pitch boolean
---@field envelope? string

---@class Quire.transport_punch_args
---@field enabled? boolean
---@field clear? boolean
---@field start_seconds? number
---@field start_beat? number
---@field start_bar? integer
---@field end_seconds? number
---@field end_beat? number
---@field end_bar? integer

---@class Quire.transport_punch_result
---@field enabled boolean
---@field start_seconds number
---@field end_seconds number

---@class Quire.transport_record_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.transport_seek_args
---@field seconds? number
---@field beat? number
---@field bar? integer

---@class Quire.transport_seek_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.transport_stop_result
---@field playing boolean
---@field recording boolean
---@field counting_in boolean
---@field seconds number
---@field beat number
---@field sample integer
---@field loop table
---@field punch table
---@field recorded? table[]

---@class Quire.vca_add_args
---@field group string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field mute? boolean
---@field solo? boolean
---@field followers? string[]

---@class Quire.vca_members_set_args
---@field group string
---@field tracks? string[]
---@field vcas? string[]

---@class Quire.vca_remove_args
---@field group string

---@class Quire.vca_set_args
---@field group string
---@field volume_db? number|string Quantity strings are range-checked by the server.
---@field mute? boolean
---@field solo? boolean
---@field followers? string[]

---@class Quire.analyze
---@field compare fun(args: Quire.analyze_compare_args): Quire.analyze_compare_result Compare two renders or track outputs by perceptual descriptors.
---@field masking fun(args: Quire.analyze_masking_args): table Estimate masking between two tracks over the same range.
---@field render fun(args: Quire.analyze_render_args): table Analyze a render id or audio path: loudness, spectrum, envelope and stereo.
---@field track fun(args: Quire.analyze_track_args): table Render one track's output offline and analyze it.

---@class Quire.audio
---@field quantize fun(args: Quire.audio_quantize_args): table Quantize detected audio hits together using stretch markers or slices.

---@class Quire.audio.warp
---@field detach fun(args: Quire.audio_warp_detach_args): table Detach selected items from jointly processed warp bundles.
---@field reset fun(args: Quire.audio_warp_reset_args): table Remove selected items' warping while keeping detected hits.

---@class Quire.autoitem
---@field add fun(args: Quire.autoitem_add_args): table Put an automation item on an envelope: a block that replaces it there.
---@field duplicate fun(args: Quire.autoitem_duplicate_args): table Copy an automation item (pooled by default: edits to either reach both).
---@field list fun(args?: Quire.autoitem_list_args): table List automation items (all, or one envelope's), with their points.
---@field remove fun(args: Quire.autoitem_remove_args): table Remove an automation item (the envelope under it plays again).
---@field set fun(args: Quire.autoitem_set_args): table Move, resize, loop, scale or rewrite an automation item.
---@field unpool fun(args: Quire.autoitem_unpool_args): table Take an automation item out of its pool (its points stay).

---@class Quire.branch
---@field create fun(args: Quire.branch_create_args): Quire.branch_create_result Start a branch from a branch's head or an earlier version, and switch to it.
---@field delete fun(args: Quire.branch_delete_args): table Delete a branch (not main, not the current one).
---@field diff fun(args: Quire.branch_diff_args): Quire.branch_diff_result What differs between two branch heads: objects added, removed, changed (fields).
---@field list fun(): Quire.branch_list_result List branches and the current one.
---@field merge fun(args: Quire.branch_merge_args): Quire.branch_merge_result Merge a branch into another (default: main) by replaying its edits.
---@field ["render-ab"] fun(args: Quire.branch_render_ab_args): table Render two branch heads over the same range and compare them.
---@field switch fun(args: Quire.branch_switch_args): Quire.branch_switch_result Make another branch current (edits, undo and playback follow it).

---@class Quire.dsp
---@field check fun(args?: Quire.dsp_check_args): Quire.dsp_check_result Compile a DSP script without saving it; return its diagnostics.
---@field edit fun(args: Quire.dsp_edit_args): Quire.dsp_edit_result Change part of a library script by replacing text; compile it, as dsp.write.
---@field export fun(args: Quire.dsp_export_args): Quire.dsp_export_result Export a DSP script as a plugin for other DAWs: a CLAP file, or C++ source.
---@field get fun(args: Quire.dsp_get_args): Quire.dsp_get_result Read a DSP script from the library.
---@field list fun(): Quire.dsp_list_result List the DSP scripts in the library, compiled, with their plugin ids.
---@field preview fun(args?: Quire.dsp_preview_args): Quire.dsp_preview_result Draw a plugin's editor to a PNG, after a test signal plays through it.
---@field test fun(args?: Quire.dsp_test_args): Quire.dsp_test_result Play test signals or notes through a plugin and measure it: levels, THD, pitch.
---@field write fun(args: Quire.dsp_write_args): Quire.dsp_write_result Save a DSP script to the library and compile it, with diagnostics.

---@class Quire.engine
---@field devices fun(args?: Quire.engine_devices_args): Quire.engine_devices_result List native audio device names, host APIs, rates and channel capacities.
---@field inputs fun(): table The device's audio inputs, MIDI devices and the outputs masters play to.
---@field start fun(args?: Quire.engine_start_args): Quire.engine_start_result Start playback through JACK, native PortAudio, or a device-free dummy.
---@field status fun(): Quire.engine_status_result Real-time engine state: backend, rate, block, device period, latency, xruns.
---@field stop fun(): table Stop real-time playback and release the audio device.

---@class Quire.env
---@field add fun(args: Quire.env_add_args): Quire.env_add_result Automate a parameter: a track's or route's mixer control, or a plugin's.
---@field list fun(args?: Quire.env_list_args): Quire.env_list_result List envelopes (optionally of one track, route or plugin).
---@field remove fun(args: Quire.env_remove_args): table Remove an envelope (the parameter returns to its set value).
---@field set fun(args: Quire.env_set_args): table Set an envelope's options: active (heard), mode (read, touch, latch, write).

---@class Quire.env.points
---@field set fun(args: Quire.env_points_set_args): Quire.env_points_set_result Replace an envelope's points (all of them, or those in a beat range).

---@class Quire.fx
---@field add fun(args: Quire.fx_add_args): Quire.fx_add_result Insert a plugin into a track's (or a take's) FX chain and return its id.
---@field copy fun(args: Quire.fx_copy_args): Quire.fx_copy_result Copy a plugin and its current settings into a track's FX chain.
---@field describe fun(args: Quire.fx_describe_args): Quire.fx_describe_result Describe an instance and its live problems; full adds parameters and patch.
---@field meters fun(args: Quire.fx_meters_args): Quire.fx_meters_result Read a plugin's meters and readouts, playing now or as a render ended.
---@field move fun(args: Quire.fx_move_args): table Move a plugin within a track chain or to another track.
---@field remove fun(args: Quire.fx_remove_args): table Remove a plugin instance (and its envelopes).
---@field restart fun(args: Quire.fx_restart_args): table Re-create a plugin instance from its saved patch/state (after a crash).
---@field set fun(args: Quire.fx_set_args): table Set instance options: bypass, offline, wet (%), label, isolation, pins.

---@class Quire.fx.chain
---@field delete fun(args: Quire.fx_chain_delete_args): Quire.fx_chain_delete_result Delete a saved FX chain preset by name.
---@field list fun(): Quire.fx_chain_list_result List saved FX chain presets by name, path and effect count.
---@field load fun(args: Quire.fx_chain_load_args): Quire.fx_chain_load_result Append or replace a track's FX chain from a preset in one undo step.
---@field save fun(args: Quire.fx_chain_save_args): Quire.fx_chain_save_result Save an ordered FX chain as a named preset.

---@class Quire.fx.learn
---@field list fun(args?: Quire.fx_learn_list_args): Quire.fx_learn_list_result List the MIDI controller bindings of plugin parameters.
---@field remove fun(args: Quire.fx_learn_remove_args): table Remove a plugin parameter's MIDI controller binding.
---@field set fun(args: Quire.fx_learn_set_args): Quire.fx_learn_set_result Let a MIDI controller (CC, note, pitch bend) move a plugin parameter.

---@class Quire.fx.param
---@field get fun(args: Quire.fx_param_get_args): Quire.fx_param_get_result Read one saved plugin parameter by semantic key or numeric CLAP id.
---@field set fun(args: Quire.fx_param_set_args): Quire.fx_param_set_result Set semantic parameters in units, or plain plugin parameters by name/id.

---@class Quire.fx.patch
---@field get fun(args: Quire.fx_patch_get_args): Quire.fx_patch_get_result Read a semantic plugin's saved patch.
---@field list fun(args: Quire.fx_patch_list_args): Quire.fx_patch_list_result List an instance's read-only factory patches, by name.
---@field merge fun(args: Quire.fx_patch_merge_args): Quire.fx_patch_merge_result Merge a partial patch into a plugin instance; returns the effective change.
---@field set fun(args: Quire.fx_patch_set_args): Quire.fx_patch_set_result Replace a semantic patch, or apply a factory patch by name.

---@class Quire.fx.preset
---@field delete fun(args: Quire.fx_preset_delete_args): Quire.fx_preset_delete_result Delete one of the user's presets for a plugin; factory presets stay.
---@field list fun(args: Quire.fx_preset_list_args): Quire.fx_preset_list_result List a plugin's presets: its factory ones, then the user's.
---@field load fun(args: Quire.fx_preset_load_args): Quire.fx_preset_load_result Apply a factory or user preset to a plugin instance in one undo step.
---@field save fun(args: Quire.fx_preset_save_args): Quire.fx_preset_save_result Save a plugin instance's current settings as a named user preset.

---@class Quire.group
---@field add fun(args: Quire.group_add_args): Quire.group_add_result Create a named track group with optional links and members.
---@field describe fun(args: Quire.group_describe_args): Quire.group_describe_result Describe a track group's membership, links and VCA ancestry.
---@field list fun(): Quire.group_list_result List track groups, linked attributes and optional VCA controls.
---@field remove fun(args: Quire.group_remove_args): table Remove a track group and its VCA, keeping its tracks.
---@field set fun(args: Quire.group_set_args): table Change a group's name, color, links or active state.
---@field targets fun(args: Quire.group_targets_args): Quire.group_targets_result Resolve linked tracks or matching items for an edit or selection.

---@class Quire.group.members
---@field set fun(args: Quire.group_members_set_args): table Replace a group's track membership; an empty array clears it.

---@class Quire.history
---@field list fun(): Quire.history_list_result List the current branch's recent versions (newest first).
---@field redo fun(): Quire.history_redo_result Redo the last undone change.
---@field undo fun(): Quire.history_undo_result Undo the last change on the current branch.

---@class Quire.item
---@field add fun(args: Quire.item_add_args): Quire.item_add_result Add an item on a track: MIDI in notation, a MIDI file or an audio file.
---@field ["apply-fx"] fun(args: Quire.item_apply_fx_args): Quire.item_apply_fx_result Render an item through its take and track FX into a new audio take.
---@field consolidate fun(args: Quire.item_consolidate_args): table Consolidate selected audio and MIDI per track in a background job.
---@field duplicate fun(args: Quire.item_duplicate_args): Quire.item_duplicate_result Copy an item and its takes to a new beat position (default: immediately after).
---@field ["explode-takes"] fun(args: Quire.item_explode_takes_args): Quire.item_explode_takes_result Spread an item's takes onto new tracks, or one after another on its track.
---@field export fun(args: Quire.item_export_args): Quire.item_export_result Write an item's raw audio (WAV) or its MIDI (SMF) to a file.
---@field heal fun(args: Quire.item_heal_args): table Heal compatible consecutive audio pieces without rendering.
---@field ["mix-to-track"] fun(args: Quire.item_mix_to_track_args): table Mix the selected items through their strips onto a new muted channel.
---@field move fun(args: Quire.item_move_args): table Move an item in time (position in beats or bar) and/or to another track.
---@field paste fun(args: Quire.item_paste_args): Quire.item_paste_result Copy items as they were in a version (a clipboard) to tracks and beats.
---@field remove fun(args: Quire.item_remove_args): table Remove an item with its takes (unpooled sources go too).
---@field set fun(args: Quire.item_set_args): table Set an item's position, length, playback, fades, name, color, or lock.
---@field split fun(args: Quire.item_split_args): Quire.item_split_result Split an item at a beat, optionally at the prior audio zero crossing.

---@class Quire.item.consolidate
---@field commit fun(args: Quire.item_consolidate_commit_args): table Attach consolidated audio and merge MIDI as one replayable edit.

---@class Quire.item.mix-to-track
---@field commit fun(args: Quire.item_mix_to_track_commit_args): table Put a finished item mix on a new muted channel as one replayable edit.

---@class Quire.job
---@field cancel fun(args: Quire.job_cancel_args): table Cancel a job.
---@field get fun(args: Quire.job_get_args): table Get a job's status and progress.
---@field list fun(): table List session jobs.
---@field status fun(args: Quire.job_status_args): table Get a job's status.
---@field wait fun(args: Quire.job_wait_args): table Wait for a job.

---@class Quire.log
---@field list fun(args?: Quire.log_list_args): table Recent commands with origin, arguments and outcome (the command log).

---@class Quire.marker
---@field add fun(args?: Quire.marker_add_args): Quire.marker_add_result Add a named marker in beats.
---@field list fun(): Quire.marker_list_result List rulers in this project.
---@field remove fun(args: Quire.marker_remove_args): table Remove a ruler.
---@field set fun(args: Quire.marker_set_args): table Edit a ruler's position, name, or color.

---@class Quire.midi
---@field edit fun(args: Quire.midi_edit_args): table Edit a MIDI item's notes and CCs: add, remove, move, resize, velocity, quantize.
---@field get fun(args: Quire.midi_get_args): Quire.midi_get_result Read an item's MIDI as notation.
---@field notes fun(args: Quire.midi_notes_args): table List a MIDI item's notes and CC points (beats from the item's start).
---@field write fun(args: Quire.midi_write_args): table Replace an item's MIDI with notation (the item keeps its position and length).

---@class Quire.plugin
---@field list fun(args?: Quire.plugin_list_args): Quire.plugin_list_result List scanned plugins; filter matches id, name, or vendor.
---@field scan fun(args?: Quire.plugin_scan_args): Quire.plugin_scan_result Rescan configured plugin paths and return the available plugins.

---@class Quire.project
---@field get fun(): Quire.project_get_result Show the project: name, rate, tempo, meter, file, version and branch.
---@field new fun(args?: Quire.project_new_args): Quire.project_new_result Start a new, empty project (replaces the current one and its history).
---@field open fun(args?: Quire.project_open_args): Quire.project_open_result Open a project file (replaces the current project and its history).
---@field recover fun(args?: Quire.project_recover_args): Quire.project_recover_result Put recordings a crash left out back into the project (one undo step).
---@field recoverable fun(): Quire.project_recoverable_result List recordings a crash left out of the project.
---@field save fun(args?: Quire.project_save_args): Quire.project_save_result Save the project (canonical JSON) to its file or a new path.
---@field set fun(args?: Quire.project_set_args): table Change project metadata (name, title, author, notes, Console 9, timebase).

---@class Quire.project.template
---@field delete fun(args: Quire.project_template_delete_args): Quire.project_template_delete_result Delete a saved project template by name.
---@field list fun(): Quire.project_template_list_result List saved project templates by name and path.
---@field load fun(args: Quire.project_template_load_args): Quire.project_template_load_result Start a new, unsaved project from a project template.
---@field save fun(args: Quire.project_template_save_args): Quire.project_template_save_result Save the whole project as a named project template.

---@class Quire.region
---@field add fun(args?: Quire.region_add_args): Quire.region_add_result Add a named region in beats.
---@field list fun(): Quire.region_list_result List rulers in this project.
---@field remove fun(args: Quire.region_remove_args): table Remove a ruler.
---@field set fun(args: Quire.region_set_args): table Edit a ruler's position, name, or color.

---@class Quire.registry
---@field describe fun(args: Quire.registry_describe_args): table Describe a command.
---@field list fun(args?: Quire.registry_list_args): table List available commands.

---@class Quire.render
---@field ["dry-run"] fun(args?: Quire.render_dry_run_args): table List the files a render would write, with lengths, without rendering.
---@field mix fun(args?: Quire.render_mix_args): table Render the mix offline with the project's render settings (default: whole song).
---@field regions fun(args?: Quire.render_regions_args): table Render each region (or the render matrix's tracks per region) to files.
---@field settings fun(args?: Quire.render_settings_args): table The render settings that apply, and the layer each one comes from.
---@field stems fun(args?: Quire.render_stems_args): table Render each selected track to its own file (stems).

---@class Quire.render.preset
---@field delete fun(args?: Quire.render_preset_delete_args): table Delete one of the user's render presets, or the user's render default.
---@field list fun(): table Render presets: the factory ones, then the user's, with their settings.
---@field load fun(args?: Quire.render_preset_load_args): table Replace the project's render settings with a preset (one undo step).
---@field save fun(args?: Quire.render_preset_save_args): table Save render settings as a named preset, or as the user's render default.

---@class Quire.render.queue
---@field add fun(args: Quire.render_queue_add_args): table Queue a render (mix, stems or regions) to run later with render.queue.run.
---@field list fun(): table The queued renders, in the order render.queue.run runs them.
---@field remove fun(args?: Quire.render_queue_remove_args): table Take a render off the queue (id), or clear it (all: true).
---@field run fun(args?: Quire.render_queue_run_args): table Run every queued render in order; each one leaves the queue when it is done.

---@class Quire.route
---@field add fun(args: Quire.route_add_args): Quire.route_add_result Send one track to another track, bus, the master, or a plug-in's key.
---@field list fun(): Quire.route_list_result List sends with endpoints and mixer settings.
---@field matrix fun(): table What can be routed where: strips, plug-in keys, sends and blocked pairs.
---@field remove fun(args: Quire.route_remove_args): table Remove a send.
---@field set fun(args: Quire.route_set_args): table Change a route's endpoints, tap, level, pan, mute, or channel map.

---@class Quire.script
---@field run fun(args: Quire.script_run_args): Quire.script_run_result Run a Lua script on the command bus.

---@class Quire.session
---@field describe fun(): Quire.session_describe_result Describe the whole session in compact text (tracks, items, fx, routes).

---@class Quire.take
---@field activate fun(args: Quire.take_activate_args): table Choose the take heard from an item.
---@field add fun(args: Quire.take_add_args): Quire.take_add_result Add a take to an item, optionally copied from another take of that item.
---@field extract fun(args: Quire.take_extract_args): Quire.take_extract_result Take a take out of its item into an item of its own on any channel.
---@field list fun(args: Quire.take_list_args): Quire.take_list_result List the takes of an item and identify its active take.
---@field remove fun(args: Quire.take_remove_args): Quire.take_remove_result Delete a take from its item (the item goes with its last take).
---@field set fun(args: Quire.take_set_args): table Set take gain, pan, channel mode, name, source offset, or playrate and pitch.

---@class Quire.take.hits
---@field detect fun(args: Quire.take_hits_detect_args): Quire.take_hits_detect_result Detect transient candidates in audio takes, then commit one undoable edit.
---@field set fun(args: Quire.take_hits_set_args): table Replace a take's transient candidates in original source seconds.

---@class Quire.take.stretch
---@field add fun(args: Quire.take_stretch_add_args): table Add a pitch-preserving audio stretch marker.
---@field move fun(args: Quire.take_stretch_move_args): table Move an audio stretch marker's item position.
---@field remove fun(args: Quire.take_stretch_remove_args): table Remove an interior audio stretch marker.

---@class Quire.tempo
---@field set fun(args: Quire.tempo_set_args): Quire.tempo_set_result Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.

---@class Quire.time
---@field convert fun(args?: Quire.time_convert_args): Quire.time_convert_result Convert a seconds, beat, or bar position to all three time domains.

---@class Quire.timesig
---@field remove fun(args: Quire.timesig_remove_args): table Remove the meter change at a bar: the meter before it continues.
---@field set fun(args: Quire.timesig_set_args): table Set the meter from a bar on, e.g. {"bar": 9, "meter": "3/4"}.

---@class Quire.track
---@field add fun(args?: Quire.track_add_args): Quire.track_add_result Add a channel, buss or master strip and return its id.
---@field describe fun(args: Quire.track_describe_args): Quire.track_describe_result Describe track groups and requested/applied effective gain at a time.
---@field duplicate fun(args: Quire.track_duplicate_args): Quire.track_duplicate_result Duplicate channels or busses with their FX, items, sends and automation.
---@field freeze fun(args: Quire.track_freeze_args): table Freeze a channel's items and FX to pre-fader audio and release its plugins.
---@field list fun(args?: Quire.track_list_args): Quire.track_list_result List tracks in tree order with their level, mute/solo, fx and items.
---@field move fun(args: Quire.track_move_args): table Move a track under another track or reorder it among its siblings.
---@field remove fun(args: Quire.track_remove_args): table Remove a track with its items, fx, routes and envelopes (not a folder).
---@field set fun(args: Quire.track_set_args): table Set track fields: name, type, volume, pan, mute, solo, polarity, mono, arming.
---@field unfreeze fun(args: Quire.track_unfreeze_args): table Restore a frozen channel's original items and plugin state.

---@class Quire.track.freeze
---@field commit fun(args: Quire.track_freeze_commit_args): table Attach a verified freeze artifact as one replayable edit.

---@class Quire.track.template
---@field delete fun(args: Quire.track_template_delete_args): Quire.track_template_delete_result Delete a saved track template by name.
---@field list fun(): Quire.track_template_list_result List saved track templates by name, path and effect count.
---@field load fun(args?: Quire.track_template_load_args): Quire.track_template_load_result Add a strip from a track template (settings and FX) in one undo step.
---@field save fun(args: Quire.track_template_save_args): Quire.track_template_save_result Save a strip (settings and FX chains) as a named track template.

---@class Quire.transport
---@field get fun(): Quire.transport_get_result The play position and whether it is playing.
---@field loop fun(args?: Quire.transport_loop_args): Quire.transport_loop_result Set or toggle the loop range: playback reaching its end repeats it.
---@field metronome fun(args?: Quire.transport_metronome_args): Quire.transport_metronome_result Turn the metronome on or off and set its level; returns the settings.
---@field pause fun(): Quire.transport_pause_result Pause playback where it is (ends a recording).
---@field play fun(): Quire.transport_play_result Start playback from the current position.
---@field punch fun(args?: Quire.transport_punch_args): Quire.transport_punch_result Set or toggle the punch range: recordings keep only that range.
---@field record fun(): Quire.transport_record_result Play and record every armed track's input or master's output.
---@field seek fun(args?: Quire.transport_seek_args): Quire.transport_seek_result Move the play position (one of seconds, beat or bar).
---@field stop fun(): Quire.transport_stop_result Stop playback and return to where it started (ends a recording).

---@class Quire.transport.playrate
---@field ["apply-to-tempo"] fun(): table Scale every tempo-point BPM by master playrate, then reset the rate to 1.
---@field get fun(): Quire.transport_playrate_get_result Get requested/effective master playrate, pitch mode and active envelope.
---@field set fun(args?: Quire.transport_playrate_set_args): Quire.transport_playrate_set_result Set the project's master playrate ratio and preserve-pitch flag.

---@class Quire.vca
---@field add fun(args: Quire.vca_add_args): table Add an audio-free VCA fader to a track group.
---@field remove fun(args: Quire.vca_remove_args): table Remove a VCA and its automation, keeping the track group.
---@field set fun(args: Quire.vca_set_args): table Set a VCA's level, mute or solo without moving member faders.

---@class Quire.vca.members
---@field set fun(args: Quire.vca_members_set_args): table Replace the tracks or nested VCAs controlled by a VCA.

---@class Quire
---@field analyze Quire.analyze Commands named analyze.*
---@field audio Quire.audio Commands named audio.*
---@field autoitem Quire.autoitem Commands named autoitem.*
---@field branch Quire.branch Commands named branch.*
---@field dsp Quire.dsp Commands named dsp.*
---@field engine Quire.engine Commands named engine.*
---@field env Quire.env Commands named env.*
---@field fx Quire.fx Commands named fx.*
---@field group Quire.group Commands named group.*
---@field history Quire.history Commands named history.*
---@field item Quire.item Commands named item.*
---@field job Quire.job Commands named job.*
---@field log Quire.log Commands named log.*
---@field marker Quire.marker Commands named marker.*
---@field midi Quire.midi Commands named midi.*
---@field plugin Quire.plugin Commands named plugin.*
---@field project Quire.project Commands named project.*
---@field region Quire.region Commands named region.*
---@field registry Quire.registry Commands named registry.*
---@field render Quire.render Commands named render.*
---@field route Quire.route Commands named route.*
---@field script Quire.script Commands named script.*
---@field session Quire.session Commands named session.*
---@field take Quire.take Commands named take.*
---@field tempo Quire.tempo Commands named tempo.*
---@field time Quire.time Commands named time.*
---@field timesig Quire.timesig Commands named timesig.*
---@field track Quire.track Commands named track.*
---@field transport Quire.transport Commands named transport.*
---@field vca Quire.vca Commands named vca.*
---@field run fun(name: string, args?: table): any Run any registered command by name, e.g. the ones whose names are not Lua identifiers.
---@field batch fun(label: string, body: fun()) Run the calls inside body as one transaction: one undo step, rolled back when one of them fails.
---@field on fun(topic: string, handler: fun(payload: table, topic: string), filter?: table): integer Subscribe to bus events; returns a handle for q.off.
---@field off fun(handle: integer) Drop a subscription.
---@field log fun(...) Print a line to the script's log (the same as print).
---@field null any The JSON null: Lua tables cannot hold nil.
---@field name string The name this script runs under (its origin is `lua:<name>`).
