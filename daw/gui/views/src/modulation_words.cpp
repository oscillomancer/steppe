// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/modulation_words.h"

#include <cctype>
#include <string>
#include <vector>

#include "supple/views/object_name.h"

namespace supple::views {
namespace {

// What a copying command does, as the start of a sentence.
std::string copying(std::string_view command) {
    if (command == "item.paste") return "Pasting";
    if (command == "item.duplicate") return "Duplicating";
    if (command == "item.split") return "Splitting";
    if (command == "item.consolidate" || command == "item.consolidate.commit")
        return "Consolidating";
    if (command == "item.add") return "Copying the clip to the timeline";
    if (command == "clip.add") return "Making the clip";
    if (command == "clip.duplicate") return "Copying the clip";
    if (command == "scene.capture") return "Capturing the scene";
    if (command == "scene.insert") return "Inserting the scene";
    if (command == "arrangement.build") return "Building the arrangement";
    if (command == "track.duplicate") return "Duplicating the track";
    if (command == "launch.commit") return "Committing the performance";
    if (command == "take.add") return "Copying the take";
    return "The copy";
}

// What a moving command does, as the start of a sentence.
std::string moving(std::string_view command) {
    if (command == "item.move") return "Moving the item there";
    if (command == "clip.move") return "Moving the clip there";
    if (command == "take.extract") return "Moving the take there";
    if (command == "item.explode-takes") return "Exploding the takes";
    return "The move";
}

// The commands that make, change or remove a route keep their own words
// (gui.md §2): only what M5 adds to copies, moves and joins is said here.
bool routeCommand(std::string_view command) {
    return command == "modulate" || command.starts_with("modulation.");
}

std::string capitalized(std::string text) {
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

constexpr std::string_view kRemove = "Remove modulation in the plug-in parameter's menu";

}  // namespace

std::optional<std::string> modulationRefusalWords(const model::Session&, std::string_view command,
                                                  const Error& e) {
    if (routeCommand(command)) return std::nullopt;
    if (e.code == "modulation.limit")
        return copying(command) + " would make more than " +
               std::to_string(model::kMaxModulations) +
               " modulation routes, the most a project holds. Remove some first (" +
               std::string(kRemove) + "); a copied chain brings its routes with it.";
    if (e.code == "modulation.cycle") {
        if (command == "item.explode-takes")
            return "Exploding the takes would put one on a track that feeds the rack "
                   "modulating its plug-ins, so the modulation would loop. Explode them in "
                   "sequence on this track, or remove the modulation first (" +
                   std::string(kRemove) + ").";
        return moving(command) +
               " would loop its modulation: that track feeds the rack that modulates the "
               "take's plug-ins. Choose another track, or remove the modulation first (" +
               std::string(kRemove) + ").";
    }
    if (e.code == "item.heal_fx")
        return "The pieces' take plug-ins differ (the plug-ins, their settings, their "
               "automation or their modulation), so they cannot be joined. Make them match (" +
               std::string(kRemove) + " removes a route), or consolidate them instead.";
    return std::nullopt;
}

Error modulationRefusal(const model::Session& s, std::string_view command, Error e) {
    if (auto words = modulationRefusalWords(s, command, e)) {
        e.message = std::move(*words);
        e.hint.clear();
    }
    return e;
}

std::optional<std::string> modulationWarningWords(const model::Session& s, std::string_view,
                                                  std::string_view code, std::string_view path) {
    if (code == "modulation.stacked")
        return "The new modulation adds to one already there from the same kind of source: "
               "their movements add up.";
    if (code == "modulation.removed") return "A modulation route went with it.";
    if (code != "modulation.not_copied") return std::nullopt;
    // The original route, at /modulations/<id>; the warning gives no reason
    // apart from its message, so the model says which: its source is not
    // in the project, the project is full (a recording's copy, which is
    // never refused), or the copy would loop.
    constexpr std::string_view prefix = "/modulations/";
    std::optional<model::Id> id;
    if (path.starts_with(prefix)) id = model::Id::parse(path.substr(prefix.size()));
    const model::Modulation* original = id ? s.modulations.find(*id) : nullptr;
    const std::string name = original ? objectName(s, *id) : "A modulation";
    if (!original || !s.modules.find(original->source.module))
        return capitalized(name) +
               " was not copied: its rack is not in the project now. Modulate the copy "
               "again once it has one.";
    if (s.modulations.size() >= model::kMaxModulations)
        return capitalized(name) + " was not copied: the project holds the most routes it can (" +
               std::to_string(model::kMaxModulations) + "). Remove some (" + std::string(kRemove) +
               "), then modulate the copy again.";
    return capitalized(name) +
           " was not copied: the copy's track feeds the rack that makes it, so it would loop. "
           "Modulate the copy from a rack that track does not feed.";
}

std::string modulationNote(const model::Session& s, std::string_view command,
                           const json::Value& result) {
    const json::Value* warnings = result.isObject() ? result.find("warnings") : nullptr;
    if (!warnings || !warnings->isArray()) return {};
    std::vector<std::string> notCopied;
    std::string stacked;
    int removed = 0;
    for (const json::Value& w : warnings->asArray()) {
        if (!w.isObject()) continue;
        const json::Value* code = w.find("code");
        const json::Value* path = w.find("path");
        if (!code || !code->isString()) continue;
        const std::string at = path && path->isString() ? path->asString() : std::string();
        const auto words = modulationWarningWords(s, command, code->asString(), at);
        if (!words) continue;
        if (code->asString() == "modulation.not_copied")
            notCopied.push_back(*words);
        else if (code->asString() == "modulation.removed")
            ++removed;
        else
            stacked = *words;
    }
    std::string note;
    auto add = [&note](const std::string& text) { note += (note.empty() ? "" : " ") + text; };
    if (!notCopied.empty()) {
        add(notCopied.front());
        if (notCopied.size() == 2) add("And 1 more modulation was not copied.");
        if (notCopied.size() > 2)
            add("And " + std::to_string(notCopied.size() - 1) +
                " more modulations were not copied.");
    }
    if (removed == 1) add(*modulationWarningWords(s, command, "modulation.removed", ""));
    if (removed > 1) add(std::to_string(removed) + " modulation routes went with it.");
    if (!stacked.empty()) add(stacked);
    return note;
}

}  // namespace supple::views
