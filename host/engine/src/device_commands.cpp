// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Audio engine commands: engine.devices, engine.start, engine.stop, engine.inputs and
// engine.status.
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- engine / transport -------------------------------------------------------------------

enum class BackendArg { Jack, Dummy, PortAudio };

struct EngineDevicesArgs {
    std::optional<BackendArg> backend;
};

struct EngineDevicesResult {
    std::vector<AudioDevice> devices;
};

struct EngineStartArgs {
    std::optional<BackendArg> backend;
    std::optional<std::string> hostApi, inputDevice, outputDevice;
    std::optional<double> rate;
    std::optional<std::int64_t> inputs, outputs;
    std::optional<std::int64_t> block;
    std::optional<bool> anticipative, forceGraph;
};

schema::Schema<MeterDrops> meterDropsSchema() {
    return schema::object<MeterDrops>("meter_drops")
        .field("renderer", &MeterDrops::renderer, schema::integer())
        .field("callback", &MeterDrops::callback, schema::integer())
        .build();
}

schema::Schema<RuntimeProtectionError> runtimeProtectionErrorSchema() {
    return schema::object<RuntimeProtectionError>("runtime_protection_error")
        .field("code", &RuntimeProtectionError::code, schema::string())
        .field("message", &RuntimeProtectionError::message, schema::string())
        .field("path", &RuntimeProtectionError::path, schema::string())
        .field("hint", &RuntimeProtectionError::hint, schema::string())
        .field("errno", &RuntimeProtectionError::errnoValue, schema::integer())
        .build();
}

schema::Schema<RuntimeProtectionStatus> runtimeProtectionSchema() {
    return schema::object<RuntimeProtectionStatus>("runtime_protection_status")
        .field("requested", &RuntimeProtectionStatus::requested, schema::boolean())
        .field("active", &RuntimeProtectionStatus::active, schema::boolean())
        .field("error", &RuntimeProtectionStatus::error, runtimeProtectionErrorSchema())
        .build();
}

// memory_lock's fields, and whether a live bridged plug-in holds it (ADR-0086).
schema::Schema<RuntimeProtectionStatus> cpuIdleLatencySchema() {
    return schema::object<RuntimeProtectionStatus>("cpu_idle_latency_status")
        .field("requested", &RuntimeProtectionStatus::requested, schema::boolean())
        .field("active", &RuntimeProtectionStatus::active, schema::boolean())
        .field("automatic", &RuntimeProtectionStatus::automatic, schema::boolean())
        .field("error", &RuntimeProtectionStatus::error, runtimeProtectionErrorSchema())
        .build();
}

schema::Schema<ThpOptOutStatus> thpOptOutSchema() {
    return schema::object<ThpOptOutStatus>("thp_opt_out_status")
        .field("active", &ThpOptOutStatus::active, schema::boolean())
        .field("advised_only", &ThpOptOutStatus::advisedOnly, schema::boolean())
        .field("inherited", &ThpOptOutStatus::inherited, schema::boolean())
        .field("error", &ThpOptOutStatus::error, runtimeProtectionErrorSchema())
        .build();
}

schema::Schema<EngineStatus::AudioPageFaults> audioPageFaultsSchema() {
    return schema::object<EngineStatus::AudioPageFaults>("audio_page_faults")
        .field("scope", &EngineStatus::AudioPageFaults::scope,
               schema::string().pattern("^(audio_threads|process|none)$",
                                        "audio_threads, process or none"))
        .field("minor", &EngineStatus::AudioPageFaults::minor, schema::integer())
        .field("major", &EngineStatus::AudioPageFaults::major, schema::integer())
        .field("threads", &EngineStatus::AudioPageFaults::threads, schema::integer())
        .field("saturated", &EngineStatus::AudioPageFaults::saturated, schema::boolean())
        .build();
}

