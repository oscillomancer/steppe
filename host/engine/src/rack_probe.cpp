// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// rack.probe (rack.md §7): an agent's ears on a patch. An offline render of
// a copy of the session with a listener cabled to each port asked for; each
// recording is described as its signal kind suggests: level and pitch for
// audio, rate and duty for gates, the notes a pitch visits, the range and
// speed of a CV.
#include <algorithm>
#include <cmath>
#include <set>

#include "command_validation.h"
#include "rack_runtime.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/model/schemas.h"

namespace supple::engine {

Result<std::pair<model::MidiSource, double>> notationSource(const model::Session& s,
                                                            const std::string& text, double atBeat);

namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;
namespace sd = script::detail;

json::Value j(std::string_view text) { return json::parse(text).value(); }

struct CaptureArgs {
    std::string rack, pitch, gate, track;
    std::optional<std::string> velocity, notes, name;
    std::optional<double> beats, fromBeat, at, quantize;
    bool async = false;
};

struct CaptureResult {
    midi::Sequence sequence;
    std::int64_t voices = 0;
    json::Value warnings = json::Value::array();
};

// Store rendered notes in the edit log so branch replay never renders them again.
struct CaptureCommitArgs {
    Id track;
    model::MidiSource source;
    std::string name;
    double at = 0, beats = 0;
    std::int64_t voices = 0;
    std::vector<std::string> warnings;
};
auto captureCommitSchema() {
    return schema::object<CaptureCommitArgs>("rack_capture_commit_args")
        .field("track", &CaptureCommitArgs::track, model::idSchema())
        .field("source", &CaptureCommitArgs::source, model::schemaOf<model::MidiSource>())
        .field("name", &CaptureCommitArgs::name, schema::string())
        .field("at", &CaptureCommitArgs::at, schema::number().min(0))
        .field("beats", &CaptureCommitArgs::beats, schema::number().range(.25, 1024))
        .field("voices", &CaptureCommitArgs::voices, schema::integer().range(0, 16))
        .field("warnings", &CaptureCommitArgs::warnings, schema::arrayOf(schema::string()))
        .build();
}

struct ProbeArgs {
    std::string rack;
    std::vector<std::string> ports;
    std::optional<double> seconds, fromBeat;
    std::optional<std::string> notes;
};

double rounded(double x, double step = 1e-4) { return std::round(x / step) * step; }

std::string noteName(int midi) {
    static const char* const names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                        "F#", "G",  "G#", "A",  "A#", "B"};
    const int pc = ((midi % 12) + 12) % 12;
    return std::string(names[pc]) + std::to_string(midi / 12 - 1 - (midi < 0 && pc ? 1 : 0));
}

std::string fixed(double x, int digits) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", digits, x);
    return buf;
}

std::string decibels(double x) {
    return x <= 1e-9 ? "-inf dB" : fixed(20 * std::log10(x), 1) + " dB";
}

// The fundamental of a periodic stretch (YIN's cumulative mean normalised
// difference), 20 Hz .. 5 kHz; none when the signal is not periodic.
std::optional<double> fundamental(const std::vector<double>& x, double rate) {
    const auto maxLag = static_cast<std::size_t>(rate / 20.0);
    const auto minLag = static_cast<std::size_t>(std::max(2.0, rate / 5000.0));
    const std::size_t window = 2048;
    if (x.size() < window + maxLag + 1) return std::nullopt;
    const std::size_t from = (x.size() - window - maxLag) / 2;  // the middle
    std::vector<double> d(maxLag + 1, 0.0);
    for (std::size_t lag = 1; lag <= maxLag; ++lag) {
        double sum = 0;
        for (std::size_t i = 0; i < window; ++i) {
            const double diff = x[from + i] - x[from + i + lag];
            sum += diff * diff;
        }
        d[lag] = sum;
    }
    double running = 0;
    std::vector<double> cmnd(maxLag + 1, 1.0);
    for (std::size_t lag = 1; lag <= maxLag; ++lag) {
        running += d[lag];
        cmnd[lag] = running > 0 ? d[lag] * static_cast<double>(lag) / running : 1.0;
    }
    std::size_t best = 0;
    for (std::size_t lag = minLag; lag < maxLag; ++lag)
        if (cmnd[lag] < 0.15) {
            while (lag + 1 < maxLag && cmnd[lag + 1] < cmnd[lag]) ++lag;
            best = lag;
            break;
        }
    if (best == 0) return std::nullopt;
    // Parabolic interpolation around the dip.
    const double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
    const double den = a - 2 * b + c;
    const double shift = std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return rate / (static_cast<double>(best) + shift);
}

