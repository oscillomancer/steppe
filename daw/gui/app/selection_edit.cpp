// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "selection_edit.h"
#include "supple/model/groups.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

namespace supple::gui {
namespace {

constexpr double kSilenceDb = -150.0, kTopDb = 12.0;
constexpr const char* kShared[] = {"mute",         "solo",     "armed",        "monitor",
                                   "phase_invert", "mono",     "record_input", "record_midi",
                                   "volume_db",    "pan",      "width",        "pan_left",
                                   "pan_right",    "pan_mode", "pan_law"};

const model::Track* trackOf(const model::Session& s, const std::string& text) {
    const auto id = model::Id::parse(text);
    return id ? s.tracks.find(*id) : nullptr;
}

}  // namespace

Result<json::Value> SelectionEdit::set(json::Value args, const std::string& gesture) {
    cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
    origin.gesture = gesture;
    if (const auto* bypass = args.find("bypass_groups"); bypass && bypass->asBool())
        return dispatcher_.execute({"track.set", std::move(args), origin});
    const auto* named = args.find("track");
    const std::string track = named && named->isString() ? named->asString() : std::string();
    const model::Session s = app_.head();
    const auto* source = trackOf(s, track);
    if (!source) return dispatcher_.execute({"track.set", std::move(args), origin});
    std::vector<std::string> seeds{track};
    const bool shared = std::any_of(std::begin(kShared), std::end(kShared),
                                    [&](const char* k) { return args.find(k) != nullptr; });
    if (shared && ui_.selectedTracks.size() > 1 && trackSelected(ui_, track)) {
        seeds = ui_.selectedTracks;
        std::erase_if(seeds, [&](const auto& id) { return !trackOf(s, id) || id == track; });
        seeds.insert(seeds.begin(), track);  // the touched member drives its component
    }
    std::map<std::string, json::Value> edits;
    std::map<std::string, std::string> relativeLevel;
    std::map<std::string, std::map<std::string, std::string>> relativePan;
    for (const auto& seed : seeds) edits[seed] = args;
    for (const char* field : {"volume_db", "pan", "width", "pan_left", "pan_right", "pan_mode",
                              "pan_law", "mute", "solo", "armed", "monitor"}) {
        const auto* value = args.find(field);
        if (!value) continue;
        const std::string attribute = std::string_view(field) == "volume_db" ? "volume" : field;
        std::set<std::string> covered;
        for (const auto& seed : seeds) {
            if (covered.contains(seed)) continue;
            const auto id = model::Id::parse(seed);
            if (!id) continue;
            const auto targets = model::linkedTracks(s, *id, attribute);
            for (auto target : targets) {
                const auto text = target.str();
                if (!edits.contains(text)) edits[text] = json::Value::object();
                edits[text][field] = *value;
                covered.insert(text);
                if (targets.size() > 1) {
                    if (attribute == "volume") relativeLevel[text] = seed;
                    if (attribute == "pan" || attribute == "width" || attribute == "pan_left" ||
                        attribute == "pan_right")
                        relativePan[field][text] = seed;
                }
            }
        }
    }
    const bool level = args.find("volume_db"), pan = args.find("pan") || args.find("width") ||
                                                     args.find("pan_left") ||
                                                     args.find("pan_right");
    if (!gesture.empty() && (level || pan) && gesture != gesture_) {
        gesture_ = gesture;
        start_.clear();
        for (const auto& [id, unused] : edits) {
            (void)unused;
            if (const auto* t = trackOf(s, id)) start_[id] = *t;
        }
    }
    const bool drag = !gesture.empty() && gesture == gesture_ && start_.contains(track);
    auto apply = [&]() -> Result<json::Value> {
        Result<json::Value> last = json::Value::object();
        for (auto& [id, each] : edits) {
            const auto* t = trackOf(s, id);
            if (!t) continue;
            each["track"] = id;
            each["bypass_groups"] = true;
            if (t->type != model::StripType::Channel && id != track) {
                for (const char* field : {"monitor", "record_input", "record_midi", "record_rack"})
                    each.asObject().erase(field);
                // Masters are armed to record their output (ADR-0057); busses never.
                if (t->type == model::StripType::Buss) each.asObject().erase("armed");
            }
            const auto start = drag && start_.contains(id) ? start_.at(id) : *t;
            if (each.find("volume_db") && (drag || relativeLevel.contains(id))) {
                const auto& driver = drag ? track : relativeLevel.at(id);
                const double ref = drag ? start_.at(track).volumeDb : trackOf(s, driver)->volumeDb;
                const double desired = args["volume_db"].asDouble();
                each["volume_db"] =
                    desired <= kSilenceDb ? kSilenceDb
                    : (ref <= kSilenceDb || start.volumeDb <= kSilenceDb)
                        ? (id == driver ? desired : start.volumeDb)
                        : std::clamp(start.volumeDb + desired - ref, kSilenceDb, kTopDb);
            }
            for (const auto& [field, member] : {std::pair{"pan", &model::Track::pan},
                                                {"width", &model::Track::width},
                                                {"pan_left", &model::Track::panLeft},
                                                {"pan_right", &model::Track::panRight}}) {
                if (each.find(field) && (drag || relativePan[field].contains(id))) {
                    const double ref = drag ? start_.at(track).*member
                                            : trackOf(s, relativePan[field].at(id))->*member;
                    each[field] =
                        std::clamp(start.*member + args[field].asDouble() - ref, -100.0, 100.0);
                }
            }
            if (t->type == model::StripType::Master && id != track)
                for (const char* field : {"width", "pan_left", "pan_right", "pan_mode", "pan_law"})
                    each.asObject().erase(field);
            last = dispatcher_.execute({"track.set", each, origin});
            if (!last) return last;
        }
        return last;
    };
    if (edits.size() <= 1) return apply();
    return dispatcher_.transact("Edit " + std::to_string(edits.size()) + " tracks", origin, apply);
}

}  // namespace supple::gui
