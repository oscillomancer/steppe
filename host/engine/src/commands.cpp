// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "command_validation.h"
#include "freeze_commands.h"
#include "supple/model/numeric.h"
// The engine's command catalogue (session-model §7) — the core set. Every
// command is a thin, typed wrapper: validate references with helpful errors,
// edit through the open EditTxn (edits) or read the view (queries), return
// IDs and the few fields an agent needs next.
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <set>

#include "analysis_commands.h"
#include "command_helpers.h"
#include "media_files.h"
#include "render_commands.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/midi/midi.h"
#include "supple/model/context.h"
#include "supple/model/groups.h"
#include "supple/model/render_settings.h"
#include "supple/model/schemas.h"
#include "supple/notation/notation.h"
#include "supple/params/params.h"

namespace supple::engine {

void registerRouteCommands(cmd::Registry& registry);
void registerLogCommands(cmd::Registry& registry);
void registerCrashReportCommands(cmd::Registry& registry);
void registerRulerCommands(cmd::Registry& registry);
void registerContextCommands(cmd::Registry& registry);
void registerPluginCommands(cmd::Registry& registry);
void registerGroupCommands(cmd::Registry& registry);
void registerMidiCommands(cmd::Registry& registry);
void registerChainCommands(cmd::Registry& registry);
void registerTakeCommands(cmd::Registry& registry);
void registerAudioCommands(cmd::Registry& registry);
void registerConsolidateCommands(cmd::Registry& registry);
void registerExportCommands(cmd::Registry& registry);
void registerMixCommands(cmd::Registry& registry);
void registerHealCommands(cmd::Registry& registry);
void registerSyncCommands(cmd::Registry& registry);
void registerLearnCommands(cmd::Registry& registry);
void registerControlLearnCommands(cmd::Registry& registry);
void registerDspCommands(cmd::Registry& registry);
void registerRackCommands(cmd::Registry& registry);
void registerRackClipCommands(cmd::Registry& registry);
void registerRackLevelCommands(cmd::Registry& registry);
void registerModuleCommands(cmd::Registry& registry);
void registerMidiPortCommands(cmd::Registry& registry);
void registerRackProbe(cmd::Registry& registry);
void registerProjectCommands(cmd::Registry& registry);
void registerTempoCommands(cmd::Registry& registry);
void registerTrackCommands(cmd::Registry& registry);
void registerItemCommands(cmd::Registry& registry);
void registerFxCommands(cmd::Registry& registry);
void registerHistoryCommands(cmd::Registry& registry);
void registerEnvelopeCommands(cmd::Registry& registry);
void registerDeviceCommands(cmd::Registry& registry);
void registerTransportCommands(cmd::Registry& registry);
void registerAuditionCommands(cmd::Registry& registry);

// Exposed to rack.probe (rack_probe.cpp).
Result<std::pair<model::MidiSource, double>> notationSource(const model::Session& s,
                                                            const std::string& text,
                                                            double atBeat) {
    return compileNotation(s, text, atBeat);
}

void registerEngineCommands(cmd::Registry& registry) {
    // Each command is built in its own immediately-invoked lambda, so an
    // unoptimized build reuses one frame instead of stacking every command's
    // locals (Windows' 1 MB main stack overflowed).
    registerFreezeCommands(registry);
    registerRouteCommands(registry);
    registerLogCommands(registry);
    registerCrashReportCommands(registry);
    registerRulerCommands(registry);
    registerContextCommands(registry);
    registerPluginCommands(registry);
    registerGroupCommands(registry);
    registerMidiCommands(registry);
    registerChainCommands(registry);
    registerTakeCommands(registry);
    registerAudioCommands(registry);
    registerConsolidateCommands(registry);
    registerExportCommands(registry);
    registerMixCommands(registry);
    registerHealCommands(registry);
    registerSyncCommands(registry);
    registerLearnCommands(registry);
    registerControlLearnCommands(registry);
    registerDspCommands(registry);
    registerRackCommands(registry);
    registerRackClipCommands(registry);
    registerRackLevelCommands(registry);
    registerModuleCommands(registry);
    registerMidiPortCommands(registry);
    registerRackProbe(registry);
    lua::registerScriptCommands(registry);
    registerProjectCommands(registry);
    registerTempoCommands(registry);
    registerTrackCommands(registry);
    registerItemCommands(registry);
    registerFxCommands(registry);
    registerRenderCommands(registry);
    registerEngineAnalysisCommands(registry);
    registerHistoryCommands(registry);
    registerEnvelopeCommands(registry);
    registerDeviceCommands(registry);
    registerTransportCommands(registry);
    registerAuditionCommands(registry);
}

// --- describe ----------------------------------------------------------------------------------

std::string describeSession(const model::Session& s) {
    std::string out;
    const auto meter = s.timeMap->meterAt(1);
    const double endBeat = model::lastItemBeat(s);
    char head[256];
    std::snprintf(head, sizeof head, "Project \"%s\" · %g kHz · %g BPM %d/%d · %.1f s\n",
                  s.project->name.c_str(), s.project->sampleRate / 1000.0, s.timeMap->bpmAt(0),
                  meter.numerator, meter.denominator, s.timeMap->secondsAt(endBeat));
    out += head;
    if (const auto keyId = model::keyPointAt(s, 0))
        if (const auto* key = s.context.find(*keyId)) {
            out.pop_back();  // the header's newline
            out += " · " + model::keyWords(*key) + "\n";
        }
    auto barOf = [&](double beat) {
        const auto bb = s.timeMap->barBeat(beat);
        char b[48];
        if (bb.beat == 1 && bb.fraction == 0)
            std::snprintf(b, sizeof b, "bar %d", bb.bar);
        else
            std::snprintf(b, sizeof b, "bar %d.%d", bb.bar, bb.beat);
        return std::string(b);
    };
    // The tempo and meter changes after the first, which the header omits.
    if (s.tempo.size() > 1) {
        std::vector<model::TempoPointObj> points;
        for (const auto& e : s.tempo) points.push_back(*e.value);
        std::sort(points.begin(), points.end(),
                  [](const auto& a, const auto& b) { return a.beat < b.beat; });
        out += "tempo:";
        for (std::size_t i = 1; i < points.size(); ++i) {
            char t[64];
            std::snprintf(t, sizeof t, " %s%s %g BPM%s", i > 1 ? "· " : "",
                          barOf(points[i].beat).c_str(), points[i].bpm,
                          points[i].ramp ? " (ramps on)" : "");
            out += t;
        }
        out += "\n";
    }
    if (s.meters.size() > 1) {
        std::vector<model::MeterObj> meters;
        for (const auto& e : s.meters) meters.push_back(*e.value);
        std::sort(meters.begin(), meters.end(),
                  [](const auto& a, const auto& b) { return a.bar < b.bar; });
        out += "meters:";
        for (std::size_t i = 1; i < meters.size(); ++i)
            out += std::string(i > 1 ? " · " : " ") + "bar " + std::to_string(meters[i].bar) + " " +
                   std::to_string(meters[i].numerator) + "/" +
                   std::to_string(meters[i].denominator);
        out += "\n";
    }
    std::function<void(Id, int)> walk = [&](Id id, int depth) {
        const model::Track& t = *s.tracks.find(id);
        {
            std::string line(static_cast<std::size_t>(2 * depth), ' ');
            line += id.str() + " " + (t.name.empty() ? "(unnamed)" : t.name);
            line += " [" + std::string(model::stripTypeName(t.type)) + "]";
            if (t.freeze) line += " [Frozen " + t.freeze->source.str() + "]";
            if (t.volumeDb != 0) {
                char v[32];
                std::snprintf(v, sizeof v, " · vol %+.1f dB", t.volumeDb);
                line += v;
            }
            if (t.pan != 0) line += " · pan " + std::to_string(static_cast<int>(t.pan));
            if (t.mute) line += " · muted";
            if (t.solo == model::Solo::On || t.solo == model::Solo::InPlace) line += " · solo";
            if (!t.chain.empty()) {
                line += " · fx:";
                for (Id f : t.chain) {
                    const model::FxInstance& fx = *s.fx.find(f);
                    line += " " + f.str() + " " +
                            (fx.plugin.name.empty() ? fx.plugin.id : fx.plugin.name);
                    if (fx.bypass) line += " (bypassed)";
                }
            }
            out += line + "\n";
            for (const auto& e : s.items) {
                if (e.value->track != id) continue;
                const model::Item& it = *e.value;
                std::string il(static_cast<std::size_t>(2 * depth + 2), ' ');
                const double start = model::itemSpan(s, it).startBeat;
                il += e.id.str() + " " + barOf(start) + "–" +
                      barOf(start + model::itemLengthBeats(s, it));
                if (model::itemTimebase(s, it) != model::Timebase::Beats)
                    il += model::itemTimebase(s, it) == model::Timebase::Time ? " [time]"
                                                                              : " [beats position]";
                const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
                if (take && take->source) {
                    if (take->source->kind == model::Kind::Midi) {
                        const model::MidiSource* m = s.midi.find(*take->source);
                        il += " (MIDI " + take->source->str() + ", " +
                              std::to_string(m ? m->sequence.notes.size() : 0) + " notes)";
                    } else {
                        const model::MediaSource* m = s.media.find(*take->source);
                        il += " (audio " + take->source->str() + " " +
                              (m ? fs::path(m->path).filename().string() : "") + ")";
                    }
                }
                if (take) {
                    if (take->transients)
                        il += " · " + std::to_string(take->transients->positions.size()) + " hits";
                    if (!take->stretchMarkers.empty())
                        il += " · " + std::to_string(take->stretchMarkers.size()) + " warp anchors";
                }
                if (it.mute) il += " muted";
                out += il + "\n";
            }
        }
        for (Id c : t.children) walk(c, depth + 1);
    };
    for (Id master : s.project->masters) walk(master, 0);
    if (!s.routes.empty()) {
        out += "routes:";
        for (const auto& e : s.routes) {
            const model::Route& r = *e.value;
            out += " " + e.id.str() + " " + (r.from.track ? r.from.track->str() : "in") + "→" +
                   (r.to.track ? r.to.track->str() : "out");
        }
        out += "\n";
    }
    if (!s.markers.empty()) {
        out += "markers:";
        for (const auto& e : s.markers)
            out +=
                " " + e.id.str() + " \"" + e.value->name + "\" " +
                barOf(e.value->timebase == model::Timebase::Time ? s.timeMap->beatAt(e.value->pos)
                                                                 : e.value->pos);
        out += "\n";
    }
    if (!s.regions.empty()) {
        out += "regions:";
        for (const auto& e : s.regions) {
            const auto beat = [&](double pos) {
                return e.value->timebase == model::Timebase::Time ? s.timeMap->beatAt(pos) : pos;
            };
            out += " " + e.id.str() + " \"" + e.value->name + "\" " + barOf(beat(e.value->start)) +
                   "–" + barOf(beat(e.value->end)) + (e.value->section ? "" : " (not a section)");
        }
        out += "\n";
    }
    if (!s.context.empty()) {
        // By beat: the points come in id order. The first points of a long list only.
        constexpr std::size_t kShown = 64;
        auto list = [&](model::ContextType type, const char* label) {
            std::vector<std::pair<double, Id>> rows;
            for (const auto& e : s.context)
                if (e.value->type == type) rows.push_back({e.value->beat, e.id});
            if (rows.empty()) return;
            std::sort(rows.begin(), rows.end());
            out += std::string("  ") + label + ":";
            for (std::size_t i = 0; i < rows.size() && i < kShown; ++i) {
                const model::ContextPoint& p = *s.context.find(rows[i].second);
                std::string what;
                if (type == model::ContextType::Key) {
                    what = model::keyWords(p);
                } else if (p.symbol.empty()) {
                    what = "(no chord)";
                } else {
                    what = p.symbol + (model::parseChordPoint(p) ? "" : "?");
                }
                out += std::string(i ? " ·" : "") + " " + barOf(rows[i].first) + " " + what;
            }
            if (rows.size() > kShown)
                out += " · … " + std::to_string(rows.size() - kShown) + " more";
            out += "\n";
        };
        out += "context:\n";
        list(model::ContextType::Key, "keys");
        list(model::ContextType::Chord, "chords");
    }
    return out;
}

}  // namespace supple::engine
