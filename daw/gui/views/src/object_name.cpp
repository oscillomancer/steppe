// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/object_name.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "supple/model/take_owner.h"
#include "supple/views/lanes.h"
#include "supple/views/launcher.h"

namespace supple::views {

namespace {

std::string barWords(const model::Session& s, double beat) {
    return "bar " + std::to_string(s.timeMap->barBeat(std::max(0.0, beat)).bar);
}

double beatOf(const model::Session& s, double pos, model::Timebase timebase) {
    return timebase == model::Timebase::Time ? s.timeMap->beatAt(pos) : pos;
}

// A parameter's owner and name, as "Bass's volume" or "the cutoff".
std::string targetWords(const model::Session& s, const model::EnvTarget& target) {
    const model::Kind kind = target.object.kind;
    const bool named = kind == model::Kind::Track || kind == model::Kind::Take ||
                       kind == model::Kind::Fx || kind == model::Kind::Group;
    const std::string param = target.param.empty() ? std::string("parameter") : target.param;
    return named ? objectName(s, target.object) + "'s " + param : "the " + param;
}

std::string moduleWords(const model::Session& s, model::Id id) {
    const model::RackModule* m = s.modules.find(id);
    if (!m) return "a removed module";
    if (!m->label.empty()) return m->label;
    if (m->type == model::kPluginModule && m->fx) return objectName(s, *m->fx);
    if (std::string label = model::moduleLabel(s, id); !label.empty()) return label;
    const auto dot = m->type.rfind('.');
    return m->type.substr(dot == std::string::npos ? 0 : dot + 1);
}

std::string routeEndWords(const model::Session& s, const model::RouteEnd& end) {
    if (end.fx) return objectName(s, *end.fx) + "'s key";
    if (end.track) return objectName(s, *end.track);
    if (end.module) return objectName(s, *end.module);
    if (end.midiDevice) return *end.midiDevice;
    return "the hardware";
}

}  // namespace

std::string objectName(const model::Session& s, model::Id id) {
    switch (id.kind) {
        case model::Kind::Track:
            return s.tracks.find(id) ? model::trackLabel(s, id) : std::string("a removed track");
        case model::Kind::Scene: {
            const model::Scene* sc = s.scenes.find(id);
            if (sc && !sc->name.empty()) return sc->name;
            const std::vector<model::Id> order = launcherColumns(s).scenes;
            const auto at = std::find(order.begin(), order.end(), id);
            return at == order.end() ? std::string("a removed scene")
                                     : "Scene " + std::to_string(at - order.begin() + 1);
        }
        case model::Kind::Clip: {
            const model::Clip* c = s.clips.find(id);
            if (!c) return "a removed clip";
            if (!c->name.empty()) return c->name;
            return objectName(s, c->track) + "'s clip in " + objectName(s, c->scene);
        }
        case model::Kind::Item: {
            const model::Item* it = s.items.find(id);
            if (!it) return "a removed item";
            if (std::string label = Lanes::itemLabel(s, *it); !label.empty()) return label;
            // The bar, and the beat when another item of the track starts in
            // that bar too, so two unnamed ones read apart.
            const double start = std::max(0.0, model::itemSpan(s, *it).startBeat);
            const auto at = s.timeMap->barBeat(start);
            bool shared = false;
            for (const auto& e : s.items)
                if (e.id != id && e.value->track == it->track &&
                    s.timeMap->barBeat(std::max(0.0, model::itemSpan(s, *e.value).startBeat)).bar ==
                        at.bar)
                    shared = true;
            return objectName(s, it->track) + "'s item at bar " + std::to_string(at.bar) +
                   (shared ? ", beat " + std::to_string(at.beat) : std::string());
        }
        case model::Kind::Take: {
            const model::Take* tk = s.takes.find(id);
            if (!tk) return "a removed take";
            if (!tk->name.empty()) return tk->name;
            const auto owner = model::takeOwner(s, id);
            return owner ? objectName(s, *owner) : std::string("a take");
        }
        case model::Kind::Envelope: {
            const model::Envelope* en = s.envelopes.find(id);
            if (!en) return "a removed envelope";
            return targetWords(s, en->target) + " automation";
        }
        case model::Kind::AutoItem: {
            const model::AutomationItem* ai = s.autoItems.find(id);
            if (!ai) return "a removed automation item";
            if (!ai->name.empty()) return ai->name;
            return objectName(s, ai->envelope) + " item at " + barWords(s, ai->position);
        }
        case model::Kind::Region: {
            const model::Region* rg = s.regions.find(id);
            if (!rg) return "a removed section";
            if (!rg->name.empty()) return rg->name;
            return std::string(rg->section ? "the section at " : "the range at ") +
                   barWords(s, beatOf(s, rg->start, rg->timebase));
        }
        case model::Kind::Marker: {
            const model::Marker* mk = s.markers.find(id);
            if (!mk) return "a removed marker";
            if (!mk->name.empty()) return mk->name;
            return "the marker at " + barWords(s, beatOf(s, mk->pos, mk->timebase));
        }
        case model::Kind::TempoPoint: {
            const model::TempoPointObj* tp = s.tempo.find(id);
            return tp ? "the tempo change at " + barWords(s, tp->beat)
                      : std::string("a removed tempo change");
        }
        case model::Kind::Meter: {
            const model::MeterObj* ts = s.meters.find(id);
            return ts ? "the meter change at bar " + std::to_string(ts->bar)
                      : std::string("a removed meter change");
        }
        case model::Kind::Context: {
            const model::ContextPoint* cx = s.context.find(id);
            if (!cx) return "a removed key or chord";
            return std::string(cx->type == model::ContextType::Key ? "the key at "
                                                                   : "the chord at ") +
                   barWords(s, cx->beat);
        }
        case model::Kind::Fx: {
            const model::FxInstance* fx = s.fx.find(id);
            if (!fx) return "a removed plug-in";
            if (!fx->label.empty()) return fx->label;
            if (!fx->plugin.name.empty()) return fx->plugin.name;
            return fx->plugin.id.empty() ? std::string("a plug-in") : fx->plugin.id;
        }
        case model::Kind::Route: {
            const model::Route* r = s.routes.find(id);
            if (!r) return "a removed send";
            return "the send from " + routeEndWords(s, r->from) + " to " + routeEndWords(s, r->to);
        }
        case model::Kind::Group: {
            const model::TrackGroup* g = s.groups.find(id);
            if (!g) return "a removed group";
            return g->name.empty() ? std::string("an unnamed group") : g->name;
        }
        case model::Kind::Rack:
            return s.racks.find(id) ? model::rackLabel(s, id) : std::string("a removed rack");
        case model::Kind::Module:
            return moduleWords(s, id);
        case model::Kind::Cable: {
            const model::Cable* cb = s.cables.find(id);
            if (!cb) return "a removed cable";
            return "the cable from " + objectName(s, cb->from.object) + " to " +
                   objectName(s, cb->to.object);
        }
        case model::Kind::Modulation: {
            const model::Modulation* mr = s.modulations.find(id);
            if (!mr) return "a removed modulation";
            return "the modulation of " + targetWords(s, mr->target);
        }
        case model::Kind::Media: {
            const model::MediaSource* md = s.media.find(id);
            if (!md) return "a removed file";
            std::string_view name = md->path;
            if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos)
                name.remove_prefix(slash + 1);
            return name.empty() ? std::string("a file") : std::string(name);
        }
        case model::Kind::Midi: {
            for (const auto& e : s.takes)
                if (e.value->source == id) return objectName(s, e.id);
            return "a MIDI part";
        }
        case model::Kind::Lane:
            return "a lane";
        case model::Kind::RenderJob:
            return "a render";
        case model::Kind::Project:
            return "the project";
    }
    return "an object";
}

