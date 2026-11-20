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
#include "clip_launcher.h"
#include "command_helpers.h"
#include "media_files.h"
#include "render_commands.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/midi/midi.h"
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
void registerModulationCommands(cmd::Registry& registry);
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
void registerClipLauncherCommands(cmd::Registry& registry);

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
    registerModulationCommands(registry);
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
    registerClipLauncherCommands(registry);
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
    auto barOf = [&](double beat) {
        const auto bb = s.timeMap->barBeat(beat);
        char b[48];
        if (bb.beat == 1 && bb.fraction == 0)
            std::snprintf(b, sizeof b, "bar %d", bb.bar);
        else
            std::snprintf(b, sizeof b, "bar %d.%d", bb.bar, bb.beat);
        return std::string(b);
    };
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
            std::string routes;  // modulation into this track's FX or mix (ADR-0096)
            for (const auto& e : s.modulations) {
                const model::Modulation& m = *e.value;
                if (model::modulationTargetNode(s, m.target) != id) continue;
                char amount[32];
                std::snprintf(amount, sizeof amount, " %+g", m.depth);
                routes += " " + e.id.str() + " " + m.source.module.str() + "." + m.source.input +
                          "→" + m.target.object.str() + " " + m.target.param + amount +
                          (m.enabled ? "" : " (off)");
            }
            if (!routes.empty())
                out += std::string(static_cast<std::size_t>(2 * depth + 2), ' ') +
                       "modulation:" + routes + "\n";
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
                   "–" + barOf(beat(e.value->end));
        }
        out += "\n";
    }
    out += describeScenes(s);  // clips never move the project length
    return out;
}

}  // namespace supple::engine
