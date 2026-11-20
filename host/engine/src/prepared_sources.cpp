// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <map>
#include <set>
#include "stretch.h"
#include "supple/dsp/dsp.h"
#include "supple/engine/offline.h"
#include "supple/model/audio_warp.h"
#include "supple/model/schemas.h"
#include "supple/model/take_owner.h"

namespace supple::engine {
namespace {
std::filesystem::path sourcePath(const model::Session& s, const model::MediaSource& m,
                                 const std::filesystem::path& dir) {
    std::filesystem::path path = m.path;
    if (path.is_relative()) {
        const auto media = dir / s.project->mediaDir / path;
        std::error_code error;
        path = std::filesystem::exists(media, error) ? media : dir / path;
    }
    return path;
}
// The item holding a take, through each take's owner found once
// (model::TakeOwners) rather than by a search per take.
const model::Item* ownerOf(const model::Session& s, const model::TakeOwners& owners, model::Id id) {
    const auto owner = owners.of(id);
    return owner ? s.items.find(*owner) : nullptr;
}
}  // namespace
Result<std::string> sourcePreparationKey(const model::Session& s, const std::filesystem::path& dir,
                                         double rate) {
    json::Value key = json::Value::array();
    key.push_back(dir.generic_string());
    key.push_back(rate);
    const model::TakeOwners owners(s);
    for (const auto& e : s.takes) {
        const auto& t = *e.value;
        const auto* md = t.source ? s.media.find(*t.source) : nullptr;
        const auto* it = ownerOf(s, owners, e.id);
        if (!md || !it) continue;
        // Only source-affecting fields: gain/FX/mute and hit overlays do not
        // re-run preparation. File metadata catches unhashed external changes.
        auto take = t;
        take.transients.reset();
        take.chain.clear();
        take.takeMarkers.clear();
        take.name.clear();
        take.gainDb = take.pan = 0;
        take.phaseInvert = false;
        take.channelMode = model::ChannelMode::Normal;
        key.push_back(e.id.str());
        key.push_back(it->activeTake == e.id);
        key.push_back(model::toJson(take));
        key.push_back(model::toJson(*md));
        const auto span = model::itemSpan(s, *it);
        key.push_back(span.endSec - span.startSec);
        key.push_back(it->loop);
        if (md->hash.empty()) {
            const auto path = sourcePath(s, *md, dir);
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            key.push_back(ec ? "missing" : std::to_string(size));
            const auto stamp = std::filesystem::last_write_time(path, ec);
            key.push_back(
                ec ? "missing"
                   : std::to_string(static_cast<long long>(stamp.time_since_epoch().count())));
        }
    }
    return json::writeCanonical(key);
}
ResidentSamples residentSamples(const model::Session& s, const std::filesystem::path& dir,
                                double rate) {
    const model::TakeOwners owners(s);
    std::set<model::Id> needed;
    for (const auto& e : s.items)
        if (e.value->activeTake) needed.insert(*e.value->activeTake);
    for (auto id : std::vector<model::Id>(needed.begin(), needed.end()))
        if (const auto* t = s.takes.find(id))
            needed.insert(t->warpPeers.begin(), t->warpPeers.end());
    ResidentSamples out;
    std::set<std::string> decoded, variants;
    // One take's share. A warped, unlooped item's audio is rendered for its
    // span; anything else is a variant of the whole source.
    auto add = [&](const model::Take& take, const model::MediaSource& md, const model::Item* item,
                   double& total) {
        const MediaVariant variant{take.playrate, take.pitch, take.preservePitch, take.pitchMode};
        const auto identity = md.hash.empty() ? sourcePath(s, md, dir).generic_string() : md.hash;
        if (decoded.insert(identity).second) total += static_cast<double>(md.frames) * md.channels;
        if (item && model::activeWarp(take) && !item->loop) {
            const auto span = model::itemSpan(s, *item);
            total += std::ceil((span.endSec - span.startSec) * rate) * md.channels;
        } else {
            const bool stretch = variant.stretch();
            const auto variantKey =
                identity + (stretch ? "|s" : "|r") + json::formatDouble(variant.playrate) + "|" +
                json::formatDouble(stretch || !variant.preservePitch ? variant.pitch : 0.);
            if (variants.insert(variantKey).second)
                total += static_cast<double>(md.frames) * rate / md.sampleRate / variant.speed() *
                         md.channels;
        }
    };
    for (auto id : needed) {
        const auto* take = s.takes.find(id);
        const auto* md = take && take->source ? s.media.find(*take->source) : nullptr;
        const auto* item = ownerOf(s, owners, id);
        if (!md || !item || md->sampleRate <= 0) continue;
        add(*take, *md, item, out.arrangement);
    }
    for (const auto& e : s.clips) {
        const auto* take = e.value->activeTake ? s.takes.find(*e.value->activeTake) : nullptr;
        const auto* md = take && take->source ? s.media.find(*take->source) : nullptr;
        if (!md || md->sampleRate <= 0) continue;
        add(*take, *md, nullptr, out.clips);
    }
    return out;
}

Result<std::shared_ptr<const PreparedSources>> MediaCache::prepareSources(
    const model::Session& s, const std::filesystem::path& dir, double rate,
    const std::function<bool()>& cancelled) {
    dsp::DenormalGuard denormals;
    auto key = sourcePreparationKey(s, dir, rate);
    if (!key) return key.error();
    if (prepared_ && prepared_->key == *key) return prepared_;
    auto out = std::make_shared<PreparedSources>();
    out->key = *key;
    out->sampleRate = rate;
    out->session = s;
    const model::TakeOwners owners(s);
    std::set<model::Id> needed;
    for (const auto& e : s.items)
        if (e.value->activeTake) needed.insert(*e.value->activeTake);
    for (auto id : std::vector<model::Id>(needed.begin(), needed.end()))
        if (const auto* t = s.takes.find(id))
            needed.insert(t->warpPeers.begin(), t->warpPeers.end());
    const double estimate = residentSamples(s, dir, rate).arrangement;
    if (!(estimate >= 0 && estimate <= kPreparedSampleBudget))
        return Error("media.budget", "prepared sources exceed the in-memory sample budget", {},
                     "use shorter source files, fewer channels or a faster playback rate");
    for (auto id : needed) {
        if (out->takes.contains(id)) continue;
        if (cancelled && cancelled())
            return Error("job.cancelled", "source preparation cancelled", {},
                         "retry for the current version");
        const auto* t = s.takes.find(id);
        const auto* it = ownerOf(s, owners, id);
        const auto* md = t && t->source ? s.media.find(*t->source) : nullptr;
        if (!t || !md || !it) continue;
        if (!model::activeWarp(*t) || it->loop) {
            MediaVariant variant{t->playrate, t->pitch, t->preservePitch, t->pitchMode};
            auto data = get(sourcePath(s, *md, dir), rate, variant, md->hash);
            if (data)
                out->takes[id].data = *data;
            else
                out->takes[id].error = data.error();
            continue;
        }
        const auto span = model::itemSpan(s, *it);
        const double duration = span.endSec - span.startSec;
        const auto peers = t->warpPeers.empty() ? std::vector<model::Id>{id} : t->warpPeers;
        const auto* base = s.takes.find(peers.front());
        if (!base) return Error("warp.peers", "missing warp peer", {}, "detach the invalid bundle");
        std::vector<std::shared_ptr<const audio::AudioData>> inputs;
        std::size_t channels = 0;
        for (auto peerId : peers) {
            const auto* peer = s.takes.find(peerId);
            const auto* media = peer && peer->source ? s.media.find(*peer->source) : nullptr;
            if (!media)
                return Error("warp.peers", "warp peer has no audio source", {},
                             "choose audio takes");
            auto data = get(sourcePath(s, *media, dir), rate, {}, media->hash);
            if (!data) return data.error();
            channels += (*data)->channelCount();
            inputs.push_back(*data);
        }
        const double from = std::floor((model::audioSourceAt(*base, -1) - .5) * rate) / rate;
        const double end = model::audioSourceAt(*base, duration + 1) + .5;
        const double inputFrames = std::ceil((end - from) * rate);
        // Clip boundaries are rounded separately on the project timeline. Their
        // difference can be ceil(duration), so retain that last source frame.
        const double outputFrames = std::ceil(duration * rate);
        if (!channels || channels > 128 || inputFrames < 0 || outputFrames < 0 ||
            (inputFrames + outputFrames) * static_cast<double>(channels) > (1 << 28))
            return Error("warp.budget", "warp bundle exceeds the channel-frame budget", {},
                         "shorten the items or detach some channels");
        audio::AudioData packed;
        packed.sampleRate = rate;
        for (std::size_t p = 0; p < peers.size(); ++p) {
            const auto* peer = s.takes.find(peers[p]);
            const auto first = static_cast<std::int64_t>(
                std::llround((from + peer->startOffset - base->startOffset) * rate));
            for (const auto& channel : inputs[p]->channels) {
                auto& samples =
                    packed.channels.emplace_back(static_cast<std::size_t>(inputFrames), 0.f);
                for (std::size_t n = 0; n < samples.size(); ++n) {
                    const auto at = first + static_cast<std::int64_t>(n);
                    if (at >= 0 && static_cast<std::size_t>(at) < channel.size())
                        samples[n] = channel[static_cast<std::size_t>(at)];
                }
            }
        }
        auto map = *base;
        model::slipAudioTake(map, -from);
        auto warped = warpAudio(packed, map, static_cast<std::size_t>(outputFrames), cancelled);
        if (!warped) return warped.error();
        std::size_t channel = 0;
        for (std::size_t p = 0; p < peers.size(); ++p) {
            auto data = std::make_shared<audio::AudioData>();
            data->sampleRate = rate;
            for (std::size_t c = 0; c < inputs[p]->channelCount(); ++c)
                data->channels.push_back(std::move(warped->channels[channel++]));
            out->takes[peers[p]] = {std::move(data), true, {}};
        }
    }
    prepared_ = out;
    return prepared_;
}
}  // namespace supple::engine
