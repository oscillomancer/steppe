// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Transport commands: play, stop, record, seek, playrate, metronome, loop and punch.
#include "command_helpers.h"

namespace supple::engine {

namespace {

struct SeekArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};

struct RangeArgs {
    std::optional<bool> enabled, clear;
    std::optional<double> startSeconds, startBeat, endSeconds, endBeat;
    std::optional<std::int64_t> startBar, endBar;
};

schema::Schema<TransportRange> rangeSchema(const char* name) {
    return schema::object<TransportRange>(name)
        .field("enabled", &TransportRange::enabled, schema::boolean())
        .field("start_seconds", &TransportRange::start, schema::number())
        .field("end_seconds", &TransportRange::end, schema::number())
        .build();
}

schema::Schema<TransportInfo> transportSchema() {
    auto recorded = recordedSchema();
    return schema::object<TransportInfo>("transport")
        .field("playing", &TransportInfo::playing, schema::boolean())
        .field("recording", &TransportInfo::recording, schema::boolean())
        .field("counting_in", &TransportInfo::countingIn, schema::boolean())
        .field("seconds", &TransportInfo::seconds, schema::number())
        .field("beat", &TransportInfo::beat, schema::number())
        .field("sample", &TransportInfo::sample, schema::integer())
        .field("loop", &TransportInfo::loop, rangeSchema("loop"))
        .field("punch", &TransportInfo::punch, rangeSchema("punch"))
        .field("recorded", &TransportInfo::recorded, schema::arrayOf(recorded))
        .build();
}

struct PlayrateArgs {
    std::optional<double> rate;
    std::optional<bool> preservePitch;
};

struct PlayrateInfo {
    double requestedRate = 1, effectiveRate = 1;
    bool preservePitch = false;
    std::optional<std::string> envelope;
};

schema::Schema<PlayrateInfo> playrateSchema() {
    return schema::object<PlayrateInfo>("transport_playrate")
        .field("requested_rate", &PlayrateInfo::requestedRate, schema::number())
        .field("effective_rate", &PlayrateInfo::effectiveRate, schema::number())
        .field("preserve_pitch", &PlayrateInfo::preservePitch, schema::boolean())
        .field("envelope", &PlayrateInfo::envelope, schema::string())
        .build();
}

PlayrateInfo playrateInfo(const model::Session& session, const Engine* engine = nullptr) {
    PlayrateInfo out;
    out.requestedRate = session.project->playrate.rate;
    out.preservePitch = session.project->playrate.preservePitch;
    if (const auto envelope = model::activePlayrateEnvelope(session))
        out.envelope = envelope->str();
    // Consumption telemetry can lag requested settings at the render horizon
    // and through a compensated source transition (ADR-0045).
    out.effectiveRate = engine ? engine->effectivePlayrate() : 1.;
    return out;
}

}  // namespace

void registerTransportCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- transport.playrate.*
    [&] {
        Command<NoArgs, PlayrateInfo> c;
        c.name = "transport.playrate.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Get requested/effective master playrate, pitch mode and active envelope.";
        c.description =
            "requested_rate is the saved project ratio; envelope names the active nonempty "
            "tr_0/playrate envelope, including automation items. No engine is required. "
            "effective_rate follows consumed output, including compensated source transitions "
            "and graph/backend output latency; it is 1.0 without a running engine. GUI clients "
            "can use EngineApp::playrateState() without executing/logging a query each sync.";
        c.tags = {"transport", "automation"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = playrateSchema();
        c.examples = {{"Factory settings", j("{}"),
                       j(R"({"requested_rate":1.0,"effective_rate":1.0,"preserve_pitch":false})"),
                       "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<PlayrateInfo> {
            return playrateInfo(app(ctx).view(), app(ctx).engine());
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<PlayrateArgs, PlayrateInfo> c;
        c.name = "transport.playrate.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set the project's master playrate ratio and preserve-pitch flag.";
        c.description =
            "rate: finite 0.25..4.0 (default 1); preserve_pitch defaults false in a new project. "
            "Omitted fields keep their values. Updates during a drag use Origin.gesture like "
            "strip faders, and write/touch/latch capture tr_0/playrate coarsely: unlike strip "
            "levels (ADR-0061), the curve keeps playing while it is written (no hold or touch "
            "return). Recording requires unity. "
            "Settings are saved/undoable. File sources convert before take FX and live input "
            "mixing; instruments retain their pitch. Scalar edits ramp and converter changes "
            "crossfade for 10 ms after priming. Edits include the render horizon and compensated "
            "source lookahead; newer requests coalesce while a transition finishes. Long-latency "
            "take chains can exceed the usual 200 ms response. effective_rate follows audible "
            "output, independently of requested_rate.";
        c.tags = {"transport", "automation"};
        c.args = schema::object<PlayrateArgs>("transport_playrate_set_args")
                     .field("rate", &PlayrateArgs::rate,
                            schema::number().range(model::kMinPlayrate, model::kMaxPlayrate))
                     .field("preserve_pitch", &PlayrateArgs::preservePitch, schema::boolean())
                     .build();
        c.result = playrateSchema();
        c.errors = {{"record.playrate", "recording requires unity master playrate",
                     "stop recording before changing rate or enabling a non-unity rate envelope"}};
        c.examples = {
            {"Store double speed with pitch preservation", j(R"({"rate":2,"preserve_pitch":true})"),
             j(R"({"requested_rate":2.0,"effective_rate":1.0,"preserve_pitch":true})"), "empty"}};
        c.run = [](const PlayrateArgs& a, Context& ctx) -> Result<PlayrateInfo> {
            EngineApp& app = engine::app(ctx);
            // Refuse before recording an automation move into transient writer
            // state; a failed transaction must not leave a latch hold behind.
            if (app.engine() && app.engine()->recording() && a.rate && *a.rate != 1)
                return Error("record.playrate", "recording requires unity master playrate", "/rate",
                             "stop recording before changing master playrate");
            auto project = app.view().project;
            if (a.rate || a.preservePitch) {
                auto next = *project;
                if (a.rate) next.playrate.rate = *a.rate;
                if (a.preservePitch) next.playrate.preservePitch = *a.preservePitch;
                if (next.playrate != project->playrate) app.txn().setProject(std::move(next));
                if (a.rate) app.recordAutomation(model::kMaster, "playrate", *a.rate);
            }
            return playrateInfo(app.view(), app.engine());
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, Empty> c;
        c.name = "transport.playrate.apply-to-tempo";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Scale every tempo-point BPM by master playrate, then reset the rate to 1.";
        c.description =
            "One version: multiply every BPM by the saved scalar rate, preserving beat "
            "positions, ramp flags and meter changes, and reset rate to 1. Require stopped "
            "transport, no recording and no active nonempty master playrate envelope. "
            "Reject resulting BPM outside 1..1000 before mutation. Existing item timebases "
            "still apply; time-locked audio is not made sound-equivalent by this tempo edit.";
        c.tags = {"transport", "tempo"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.errors = {{"playrate.playing", "the transport is playing",
                     "transport.stop before applying playrate to tempo"},
                    {"record.playrate", "a recording is active", "transport.stop first"},
                    {"playrate.envelope", "an active nonempty master playrate envelope exists",
                     "disable it with env.set {active: false} before applying the scalar rate"},
                    {"tempo.range", "a scaled BPM is outside 1..1000",
                     "reduce the master rate or adjust the tempo map first"}};
        c.examples = {{"Apply the factory unity rate", j("{}"), j("{}"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            if (const auto* engine = app.engine()) {
                if (engine->recording())
                    return Error("record.playrate", "stop recording before applying playrate",
                                 "/project/playrate",
                                 "call transport.stop, then apply the playrate to tempo");
                if (engine->transport().playing || engine->transport().countingIn)
                    return Error("playrate.playing", "stop playback before applying playrate",
                                 "/transport/playing",
                                 "call transport.stop, then apply the playrate to tempo");
            }
            const auto s = app.view();
            if (const auto env = model::activePlayrateEnvelope(s))
                return Error("playrate.envelope", "an active master playrate envelope exists",
                             "/envelopes/" + env->str(),
                             "disable the envelope with env.set {active: false} first");
            const double rate = s.project->playrate.rate;
            for (const auto& e : s.tempo) {
                const double bpm = e.value->bpm * rate;
                if (!std::isfinite(bpm) || bpm < 1 || bpm > 1000)
                    return Error("tempo.range", "applying playrate puts BPM outside 1..1000",
                                 "/tempo/points/" + e.id.str() + "/bpm",
                                 "reduce master playrate or adjust this tempo point first");
            }
            if (rate != 1) {
                for (const auto& e : s.tempo)
                    app.txn().modify<model::TempoPointObj>(
                        e.id, [rate](auto& point) { point.bpm *= rate; });
                auto project = *s.project;
                project.playrate.rate = 1;
                app.txn().setProject(std::move(project));
            }
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();

    // ---------------------------------------------------------------- transport.*
    for (const auto& [action, summary, playing] :
         {std::tuple{"play", "Start playback from the current position.", true},
          std::tuple{"pause", "Pause playback where it is (ends a recording).", false},
          std::tuple{"stop", "Stop playback and return to where it started (ends a recording).",
                     false},
          std::tuple{"record", "Play and record every armed track's input or master's output.",
                     true}}) {
        Command<NoArgs, TransportInfo> c;
        c.name = std::string("transport.") + action;
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = summary;
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"},
                    {"record.nothing_armed", "no armed track has an input",
                     "track.set {armed: true, record_input: [1, 2]}, or arm a master"},
                    {"record.busy", "the audio callback did not return for 2 s (still recording)",
                     "stop again, or engine.stop (the files stay recoverable)"}};
        const bool rec = std::string(action) == "record";
        if (rec) {
            c.description =
                "Recording requires saved master rate 1 and no active master rate "
                "envelope that leaves 1 anywhere, including automation items. "
                "Non-unity recording and an armed track with unsupported output record mode "
                "are refused before opening files. An armed master records its output (after "
                "its fader, before its own items) as an item on the master; while it records, "
                "its items are silent in the punch range, or everywhere without punch. "
                "Armed channels in midi_overdub or midi_replace also record every incoming "
                "Note Out route, alongside an optional MIDI device input; no instrument is "
                "required. Rack Out audio inputs record stereo after input FX, before items "
                "and track inserts.";
            c.errors.push_back({"record.rack_input",
                                "the loaded plan has no armed rack input for the target",
                                "select an Out with track.set {record_rack, armed: true} and load "
                                "it before recording"});
            c.errors.push_back(
                {"record.midi_route", "a Note Out recording target is not armed in the loaded plan",
                 "arm the destination in a MIDI record mode and load it before recording"});
            c.errors.push_back({"record.playrate", "master playrate can leave unity",
                                "set rate to 1 and disable non-unity master playrate automation"});
            c.errors.push_back({"record.output_unsupported",
                                "output recording is not supported yet",
                                "use track.set {record_mode: input} on the reported track"});
        }
        c.examples = {{"While the engine runs", j("{}"),
                       j(std::string(R"({"playing": )") + (playing ? "true" : "false") +
                         R"(, "recording": )" + (rec ? "true" : "false") +
                         R"(, "seconds": "<any>", "beat": "<any>", "sample": "<any>",)"
                         R"( "loop": "<any>", "punch": "<any>", "counting_in": false})"),
                       rec ? "engine_armed" : "engine_running"}};
        c.run = [act = std::string(action)](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return app(ctx).transport(act);
        };
        addChecked(registry, std::move(c));
    }
    [&] {
        Command<SeekArgs, TransportInfo> c;
        c.name = "transport.seek";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move the play position (one of seconds, beat or bar).";
        c.description =
            "With master playrate enabled, prepare file-source converters off-thread "
            "before publishing the audible seek. Existing playback and live monitoring "
            "continue until ready; preparation errors leave the old seek epoch intact.";
        c.tags = {"transport"};
        c.args = schema::object<SeekArgs>("seek_args")
                     .field("seconds", &SeekArgs::seconds, schema::number().min(0))
                     .field("beat", &SeekArgs::beat, schema::number().min(0))
                     .field("bar", &SeekArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = transportSchema();
        c.errors = {
            {"engine.stopped", "the engine is not running", "engine.start first"},
            {"transport.position", "not exactly one position", "give seconds, beat or bar"}};
        c.examples = {
            {"To bar 2 (120 BPM, 4/4)", j(R"({"bar": 2})"),
             j(R"({"playing": false, "recording": false, "seconds": 2.0, "beat": 4.0, "sample": 96000,
                    "loop": "<any>", "punch": "<any>", "counting_in": false})"),
             "engine_running"}};
        c.run = [](const SeekArgs& a, Context& ctx) -> Result<TransportInfo> {
            EngineApp& app = engine::app(ctx);
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("transport.position", "give exactly one position", "",
                             "seconds, beat or bar");
            const model::Session s = app.view();
            const double seconds =
                a.seconds
                    ? *a.seconds
                    : s.timeMap->secondsAt(beatsOf(
                          s, a.beat,
                          a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt));
            SUPPLE_TRY(model::checkedSeconds(
                s, seconds,
                a.seconds ? "/seconds"
                : a.bar   ? "/bar"
                          : "/beat",
                app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
            return app.transport("seek", seconds);
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, TransportInfo> c;
        c.name = "transport.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The play position and whether it is playing.";
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"}};
        c.examples = {
            {"Stopped at the start", j("{}"),
             j(R"({"playing": false, "recording": false, "seconds": 0.0, "beat": 0.0, "sample": 0,
                    "loop": {"enabled": false, "start_seconds": 0.0, "end_seconds": 0.0},
                    "punch": {"enabled": false, "start_seconds": 0.0, "end_seconds": 0.0},
                    "counting_in": false})"),
             "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return app(ctx).transport("get");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        struct MetronomeArgs {
            std::optional<bool> enabled;
            std::optional<double> volumeDb;
            std::optional<int> countInBars;
            std::optional<bool> countInPlayback;
        };
        Command<MetronomeArgs, MetronomeSettings> c;
        c.name = "transport.metronome";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Turn the metronome on or off and set its level; returns the settings.";
        c.description =
            "A click on every beat of the meter while the transport plays (the bar's first "
            "accented), in the live output only: renders and recordings never contain it. "
            "count_in_bars: that many bars of clicks (at the meter and tempo where playback "
            "starts) before transport.record begins — the position holds and nothing is "
            "recorded until the count ends; count_in_playback counts in before transport.play "
            "too. The count clicks even with the metronome off. "
            "Transport state: not an undo step, not saved in the project. No arguments: the "
            "current settings.";
        c.tags = {"transport", "recording"};
        c.args = schema::object<MetronomeArgs>("metronome_args")
                     .field("enabled", &MetronomeArgs::enabled, schema::boolean())
                     .field("volume_db", &MetronomeArgs::volumeDb,
                            schema::number().range(-60, 6).unit(units::Unit::dB))
                     .field("count_in_bars", &MetronomeArgs::countInBars,
                            schema::integerOf<int>().range(0, 8))
                     .field("count_in_playback", &MetronomeArgs::countInPlayback, schema::boolean())
                     .build();
        c.result =
            schema::object<MetronomeSettings>("metronome")
                .field("enabled", &MetronomeSettings::enabled, schema::boolean())
                .field("volume_db", &MetronomeSettings::volumeDb, schema::number())
                .field("count_in_bars", &MetronomeSettings::countInBars, schema::integerOf<int>())
                .field("count_in_playback", &MetronomeSettings::countInPlayback, schema::boolean())
                .build();
        c.examples = {{"On", j(R"({"enabled": true})"),
                       j(R"({"enabled": true, "volume_db": -6.0, "count_in_bars": 0,
                               "count_in_playback": false})"),
                       "empty"},
                      {"Two bars of count-in before recording", j(R"({"count_in_bars": 2})"),
                       j(R"({"enabled": false, "volume_db": -6.0, "count_in_bars": 2,
                               "count_in_playback": false})"),
                       "empty"}};
        c.run = [](const MetronomeArgs& a, Context& ctx) -> Result<MetronomeSettings> {
            EngineApp& app = engine::app(ctx);
            MetronomeSettings m = app.metronome();
            if (a.enabled) m.enabled = *a.enabled;
            if (a.volumeDb) m.volumeDb = *a.volumeDb;
            if (a.countInBars) m.countInBars = *a.countInBars;
            if (a.countInPlayback) m.countInPlayback = *a.countInPlayback;
            app.setMetronome(m);
            return m;
        };
        addChecked(registry, std::move(c));
    }();
    for (const bool loop : {true, false}) {
        Command<RangeArgs, TransportRange> c;
        c.name = loop ? "transport.loop" : "transport.punch";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = loop ? "Set or toggle the loop range: playback reaching its end repeats it."
                         : "Set or toggle the punch range: recordings keep only that range.";
        c.description =
            loop ? "start and end as seconds, beat or bar (start_bar 5, end_bar 9 = bars 5-8); "
                   "a range turns the loop on unless enabled says otherwise; enabled alone "
                   "toggles the current range; clear removes the range and turns the loop off. "
                   "Recording while looping makes one take per pass on one item. Transport "
                   "state: not an undo step, not saved in the project."
                 : "start and end as seconds, beat or bar; a range turns punch on unless enabled "
                   "says otherwise; clear removes the range and turns punch off. The recording "
                   "keeps running; the committed items cover only the punch range (the file "
                   "keeps everything). Not an undo step.";
        c.tags = {"transport", "recording"};
        c.args = schema::object<RangeArgs>("range_args")
                     .field("enabled", &RangeArgs::enabled, schema::boolean())
                     .field("clear", &RangeArgs::clear, schema::boolean())
                     .field("start_seconds", &RangeArgs::startSeconds, schema::number().min(0))
                     .field("start_beat", &RangeArgs::startBeat, schema::number().min(0))
                     .field("start_bar", &RangeArgs::startBar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("end_seconds", &RangeArgs::endSeconds, schema::number().min(0))
                     .field("end_beat", &RangeArgs::endBeat, schema::number().min(0))
                     .field("end_bar", &RangeArgs::endBar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = rangeSchema(loop ? "loop" : "punch");
        c.errors = {{"transport.range",
                     "not one start and one end, end not after start, or clear with a range",
                     "e.g. start_bar 5, end_bar 9"}};
        c.examples = {
            {"Bars 2-3 (120 BPM, 4/4)", j(R"({"start_bar": 2, "end_bar": 4})"),
             j(R"({"enabled": true, "start_seconds": 2.0, "end_seconds": 6.0})"), "empty"},
            {"Remove the range", j(R"({"clear": true})"),
             j(R"({"enabled": false, "start_seconds": 0, "end_seconds": 0})"), "empty"}};
        c.run = [loop](const RangeArgs& a, Context& ctx) -> Result<TransportRange> {
            EngineApp& app = engine::app(ctx);
            TransportRange r = loop ? app.loop() : app.punch();
            const int starts =
                a.startSeconds.has_value() + a.startBeat.has_value() + a.startBar.has_value();
            const int ends =
                a.endSeconds.has_value() + a.endBeat.has_value() + a.endBar.has_value();
            if (starts > 1 || ends > 1 || starts != ends)
                return Error("transport.range", "give one start and one end (or neither)", "",
                             "provide exactly one start_seconds/start_beat/start_bar and one "
                             "end_seconds/end_beat/end_bar, or omit all six");
            if (a.clear.value_or(false)) {
                if (starts == 1 || a.enabled.value_or(false))
                    return Error("transport.range", "clear removes the range", "/clear",
                                 "send clear alone, or a start and end without clear");
                r = {};
            } else if (starts == 1) {
                const model::Session s = app.view();
                auto at = [&](std::optional<double> sec, std::optional<double> beat,
                              std::optional<std::int64_t> bar) {
                    return sec ? *sec
                               : s.timeMap->secondsAt(
                                     beatsOf(s, beat,
                                             bar ? std::optional<int>(static_cast<int>(*bar))
                                                 : std::nullopt));
                };
                const double start = at(a.startSeconds, a.startBeat, a.startBar);
                const double end = at(a.endSeconds, a.endBeat, a.endBar);
                SUPPLE_TRY(model::checkedSeconds(
                    s, start,
                    a.startSeconds ? "/start_seconds"
                    : a.startBar   ? "/start_bar"
                                   : "/start_beat",
                    app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
                SUPPLE_TRY(model::checkedSeconds(
                    s, end,
                    a.endSeconds ? "/end_seconds"
                    : a.endBar   ? "/end_bar"
                                 : "/end_beat",
                    app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
                if (!(end > start))
                    return Error("transport.range", "the end must come after the start",
                                 a.endSeconds ? "/end_seconds"
                                 : a.endBar   ? "/end_bar"
                                              : "/end_beat",
                                 "set the supplied end_seconds/end_beat/end_bar later than the "
                                 "supplied start_seconds/start_beat/start_bar");
                r.start = start;
                r.end = end;
                r.enabled = a.enabled.value_or(true);
            } else if (a.enabled) {
                if (*a.enabled && !(r.end > r.start))
                    return Error("transport.range", "there is no range to turn on", "",
                                 "provide one start_seconds/start_beat/start_bar and one "
                                 "end_seconds/end_beat/end_bar with end later than start");
                r.enabled = *a.enabled;
            }
            if (loop)
                app.setLoop(r);
            else
                app.setPunch(r);
            return r;
        };
        addChecked(registry, std::move(c));
    }
}

}  // namespace supple::engine
