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

// The argument a refusal names: "to" for "/args/to" (or "/to").
std::string_view argumentOf(std::string_view path) {
    if (path.starts_with("/args")) path.remove_prefix(5);
    if (path.starts_with("/")) path.remove_prefix(1);
    return path.substr(0, path.find('/'));
}

// The words for each edit that can close a modulation cycle, by command and
// the argument it is refused at (session-model.md, "Edits that can close a
// modulation cycle"). A command with one row here takes it whatever the
// path; one with several takes its argument's, or none.
struct CycleWords {
    std::string_view command, argument, cause, remedy;
};
constexpr CycleWords kCycleWords[] = {
    {"track.move", "parent",
     "Moving the track there would loop its modulation: it would feed the "
     "rack that modulates it.",
     "Put it elsewhere"},
    {"track.set", "parent_send",
     "The parent send would loop its modulation: the parent feeds the "
     "rack that modulates the track.",
     "Leave it off"},
    {"track.set", "record_rack",
     "Taking that rack's Out as the input would loop its modulation: "
     "the track feeds the rack that modulates what plays into it.",
     "Pick another rack's Out"},
    {"route.add", "to",
     "The send would loop its modulation: where it goes feeds, or plays a Notes "
     "module in, the rack that modulates the sending track.",
     "Send it elsewhere"},
    {"route.add", "fx",
     "The key route would loop its modulation: the plug-in it keys feeds the "
     "rack that modulates the sending track.",
     "Key another plug-in"},
    {"route.set", "to",
     "The send would loop its modulation: where it goes feeds, or plays a Notes "
     "module in, the rack that modulates the sending track.",
     "Keep its destination"},
    {"route.set", "from",
     "The send would loop its modulation: where it goes feeds, or plays a "
     "Notes module in, the rack that modulates the sending track.",
     "Keep its source"},
    {"route.set", "fx",
     "The key route would loop its modulation: the plug-in it keys feeds the "
     "rack that modulates the sending track.",
     "Keep the plug-in it keys"},
    {"route.set", "route",
     "The send would loop its modulation: where it goes feeds, or plays a "
     "Notes module in, the rack that modulates the sending track.",
     "Leave the send as it was"},
    {"module.add", "source",
     "A Notes module playing that track would loop its modulation: a MIDI "
     "send from what its rack modulates reaches the track.",
     "Choose another track for it to play"},
    {"module.add", "rack",
     "A Notes module here would loop its modulation: it plays the rack's "
     "channel, which a MIDI send from what the rack modulates reaches.",
     "Add it, then choose another track for it to play"},
    {"module.set", "source",
     "Playing that track would loop its modulation: a MIDI send from what "
     "the rack modulates reaches it.",
     "Choose another track"},
    {"module.set", "source_sends",
     "Hearing MIDI sends would loop its modulation: one comes from "
     "what the rack modulates.",
     "Leave them off"},
    {"rack.set", "track",
     "Moving the rack's home there would loop its modulation: its Notes "
     "module would hear what the rack modulates.",
     "Keep it on its channel"},
    {"rack.paste", "rack",
     "A pasted Notes module would loop its modulation: it would hear a MIDI "
     "send from what the rack modulates.",
     "Paste into another rack or remove that MIDI send"},
    {"rack.preset.load", "rack",
     "The patch's Notes module would loop its modulation: it plays the "
     "rack's channel, which a MIDI send from what the rack modulates reaches.",
     "Load it into another rack or remove that MIDI send"},
    {"track.remove", "track",
     "Deleting the track would loop its modulation: the Notes modules "
     "that played it would play their rack's channel instead and hear "
     "what the rack modulates.",
     "Choose what they play first"},
    {"fx.move", "track",
     "Moving the plug-in there would loop its modulation: that track feeds the "
     "rack that modulates the plug-in.",
     "Choose another track"},
    {"item.move", "track",
     "Moving the item there would loop its modulation: that track feeds the "
     "rack that modulates the take's plug-ins.",
     "Choose another track"},
    {"clip.move", "track",
     "Moving the clip there would loop its modulation: that track feeds the "
     "rack that modulates the take's plug-ins.",
     "Choose another track"},
    {"take.extract", "track",
     "Moving the take there would loop its modulation: that track feeds "
     "the rack that modulates the take's plug-ins.",
     "Choose another track"},
    {"item.explode-takes", "item",
     "Exploding the takes would put one on a track that feeds the "
     "rack modulating its plug-ins, so the modulation would loop.",
     "Explode them in sequence on this track"},
};

std::optional<std::string> cycleWords(std::string_view command, std::string_view path) {
    const std::string_view argument = argumentOf(path);
    const CycleWords* only = nullptr;
    int rows = 0;
    for (const CycleWords& w : kCycleWords) {
        if (w.command != command) continue;
        ++rows;
        only = &w;
        if (w.argument == argument)
            return std::string(w.cause) + " " + std::string(w.remedy) +
                   ", or remove the modulation first (" + std::string(kRemove) + ").";
    }
    if (rows == 1)
        return std::string(only->cause) + " " + std::string(only->remedy) +
               ", or remove the modulation first (" + std::string(kRemove) + ").";
    return std::nullopt;
}

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
    if (e.code == "modulation.cycle") return cycleWords(command, e.path);
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