schema::Schema<ThreadClass> threadClassSchema() {
    return schema::object<ThreadClass>("thread_class")
        .field("policy", &ThreadClass::policy,
               schema::enumeration<ThreadPolicy::Kind>({{ThreadPolicy::Kind::None, "none"},
                                                        {ThreadPolicy::Kind::Normal, "normal"},
                                                        {ThreadPolicy::Kind::Fifo, "fifo"},
                                                        {ThreadPolicy::Kind::RoundRobin, "rr"},
                                                        {ThreadPolicy::Kind::High, "high"}}))
        .field("priority", &ThreadClass::priority, schema::integer().range(0, 99))
        .build();
}

schema::Schema<EngineScheduling> engineSchedulingSchema() {
    return schema::object<EngineScheduling>("engine_scheduling")
        .field("callback", &EngineScheduling::callback, threadClassSchema())
        .field("live", &EngineScheduling::live, threadClassSchema())
        .field("renderer", &EngineScheduling::renderer, threadClassSchema())
        .field("background", &EngineScheduling::background, threadClassSchema())
        .build();
}

schema::Schema<EngineBridgeTiming> engineBridgeTimingSchema() {
    using T = EngineBridgeTiming;
    return schema::object<T>("engine_bridge_timing")
        .field("calls", &T::calls, schema::integer())
        .field("late", &T::late, schema::integer())
        .field("settles", &T::settles, schema::integer())
        .field("past_deadline", &T::pastDeadline, schema::integer())
        .field("spun", &T::spun, schema::integer())
        .field("wait_max_us", &T::waitMaxUs, schema::number())
        .field("wait_mean_us", &T::waitMeanUs, schema::number())
        .field("wake_max_us", &T::wakeMaxUs, schema::number())
        .field("wake_mean_us", &T::wakeMeanUs, schema::number())
        .field("work_max_us", &T::workMaxUs, schema::number())
        .field("return_max_us", &T::returnMaxUs, schema::number())
        .field("wakes_over_200us", &T::wakesOver200us, schema::integer())
        .field("wakes_over_1ms", &T::wakesOver1ms, schema::integer())
        .field("bridge_cpus", &T::bridgeCpus, schema::arrayOf(schema::integer()))
        .field("migrations", &T::migrations, schema::integer())
        .field("class_mismatches", &T::classMismatches, schema::integer())
        .field("longest_cycle_us", &T::longestCycleUs, schema::number())
        .field("overruns_late_wait", &T::overrunsLateWait, schema::integer())
        .field("overruns_slow_bridge", &T::overrunsSlowBridge, schema::integer())
        .field("overruns_not_waiting", &T::overrunsNotWaiting, schema::integer())
        .build();
}

schema::Schema<EngineWorkgroup> engineWorkgroupSchema() {
    return schema::object<EngineWorkgroup>("engine_workgroup")
        .field("state", &EngineWorkgroup::state, schema::string())
        .field("joined", &EngineWorkgroup::joined, schema::integer())
        .field("failed", &EngineWorkgroup::failed, schema::integer())
        .field("os_error", &EngineWorkgroup::osError, schema::integer())
        .field("message", &EngineWorkgroup::message, schema::string())
        .field("hint", &EngineWorkgroup::hint, schema::string())
        .build();
}

schema::Schema<MidiInputWarning> midiInputWarningSchema() {
    return schema::object<MidiInputWarning>("midi_input_warning")
        .field("device", &MidiInputWarning::device, schema::string())
        .field("os_error", &MidiInputWarning::osError, schema::integer())
        .field("message", &MidiInputWarning::message, schema::string())
        .field("hint", &MidiInputWarning::hint, schema::string())
        .build();
}