// What the port carried. `voices`: its first channels (at least one).
json::Value describe(const std::vector<std::vector<double>>& voices, int width,
                     const sd::PortModel& port, const std::string& end, double rate, double seconds,
                     double bpm) {
    json::Value out = json::Value::object();
    out["port"] = end;
    static const char* const kinds[] = {"audio", "pitch", "gate", "cv", "unipolar", "phase"};
    const std::string kind = kinds[static_cast<int>(port.signal)];
    out["signal"] = kind;
    out["channels"] = static_cast<std::int64_t>(std::max(width, 1));
    const std::size_t frames = voices[0].size();
    const double n = std::max<double>(1, static_cast<double>(frames));
    // Audio is heard as the voices' sum; the rest is read per voice.
    std::vector<double> mix(frames, 0.0);
    for (const auto& v : voices)
        for (std::size_t i = 0; i < frames && i < v.size(); ++i) mix[i] += v[i];
    const std::vector<double>& x = port.signal == sd::PortModel::Signal::Audio ? mix : voices[0];
    double peak = 0, sum = 0, squares = 0, lo = 0, hi = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double v = x[i];
        peak = std::max(peak, std::fabs(v));
        sum += v;
        squares += v * v;
        lo = i ? std::min(lo, v) : v;
        hi = i ? std::max(hi, v) : v;
    }
    const double dc = sum / n, rms = std::sqrt(squares / n);
    out["peak"] = rounded(peak);
    out["rms"] = rounded(rms);
    out["dc"] = rounded(dc);
    out["min"] = rounded(lo);
    out["max"] = rounded(hi);
    const int heard = static_cast<int>(voices.size());
    const std::string poly =
        width <= 1 ? std::string()
        : port.signal == sd::PortModel::Signal::Audio ||
                port.signal == sd::PortModel::Signal::Gate ||
                port.signal == sd::PortModel::Signal::Pitch
            ? " (" +
                  (width > heard ? "the first " + std::to_string(heard) + " of " : std::string()) +
                  std::to_string(width) + " voices)"
            : " (voice 1 of " + std::to_string(width) + ")";
    std::string text;
    if (port.signal == sd::PortModel::Signal::Gate) {
        // Rises (Schmitt 0.5 / 0.25) of every voice; high while any voice is.
        int rises = 0;
        std::vector<bool> on(voices.size(), false);
        std::size_t high = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            bool any = false;
            for (std::size_t c = 0; c < voices.size(); ++c) {
                const double v = voices[c][i];
                if (!on[c] && v >= 0.5) {
                    on[c] = true;
                    ++rises;
                } else if (on[c] && v < 0.25) {
                    on[c] = false;
                }
                any = any || on[c];
            }
            high += any;
        }
        const double duty = static_cast<double>(high) / n, beats = seconds * bpm / 60.0;
        json::Value g = json::Value::object();
        g["count"] = static_cast<std::int64_t>(rises);
        g["per_second"] = rounded(rises / seconds, 0.01);
        g["per_beat"] = rounded(rises / beats, 0.01);
        g["duty"] = rounded(duty, 0.01);
        out["gates"] = std::move(g);
        text = rises == 0 ? "gate, never high"
                          : "gate, " + std::to_string(rises) + " in " + fixed(seconds, 2) + " s (" +
                                fixed(rises / beats, 2) + " per beat), high " +
                                fixed(100 * duty, 0) + " % of the time";
    } else if (port.signal == sd::PortModel::Signal::Pitch) {
        // The notes the voices move to, in time order (1 = an octave, 0 = C4).
        std::vector<int> seen;
        std::set<int> distinct;
        std::vector<int> last(voices.size(), INT32_MIN);
        for (std::size_t i = 0; i < frames; ++i)
            for (std::size_t c = 0; c < voices.size(); ++c) {
                const int note = static_cast<int>(std::lround(60 + 12 * voices[c][i]));
                if (note == last[c]) continue;
                last[c] = note;
                if (!distinct.count(note) || (!seen.empty() && seen.back() != note))
                    if (seen.size() < 64) seen.push_back(note);
                distinct.insert(note);
            }
        json::Value notes = json::Value::array();
        std::string list;
        for (std::size_t k = 0; k < seen.size(); ++k) {
            notes.push_back(noteName(seen[k]));
            if (k < 16) list += (list.empty() ? "" : " ") + noteName(seen[k]);
        }
        out["notes"] = std::move(notes);
        text = "pitch, " + std::to_string(distinct.size()) + " distinct note" +
               (distinct.size() == 1 ? "" : "s") + ": " + list + (seen.size() > 16 ? " …" : "");
    } else if (peak < 1e-9) {
        text = kind + ", silent";
    } else if (port.signal == sd::PortModel::Signal::Audio) {
        out["peak_db"] = rounded(20 * std::log10(peak), 0.1);
        out["rms_db"] = rounded(rms > 1e-9 ? 20 * std::log10(rms) : -200, 0.1);
        text = "audio, peak " + decibels(peak) + ", rms " + decibels(rms);
        if (auto f = fundamental(x, rate)) {
            const double midi = 69 + 12 * std::log2(*f / 440.0);
            const int note = static_cast<int>(std::lround(midi));
            json::Value p = json::Value::object();
            p["hz"] = rounded(*f, 0.01);
            p["note"] = noteName(note);
            p["cents"] = rounded(100 * (midi - note), 1);
            out["pitch"] = std::move(p);
            text += ", " + fixed(*f, 1) + " Hz (" + noteName(note) + " " +
                    (midi >= note ? "+" : "") + fixed(100 * (midi - note), 0) + " ct)";
        } else {
            text += ", not periodic";
        }
        if (std::fabs(dc) > 0.01) text += ", DC " + fixed(dc, 3);
    } else {
        // CV, unipolar, phase: range and how fast it moves.
        const double mid = 0.5 * (lo + hi), span = hi - lo;
        int cycles = 0;
        bool up = false;
        for (double v : x)
            if (span <= 1e-6) {
                break;
            } else if (!up && v >= mid + 0.1 * span) {
                up = true;
                ++cycles;
            } else if (up && v < mid - 0.1 * span) {
                up = false;
            }
        out["rate_hz"] = rounded(cycles / seconds, 0.01);
        text = kind + ", " + fixed(lo, 3) + " to " + fixed(hi, 3) + ", mean " + fixed(dc, 3) +
               (cycles > 1    ? ", about " + fixed(cycles / seconds, 2) + " cycles a second"
                : span > 1e-6 ? ", one sweep"
                              : ", steady");
    }
    out["description"] = text + poly;
    return out;
}

