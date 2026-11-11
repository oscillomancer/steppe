// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI clock and MTC output (ADR-0073): EngineApp resolves the MIDI port
// records (midi_ports.h, shared with hardware MIDI routing, ADR-0072) against
// the device's outputs for the engine's sync configuration, and reports what
// each output sends. midi.outputs.get / set are midi_port_commands.cpp's.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "supple/engine/app.h"

namespace supple::engine {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A port's name without its direction, for telling whether an input and an
// output are the same device: "system:midi_capture_1" and
// "system:midi_playback_1", "Interface MIDI In" and "Interface MIDI Out".
std::string deviceKey(std::string_view name) {
    std::string s = lower(name);
    for (std::string_view word : {"capture", "playback", "midiin", "midiout"})
        for (std::size_t at; (at = s.find(word)) != std::string::npos;) s.erase(at, word.size());
    std::string key, token;
    auto flush = [&] {
        if (!token.empty() && token != "in" && token != "out" && token != "input" &&
            token != "output" && token != "source" && token != "destination") {
            if (!key.empty()) key += ' ';
            key += token;
        }
        token.clear();
    };
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            token += c;
        else
            flush();
    }
    flush();
    return key;
}

bool sameDevice(std::string_view input, std::string_view output) {
    const std::string a = deviceKey(input);
    return !a.empty() && a == deviceKey(output);
}

}  // namespace

void EngineApp::resolveMidiOutputs(SyncConfig& c, std::vector<std::string>* warnings,
                                   std::vector<std::vector<std::string>>* perPort) const {
    if (!engine_) return;
    const std::vector<std::string> ports = engine_->midiOutputPorts();
    const std::vector<std::int64_t> latency = engine_->midiOutputLatency();
    const double rate = engine_->config().sampleRate;
    if (perPort) perPort->assign(ports.size(), {});
    // The sync master's device: what it sends is not echoed back to it.
    std::string master;
    if ((c.source == SyncSource::MidiClock || c.source == SyncSource::Mtc) && c.midiPort >= 0) {
        const auto inputs = engine_->midiPorts();
        if (static_cast<std::size_t>(c.midiPort) < inputs.size())
            master = inputs[static_cast<std::size_t>(c.midiPort)];
    }
    for (std::size_t i = 0; i < ports.size() && i < kMaxSyncOutputs; ++i) {
        const MidiPortSettings s = midiPortSettings(midiOutputsPreferences_, ports[i]);
        auto warn = [&](const std::string& text) {
            if (warnings) warnings->push_back(text);
            if (perPort) (*perPort)[i].push_back(text);
        };
        SyncOutput& out = c.outputs[i];
        if (!s.enabled) continue;  // a disabled port sends nothing
        out.clock = s.clock;
        out.transport = s.transport;
        out.spp = s.spp;
        out.mtc = s.mtc;
        out.whileStopped = s.clockWhileStopped;
        out.delay =
            (i < latency.size() ? latency[i] : 0) + std::llround(s.offsetMs / 1000.0 * rate);
        if (!master.empty() && sameDevice(master, ports[i])) {
            const bool clockMaster = c.source == SyncSource::MidiClock;
            const bool echoes = clockMaster ? (out.clock || out.transport || out.spp) : out.mtc;
            if (clockMaster) out.clock = out.transport = out.spp = out.whileStopped = false;
            if (!clockMaster) out.mtc = false;
            if (echoes)
                warn("midi.outputs: " + ports[i] + " is the sync master's device (" + master +
                     "), so its " + (clockMaster ? "clock" : "MTC") +
                     " is not sent back to it; send it to another output");
        }
    }
}


void EngineApp::addClockReport(json::Value& port, int index, const MidiPortSettings& s,
                               const std::vector<std::vector<std::string>>& perPort) const {
    port["clock"] = s.clock;
    port["transport"] = s.transport;
    port["spp"] = s.spp;
    port["mtc"] = s.mtc;
    port["clock_while_stopped"] = s.clockWhileStopped;
    auto sends = json::Value::array(), suppressed = json::Value::array();
    double delayMs = 0;
    std::int64_t sent = 0;
    if (engine_ && index >= 0) {
        const auto i = static_cast<std::size_t>(index);
        const double rate = engine_->config().sampleRate;
        const auto latency = engine_->midiOutputLatency();
        const std::int64_t frames = i < latency.size() ? latency[i] : 0;
        if (i < perPort.size())
            for (const auto& w : perPort[i]) {
                auto warning = json::Value::object();
                warning["device"] = port.find("name")->asString();
                warning["os_error"] = 0;
                warning["code"] = "midi.outputs.sync";
                warning["message"] = w;
                warning["hint"] = "send clock or MTC to an output other than the sync master";
                port["warnings"].push_back(std::move(warning));
            }
        if (i < kMaxSyncOutputs) {
            const SyncOutput& out = engine_->syncConfig().outputs[i];
            auto kind = [&](bool wanted, bool now, const char* name) {
                if (wanted && now) sends.push_back(name);
                if (wanted && !now && s.enabled) suppressed.push_back(name);
            };
            kind(s.clock, out.clock, "clock");
            kind(s.transport, out.transport, "transport");
            kind(s.spp, out.spp, "spp");
            kind(s.mtc, out.mtc, "mtc");
            kind(s.clock && s.clockWhileStopped, out.clock && out.whileStopped,
                 "clock_while_stopped");
            delayMs = static_cast<double>(static_cast<std::int64_t>(engine_->outputLatency()) +
                                          (out.sends() ? out.delay : frames)) *
                      1000.0 / rate;
            sent = static_cast<std::int64_t>(engine_->syncOutputSent(i));
        }
    }
    port["sends"] = std::move(sends);
    port["suppressed"] = std::move(suppressed);
    port["delay_ms"] = delayMs;
    port["sent"] = sent;
}

std::vector<std::vector<std::string>> EngineApp::clockWarnings() const {
    std::vector<std::vector<std::string>> perPort;
    if (!engine_) return perPort;
    SyncConfig fresh = engine_->syncConfig();
    fresh.outputs = {};
    resolveMidiOutputs(fresh, nullptr, &perPort);
    return perPort;
}

}  // namespace supple::engine
