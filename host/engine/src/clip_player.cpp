// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players (ADR-0097, engine.md §1.4.2): a channel's
// clips compiled into its node, what it plays as transport state, and the
// switch between them. A clip's phase is a pure function of the song
// position and its anchor, so both executors and the offline replay play
// the same samples.
#include <algorithm>
#include <cmath>

#include "pan.h"
#include "plan.h"
#include "supple/model/source_mapping.h"

namespace supple::engine::detail {

namespace {

// A clip note's origin is flagged so that it never shares an item's
// address (an item and a clip may have the same number).
constexpr std::uint64_t kClipOrigin = std::uint64_t{1} << 63;

int rank(const host::Event& e) {
    return e.kind == host::Event::Kind::NoteOff ? 0 : e.kind == host::Event::Kind::NoteOn ? 2 : 1;
}

host::Event control(const midi::Control& c) {
    host::Event ev;
    ev.kind = host::Event::Kind::Midi;
    const auto ch = static_cast<std::uint8_t>(c.channel & 0x0f);
    const auto b7 = [](std::int32_t v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 127)); };
    switch (c.kind) {
        case midi::ControlKind::CC:
            ev.midi[0] = static_cast<std::uint8_t>(0xb0 | ch);
            ev.midi[1] = b7(c.number);
            ev.midi[2] = b7(c.value);
            break;
        case midi::ControlKind::ProgramChange:
            ev.midi[0] = static_cast<std::uint8_t>(0xc0 | ch);
            ev.midi[1] = b7(c.value);
            break;
        case midi::ControlKind::ChannelPressure:
            ev.midi[0] = static_cast<std::uint8_t>(0xd0 | ch);
            ev.midi[1] = b7(c.value);
            break;
        case midi::ControlKind::PolyPressure:
            ev.midi[0] = static_cast<std::uint8_t>(0xa0 | ch);
            ev.midi[1] = b7(c.number);
            ev.midi[2] = b7(c.value);
            break;
        case midi::ControlKind::PitchBend: {
            const std::int32_t v = std::clamp(c.value, -8192, 8191) + 8192;
            ev.midi[0] = static_cast<std::uint8_t>(0xe0 | ch);
            ev.midi[1] = static_cast<std::uint8_t>(v & 0x7f);
            ev.midi[2] = static_cast<std::uint8_t>((v >> 7) & 0x7f);
            break;
        }
    }
    return ev;
}

// A MIDI clip's events over its period: each pass of the loop window (or the
// content from the take's start), notes cut at the window's and the
// period's end, sorted by beat with offs before controls before ons.
void midiEvents(ClipPlayer& pl, const model::Clip& clip, Id clipId, const model::Take& take,
                const model::MidiSource& src, std::int32_t& nextId) {
    const midi::Sequence& seq = src.sequence;
    if (seq.ppq <= 0 || !(clip.length > 0)) return;
    const model::SourceBeatMapping mapping{take.startOffset,
                                           take.playrate > 0 ? take.playrate : 1.0, 0};
    const double period = clip.length;
    const bool looping = clip.loop.on && clip.loop.length > 0;
    const double from = looping ? clip.loop.start : 0.0;
    const double window = looping ? clip.loop.length : period;
    const int passes = looping ? static_cast<int>(std::ceil(period / window - 1e-9)) : 1;
    for (int r = 0; r < passes; ++r) {
        const double base = r * window;
        for (const midi::Note& n : seq.notes) {
            const double on = mapping.itemBeat(static_cast<double>(n.start) / seq.ppq);
            if (on < from || on >= from + window) continue;
            const double at = base + on - from;
            if (at >= period) continue;
            const double offContent = std::min(
                mapping.itemBeat(static_cast<double>(n.start + n.length) / seq.ppq), from + window);
            const double off = std::min(base + offContent - from, period);
            if (!(off - at > 1e-9)) continue;
            ClipEvent a;
            a.beat = at;
            a.origin = {kClipOrigin | clipId.n, clip.activeTake->n, take.source->n, n.start, r};
            a.event.kind = host::Event::Kind::NoteOn;
            a.event.channel = n.channel;
            a.event.key = n.pitch;
            a.event.noteId = nextId++;
            a.event.velocity = n.velocity / 127.0;
            pl.events.push_back(a);
            ClipEvent b = a;
            b.beat = off;
            b.event.kind = host::Event::Kind::NoteOff;
            b.event.velocity = n.offVelocity / 127.0;
            pl.events.push_back(b);
        }
        for (const midi::Control& c : seq.controls) {
            const double t = mapping.itemBeat(static_cast<double>(c.tick) / seq.ppq);
            if (t < from || t >= from + window) continue;
            const double at = base + t - from;
            if (at >= period) continue;
            ClipEvent ev;
            ev.beat = at;
            ev.event = control(c);
            pl.events.push_back(ev);
        }
    }
    std::stable_sort(
        pl.events.begin(), pl.events.end(), [](const ClipEvent& a, const ClipEvent& b) {
            return a.beat < b.beat || (a.beat == b.beat && rank(a.event) < rank(b.event));
        });
}