// `text` as an output of a module in `rack`: the end and its port.
Result<std::pair<model::CableEnd, const sd::PortModel*>> outputEnd(EngineApp& e,
                                                                   const model::Session& s, Id rack,
                                                                   const std::string& text,
                                                                   const std::string& path,
                                                                   const char* code) {
    const auto end = model::CableEnd::parse(text);
    const model::RackModule* m = end ? s.modules.find(end->object) : nullptr;
    if (!m || m->rack != rack)
        return Error(code, text + " is not a module of " + rack.str(), path,
                     "rack.get lists its modules");
    const auto* type = e.modules()->find(m->type);
    const int output = type ? type->type->output(end->key) : -1;
    if (output < 0) {
        std::string keys;
        if (type)
            for (int o : type->type->outputs())
                keys += (keys.empty() ? "" : ", ") +
                        type->type->ports()[static_cast<std::size_t>(o)].key;
        return Error(code, text + " is not an output", path,
                     keys.empty() ? "module.describe lists outputs"
                                  : end->object.str() + " has outputs " + keys);
    }
    return std::pair{*end, &type->type->output(output)};
}

// Items on the tracks the rack's Notes modules play, holding `notes`
// (notation) from `fromBeat`: what rack.probe and rack.capture play.
Status addNotes(model::EditTxn& t, const model::Session& s, Id rack, const std::string& notes,
                double fromBeat) {
    auto midi = notationSource(s, notes, fromBeat);
    if (!midi) {
        Error err = std::move(midi).error();
        err.path = "/notes";
        return err;
    }
    std::set<Id> tracks;
    for (const auto& m : s.modules)
        if (m.value->rack == rack && m.value->type == model::kNotesModule)
            tracks.insert(m.value->source.value_or(s.racks.find(rack)->track));
    for (Id track : tracks) {
        const Id source = t.create(midi->first);
        model::Take take;
        take.source = source;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.position = fromBeat;
        item.length = std::max(midi->second, 1e-3);
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return {};
}

}  // namespace

void registerRackCapture(cmd::Registry& registry);

