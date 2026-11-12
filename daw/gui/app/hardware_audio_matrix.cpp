// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Hardware Audio is a view of record_input and masters' hw_output, not routes.
#include "routing_matrix.h"

#include <algorithm>
#include <array>
#include <map>

#include "supple/engine/realtime.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {
const cmd::Origin kAudioOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::vector<int> recordChannels(const model::Session& session, const model::Track& track) {
    if (track.record.input)
        if (const auto* route = session.routes.find(*track.record.input);
            route && route->from.hwChannels)
            return *route->from.hwChannels;
    return {};
}

json::Value channelsJson(const std::vector<int>& channels) {
    auto value = json::Value::array();
    for (int channel : channels) value.push_back(channel);
    return value;
}

std::string sideId(const std::string& signal, int side) {
    return signal + (side == 1 ? ":L" : side == 2 ? ":R" : "");
}

bool contains(const std::vector<int>& channels, int channel) {
    return std::find(channels.begin(), channels.end(), channel) != channels.end();
}
}  // namespace

void RoutingMatrix::setHardwareAudioMode() {
    setMidiMode(false);
    hardwareAudioMode_ = true;
    selection_.clear();
    anchor_.reset();
    anchorRow_.reset();
    anchorCol_.reset();
    if (isOpen()) (void)reload();
}

json::Value RoutingMatrix::saveAudioSplits() const {
    auto value = json::Value::array();
    for (const auto& signal : audioSplits_) value.push_back(signal);
    return value;
}

void RoutingMatrix::loadAudioSplits(const json::Value& value) {
    audioSplits_.clear();
    if (value.isArray())
        for (const auto& signal : value.asArray())
            if (signal.isString() && signal.asString().size() < 128)
                audioSplits_.insert(signal.asString());
    if (hardwareAudioMode_ && isOpen()) (void)reload();
}

std::vector<ui::MenuItem> RoutingMatrix::audioSignalMenu(bool row, std::size_t index) {
    if (!hardwareAudioMode_ ||
        (row ? index >= model_.sources.size() : index >= model_.destinations.size()))
        return {};
    const auto signal = row ? model_.sources[index].signal : model_.destinations[index].signal;
    const int width = row ? model_.sources[index].width : model_.destinations[index].width;
    const bool split = audioSplits_.contains(signal);
    return {{split ? "Join" : "Split into L/R", "",
             [this, signal, split] {
                 if (split)
                     audioSplits_.erase(signal);
                 else
                     audioSplits_.insert(signal);
                 selection_.clear();
                 anchor_.reset();
                 anchorRow_.reset();
                 anchorCol_.reset();
                 (void)reload();
             },
             width == 2}};
}

void RoutingMatrix::splitAllAudio(bool split) {
    if (!hardwareAudioMode_) return;
    for (const auto& s : model_.sources)
        if (s.width == 2) {
            if (split)
                audioSplits_.insert(s.signal);
            else
                audioSplits_.erase(s.signal);
        }
    for (const auto& d : model_.destinations)
        if (d.width == 2) {
            if (split)
                audioSplits_.insert(d.signal);
            else
                audioSplits_.erase(d.signal);
        }
    selection_.clear();
    anchor_.reset();
    anchorRow_.reset();
    anchorCol_.reset();
    (void)reload();
}

bool RoutingMatrix::hardwareAudioChanged() const {
    const auto* backend = app_.backend();
    return audioBackend_ != backend || audioInputs_ != (backend ? backend->config().inputs : 0) ||
           audioOutputs_ != (backend ? backend->config().outputs : 0) ||
           ltcOutput_ != app_.resolvedLtcOutputChannel();
}