std::string objectNames(const model::Session& s, std::string_view text) {
    auto word = [&](std::size_t i) {
        return i < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_');
    };
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        // A whole id: two lower-case letters, "_", digits, not inside a word.
        const bool starts = i + 3 < text.size() && (i == 0 || !word(i - 1)) &&
                            std::islower(static_cast<unsigned char>(text[i])) &&
                            std::islower(static_cast<unsigned char>(text[i + 1])) &&
                            text[i + 2] == '_' &&
                            std::isdigit(static_cast<unsigned char>(text[i + 3]));
        if (starts) {
            std::size_t end = i + 3;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (!word(end))
                if (const auto id = model::Id::parse(text.substr(i, end - i))) {
                    const std::string name = objectName(s, *id);
                    // "Bass (tr_2)": a message that gives the name already
                    // says it once, not "Bass (Bass)".
                    const std::string given = name + " (";
                    if (i > 0 && text[i - 1] == '(' && end < text.size() && text[end] == ')' &&
                        out.size() >= given.size() &&
                        out.compare(out.size() - given.size(), given.size(), given) == 0) {
                        out.resize(out.size() - 2);
                        i = end + 1;
                        continue;
                    }
                    out += name;
                    i = end;
                    continue;
                }
        }
        out += text[i++];
    }
    return out;
}

Error objectNames(const model::Session& s, Error error) {
    error.message = objectNames(s, error.message);
    error.hint = objectNames(s, error.hint);
    for (Error& detail : error.details) detail = objectNames(s, std::move(detail));
    return error;
}

}  // namespace supple::views