void registerRackProbe(cmd::Registry& registry) {
    registerRackCapture(registry);
    Command<ProbeArgs, json::Value> c;
    c.name = "rack.probe";
    c.kind = Kind::Query;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Listen to a rack's ports: level, pitch, gate rate, notes, a CV's range.";
    c.description =
        "Renders the project offline from from_beat (default 0) for seconds (default 2, at "
        "most 10) with a listener on each port (\"mo_N.<output key>\", at most 8) and "
        "describes what each carried: audio's peak and rms (dB) and its pitch when periodic "
        "(a poly cable's first 4 voices summed); gates counted over the voices, per beat, "
        "and how long any is high; the notes the voices' pitches visit; a CV's range and "
        "speed (voice 1). notes: notation (as midi.write takes it) the rack's "
        "Notes modules play from from_beat, besides what their tracks hold. Nothing is "
        "changed; playback is not disturbed.";
    c.tags = {"rack", "modular", "analysis"};
    c.args = schema::object<ProbeArgs>("rack_probe_args")
                 .field("rack", &ProbeArgs::rack,
                        schema::string().pattern("^rk_[0-9]+$", "Rack id").doc("Rack id"))
                 .field("ports", &ProbeArgs::ports,
                        schema::arrayOf(schema::string().pattern("^mo_[0-9]+\\.[a-z][a-z0-9_]*$",
                                                                 "an output port"))
                            .minItems(1)
                            .maxItems(8))
                 .field("seconds", &ProbeArgs::seconds, schema::number().range(0.05, 10))
                 .field("from_beat", &ProbeArgs::fromBeat, schema::number().min(0))
                 .field("notes", &ProbeArgs::notes, schema::string().minLength(1))
                 .build();
    c.result = schema::any();
    c.errors = {{"rack.unknown", "no such rack", "rack.list"},
                {"rack.port", "not an output of a module in the rack", "rack.get lists them"}};
    c.examples = {{"Listen to Notes' gate and the VCA while a chord plays",
                   j(R"({"rack": "rk_1", "ports": ["mo_2.gate", "mo_3.out"], "seconds": 1,
               "notes": "voice v; 1: [C4 E4 G4]:h r:h"})"),
                   j(R"({"seconds": 1.0, "from_beat": 0.0, "ports": "<any>"})"), "song_with_rack"}};
    c.run = [](const ProbeArgs& a, Context& ctx) -> Result<json::Value> {
        EngineApp& e = ctx.service<EngineApp>();
        const model::Session s = e.view();
        const auto rack = Id::parse(a.rack);
        if (!rack || !s.racks.contains(*rack))
            return Error("rack.unknown", "no rack " + a.rack, "/rack", "rack.list");
        const double seconds = a.seconds.value_or(2.0), fromBeat = a.fromBeat.value_or(0.0);
        model::EditTxn t(s, s.version + 1);
        std::vector<std::pair<Id, const sd::PortModel*>> probes;
        for (std::size_t k = 0; k < a.ports.size(); ++k) {
            auto end =
                outputEnd(e, s, *rack, a.ports[k], "/ports/" + std::to_string(k), "rack.port");
            if (!end) return std::move(end).error();
            model::RackModule probe;
            probe.rack = *rack;
            probe.type = std::string(detail::kProbeModule);
            probe.version = "1.0.0";
            probe.params = json::Value::object();
            probe.params["seconds"] = seconds + 0.01;
            const Id id = t.create(probe);
            t.create(model::Cable{end->first, {id, "in"}, std::nullopt, ""});
            probes.emplace_back(id, end->second);
        }
        if (a.notes) SUPPLE_TRY(addNotes(t, s, *rack, *a.notes, fromBeat));
        auto copy = t.commit();
        if (!copy) return std::move(copy).error();
        InstancePool pool([&e](const model::PluginRef& ref) { return e.findPlugin(ref.id); },
                          e.options().identity);
        auto catalog = std::make_shared<rack::Catalog>(*e.modules());
        catalog->add(detail::probeModuleType());
        OfflineSettings settings;
        settings.startSeconds = copy->timeMap->secondsAt(fromBeat);
        settings.endSeconds = settings.startSeconds + seconds;
        settings.projectDir = e.projectDir();
        settings.modules = std::move(catalog);
        auto rendered = renderOffline(*copy, pool, settings);
        if (!rendered) return std::move(rendered).error();
        const double rate = copy->project->sampleRate;
        const double bpm = copy->timeMap->bpmAt(fromBeat);
        json::Value out = json::Value::object();
        out["seconds"] = seconds;
        out["from_beat"] = fromBeat;
        json::Value ports = json::Value::array();
        for (std::size_t k = 0; k < probes.size(); ++k) {
            const rack::Instance* in = pool.racks().find(probes[k].first);
            const auto* probe = in ? dynamic_cast<const detail::ProbeModule*>(in->host()) : nullptr;
            if (!probe) return Error("rack.probe", "the probe did not run", "/ports", "");
            std::vector<std::vector<double>> voices;
            const int heard = std::clamp(probe->width, 1, detail::kProbeChannels);
            for (int ch = 0; ch < heard; ++ch) {
                const auto& v = probe->recorded[static_cast<std::size_t>(ch)];
                voices.emplace_back(v.begin(), v.end());
            }
            ports.push_back(
                describe(voices, probe->width, *probes[k].second, a.ports[k], rate, seconds, bpm));
        }
        out["ports"] = std::move(ports);
        if (!rendered->warnings.empty()) {
            json::Value warnings = json::Value::array();
            for (const auto& w : rendered->warnings) warnings.push_back(w);
            out["warnings"] = std::move(warnings);
        }
        return out;
    };
    registry.add(std::move(c));
}