Status RoutingMatrix::reloadHardwareAudio() {
    auto args = json::Value::object();
    args["mode"] = "audio";
    auto matrix = exec("route.matrix", std::move(args));
    if (!matrix) return std::move(matrix).error();
    auto device = exec("engine.inputs", json::Value::object());
    if (!device) return std::move(device).error();
    const auto session = app_.view();
    MatrixModel next;
    next.hardwareAudio = true;
    const int ltc = app_.resolvedLtcOutputChannel();
    std::map<std::string, std::vector<int>> inputs, outputs;
    int inputCount = 0, outputCount = ltc;
    std::array<std::string, 64> inputNames{}, outputNames{};
    for (const auto& row : matrix->find("sources")->asArray()) {
        const auto id = model::Id::parse(row.find("id")->asString());
        const auto* track = id ? session.tracks.find(*id) : nullptr;
        if (!track) continue;
        inputs[id->str()] = recordChannels(session, *track);
        if (track->type == model::StripType::Master)
            outputs[id->str()] = model::hardwareOutputs(*track, *id);
        for (int c : inputs[id->str()]) inputCount = std::max(inputCount, c);
        for (int c : outputs[id->str()]) outputCount = std::max(outputCount, c);
    }
    auto readNames = [](const json::Value& rows, auto& names, int& count) {
        for (const auto& row : rows.asArray()) {
            const int channel = static_cast<int>(row.find("channel")->asInt());
            if (channel < 1 || channel > 64) continue;
            names[static_cast<std::size_t>(channel - 1)] = row.find("port")->asString();
            count = std::max(count, channel);
        }
    };
    readNames(*device->find("audio"), inputNames, inputCount);
    readNames(*device->find("outputs"), outputNames, outputCount);
    // Physical pairs are views over consecutively numbered device channels.
    auto endpoints = [&](bool output) {
        const int count = std::min(output ? outputCount : inputCount, 64);
        const auto& names = output ? outputNames : inputNames;
        const auto* backend = app_.backend();
        const int available = backend ? static_cast<int>(output ? backend->config().outputs
                                                                : backend->config().inputs)
                                      : 0;
        for (int first = 1; first <= count; first += 2) {
            const int width = first < count ? 2 : 1;
            const auto signal =
                std::string(output ? "audio_out:" : "audio_in:") + std::to_string(first);
            const bool split = width == 2 && audioSplits_.contains(signal);
            for (int side = split ? 1 : 0; side <= (split ? 2 : 0); ++side) {
                std::vector<int> channels = side         ? std::vector<int>{first + side - 1}
                                            : width == 2 ? std::vector<int>{first, first + 1}
                                                         : std::vector<int>{first};
                std::string label = output ? "Output " : "Input ";
                label += MatrixModel::pairText(channels.front(), static_cast<int>(channels.size()));
                if (side) label += side == 1 ? " L" : " R";
                for (int c : channels)
                    if (!names[static_cast<std::size_t>(c - 1)].empty())
                        label += " · " + names[static_cast<std::size_t>(c - 1)];
                if (channels.back() > available) label += " (unavailable)";
                if (output && contains(channels, ltc)) label += " [LTC]";
                if (output) {
                    MatrixModel::Destination d;
                    d.id = sideId(signal, side);
                    d.signal = signal;
                    d.side = side;
                    d.width = width;
                    d.name = std::move(label);
                    d.kind = "audio_output";
                    d.deviceChannels = std::move(channels);
                    next.destinations.push_back(std::move(d));
                } else {
                    MatrixModel::Source s;
                    s.id = sideId(signal, side);
                    s.signal = signal;
                    s.side = side;
                    s.width = width;
                    s.name = std::move(label);
                    s.type = "audio_input";
                    s.deviceChannels = std::move(channels);
                    next.sources.push_back(std::move(s));
                }
            }
        }
    };
    endpoints(false);
    for (const auto& row : matrix->find("sources")->asArray()) {
        const auto signal = row.find("id")->asString();
        const auto id = model::Id::parse(signal);
        const auto* track = id ? session.tracks.find(*id) : nullptr;
        if (!track) continue;
        const int width = std::min(2, track->channels);
        const auto name = row.find("name")->asString();
        const auto type = row.find("type")->asString();
        const int depth = static_cast<int>(row.find("depth")->asInt());
        const bool split = width == 2 && audioSplits_.contains(signal);
        for (int side = split ? 1 : 0; side <= (split ? 2 : 0); ++side) {
            MatrixModel::Source s;
            s.id = sideId(signal, side);
            s.signal = signal;
            s.side = side;
            s.width = width;
            s.name = name + (side == 1 ? " L" : side == 2 ? " R" : "");
            s.type = type;
            s.depth = depth;
            next.sources.push_back(s);
            if (type == "channel") {
                MatrixModel::Destination d;
                d.id = s.id;
                d.signal = signal;
                d.side = side;
                d.width = width;
                d.name = s.name;
                d.kind = "track";
                d.type = type;
                next.destinations.push_back(std::move(d));
            }
        }
    }
    endpoints(true);
    for (const auto& s : next.sources)
        for (const auto& d : next.destinations) {
            const bool input = s.type == "audio_input" && d.kind == "track";
            const bool output = s.type == "master" && d.kind == "audio_output";
            if (!input && !output) continue;
            const auto& channels = input ? inputs[d.signal] : outputs[s.signal];
            std::string rule;
            if (input && d.side) {
                if (s.deviceChannels.size() != 1)
                    rule = "split the device input pair to choose one channel";
                else if (channels.size() < 2)
                    rule =
                        "assign a stereo input first; one mono record input feeds both L and R, "
                        "and cannot leave a gap";
            }
            if (output && s.side) {
                if (d.deviceChannels.size() != 1)
                    rule = "split the hardware output pair to choose one channel";
                else if (channels.size() != 2)
                    rule =
                        "assign a stereo output first; hw_output cannot silence just one side of a "
                        "master";
            }
            if (!rule.empty()) next.cellRules[{s.id, d.id}] = std::move(rule);
            bool connected = false, warning = false;
            for (int lane = 1; lane <= 2 && !channels.empty(); ++lane) {
                const int ch =
                    channels[std::min(static_cast<std::size_t>(lane - 1), channels.size() - 1)];
                const bool matches =
                    input ? (!d.side || d.side == lane) && contains(s.deviceChannels, ch)
                          : (!s.side || s.side == lane) && contains(d.deviceChannels, ch);
                connected = connected || matches;
                warning = warning || (output && matches && ch == ltc);
            }
            if (connected) {
                MatrixModel::Send send;
                send.id = std::string(input ? "audio.input:" : "audio.output:") +
                          (input ? d.signal : s.signal);
                send.from = s.id;
                send.to = d.id;
                if (warning)
                    send.warning =
                        "LTC replaces " + s.name + "'s audio on output " + std::to_string(ltc);
                next.sends.push_back(std::move(send));
            }
        }
    audioBackend_ = app_.backend();
    audioInputs_ = audioBackend_ ? audioBackend_->config().inputs : 0;
    audioOutputs_ = audioBackend_ ? audioBackend_->config().outputs : 0;
    ltcOutput_ = ltc;
    return adoptModel(std::move(next));
}

