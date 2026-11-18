// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "generate.h"

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/json/parse.h"

namespace supple::gui {
namespace {

// 30 s of stereo: a decaying pluck every beat at 120 BPM over noise-free
// partials, different per variant so the waveforms are told apart.
audio::AudioData variant(int k) {
    audio::AudioData a;
    a.sampleRate = 48000;
    const std::size_t frames = 30 * 48000;
    a.channels.assign(2, std::vector<float>(frames));
    const double f0 = 55.0 * std::pow(2.0, k / 4.0);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / a.sampleRate;
        const double beat = std::fmod(t, 0.5);
        const double env = std::exp(-beat * (4.0 + k)) * (0.35 + 0.25 * std::sin(t * 0.7 + k));
        const double s = std::sin(2 * std::numbers::pi * f0 * t) +
                         0.4 * std::sin(2 * std::numbers::pi * f0 * 2.01 * t);
        a.channels[0][i] = static_cast<float>(env * s * 0.6);
        a.channels[1][i] = static_cast<float>(env * s * 0.6 * (0.6 + 0.4 * std::cos(t * 0.3 + k)));
    }
    return a;
}

std::string pattern(int track) {
    static const char* names[] = {"C", "D", "E", "F", "G", "A", "B"};
    std::string bar = "1:";
    for (int n = 0; n < 8; ++n) {
        const int step = (track * 3 + n * (n % 3 + 1)) % 14;
        bar += " " + std::string(names[step % 7]) + std::to_string(3 + step / 7) + ":e";
    }
    return "voice lead\n" + bar + "\n2-8 = 1\n";
}

std::filesystem::path generatedFile(const std::filesystem::path& mediaDir, int k) {
    return mediaDir / ("generated-" + std::to_string(k) + ".wav");
}

}  // namespace

Status writeGeneratedMedia(const std::filesystem::path& mediaDir) {
    std::error_code ec;
    std::filesystem::create_directories(mediaDir, ec);
    for (int k = 0; k < 8; ++k) {
        const auto path = generatedFile(mediaDir, k);
        if (std::filesystem::exists(path)) continue;
        auto wav = audio::encodeWav(variant(k));
        if (!wav) return std::move(wav).error();
        if (auto w = writeFileAtomic(path, std::span<const std::uint8_t>(*wav)); !w) return w;
    }
    return {};
}

Status generateSession(cmd::Dispatcher& dispatcher, const std::filesystem::path& mediaDir,
                       int tracks) {
    SUPPLE_TRY(writeGeneratedMedia(mediaDir));
    std::vector<std::filesystem::path> files;
    for (int k = 0; k < 8; ++k) files.push_back(generatedFile(mediaDir, k));
    auto run = [&](std::string command, json::Value args) -> Result<json::Value> {
        return dispatcher.execute(
            {std::move(command), std::move(args), {cmd::Origin::Type::Internal, "generate", {}}});
    };
    auto fromJson = [&](std::string_view text) {
        auto parsed = json::parse(text);
        return parsed ? *parsed : json::Value::object();
    };
    // One transaction: the whole generated session is one undo step (and one
    // invariant check, so a thousand tracks stay quick).
    auto body = [&]() -> Result<json::Value> {
        for (int i = 0; i < tracks; ++i) {
            json::Value add = json::Value::object();
            add["name"] = "Track " + std::to_string(i + 1);
            auto track = run("track.add", std::move(add));
            if (!track) return std::move(track).error();
            const std::string id = (*track)["track"].asString();
            json::Value audioItem = json::Value::object();
            audioItem["track"] = id;
            audioItem["bar"] = 1 + (i % 4);
            audioItem["audio_file"] = files[static_cast<std::size_t>(i) % files.size()].string();
            auto added = run("item.add", std::move(audioItem));
            if (!added) return std::move(added).error();
            const std::string audioId = (*added)["item"].asString();
            const std::string audioTake = (*added)["take"].asString();
            // Fades on every audio item; every third muted, every third a second
            // take, so the arrange view's item states are all exercised.
            json::Value fade = json::Value::object();
            fade["item"] = audioId;
            fade["fade_in"] = 0.5;
            fade["fade_out"] = 0.75;
            if (i % 3 == 1) fade["mute"] = true;
            if (auto r = run("item.set", std::move(fade)); !r) return std::move(r).error();
            if (i % 3 == 2) {
                json::Value take = json::Value::object();
                take["item"] = audioId;
                take["from"] = audioTake;
                if (auto r = run("take.add", std::move(take)); !r) return std::move(r).error();
            }
            // Locked last: a locked item refuses every edit, a new take among
            // them (the 18th track has both). The arrange view draws the padlock.
            if (i % 7 == 3) {
                json::Value lock = json::Value::object();
                lock["item"] = audioId;
                lock["locked"] = true;
                if (auto r = run("item.set", std::move(lock)); !r) return std::move(r).error();
            }
            json::Value midiItem = json::Value::object();
            midiItem["track"] = id;
            midiItem["bar"] = 21;
            midiItem["notation"] = pattern(i);
            if (auto r = run("item.add", std::move(midiItem)); !r) return std::move(r).error();
        }
        if (tracks > 0) {
            // Past the MIDI items (bar 21) so their times stay at the initial
            // tempo: a tempo point at bar 33, a meter change at bar 35, three
            // markers, and a region over the first 16 bars.
            auto r = run("tempo.set", fromJson(R"({"points":[{"beat":0,"bpm":120},)"
                                               R"({"beat":128,"bpm":140}]})"));
            if (!r) return std::move(r).error();
            if (auto m = run("timesig.set", fromJson(R"({"bar":35,"meter":"3/4"})")); !m)
                return std::move(m).error();
            for (const auto& [pos, name] : {std::pair{0, "Intro"}, {32, "Verse"}, {64, "Chorus"}}) {
                json::Value marker = json::Value::object();
                marker["pos"] = pos;
                marker["name"] = name;
                if (auto m = run("marker.add", std::move(marker)); !m) return std::move(m).error();
            }
            if (auto g =
                    run("region.add",
                        fromJson(R"({"start":0,"end":64,"name":"Section A","color":"#7fa3cf"})"));
                !g)
                return std::move(g).error();
        }
        return json::Value::object();
    };
    auto done = dispatcher.transact("generate session",
                                    {cmd::Origin::Type::Internal, "generate", {}}, body);
    if (!done) return done.error();
    return {};
}

}  // namespace supple::gui