schema::Schema<EngineStatus> engineStatusSchema() {
    return schema::object<EngineStatus>("engine_status")
        .field("running", &EngineStatus::running, schema::boolean())
        .field("preparing_audio", &EngineStatus::preparingAudio, schema::boolean())
        .field("prepared_version", &EngineStatus::preparedVersion,
               schema::integerOf<std::uint64_t>())
        .field("preparation_version", &EngineStatus::preparationVersion,
               schema::integerOf<std::uint64_t>())
        .field("preparation_error", &EngineStatus::preparationError, schema::string())
        .field("backend", &EngineStatus::backend, schema::string())
        .field("sample_rate", &EngineStatus::sampleRate, schema::number())
        .field("block", &EngineStatus::block, schema::integer())
        .field("period", &EngineStatus::period, schema::integer())
        .field("plan_latency", &EngineStatus::planLatency, schema::integer())
        .field("device_latency", &EngineStatus::deviceLatency, schema::integer())
        .field("xruns", &EngineStatus::xruns, schema::integer())
        .field("bridge_late", &EngineStatus::bridgeLate, schema::integer())
        .field("bridge_timing", &EngineStatus::bridgeTiming, engineBridgeTimingSchema())
        .field("midi_dropped", &EngineStatus::midiDropped, schema::integer())
        .field("midi_inputs", &EngineStatus::midiInputs, schema::any())
        .field("midi_outputs", &EngineStatus::midiOutputs, schema::any())
        .field("midi_lead_frames", &EngineStatus::midiLeadFrames, schema::integer())
        .field("midi_out_dropped", &EngineStatus::midiOutDropped, schema::integer())
        .field("midi_output_warnings", &EngineStatus::midiOutputWarnings,
               schema::arrayOf(midiInputWarningSchema()))
        .field("midi_output_latency", &EngineStatus::midiOutputLatency,
               schema::arrayOf(schema::integer()))
        .field("midi_warnings", &EngineStatus::midiWarnings,
               schema::arrayOf(midiInputWarningSchema()))
        .field("midi_devices_changed", &EngineStatus::midiDevicesChanged, schema::boolean())
        .field("midi_devices_message", &EngineStatus::midiDevicesMessage, schema::string())
        .field("cycles", &EngineStatus::cycles, schema::integer())
        .field("process_errors", &EngineStatus::processErrors, schema::integer())
        .field("restart_needed", &EngineStatus::restartNeeded, schema::boolean())
        .field("anticipative", &EngineStatus::anticipative, schema::boolean())
        .field("anticipation_misses", &EngineStatus::anticipationMisses, schema::integer())
        .field("live_helpers", &EngineStatus::liveHelpers, schema::integer())
        .field("scheduling", &EngineStatus::scheduling, engineSchedulingSchema())
        .field("workgroup", &EngineStatus::workgroup, engineWorkgroupSchema())
        .field("warnings", &EngineStatus::warnings, schema::arrayOf(schema::string()))
        .field("memory_lock", &EngineStatus::memoryLock, runtimeProtectionSchema())
        .field("cpu_idle_latency", &EngineStatus::cpuIdleLatency, cpuIdleLatencySchema())
        .field("thp_opt_out", &EngineStatus::thpOptOut, thpOptOutSchema())
        .field("audio_page_faults", &EngineStatus::audioPageFaults, audioPageFaultsSchema())
        .field("meter_drops", &EngineStatus::meterDrops, meterDropsSchema())
        .field("failed_fx", &EngineStatus::failedFx, schema::arrayOf(schema::string()))
        .field("disabled_fx", &EngineStatus::disabledFx,
               schema::arrayOf(schema::string())
                   .doc("fx whose plug-in this run has disabled (plugin.override): the strip "
                        "passes audio through"))
        .field("bridge_threads_refused", &EngineStatus::bridgeThreadsRefused,
               schema::arrayOf(schema::string()))
        .field("dropped", &EngineStatus::dropped, schema::integer())
        .field("voice_dropped", &EngineStatus::voiceDropped, schema::integer())
        .field("failed_errors", &EngineStatus::failedErrors, schema::any())
        .field("sync", &EngineStatus::sync, syncStatusSchema())
        .build();
}

}  // namespace

void registerDeviceCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- engine.*
    [&] {
        Command<EngineDevicesArgs, EngineDevicesResult> c;
        c.name = "engine.devices";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List native audio device names, host APIs, rates and channel capacities.";
        c.description =
            "Enumerates PortAudio devices on macOS/Windows without opening a stream. "
            "backend defaults to portaudio; dummy returns an empty list without initializing "
            "hardware. Use exact host_api and name values with engine.start. Linux uses JACK "
            "ports and returns backend.unsupported; engine.inputs lists running JACK inputs.";
        c.tags = {"engine"};
        c.args = schema::object<EngineDevicesArgs>("engine_devices_args")
                     .field("backend", &EngineDevicesArgs::backend,
                            schema::enumeration<BackendArg>({{BackendArg::Jack, "jack"},
                                                             {BackendArg::Dummy, "dummy"},
                                                             {BackendArg::PortAudio, "portaudio"}}))
                     .build();
        const auto device =
            schema::object<AudioDevice>("audio_device")
                .field("host_api", &AudioDevice::hostApi, schema::string())
                .field("name", &AudioDevice::name, schema::string())
                .field("inputs", &AudioDevice::inputs, schema::integerOf<std::uint32_t>())
                .field("outputs", &AudioDevice::outputs, schema::integerOf<std::uint32_t>())
                .field("default_rate", &AudioDevice::defaultRate, schema::number())
                .field("default_input", &AudioDevice::defaultInput, schema::boolean())
                .field("default_output", &AudioDevice::defaultOutput, schema::boolean())
                .build();
        c.result = schema::object<EngineDevicesResult>("engine_devices_result")
                       .field("devices", &EngineDevicesResult::devices, schema::arrayOf(device))
                       .build();
        c.errors = {{"backend.unsupported", "native device enumeration is unavailable",
                     "use engine.inputs with JACK"}};
        c.examples = {
            {"Without hardware", j(R"({"backend":"dummy"})"), j(R"({"devices":[]})"), "empty"}};
        c.run = [](const EngineDevicesArgs& args, Context&) -> Result<EngineDevicesResult> {
            if (args.backend == BackendArg::Dummy) return EngineDevicesResult{};
            if (args.backend == BackendArg::Jack)
                return Error("backend.unsupported", "JACK exposes graph ports", "/backend",
                             "use engine.inputs while JACK is running");
            auto devices = PortAudioBackend::devices();
            if (!devices) return std::move(devices).error();
            return EngineDevicesResult{std::move(*devices)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EngineStartArgs, EngineStatus> c;
        c.name = "engine.start";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start playback through JACK, native PortAudio, or a device-free dummy.";
        c.description =
            "backend: jack (Linux default), portaudio (macOS/Windows native device), or dummy "
            "(default "
            "elsewhere, no device). For portaudio, host_api, input_device and output_device "
            "select exact names from engine.devices. inputs defaults to 0 (playback only); "
            "outputs (the device channels masters can play to, track.set hw_output) defaults to "
            "one per physical playback port with JACK (2 to 64) and 2 otherwise. rate defaults to "
            "the project rate; a rate given is asked of "
            "the device (CoreAudio switches the device's rate when it can, WASAPI converts) or, "
            "with JACK on PipeWire, suggested to the graph (node.rate: applied only while the "
            "devices are idle and the rate is allowed). force_graph (JACK on PipeWire, default "
            "false) instead forces block as the graph's quantum, and rate if given, while the "
            "engine runs. PortAudio opens up to "
            "16 native MIDI inputs and outputs; engine.inputs lists their names. Dummy is paced "
            "like a device. "
            "block: the device block in frames "
            "(default 256); the device's "
            "period may differ and is reported. anticipative (default true): render ahead of "
            "the playhead on worker threads (edits are heard up to 200 ms later); false renders "
            "everything in the audio callback. The engine runs at the device's rate (media "
            "is resampled; renders stay at the project's rate). Background and live thread "
            "capacities "
            "follows the app's Audio processing threads preference (Automatic by default); "
            "live helpers join each callback with no added latency. memory_lock reports the "
            "default-off host-process memory policy: requested, active, and a structured OS "
            "error on failure. cpu_idle_latency reports the 20 microsecond PM QoS request: "
            "requested by the default-off option, or automatic, held on Linux while a bridged "
            "plug-in plays live; released on engine stop. thp_opt_out: on Linux the process is "
            "kept out of "
            "transparent huge pages while the engine runs (advised_only: regions a plug-in "
            "advised keep them). A protection failure does not prevent audio "
            "startup. MIDI output refusals are in midi_output_warnings; midi_output_latency "
            "reports signed audio-minus-MIDI frames per output. MIDI inputs that refuse to open "
            "are skipped: midi_warnings reports each "
            "device, numeric os_error, message and corrective hint; audio continues.";
        c.tags = {"engine"};
        c.args = schema::object<EngineStartArgs>("engine_start_args")
                     .field("backend", &EngineStartArgs::backend,
                            schema::enumeration<BackendArg>({{BackendArg::Jack, "jack"},
                                                             {BackendArg::Dummy, "dummy"},
                                                             {BackendArg::PortAudio, "portaudio"}}))
                     .field("host_api", &EngineStartArgs::hostApi, schema::string())
                     .field("input_device", &EngineStartArgs::inputDevice, schema::string())
                     .field("output_device", &EngineStartArgs::outputDevice, schema::string())
                     .field("rate", &EngineStartArgs::rate, schema::number().range(1, 768000))
                     .field("inputs", &EngineStartArgs::inputs, schema::integer().range(0, 64))
                     .field("outputs", &EngineStartArgs::outputs, schema::integer().range(2, 64))
                     .field("block", &EngineStartArgs::block, schema::integer().range(16, 4096))
                     .field("anticipative", &EngineStartArgs::anticipative, schema::boolean())
                     .field("force_graph", &EngineStartArgs::forceGraph, schema::boolean())
                     .build();
        c.result = engineStatusSchema();
        c.errors = {
            {"engine.running", "the engine is already running", "engine.stop first"},
            {"backend.device", "device or host API was not found",
             "choose names from engine.devices"},
            {"backend.config", "device format is invalid", "check rate and channel capacities"},
            {"backend.unavailable", "JACK is not installed", "install pipewire-jack"},
            {"backend.open", "no JACK server answered", "is PipeWire running?"},
            {"backend.unsupported", "backend is unavailable on this platform", "select dummy"}};
        c.examples = {
            {"Without a device", j(R"({"backend": "dummy", "block": 128})"),
             j(R"({"running": true, "backend": "dummy", "sample_rate": 48000.0, "block": 128,
                     "period": 128, "plan_latency": 0, "device_latency": 0, "xruns": 0, "bridge_late": 0, "bridge_timing": {"calls": 0, "late": 0, "settles": 0, "past_deadline": 0, "spun": 0, "wait_max_us": 0, "wait_mean_us": 0, "wake_max_us": 0, "wake_mean_us": 0, "work_max_us": 0, "return_max_us": 0, "wakes_over_200us": 0, "wakes_over_1ms": 0, "bridge_cpus": [], "migrations": 0, "class_mismatches": 0, "longest_cycle_us": 0, "overruns_late_wait": 0, "overruns_slow_bridge": 0, "overruns_not_waiting": 0}, "midi_dropped": 0, "midi_warnings": [], "midi_out_dropped": 0, "midi_output_warnings": [], "midi_output_latency": [], "midi_inputs": [], "midi_outputs": [], "midi_lead_frames": 0,
                     "midi_devices_changed": false, "midi_devices_message": "",
                     "preparing_audio": false, "preparation_error": "",
                     "prepared_version": "<any>", "preparation_version": 0,
                     "cycles": "<any>", "process_errors": 0, "restart_needed": false,
                     "anticipative": true, "anticipation_misses": 0, "live_helpers": "<any>",
                     "scheduling": {"callback": {"policy": "normal", "priority": 0},
                                    "live": "<any>",
                                    "renderer": {"policy": "normal", "priority": 0},
                                    "background": "<any>"},
                     "workgroup": {"state": "none", "joined": 0, "failed": 0, "os_error": 0,
                                   "message": "", "hint": ""},
                     "warnings": [],
                     "memory_lock": {"requested": false, "active": false},
                     "cpu_idle_latency": {"requested": false, "active": false, "automatic": false},
                     "thp_opt_out": "<any>", "audio_page_faults": "<any>",
                     "meter_drops": {"renderer": 0, "callback": 0}, "failed_fx": [], "disabled_fx": [],
                     "bridge_threads_refused": [],
                     "dropped": 0, "voice_dropped": 0, "failed_errors": [], "sync": "<any>"})"),
             "empty"}};
        c.run = [](const EngineStartArgs& a, Context& ctx) -> Result<EngineStatus> {
            EngineApp& app = engine::app(ctx);
#if defined(__linux__)
            constexpr auto defaultBackend = BackendArg::Jack;
#else
            constexpr auto defaultBackend = BackendArg::Dummy;
#endif
            const auto backend = a.backend.value_or(defaultBackend);
            EngineDeviceOptions device;
            device.native.hostApi = a.hostApi.value_or("");
            device.native.inputDevice = a.inputDevice.value_or("");
            device.native.outputDevice = a.outputDevice.value_or("");
            device.rate = a.rate.value_or(0);
            device.inputs = static_cast<std::uint32_t>(a.inputs.value_or(0));
            device.outputs = static_cast<std::uint32_t>(a.outputs.value_or(0));
            device.forceGraph = a.forceGraph.value_or(false);
            SUPPLE_TRY(app.startEngine(backend == BackendArg::Dummy  ? "dummy"
                                       : backend == BackendArg::Jack ? "jack"
                                                                     : "portaudio",
                                       static_cast<std::uint32_t>(a.block.value_or(256)),
                                       a.anticipative.value_or(true), device));
            return app.engineStatus();
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, Empty> c;
        c.name = "engine.stop";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Stop real-time playback and release the audio device.";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.examples = {{"Stop", j("{}"), j("{}"), "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            app(ctx).stopEngine();
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, json::Value> c;
        c.name = "engine.inputs";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "The device's audio inputs, MIDI devices and the outputs masters play to.";
        c.description =
            "audio: one entry per input channel (channel, the 1-based number track.set "
            "record_input takes, and the device port it is connected to). midi: the MIDI devices "
            "(names track.set record_midi matches; JACK ports or native CoreMIDI/WinMM inputs, "
            "fixed until restart). midi_outputs: destination names for the hardware output API "
            "(routing and clock generation are separate). outputs: one entry per output channel "
            "(channel, the number "
            "track.set hw_output takes for a master, and the port it is connected to). While the "
            "engine is stopped: running false and "
            "empty lists (engine.start opens the device).";
        c.tags = {"engine", "record"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {
            {"Stopped", j("{}"),
             j(R"({"running": false, "audio": [], "midi": [], "outputs": [], "midi_outputs": []})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<json::Value> {
            EngineApp& app = engine::app(ctx);
            const Backend* backend = app.backend();
            json::Value out = json::Value::object();
            out["running"] = backend != nullptr;
            out["audio"] = json::Value::array();
            out["midi"] = json::Value::array();
            out["midi_outputs"] = json::Value::array();
            out["outputs"] = json::Value::array();
            if (!backend) return out;
            json::Value audio = json::Value::array();
            const auto names = backend->inputNames();
            const std::uint32_t count = backend->config().inputs;
            for (std::uint32_t ch = 0; ch < count; ++ch) {
                json::Value e = json::Value::object();
                e["channel"] = static_cast<std::int64_t>(ch + 1);
                e["port"] = ch < names.size() ? names[ch] : std::string();
                audio.push_back(std::move(e));
            }
            json::Value outputs = json::Value::array();
            const auto outNames = backend->outputNames();
            for (std::uint32_t ch = 0; ch < backend->config().outputs; ++ch) {
                json::Value e = json::Value::object();
                e["channel"] = static_cast<std::int64_t>(ch + 1);
                e["port"] = ch < outNames.size() ? outNames[ch] : std::string();
                outputs.push_back(std::move(e));
            }
            json::Value midi = json::Value::array();
            for (const std::string& name : backend->midiPorts()) midi.push_back(name);
            out["backend"] = backend->name();
            out["audio"] = std::move(audio);
            out["outputs"] = std::move(outputs);
            out["midi"] = std::move(midi);
            for (const auto& name : backend->midiOutputPorts()) out["midi_outputs"].push_back(name);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, EngineStatus> c;
        c.name = "engine.status";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Real-time engine state: backend, rate, block, device period, latency, xruns.";
        c.description =
            "preparing_audio: source preparation is pending; the previous ready plan stays "
            "audible. "
            "prepared_version is the last compiled version, preparation_version the pending target "
            "(zero when idle); preparation_error explains a failed preparation. "
            "plan_latency: plugin delay plus prepared MIDI audio lead in frames; midi_lead_frames "
            "reports the latter. midi_inputs/midi_outputs give per-port "
            "enabled/timing/loss/warnings, and for outputs what each sends (clock, transport, "
            "spp, mtc), as midi.inputs.get/midi.outputs.get. device_latency: frames from "
            "the engine's outputs to the device. restart_needed: the device changed rate or went "
            "away; engine.stop and engine.start again. midi_devices_changed is advisory: "
            "MIDI topology changed but audio keeps running. midi_devices_message asks for an "
            "explicit engine restart to pick up the new set; existing port names stay fixed. "
            "midi_out_dropped counts hardware output rejections, queue/device loss and pending "
            "events discarded at shutdown. midi_output_warnings lists outputs skipped at start; "
            "midi_output_latency is signed audio-minus-MIDI latency per output, in frames. "
            "sync: external sync (sync.get has the same status with the settings). "
            "midi_dropped: MIDI input messages/packets rejected or lost to bounded queues "
            "(JACK and native; zero for the dummy backend). midi_warnings lists inputs skipped at "
            "start, with "
            "device, numeric os_error, message and corrective hint. meter_drops: entries a full "
            "ring "
            "dropped (the renderer's, the audio callback's) — a strip's peak/RMS missed a block, "
            "never audio. dropped/voice_dropped count lost plug-in output/voice records. "
            "memory_lock reports requested/active and an optional OS error with errno and a "
            "corrective hint, including while stopped. Bridges are excluded. cpu_idle_latency "
            "reports the owned 20 microsecond request with the same status/error fields, and "
            "automatic: held without the option because a bridged plug-in plays live (Linux; "
            "an automatic failure is no warning). It affects system idle while held and leaves "
            "CPU frequency policy unchanged. "
            "thp_opt_out: active, advised_only, inherited (the process was already kept out "
            "when the engine started; left as it was, not undone at stop) and an optional "
            "error, Linux only (khugepaged "
            "collapsing memory would stall audio threads). audio_page_faults: page faults since "
            "the engine started; scope audio_threads counts the callback's and live helpers' "
            "(Linux), process the whole process's (macOS, Windows, where major is omitted), none "
            "while stopped; saturated: the platform's counter stopped at its limit (macOS 32-bit "
            "counts), so minor is 0 and major omitted, not counts. "
            "live_helpers is the enabled live helper capacity. scheduling: each thread class's "
            "policy (none, normal, fifo, rr, high) and priority as applied at start: the device "
            "callback, live helpers, renderer and background helpers (real-time below a "
            "real-time callback when permitted). workgroup: whether the live helpers joined the "
            "device's audio workgroup (macOS CoreAudio; state none, ready, joined, partial or "
            "failed, with the helper counts and the last refusal's OS error, message and hint). "
            "warnings includes scheduling fallbacks and "
            "plan warnings with corrective actions. failed_errors adds structured code, "
            "message, path and hint to each failure, and `incident`, the id of the local crash "
            "report the bridge's death may have caused (crash.read; the report may not exist), "
            "when it died of a signal or an exception. "
            "failed_fx: isolated plugins whose "
            "process crashed or hung (silent now; fx.restart re-creates them). "
            "bridge_late: callback cycles in which a bridged plugin had not answered by the "
            "cycle's bridge deadline (its period or 2 ms, whichever is larger); its output was "
            "silent for that cycle, and xruns counts each of these cycles too. "
            "bridge_threads_refused: isolated plugins whose bridge audio thread was refused "
            "the scheduling it asked for (it runs at what it had and can fall behind under "
            "load; grant real-time scheduling to the user and restart the engine).";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = engineStatusSchema();
        c.examples = {{"Not running", j("{}"),
                       j(R"({"running": false, "backend": "", "sample_rate": 0.0, "block": 0,
                     "period": 0, "plan_latency": 0, "device_latency": 0, "xruns": 0, "bridge_late": 0, "bridge_timing": {"calls": 0, "late": 0, "settles": 0, "past_deadline": 0, "spun": 0, "wait_max_us": 0, "wait_mean_us": 0, "wake_max_us": 0, "wake_mean_us": 0, "work_max_us": 0, "return_max_us": 0, "wakes_over_200us": 0, "wakes_over_1ms": 0, "bridge_cpus": [], "migrations": 0, "class_mismatches": 0, "longest_cycle_us": 0, "overruns_late_wait": 0, "overruns_slow_bridge": 0, "overruns_not_waiting": 0}, "midi_dropped": 0, "midi_warnings": [], "midi_out_dropped": 0, "midi_output_warnings": [], "midi_output_latency": [], "midi_inputs": [], "midi_outputs": [], "midi_lead_frames": 0,
                     "midi_devices_changed": false, "midi_devices_message": "",
                     "preparing_audio": false, "preparation_error": "",
                     "prepared_version": 0, "preparation_version": 0,
                     "cycles": 0, "process_errors": 0, "restart_needed": false,
                     "anticipative": false, "anticipation_misses": 0, "live_helpers": 0,
                     "scheduling": {"callback": {"policy": "none", "priority": 0},
                                    "live": {"policy": "none", "priority": 0},
                                    "renderer": {"policy": "none", "priority": 0},
                                    "background": {"policy": "none", "priority": 0}},
                     "workgroup": {"state": "none", "joined": 0, "failed": 0, "os_error": 0,
                                   "message": "", "hint": ""},
                     "warnings": [],
                     "memory_lock": {"requested": false, "active": false},
                     "cpu_idle_latency": {"requested": false, "active": false, "automatic": false},
                     "thp_opt_out": {"active": false, "advised_only": false, "inherited": false},
                     "audio_page_faults": {"scope": "none", "minor": 0, "major": 0, "threads": 0,
                                           "saturated": false},
                     "meter_drops": {"renderer": 0, "callback": 0}, "failed_fx": [], "disabled_fx": [],
                     "bridge_threads_refused": [],
                     "dropped": 0, "voice_dropped": 0, "failed_errors": [], "sync": "<any>"})"),
                       "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<EngineStatus> {
            return app(ctx).engineStatus();
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