Status RoutingMatrix::editHardwareAudio(bool remove) {
    auto refuse = [&](const std::string& why, const std::string& hint) -> Status {
        statusMessage_ = why + "; " + hint;
        if (status_) status_->setText(statusMessage_);
        return Error("gui.routing_audio", why, "/selection", hint);
    };
    // One assignment may occupy several split cells. Partial removal would
    // silently remove another side or change stereo into an L+R mono sum.
    if (remove)
        for (const auto& id : selectedSends())
            for (std::size_t r = 0; r < model_.sources.size(); ++r)
                for (std::size_t c = 0; c < model_.destinations.size(); ++c)
                    for (const auto* send : model_.sendsAt(r, c))
                        if (send->id == id && !isSelected(r, c))
                            return refuse("one input/output assignment spans these cells",
                                          "select all its connected cells, or join the signal "
                                          "before removing it");
    const auto session = app_.view();
    struct Edit {
        bool input = false;
        std::vector<int> channels;
        std::map<int, int> claims;
    };
    std::map<std::string, Edit> edits;
    std::string replacements;
    for (const auto& cell : selection_) {
        const auto at = locate(cell);
        if (!at) continue;
        const auto& s = model_.sources[at->first];
        const auto& d = model_.destinations[at->second];
        const auto sends = model_.sendsAt(at->first, at->second);
        if (remove ? sends.empty() : !sends.empty()) continue;
        const auto rule = model_.blocked(at->first, at->second);
        if (!remove && !rule.empty()) {
            return refuse(s.name + " → " + d.name + ": " + rule,
                          "select compatible cells only; no assignments were changed");
        }
        const bool input = s.type == "audio_input";
        const auto signal = input ? d.signal : s.signal;
        const auto id = model::Id::parse(signal);
        const auto* track = id ? session.tracks.find(*id) : nullptr;
        if (!track) continue;
        auto [found, inserted] = edits.try_emplace(signal);
        auto& edit = found->second;
        if (inserted) {
            edit.input = input;
            edit.channels =
                input ? recordChannels(session, *track) : model::hardwareOutputs(*track, *id);
        }
        if (remove) {
            edit.channels.clear();
            continue;
        }
        const int side = input ? d.side : s.side;
        const auto& requested = input ? s.deviceChannels : d.deviceChannels;
        if (!side) {
            if (!edit.claims.empty() && edit.channels != requested)
                return refuse("a signal has one input/output assignment",
                              "select one pair, one mono channel, or matching split L/R cells");
            edit.channels = requested;
            edit.claims[0] = 1;
        } else {
            // A command can clear/retype this signal between view refreshes.
            // Cached cell rules must never authorize indexing a changed map.
            if (edit.channels.size() < 2)
                return refuse("the stereo assignment changed",
                              "assign a stereo pair before editing one side");
            const int lane = side - 1;
            if (edit.claims.contains(0) ||
                (edit.claims.contains(side) && edit.claims[side] != requested.front()))
                return refuse("two selected cells assign the same side",
                              "select one hardware channel for each side");
            edit.channels[static_cast<std::size_t>(lane)] = requested.front();
            edit.claims[side] = requested.front();
        }
        const auto replacement = inputReplacement(s, d);
        if (!replacement.empty()) replacements += (replacements.empty() ? "" : "; ") + replacement;
    }
    for (const auto& [signal, edit] : edits)
        if (!edit.input && edit.channels.size() == 2 && edit.channels[0] == edit.channels[1])
            return refuse("a stereo master needs two different hardware outputs",
                          "choose a different output for L and R");
    if (edits.empty()) return {};
    auto result = dispatcher_.transact(
        remove ? "Remove hardware audio assignments" : "Assign hardware audio", kAudioOrigin,
        [&]() -> Result<json::Value> {
            for (const auto& [signal, edit] : edits) {
                auto args = json::Value::object();
                args["track"] = signal;
                args[edit.input ? "record_input" : "hw_output"] =
                    remove && edit.input ? json::Value() : channelsJson(edit.channels);
                auto changed = exec("track.set", std::move(args));
                if (!changed) return changed;
            }
            return json::Value::object();
        });
    if (!result) return std::move(result).error();
    SUPPLE_TRY(reload());
    statusMessage_ = std::move(replacements);
    if (status_) status_->setText(status());
    return {};
}
}  // namespace supple::gui