// rack.capture (rack.md §7.3): a rack's notes, printed as a MIDI item.
void registerRackCapture(cmd::Registry& registry) {
    {
        Command<CaptureCommitArgs, json::Value> commit;
        commit.name = "rack.capture.commit";
        commit.kind = Kind::Edit;
        commit.exposure = cmd::Exposure::Internal;
        commit.summary = "Attach a rack capture's rendered MIDI notes.";
        commit.args = captureCommitSchema();
        commit.result = schema::any();
        commit.run = [](const CaptureCommitArgs& a, Context& ctx) -> Result<json::Value> {
            auto& e = ctx.service<EngineApp>();
            const auto s = e.view();
            const auto* track = s.tracks.find(a.track);
            if (!track || track->type != model::StripType::Channel)
                return Error("rack.capture_stale", "capture destination is unavailable", "/track",
                             "submit rack.capture again onto an existing channel");
            SUPPLE_TRY(model::checkedBeat(s, a.at + a.beats, "/at"));
            model::EditTxn& t = e.txn();
            model::MidiSource src;
            src = a.source;
            const Id mi = t.create(std::move(src));
            model::Take take;
            take.name = a.name;
            take.source = mi;
            const Id tk = t.create(std::move(take));
            model::Item item;
            item.track = a.track;
            model::placeItem(s, item, a.at, a.beats);
            item.takes = {tk};
            item.activeTake = tk;
            const Id it = t.create(std::move(item));
            json::Value out = json::Value::object();
            out["item"] = it.str();
            out["take"] = tk.str();
            out["source"] = mi.str();
            out["position"] = a.at;
            out["length"] = a.beats;
            out["notes"] = static_cast<std::int64_t>(a.source.sequence.notes.size());
            out["voices"] = a.voices;
            if (!a.warnings.empty()) {
                out["warnings"] = json::Value::array();
                for (const auto& warning : a.warnings) out["warnings"].push_back(warning);
            }
            return out;
        };
        addChecked(registry, std::move(commit));
    }

    Command<CaptureArgs, json::Value> c;
    c.name = "rack.capture";
    c.kind = Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Record a rack's pitch and gate as notes in a MIDI item on a track.";
    c.description =
        "Renders the project offline from from_beat (default 0) for beats (default 16, at most "
        "1024) and writes the notes that the pitch and gate outputs (\"mo_N.<output>\", poly: "
        "a note per voice) play as a MIDI item on track at at (default from_beat): a gate's "
        "rise starts a note at the pitch then (round(60 + 12 x pitch)), its fall ends it. "
        "velocity: an output (0..1) read at each rise; default 96. quantize: a grid in beats "
        "(0.25 = sixteenths) for starts and lengths. notes: notation the rack's Notes modules "
        "play, as rack.probe takes it. The rack is not changed; edit the notes with midi.edit, "
        "or play them through any instrument. async:true returns {job}; rendering uses a snapshot, "
        "cancellation leaves the project unchanged, and completion adds one undo step. A changed "
        "rack, track, position or project fails with rack.capture_stale; submit the capture again.";
    c.tags = {"rack", "modular", "midi"};
    c.args = schema::object<CaptureArgs>("rack_capture_args")
                 .field("async", &CaptureArgs::async, schema::boolean())
                 .defaultValue(false)
                 .field("rack", &CaptureArgs::rack,
                        schema::string().pattern("^rk_[0-9]+$", "Rack id").doc("Rack id"))
                 .field("pitch", &CaptureArgs::pitch,
                        schema::string().pattern("^mo_[0-9]+\\.[a-z][a-z0-9_]*$", "an output port"))
                 .field("gate", &CaptureArgs::gate,
                        schema::string().pattern("^mo_[0-9]+\\.[a-z][a-z0-9_]*$", "an output port"))
                 .field("velocity", &CaptureArgs::velocity,
                        schema::string().pattern("^mo_[0-9]+\\.[a-z][a-z0-9_]*$", "an output port"))
                 .field("track", &CaptureArgs::track,
                        schema::string().pattern("^tr_[0-9]+$", "Track id").doc("Track id"))
                 .field("beats", &CaptureArgs::beats, schema::number().range(0.25, 1024))
                 .field("from_beat", &CaptureArgs::fromBeat, schema::number().min(0))
                 .field("at", &CaptureArgs::at, schema::number().min(0))
                 .field("quantize", &CaptureArgs::quantize, schema::number().range(1.0 / 64, 4))
                 .field("notes", &CaptureArgs::notes, schema::string().minLength(1))
                 .field("name", &CaptureArgs::name, schema::string())
                 .build();
    c.result = schema::any();
    c.errors = {{"rack.unknown", "no such rack", "rack.list"},
                {"rack.port", "not an output of a module in the rack", "rack.get lists them"},
                {"rack.capture", "the render is too long", "capture at most 600 seconds"},
                {"rack.capture_stale", "capture target changed during rendering",
                 "submit rack.capture again for the current rack, track and position"}};
    c.examples = {{"Print a chord the rack plays to a MIDI item on the second track",
                   j(R"({"rack": "rk_1", "pitch": "mo_2.pitch", "gate": "mo_2.gate",
               "track": "tr_2", "beats": 4, "notes": "voice v; 1: [C4 E4 G4]:h r:h"})"),
                   j(R"({"item": "<any>", "take": "<any>", "source": "<any>", "position": 0.0,
               "length": 4.0, "notes": 3, "voices": 3})"),
                   "song_with_rack_preset"}};
    c.run = [](const CaptureArgs& a, Context& ctx) -> Result<json::Value> {
        EngineApp& e = ctx.service<EngineApp>();
        if (a.async && e.inTransaction())
            return Error(
                "batch.not_transactional", "async capture cannot start inside a transaction",
                "/async",
                "run rack.capture with async:true outside the batch or script transaction");
        const model::Session s = e.view();
        const auto rack = Id::parse(a.rack);
        if (!rack || !s.racks.contains(*rack))
            return Error("rack.unknown", "no rack " + a.rack, "/rack", "rack.list");
        const auto track = Id::parse(a.track);
        const model::Track* to = track ? s.tracks.find(*track) : nullptr;
        if (!to) return Error("tr.unknown", "no track " + a.track, "/track", "see track.list");
        SUPPLE_TRY(checkItemTrack(s, *track, {}, "/track"));
        if (to->type == model::StripType::Master)
            return Error("track.strip_type", "a master holds audio items only", "/track",
                         "capture onto a channel");
        const double fromBeat = a.fromBeat.value_or(0.0), at = a.at.value_or(fromBeat);
        const double beats = a.beats.value_or(16.0);
        SUPPLE_TRY(model::checkedBeat(s, at + beats, "/beats"));
        const double start = s.timeMap->secondsAt(fromBeat);
        const double seconds = s.timeMap->secondsAt(fromBeat + beats) - start;
        if (seconds > 600)
            return Error(
                "rack.capture",
                "the capture lasts " + std::to_string(static_cast<int>(seconds)) + " seconds",
                "/beats", "capture at most 600 seconds at a time");
        // A copy with the listener cabled in.
        model::EditTxn copyTxn(s, s.version + 1);
        auto pitch = outputEnd(e, s, *rack, a.pitch, "/pitch", "rack.port");
        if (!pitch) return std::move(pitch).error();
        auto gate = outputEnd(e, s, *rack, a.gate, "/gate", "rack.port");
        if (!gate) return std::move(gate).error();
        model::RackModule listener;
        listener.rack = *rack;
        listener.type = std::string(detail::kCaptureModule);
        listener.version = "1.0.0";
        listener.params = json::Value::object();
        listener.params["use_velocity"] = a.velocity ? 1.0 : 0.0;
        const Id id = copyTxn.create(listener);
        copyTxn.create(model::Cable{pitch->first, {id, "pitch"}, std::nullopt, ""});
        copyTxn.create(model::Cable{gate->first, {id, "gate"}, std::nullopt, ""});
        if (a.velocity) {
            auto velocity = outputEnd(e, s, *rack, *a.velocity, "/velocity", "rack.port");
            if (!velocity) return std::move(velocity).error();
            copyTxn.create(model::Cable{velocity->first, {id, "velocity"}, std::nullopt, ""});
        }
        if (a.notes) SUPPLE_TRY(addNotes(copyTxn, s, *rack, *a.notes, fromBeat));
        auto copy = copyTxn.commit();
        if (!copy) return std::move(copy).error();
        auto plugins = e.plugins();
        if (!plugins) return plugins.error();
        std::vector<host::PluginInfo> catalogue(plugins->begin(), plugins->end());
        auto catalog = std::make_shared<rack::Catalog>(*e.modules());
        catalog->add(detail::captureModuleType());
        OfflineSettings settings;
        settings.startSeconds = start;
        settings.endSeconds = start + seconds;
        settings.projectDir = e.projectDir();
        settings.modules = std::move(catalog);
        auto render = [copy = std::move(*copy), catalogue = std::move(catalogue),
                       identity = e.options().identity, id, a, start, fromBeat,
                       beats](const OfflineSettings& settings) -> Result<CaptureResult> {
            InstancePool pool(
                [&catalogue](const model::PluginRef& ref) -> Result<host::PluginInfo> {
                    for (const auto& plugin : catalogue)
                        if (plugin.id == ref.id) return plugin;
                    return Error("plugin.not_found", "plugin " + ref.id + " is unavailable",
                                 "/rack", "install the plug-in, then submit rack.capture again");
                },
                identity);
            auto rendered = renderOffline(copy, pool, settings);
            if (!rendered) return std::move(rendered).error();
            const rack::Instance* in = pool.racks().find(id);
            const auto* capture =
                in ? dynamic_cast<const detail::CaptureModule*>(in->host()) : nullptr;
            if (!capture) return Error("rack.capture", "the listener did not run", "/rack", "");
            // Events to notes, in beats from the capture's start.
            const double rate = copy.project->sampleRate;
            midi::Sequence seq;
            const double ppq = seq.ppq;
            auto beatOf = [&](std::int64_t frame) {
                return copy.timeMap->beatAt(start + static_cast<double>(frame) / rate) - fromBeat;
            };
            auto snap = [&](double beat) {
                return a.quantize ? std::round(beat / *a.quantize) * *a.quantize : beat;
            };
            struct Open {
                bool on = false;
                double beat = 0;
                int pitch = 60, velocity = 96;
            };
            std::array<Open, detail::kCaptureVoices> open{};
            std::set<int> voices;
            auto close = [&](Open& o, double endBeat) {
                o.on = false;
                const double from = std::max(0.0, snap(o.beat));
                double until = std::min(beats, snap(endBeat));
                if (a.quantize) until = std::max(until, from + *a.quantize);
                if (from >= beats || until <= from) return;
                until = std::min(until, beats);
                midi::Note n;
                n.start = std::llround(from * ppq);
                n.length = std::max<std::int64_t>(1, std::llround(until * ppq) - n.start);
                n.pitch = static_cast<std::uint8_t>(o.pitch);
                n.velocity = static_cast<std::uint8_t>(o.velocity);
                seq.notes.push_back(n);
            };
            for (const auto& ev : capture->events) {
                Open& o = open[static_cast<std::size_t>(ev.voice)];
                const double beat = beatOf(ev.frame);
                if (o.on) close(o, beat);
                if (!ev.on) continue;
                o.on = true;
                o.beat = beat;
                o.pitch = rack::host::noteKey(ev.pitch);
                o.velocity = rack::host::noteVelocity(ev.velocity, ev.hasVelocity);
                voices.insert(ev.voice);
            }
            for (Open& o : open)
                if (o.on) close(o, beats);
            seq.length = std::llround(beats * ppq);
            seq.canonicalize();
            CaptureResult result;
            result.sequence = std::move(seq);
            result.voices = static_cast<std::int64_t>(voices.size());
            if (capture->full)
                result.warnings.push_back("more than " + std::to_string(detail::kCaptureEvents) +
                                          " gate changes: the later ones are left out");
            for (const auto& warning : rendered->warnings) result.warnings.push_back(warning);
            return result;
        };
        auto publish = [s, a, rack = *rack, track = *track, at, beats,
                        &dispatcher = ctx.service<cmd::Dispatcher>(),
                        origin = ctx.origin()](CaptureResult result) -> Result<json::Value> {
            CaptureCommitArgs args;
            args.track = track;
            args.source.sequence = std::move(result.sequence);
            args.name = a.name.value_or(model::rackLabel(s, rack));
            args.at = at;
            args.beats = beats;
            args.voices = result.voices;
            for (const auto& warning : result.warnings.asArray())
                args.warnings.push_back(warning.asString());
            return dispatcher.transact("rack.capture", origin, [&]() {
                return dispatcher.execute(
                    {"rack.capture.commit", captureCommitSchema().encode(args), origin});
            });
        };
        if (!a.async) {
            auto rendered = render(settings);
            if (!rendered) return rendered.error();
            return publish(std::move(*rendered));
        }
        auto& jobs = ctx.service<cmd::JobManager>();
        auto& dispatcher = ctx.service<cmd::Dispatcher>();
        const auto job = jobs.create();
        const auto generation = e.projectGeneration();
        const auto branch = e.versions().branch();
        settings.cancelFlag = jobs.cancellationFlag(job);
        settings.progressFlag = jobs.progressFlag(job);
        e.startJob([&e, &jobs, &dispatcher, job, generation, branch, s, rack = *rack,
                    track = *track, at, beats, origin = ctx.origin(), settings,
                    render = std::move(render),
                    publish = std::move(publish)](StopToken stop) mutable {
            settings.stopToken = stop;
            auto rendered = render(settings);
            if (!rendered) {
                if (!jobs.cancelled(job)) (void)jobs.fail(job, rendered.error());
                return;
            }
            dispatcher.post(
                "rack.capture.complete",
                [&e, &jobs, job, generation, branch, s, rack, track, at, beats, origin,
                 rendered = std::move(*rendered),
                 publish = std::move(publish)]() mutable -> Status {
                    if (jobs.cancelled(job)) return {};
                    const auto now = e.view();
                    bool stale = e.projectGeneration() != generation ||
                                 e.versions().branch() != branch ||
                                 now.racks.find(rack) != s.racks.find(rack) ||
                                 now.tracks.find(track) != s.tracks.find(track) ||
                                 now.project->sampleRate != s.project->sampleRate;
                    // Immutable object identities catch removals, replacements and additions.
                    auto same = [](const auto& before, const auto& after, auto selected) {
                        for (const auto& entry : before)
                            if (selected(entry) && after.find(entry.id) != entry.value.get())
                                return false;
                        for (const auto& entry : after)
                            if (selected(entry) && before.find(entry.id) != entry.value.get())
                                return false;
                        return true;
                    };
                    stale |= !same(s.tempo, now.tempo, [](const auto&) { return true; }) ||
                             !same(s.meters, now.meters, [](const auto&) { return true; });
                    auto module = [&](Id id) {
                        const auto* m = s.modules.find(id);
                        if (!m) m = now.modules.find(id);
                        return m && m->rack == rack;
                    };
                    stale |= !same(s.modules, now.modules,
                                   [&](const auto& entry) { return entry.value->rack == rack; });
                    stale |= !same(s.fx, now.fx, [&](const auto& entry) {
                        const auto owner = model::moduleOfFx(s, entry.id);
                        const auto next = model::moduleOfFx(now, entry.id);
                        return (owner && module(*owner)) || (next && module(*next));
                    });
                    stale |= !same(s.cables, now.cables, [&](const auto& entry) {
                        return module(entry.value->from.object);
                    });
                    stale |= !same(s.items, now.items,
                                   [&](const auto& entry) { return entry.value->track == track; });
                    if (stale || !model::checkedBeat(now, at + beats, "/at"))
                        return jobs.fail(job,
                                         Error("rack.capture_stale",
                                               "capture target changed during rendering", "/rack",
                                               "submit rack.capture again for the current rack, "
                                               "track and position"));
                    auto done = publish(std::move(rendered));
                    if (!done) return jobs.fail(job, done.error());
                    return jobs.complete(job, std::move(*done));
                },
                cmd::Dispatcher::PostKind::Edit);
        });
        auto out = json::Value::object();
        out["job"] = job;
        return out;
    };
    addChecked(registry, std::move(c));
}

}  // namespace supple::engine