// Sizing for prepareRackMidi: the most notes sounding at once, and the most
// events within `beats` of each other, the period repeating.
std::pair<std::size_t, std::size_t> midiSizing(const ClipPlayer& pl, double beats) {
    std::size_t held = 0, most = 0;
    for (const ClipEvent& e : pl.events) {
        if (e.event.kind == host::Event::Kind::NoteOn)
            most = std::max(most, ++held);
        else if (e.event.kind == host::Event::Kind::NoteOff && held)
            --held;
    }
    const std::size_t count = pl.events.size();
    if (count == 0 || !(pl.periodBeats > 0)) return {most, 0};
    if (beats >= pl.periodBeats)
        return {most, count * (static_cast<std::size_t>(beats / pl.periodBeats) + 2)};
    std::size_t window = 0;
    for (std::size_t a = 0, b = 0; a < count; ++a) {
        b = std::max(a, b);
        while (b < a + count && pl.events[b % count].beat + (b >= count ? pl.periodBeats : 0.0) <=
                                    pl.events[a].beat + beats)
            ++b;
        window = std::max(window, b - a);
    }
    return {most, window + 1};
}

}  // namespace

void compileClipPlayers(Plan& p, Node& n, const Session& s) {
    n.players.clear();
    n.clipVoices = n.clipWindow = 0;
    const model::Track* track = s.tracks.find(n.id);
    if (n.rack || !track || track->type != model::StripType::Channel) return;
    std::vector<Id> clips;
    for (const auto& e : s.clips)
        if (e.value->track == n.id) clips.push_back(e.id);
    if (clips.empty() || track->freeze) return;  // a frozen track plays its print
    if (p.rateEnabled) {
        // Staged (engine.md §1.4.2): clip audio is not yet on the playrate path.
        p.warnings.push_back(n.id.str() +
                             ": launch.playrate: its clips cannot play while a master playrate "
                             "is active; set the playrate to 1 to launch them");
        return;
    }
    std::int32_t nextId = 0;
    for (const TimedEvent& e : n.events) nextId = std::max(nextId, e.event.noteId + 1);
    const time::TimeMap& tm = *s.timeMap;
    double fastest = 0;
    for (const auto& t : tm.tempo()) fastest = std::max(fastest, t.bpm);
    const double blockBeats = double(p.block) / p.rate * fastest / 60.0;
    const auto& prepared = p.preparedSources;
    for (const Id id : clips) {
        const model::Clip& clip = *s.clips.find(id);
        ClipPlayer pl;
        pl.clip = id;
        pl.midi = clip.kind == model::ClipKind::Midi;
        pl.once = clip.mode == model::ClipMode::Once;
        pl.muted = clip.mute;
        const model::Take* take = clip.activeTake ? s.takes.find(*clip.activeTake) : nullptr;
        if (take && (!take->chain.empty() ||
                     std::any_of(s.envelopes.begin(), s.envelopes.end(), [&](const auto& e) {
                         return e.value->target.object == *clip.activeTake;
                     })))
            p.warnings.push_back(id.str() +
                                 ": launch.take_chain: its take's FX and envelopes do not play "
                                 "in the clip launcher yet; lay the clip on the timeline to hear "
                                 "them");
        if (pl.midi) {
            pl.periodBeats = clip.length;
            const model::MidiSource* src =
                take && take->source && take->source->kind == model::Kind::Midi
                    ? s.midi.find(*take->source)
                    : nullptr;
            if (src) midiEvents(pl, clip, id, *take, *src, nextId);
            const auto [voices, window] = midiSizing(pl, blockBeats);
            n.clipVoices = std::max(n.clipVoices, voices);
            n.clipWindow = std::max(n.clipWindow, window);
        } else {
            pl.periodFrames = time::samplesAt(clip.length, p.rate);
            pl.loopOn = clip.loop.on && clip.loop.length > 0;
            pl.loopStartFrames = time::samplesAt(clip.loop.start, p.rate);
            pl.loopFrames = time::samplesAt(clip.loop.length, p.rate);
            if (pl.loopOn && pl.loopFrames <= 0) pl.loopOn = false;
            const bool skipped =
                prepared && std::find(prepared->clipsSkipped.begin(), prepared->clipsSkipped.end(),
                                      id) != prepared->clipsSkipped.end();
            const auto found = prepared && clip.activeTake
                                   ? prepared->takes.find(*clip.activeTake)
                                   : std::map<Id, PreparedSource>::const_iterator{};
            if (take && take->source && !skipped) {
                if (!prepared || found == prepared->takes.end() || !found->second.data) {
                    p.warnings.push_back(
                        id.str() + ": " +
                        (prepared && found != prepared->takes.end() && found->second.error
                             ? found->second.error->message
                             : "source audio is not prepared"));
                } else {
                    MediaVariant variant;
                    variant.playrate = take->playrate > 0.0 ? take->playrate : 1.0;
                    variant.pitch = take->pitch;
                    variant.preservePitch = take->preservePitch;
                    variant.pitchMode = take->pitchMode;
                    Clip& c = pl.audio;
                    c.item = id;
                    c.take = *clip.activeTake;
                    c.data = found->second.data;
                    c.offset = time::samplesAt(take->startOffset, p.rate / variant.speed());
                    c.mode = take->channelMode;
                    const double g = dbToGain(clip.gainDb) * dbToGain(take->gainDb) *
                                     (take->phaseInvert ? -1.0 : 1.0);
                    const auto [pl_, pr] = balanceSides(take->pan / 100.0);
                    c.gainL = g * pl_;
                    c.gainR = g * pr;
                }
            }
            // A period that is not whole beats drifts from the bar grid.
            const double beats = clip.length * tm.bpmAt(0) / 60.0;
            if (std::abs(beats - std::round(beats)) > 0.01)
                p.warnings.push_back(id.str() + ": launch.tempo: its " +
                                     json::formatDouble(clip.length) + " s are " +
                                     json::formatDouble(std::round(beats * 100) / 100) +
                                     " beats at the project's tempo, so it drifts from the bars");
        }
        n.players.push_back(std::move(pl));
    }
    n.launchFadeFrames = std::max<std::int64_t>(1, time::samplesAt(kLaunchFadeSeconds, p.rate));
    n.arrangementGains.assign(p.block, 1.0f);
    n.launchBuf.init(p.block);
}

