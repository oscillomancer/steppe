// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/modulation_words.h"

#include <cctype>
#include <string>
#include <vector>

#include "supple/views/object_name.h"

namespace supple::views {
namespace {

// What a copying command does, as the start of a sentence; nullopt for a
// command not known to copy routes (its caller keeps its own words).
std::optional<std::string> copying(std::string_view command) {
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
    return std::nullopt;
}

// What a command that moves a take does, as the start of a sentence;
// nullopt for any other.
std::optional<std::string> movingTake(std::string_view command) {
    if (command == "item.move") return "Moving the item there";
    if (command == "clip.move") return "Moving the clip there";
    if (command == "take.extract") return "Moving the take there";
    return std::nullopt;
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
    // Only a (command, code) pair known here has words: any other keeps its
    // caller's (a generic verb once worded a send as a take's move).
    if (e.code == "modulation.limit") {
        const auto verb = copying(command);
        if (!verb) return std::nullopt;
        return *verb + " would make more than " + std::to_string(model::kMaxModulations) +
               " modulation routes, the most a project holds. Remove some first (" +
               std::string(kRemove) + "); a copied chain brings its routes with it.";
    }
    if (e.code == "modulation.cycle") {
        if (command == "route.add")
            return "The send would loop its modulation: where it goes feeds the rack that "
                   "modulates the sending track. Send it elsewhere, or remove the modulation "
                   "first (" +
                   std::string(kRemove) + ").";
        if (command == "track.move")
            return "Moving the track there would loop its modulation: it would feed the rack "
                   "that modulates it. Put it elsewhere, or remove the modulation first (" +
                   std::string(kRemove) + ").";
        if (command == "fx.move")
            return "Moving the plug-in there would loop its modulation: that track feeds the "
                   "rack that modulates the plug-in. Choose another track, or remove the "
                   "modulation first (" +
                   std::string(kRemove) + ").";
        if (command == "item.explode-takes")
            return "Exploding the takes would put one on a track that feeds the rack "
                   "modulating its plug-ins, so the modulation would loop. Explode them in "
                   "sequence on this track, or remove the modulation first (" +
                   std::string(kRemove) + ").";
        const auto verb = movingTake(command);
        if (!verb) return std::nullopt;
        return *verb +
               " would loop its modulation: that track feeds the rack that modulates the "
               "take's plug-ins. Choose another track, or remove the modulation first (" +
               std::string(kRemove) + ").";
    }
    if (e.code == "item.heal_fx" && command == "item.heal")
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
                                                  std::string_view code, std::string_view path,
                                                  std::string_view reason) {
    if (code != "modulation.removed" && code != "modulation.not_copied") return std::nullopt;
    // The route at /modulations/<id>, named from `s` when it holds it.
    constexpr std::string_view prefix = "/modulations/";
    std::optional<model::Id> id;
    if (path.starts_with(prefix)) id = model::Id::parse(path.substr(prefix.size()));
    const bool named = id && s.modulations.find(*id);
    if (code == "modulation.removed") {
        const std::string name = named ? capitalized(objectName(s, *id)) : "A modulation route";
        // The cause is the warning's reason (command-bus.md).
        if (reason == "source_unfit")
            return name + " was removed: its rack's output can no longer modulate.";
        if (reason == "target_unfit")
            return name +
                   " was removed: what it modulated can no longer be modulated (a send "
                   "turned MIDI, for instance).";
        return name + " went with it.";  // source_removed, target_removed
    }
    const std::string name = named ? capitalized(objectName(s, *id)) : "A modulation";
    if (reason == "source")
        return name +
               " was not copied: its rack is not where the copy is. Modulate the copy again once "
               "it has one.";
    if (reason == "cycle")
        return name +
               " was not copied: the copy's track feeds the rack that makes it, so it would "
               "loop. Modulate the copy from a rack that track does not feed.";
    if (reason == "limit")
        return name + " was not copied: the project holds the most routes it can (" +
               std::to_string(model::kMaxModulations) + "). Remove some (" + std::string(kRemove) +
               "), then modulate the copy again.";
    return name + " was not copied. Modulate the copy again if it needs it.";
}

std::string modulationNote(const model::Session& s, std::string_view command,
                           const json::Value& result) {
    const json::Value* warnings = result.isObject() ? result.find("warnings") : nullptr;
    if (!warnings || !warnings->isArray()) return {};
    std::vector<std::string> notCopied;
    std::string removedWords;
    int removed = 0;
    for (const json::Value& w : warnings->asArray()) {
        if (!w.isObject()) continue;
        const json::Value* code = w.find("code");
        const json::Value* path = w.find("path");
        const json::Value* reason = w.find("reason");
        if (!code || !code->isString()) continue;
        const std::string at = path && path->isString() ? path->asString() : std::string();
        const std::string why = reason && reason->isString() ? reason->asString() : std::string();
        const auto words = modulationWarningWords(s, command, code->asString(), at, why);
        if (!words) continue;
        if (code->asString() == "modulation.not_copied")
            notCopied.push_back(*words);
        else if (++removed == 1)  // modulation.removed
            removedWords = *words;
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
    if (removed == 1) add(removedWords);
    if (removed > 1) add(std::to_string(removed) + " modulation routes went with it.");
    return note;
}

void appendModulationNote(std::string& note, const model::Session& s, std::string_view command,
                          const json::Value& result) {
    const std::string said = modulationNote(s, command, result);
    if (!said.empty()) note += (note.empty() ? "" : " ") + said;
}

void collectWarnings(json::Value& into, const json::Value& result) {
    const json::Value* warnings = result.isObject() ? result.find("warnings") : nullptr;
    if (!warnings || !warnings->isArray() || warnings->asArray().empty()) return;
    if (!into.isObject()) into = json::Value::object();
    if (!into.find("warnings") || !into["warnings"].isArray())
        into["warnings"] = json::Value::array();
    for (const json::Value& w : warnings->asArray()) into["warnings"].push_back(w);
}

}  // namespace supple::views