int playerOf(const Node& n, Id clip) SUPPLE_RT {
    for (std::size_t i = 0; i < n.players.size(); ++i)
        if (n.players[i].clip == clip) return static_cast<int>(i);
    return -1;
}

void applyLaunch(Plan&, Node& n, const LaunchAction& a) SUPPLE_RT {
    LaunchState& L = n.launch;
    L.fadeFrom = L.kind == LaunchState::Kind::Arrangement ? -1
                 : L.kind == LaunchState::Kind::Clip      ? L.player
                                                          : -2;
    L.fadeAnchorSample = L.anchorSample;
    L.fadeStart = a.anchorSample;
    L.kind = a.kind;
    L.player = a.kind == LaunchState::Kind::Clip ? a.player : -1;
    L.anchorSample = a.anchorSample;
    L.anchorBeat = a.anchorBeat;
    n.releaseHeld = true;  // what was left's notes end at this block's start
}

void launchWrapped(Node& n) SUPPLE_RT {
    LaunchState& L = n.launch;
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0 &&
        n.players[static_cast<std::size_t>(L.player)].once) {
        L.kind = LaunchState::Kind::Held;
        L.player = -1;
        L.fadeFrom = -2;
    }
}

namespace {

// An audio player's sample at song sample `t`, anchored at `anchor`, scaled
// by `weight`, added into `into` at frame i.
void playerFrame(const ClipPlayer& pl, std::int64_t anchor, std::int64_t t, double weight,
                 Stereo& into, std::uint32_t i) SUPPLE_RT {
    const Clip& c = pl.audio;
    if (!c.data || pl.muted || pl.periodFrames <= 0 || weight == 0.0) return;
    const std::int64_t q = t - anchor;
    if (q < 0 || (pl.once && q >= pl.periodFrames)) return;
    const std::int64_t phase = q % pl.periodFrames;
    const std::int64_t content = pl.loopOn ? pl.loopStartFrames + phase % pl.loopFrames : phase;
    const auto& ch = c.data->channels;
    const auto frames = static_cast<std::int64_t>(c.data->frames());
    const std::int64_t f = c.offset + content;
    if (ch.empty() || f < 0 || f >= frames) return;
    const auto fi = static_cast<std::size_t>(f);
    const std::size_t right = ch.size() > 1 ? 1 : 0;
    float l = ch[0][fi], r = ch[right][fi];
    switch (c.mode) {
        case model::ChannelMode::Normal:
            break;
        case model::ChannelMode::ReverseStereo:
            std::swap(l, r);
            break;
        case model::ChannelMode::MonoMix:
            l = r = 0.5f * (l + r);
            break;
        case model::ChannelMode::Left:
            r = l;
            break;
        case model::ChannelMode::Right:
            l = r;
            break;
    }
    into.l[i] += static_cast<float>(static_cast<double>(l) * c.gainL * weight);
    into.r[i] += static_cast<float>(static_cast<double>(r) * c.gainR * weight);
}

}  // namespace

void launchAudio(Plan&, Node& n, Stereo& into, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    n.arrangementGain = nullptr;
    if (n.players.empty()) return;
    const LaunchState& L = n.launch;
    const std::int64_t song0 = pos - n.timelineShift;
    const std::int64_t F = n.launchFadeFrames;
    const bool fading = L.fadeFrom != -2 && song0 < L.fadeStart + F;
    const bool arrangement = L.kind == LaunchState::Kind::Arrangement;
    if (arrangement && !fading) return;  // the arrangement alone: nothing changes
    // x: how far into the crossfade frame i is, 0 to 1 (1 past it).
    const auto x = [&](std::uint32_t i) {
        if (!fading) return 1.0;
        const double k = double(song0 + i - L.fadeStart) / double(F);
        return std::clamp(k, 0.0, 1.0);
    };
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double in = x(i);
        n.arrangementGains[i] = static_cast<float>((arrangement ? in : 0.0) +
                                                   (fading && L.fadeFrom == -1 ? 1.0 - in : 0.0));
    }
    n.arrangementGain = n.arrangementGains.data();
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
        if (!pl.midi)
            for (std::uint32_t i = 0; i < frames; ++i)
                playerFrame(pl, L.anchorSample, song0 + i, x(i), into, i);
    }
    if (fading && L.fadeFrom >= 0) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.fadeFrom)];
        if (!pl.midi)
            for (std::uint32_t i = 0; i < frames; ++i)
                playerFrame(pl, L.fadeAnchorSample, song0 + i, 1.0 - x(i), into, i);
    }
}

void launchMidi(Plan& p, Node& n, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (L.kind != LaunchState::Kind::Clip || L.player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
    if (!pl.midi || pl.muted || pl.events.empty() || !(pl.periodBeats > 0)) return;
    const time::TimeMap& tm = *p.timeMap;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const std::int64_t song0 = pos - n.timelineShift, song1 = song0 + frames;
    const double b0 = tm.beatAt(double(song0 + origin) / p.rate);
    const double b1 = tm.beatAt(double(song1 + origin) / p.rate);
    const double period = pl.periodBeats;
    auto kLo = static_cast<std::int64_t>(std::floor((b0 - L.anchorBeat) / period)) - 1;
    auto kHi = static_cast<std::int64_t>(std::floor((b1 - L.anchorBeat) / period)) + 1;
    kLo = std::max<std::int64_t>(kLo, 0);  // nothing before the anchor
    if (pl.once) kHi = std::min<std::int64_t>(kHi, 0);
    auto& target = n.midiStage.blockEvents;
    constexpr double kSlack = 1e-6;  // beats: the sample decides
    for (std::int64_t k = kLo; k <= kHi; ++k) {
        const double base = L.anchorBeat + double(k) * period;
        const double lo = b0 - base - kSlack;
        auto it = std::lower_bound(pl.events.begin(), pl.events.end(), lo,
                                   [](const ClipEvent& e, double b) { return e.beat < b; });
        for (; it != pl.events.end() && it->beat <= b1 - base + kSlack; ++it) {
            const std::int64_t at = time::samplesAt(tm.secondsAt(base + it->beat), p.rate) - origin;
            if (at < song0 || at >= song1 || at < L.anchorSample) continue;
            host::Event e = it->event;
            e.time = static_cast<std::uint32_t>(at - song0);
            if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
        }
    }
}

}  // namespace supple::engine::detail
