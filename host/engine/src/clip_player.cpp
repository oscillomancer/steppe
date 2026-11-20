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
#include "rack_runtime.h"
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
            // Past the memory budget: no player, so it cannot be launched,
            // and a track playing it returns to the arrangement (carryLaunch).
            if (skipped) continue;
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
    // Their take chains (compiled with the timeline's): the clip's and the
    // take's gain and pan apply after the chain, as an item's do (ADR-0097,
    // amended 2026-11-20). A chain without a player never plays.
    for (std::size_t k = 0; k < n.takeChains.size(); ++k) {
        TakeChain& tc = n.takeChains[k];
        if (!tc.clipChain) continue;
        tc.player = playerOf(n, tc.item);
        if (tc.player < 0) continue;
        n.players[static_cast<std::size_t>(tc.player)].chain = static_cast<int>(k);
        const model::Clip& clip = *s.clips.find(tc.item);
        const model::Take& take = *s.takes.find(tc.take);
        const double g =
            dbToGain(clip.gainDb) * dbToGain(take.gainDb) * (take.phaseInvert ? -1.0 : 1.0);
        const auto [l, r] = balanceSides(take.pan / 100.0);
        tc.gainL = g * l;
        tc.gainR = g * r;
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
    L.fadeAnchorBeat = L.anchorBeat;
    L.fadeStart = a.anchorSample;
    // Its early half (prelaunch) is done; a later launch's may already be
    // waiting, launches closer together than the lead.
    const LaunchAction& next = n.launchNext;
    if (n.launchNextValid && next.kind == a.kind && next.player == a.player &&
        next.anchorSample == a.anchorSample)
        n.launchNextValid = false;
    L.kind = a.kind;
    L.player = a.kind == LaunchState::Kind::Clip ? a.player : -1;
    L.anchorSample = a.anchorSample;
    L.anchorBeat = a.anchorBeat;
    L.unanchored = false;
    n.releaseHeld = true;  // what was left's notes end at this block's start
}

void carryLaunch(const Node& old, Node& n) SUPPLE_RT {
    LaunchState L = old.launch;
    // A retiring player belongs to the plan that kept it.
    if (L.fadeFrom == -3) L.fadeFrom = -2;
    // The playing audio clip, if the new plan keeps it only to fade out.
    int retiring = -1;
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0)
        for (std::size_t k = 0; k < n.retiring.size(); ++k)
            if (n.retiring[k].clip == old.players[static_cast<std::size_t>(L.player)].clip)
                retiring = static_cast<int>(k);
    const LaunchState before = L;
    const auto remap = [&](int player) {
        return player < 0 ? player
                          : playerOf(n, old.players[static_cast<std::size_t>(player)].clip);
    };
    if (L.kind == LaunchState::Kind::Clip) L.player = remap(L.player);
    if (L.fadeFrom >= 0) {
        L.fadeFrom = remap(L.fadeFrom);
        if (L.fadeFrom < 0) L.fadeFrom = -2;
    }
    const bool lost = (L.kind == LaunchState::Kind::Clip && L.player < 0) ||
                      (n.players.empty() && L.kind != LaunchState::Kind::Arrangement);
    if (lost) {
        L = LaunchState{};
        n.releaseHeld = true;  // the clip's notes end
        if (retiring >= 0) {   // its audio crossfades into the arrangement
            L.fadeFrom = -3;
            L.fadeRetiring = retiring;
            L.fadeStart = kFadeAtNext;
            L.fadeAnchorSample = before.anchorSample;
            L.fadeUnanchored = before.unanchored;
        }
    }
    n.launch = L;
    // A launch reaching it early (prelaunch), by clip id too.
    n.launchNext = old.launchNext;
    n.launchNextValid = old.launchNextValid && !n.players.empty();
    if (n.launchNextValid && n.launchNext.kind == LaunchState::Kind::Clip) {
        n.launchNext.player = remap(n.launchNext.player);
        n.launchNextValid = n.launchNext.player >= 0;
    }
    // A rack's Notes feeds, by module and clip: one whose source changed, or
    // whose clip the new plan lacks, follows the arrangement again.
    if (!n.rack || !old.rack) return;
    for (NotesFeed& feed : n.rack->notes)
        for (const NotesFeed& was : old.rack->notes) {
            if (was.id != feed.id) continue;
            if (was.source != feed.source || feed.players.empty()) break;
            LaunchState F = was.launch;
            if (F.kind == LaunchState::Kind::Clip && F.player >= 0) {
                F.player = playerOf(feed, was.players[static_cast<std::size_t>(F.player)].clip);
                if (F.player < 0) {
                    F = LaunchState{};
                    feed.releaseHeld = true;
                }
            }
            feed.launch = F;
            break;
        }
}

void launchWrapped(Node& n) SUPPLE_RT {
    LaunchState& L = n.launch;
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0 &&
        n.players[static_cast<std::size_t>(L.player)].once) {
        L.kind = LaunchState::Kind::Held;
        L.player = -1;
        L.fadeFrom = -2;
    }
    if (n.rack)
        for (NotesFeed& feed : n.rack->notes) {
            LaunchState& F = feed.launch;
            if (F.kind == LaunchState::Kind::Clip && F.player >= 0 &&
                feed.players[static_cast<std::size_t>(F.player)].once) {
                F.kind = LaunchState::Kind::Held;
                F.player = -1;
            }
        }
    launchRelocated(n);
}

void launchRelocated(Node& n) SUPPLE_RT {
    // The song jumped: a switch's crossfade, measured in song samples from
    // where it began, is over.
    n.launch.unanchored = true;
    n.launch.fadeFrom = -2;
    if (n.rack)
        for (NotesFeed& feed : n.rack->notes) feed.launch.unanchored = true;
}

namespace {

// An audio player's sample at song sample `t`, anchored at `anchor`, scaled
// by `weight`, added into `into` at frame i. Before the anchor it is silent
// unless `unanchored` (a wrap or relocation since its launch) lets a loop
// clip play there by its phase. `raw`: without the clip's gain and pan (a
// clip chain's input; they apply after the chain).
void playerFrame(const ClipPlayer& pl, std::int64_t anchor, bool unanchored, std::int64_t t,
                 double weight, Stereo& into, std::uint32_t i, bool raw = false) SUPPLE_RT {
    const Clip& c = pl.audio;
    if (!c.data || pl.muted || pl.periodFrames <= 0 || weight == 0.0) return;
    const std::int64_t q = t - anchor;
    if ((q < 0 && (pl.once || !unanchored)) || (pl.once && q >= pl.periodFrames)) return;
    const std::int64_t phase = (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
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
    into.l[i] += static_cast<float>(static_cast<double>(l) * (raw ? 1.0 : c.gainL) * weight);
    into.r[i] += static_cast<float>(static_cast<double>(r) * (raw ? 1.0 : c.gainR) * weight);
}

}  // namespace

void launchAudio(Plan&, Node& n, Stereo& into, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    n.arrangementGain = nullptr;
    const std::int64_t song0 = pos - n.timelineShift;
    if (n.launch.fadeFrom == -3 && n.launch.fadeStart == kFadeAtNext) n.launch.fadeStart = song0;
    // A frozen node fades its print in after writing it (retiringIntoPrint).
    if (n.players.empty() && (n.launch.fadeFrom != -3 || n.frozenAudio)) return;
    const LaunchState& L = n.launch;
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
    // (A player with a take chain plays through it: processTake.)
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
        if (!pl.midi && pl.chain < 0)
            for (std::uint32_t i = 0; i < frames; ++i)
                playerFrame(pl, L.anchorSample, L.unanchored, song0 + i, x(i), into, i);
    }
    if (fading && L.fadeFrom >= 0) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.fadeFrom)];
        if (!pl.midi && pl.chain < 0)
            for (std::uint32_t i = 0; i < frames; ++i)
                playerFrame(pl, L.fadeAnchorSample, false, song0 + i, 1.0 - x(i), into, i);
    }
    if (fading && L.fadeFrom == -3) {
        const ClipPlayer& pl = n.retiring[static_cast<std::size_t>(L.fadeRetiring)];
        for (std::uint32_t i = 0; i < frames; ++i)
            playerFrame(pl, L.fadeAnchorSample, L.fadeUnanchored, song0 + i, 1.0 - x(i), into, i);
    }
}

void retiringIntoPrint(Node& n, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (L.fadeFrom != -3 || L.fadeStart == kFadeAtNext) return;
    const std::int64_t song0 = pos - n.timelineShift;
    const std::int64_t F = n.launchFadeFrames;
    if (F <= 0 || song0 >= L.fadeStart + F) return;
    const ClipPlayer& pl = n.retiring[static_cast<std::size_t>(L.fadeRetiring)];
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double x = std::clamp(double(song0 + i - L.fadeStart) / double(F), 0.0, 1.0);
        n.sum.l[i] = static_cast<float>(double(n.sum.l[i]) * x);
        n.sum.r[i] = static_cast<float>(double(n.sum.r[i]) * x);
        // Where the clip was heard: through the chain the print replaces.
        playerFrame(pl, L.fadeAnchorSample, L.fadeUnanchored, song0 + i - n.latChain, 1.0 - x,
                    n.sum, i);
    }
}

void addRetiring(Plan& to, const Plan& from) {
    for (Node& n : to.nodes) {
        const Node* old = nullptr;
        for (const Node& o : from.nodes)
            if (o.id == n.id) old = &o;
        if (!old) continue;
        for (const ClipPlayer& pl : old->players) {
            if (pl.midi || pl.chain >= 0 || !pl.audio.data || playerOf(n, pl.clip) >= 0) continue;
            ClipPlayer r;
            r.clip = pl.clip;
            r.midi = false;
            r.once = pl.once;
            r.muted = pl.muted;
            r.audio.data = pl.audio.data;
            r.audio.offset = pl.audio.offset;
            r.audio.mode = pl.audio.mode;
            r.audio.gainL = pl.audio.gainL;
            r.audio.gainR = pl.audio.gainR;
            r.periodFrames = pl.periodFrames;
            r.loopStartFrames = pl.loopStartFrames;
            r.loopFrames = pl.loopFrames;
            r.loopOn = pl.loopOn;
            n.retiring.push_back(std::move(r));
        }
        // A node left without players has no launch buffers of its own.
        if (!n.retiring.empty() && n.players.empty()) {
            n.launchFadeFrames =
                std::max<std::int64_t>(1, time::samplesAt(kLaunchFadeSeconds, to.rate));
            n.arrangementGains.assign(to.block, 1.0f);
            n.launchBuf.init(to.block);
        }
    }
}

namespace {

// A MIDI player's events in the song frames [song0, song0 + frames) under
// launch state L, each with its frame in `time`, into `target` (within its
// capacity: never allocates).
void clipMidi(const Plan& p, const ClipPlayer& pl, const LaunchState& L, std::int64_t song0,
              std::uint32_t frames, std::vector<host::Event>& target) SUPPLE_RT {
    if (!pl.midi || pl.muted || pl.events.empty() || !(pl.periodBeats > 0)) return;
    const time::TimeMap& tm = *p.timeMap;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const std::int64_t song1 = song0 + frames;
    const double b0 = tm.beatAt(double(song0 + origin) / p.rate);
    const double b1 = tm.beatAt(double(song1 + origin) / p.rate);
    const double period = pl.periodBeats;
    auto kLo = static_cast<std::int64_t>(std::floor((b0 - L.anchorBeat) / period)) - 1;
    auto kHi = static_cast<std::int64_t>(std::floor((b1 - L.anchorBeat) / period)) + 1;
    // Nothing before the anchor, unless a wrap or relocation since the launch
    // took the song there: a loop clip then plays by its phase (L-c).
    const bool before = L.unanchored && !pl.once;
    if (!before) kLo = std::max<std::int64_t>(kLo, 0);
    if (pl.once) kHi = std::min<std::int64_t>(kHi, 0);
    constexpr double kSlack = 1e-6;  // beats: the sample decides
    for (std::int64_t k = kLo; k <= kHi; ++k) {
        const double base = L.anchorBeat + double(k) * period;
        const double lo = b0 - base - kSlack;
        auto it = std::lower_bound(pl.events.begin(), pl.events.end(), lo,
                                   [](const ClipEvent& e, double b) { return e.beat < b; });
        for (; it != pl.events.end() && it->beat <= b1 - base + kSlack; ++it) {
            const std::int64_t at = time::samplesAt(tm.secondsAt(base + it->beat), p.rate) - origin;
            if (at < song0 || at >= song1 || (!before && at < L.anchorSample)) continue;
            host::Event e = it->event;
            e.time = static_cast<std::uint32_t>(at - song0);
            if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
        }
    }
}

}  // namespace

void launchMidi(Plan& p, Node& n, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (L.kind != LaunchState::Kind::Clip || L.player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
    // An instrument in its take chain plays its notes there (clipChainMidi).
    if (pl.chain >= 0 && n.takeChains[static_cast<std::size_t>(pl.chain)].noteTarget >= 0) return;
    clipMidi(p, pl, L, pos - n.timelineShift, frames, n.midiStage.blockEvents);
}

// --- Clip chains (ADR-0097, amended 2026-11-20) ---------------------------------

void prelaunch(Node& n, const LaunchAction& a) SUPPLE_RT {
    n.launchNext = a;
    n.launchNextValid = true;
}

ClipChainInput clipChainInput(const Node& n, int player) SUPPLE_RT {
    ClipChainInput in;
    if (player < 0) return in;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    const LaunchState& L = n.launch;
    const bool current = L.kind == LaunchState::Kind::Clip && L.player == player;
    if (n.launchNextValid) {
        const LaunchAction& a = n.launchNext;
        // Coming in (or relaunched): from its new anchor.
        if (a.kind == LaunchState::Kind::Clip && a.player == player)
            return {true, a.anchorSample, a.anchorBeat, false};
        // Going out: its notes end where the switch is heard; its audio reads
        // on through the switch's crossfade, as launchAudio's does.
        if (pl.midi) return in;
    }
    if (current) return {true, L.anchorSample, L.anchorBeat, L.unanchored};
    if (!pl.midi && L.fadeFrom == player)
        return {true, L.fadeAnchorSample, L.fadeAnchorBeat, false};
    return in;
}

bool clipChainHeard(const Node& n, int player, std::int64_t song) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (player < 0) return false;
    if (L.kind == LaunchState::Kind::Clip && L.player == player) return true;
    return L.fadeFrom == player && song < L.fadeStart + n.launchFadeFrames;
}

double launchWeight(const Node& n, int player, std::int64_t song) SUPPLE_RT {
    const LaunchState& L = n.launch;
    const std::int64_t F = n.launchFadeFrames;
    const bool fading = L.fadeFrom != -2 && song < L.fadeStart + F;
    const double x = fading ? std::clamp(double(song - L.fadeStart) / double(F), 0.0, 1.0) : 1.0;
    double w = 0;
    if (L.kind == LaunchState::Kind::Clip && L.player == player) w += x;
    if (fading && L.fadeFrom == player) w += 1.0 - x;
    return w;
}

namespace {

// The input side of output frame `i`: `lead` ahead in the timeline, folded
// over the transport loop the output has yet to wrap (true when it did).
bool foldInput(const LoopRange* loop, std::int64_t pos, std::int64_t lead, std::uint32_t i,
               std::int64_t& t) SUPPLE_RT {
    t = pos + i + lead;
    if (!loop || !loop->active() || pos + i >= loop->end || t < loop->end) return false;
    t = loop->start + (t - loop->start) % (loop->end - loop->start);
    return true;
}

}  // namespace

void clipChainAudio(const Node& n, int player, const ClipChainInput& in, std::int64_t pos,
                    std::int64_t lead, const LoopRange* loop, std::uint32_t frames,
                    Stereo& into) SUPPLE_RT {
    if (!in.on || player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::int64_t t = 0;
        const bool folded = foldInput(loop, pos, lead, i, t);
        playerFrame(pl, in.anchorSample, in.unanchored || folded, t - n.timelineShift, 1.0, into, i,
                    true);
    }
}

void clipChainMidi(const Plan& p, const Node& n, int player, const ClipChainInput& in,
                   std::int64_t pos, std::int64_t lead, const LoopRange* loop, std::uint32_t frames,
                   Stage& target) SUPPLE_RT {
    if (!in.on || player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    auto& events = target.blockEvents;
    LaunchState L;
    L.kind = LaunchState::Kind::Clip;
    L.player = player;
    L.anchorSample = in.anchorSample;
    L.anchorBeat = in.anchorBeat;
    L.unanchored = in.unanchored;
    // Where the input folds over the loop within this block (frames: none).
    std::uint32_t fold = frames;
    if (loop && loop->active() && pos < loop->end && pos + frames + lead > loop->end)
        fold = static_cast<std::uint32_t>(std::max<std::int64_t>(0, loop->end - lead - pos));
    const auto window = [&](std::uint32_t from, std::uint32_t to, bool folded) SUPPLE_RT {
        if (to <= from) return;
        std::int64_t t = 0;
        foldInput(loop, pos, lead, from, t);
        L.unanchored = in.unanchored || folded;
        const std::size_t before = events.size();
        clipMidi(p, pl, L, t - n.timelineShift, to - from, events);
        for (std::size_t k = before; k < events.size(); ++k) events[k].time += from;
    };
    window(0, fold, false);
    if (fold >= frames) return;
    // The fold: what the input holds ends there (its compiled ids, resolved
    // by prepareNoteIds against what this block started), as a chain running
    // ahead ends it at its early wrap (unityTakeEvents).
    auto& held = target.pendingVoices;  // scratch (prepareNoteIds refills it)
    held.clear();
    for (const HeldVoice& v : target.voices)
        if (!v.live && v.sourceId >= 0 && held.size() < held.capacity()) held.push_back(v);
    for (const host::Event& e : events) {
        if (e.kind == host::Event::Kind::NoteOn && held.size() < held.capacity()) {
            HeldVoice v;
            v.sourceId = e.noteId;
            v.port = e.port;
            v.channel = e.channel;
            v.key = e.key;
            held.push_back(v);
        } else if (e.kind == host::Event::Kind::NoteOff || e.kind == host::Event::Kind::NoteChoke) {
            std::erase_if(held, [&](const HeldVoice& v) { return v.sourceId == e.noteId; });
        }
    }
    for (const HeldVoice& v : held) {
        if (events.size() == events.capacity()) {
            ++target.voiceDropped;
            continue;
        }
        host::Event off;
        off.kind = host::Event::Kind::NoteOff;
        off.time = fold;
        off.noteId = v.sourceId;
        off.port = v.port;
        off.channel = v.channel;
        off.key = v.key;
        events.push_back(off);
    }
    window(fold, frames, true);
}

bool clipChainTicks(const Plan& p, const Node& n, int player, std::int64_t song,
                    std::int64_t& ticks) SUPPLE_RT {
    if (player < 0) return false;
    const LaunchState& L = n.launch;
    std::int64_t anchorSample = 0;
    double anchorBeat = 0;
    if (L.kind == LaunchState::Kind::Clip && L.player == player) {
        anchorSample = L.anchorSample;
        anchorBeat = L.anchorBeat;
    } else if (L.fadeFrom == player) {
        anchorSample = L.fadeAnchorSample;
        anchorBeat = L.fadeAnchorBeat;
    } else {
        return false;
    }
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    if (!pl.midi) {  // the phase in its period, as playerFrame reads it
        if (pl.periodFrames <= 0) return false;
        const std::int64_t q = song - anchorSample;
        ticks = (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
        return true;
    }
    if (!(pl.periodBeats > 0)) return false;
    const time::TimeMap& tm = *p.timeMap;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const double beat = tm.beatAt(double(song + origin) / p.rate);
    const double base =
        anchorBeat + std::floor((beat - anchorBeat) / pl.periodBeats) * pl.periodBeats;
    ticks = std::llround((beat - base) * kClipBeatTicks);
    return true;
}

bool clipLanePosition(const ClipLaneSource& c, std::uint32_t frame, std::int64_t& at) SUPPLE_RT {
    if (!c.in.on || c.player < 0) return false;
    const Node& n = *c.node;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(c.player)];
    std::int64_t t = 0;
    const bool folded = foldInput(c.loop, c.pos, c.ahead, frame, t);
    const std::int64_t song = t - n.timelineShift;
    const bool before = (c.in.unanchored || folded) && !pl.once;
    if (!pl.midi) {
        if (pl.periodFrames <= 0) return false;
        const std::int64_t q = song - c.in.anchorSample;
        if (q < 0 && !before) return false;
        at = (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
        return true;
    }
    if (!(pl.periodBeats > 0)) return false;
    const Plan& p = *c.plan;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const double beat = p.timeMap->beatAt(double(song + origin) / p.rate);
    if (beat < c.in.anchorBeat && !before) return false;
    const double base =
        c.in.anchorBeat + std::floor((beat - c.in.anchorBeat) / pl.periodBeats) * pl.periodBeats;
    at = std::llround((beat - base) * kClipBeatTicks);
    return true;
}

// --- A rack's Notes feed (rack_runtime.h): its source track's launches ----------

void compileFeedClips(Plan& p, NotesFeed& feed, const Session& s) {
    feed.players.clear();
    feed.launch = {};
    feed.follows = false;
    if (!p.clipPlayers || p.rateEnabled || !feed.source) return;
    const model::Track* track = s.tracks.find(*feed.source);
    if (!track || track->type != model::StripType::Channel || track->freeze) return;
    // The same tracks as compileClipPlayers: a channel with clips, not frozen.
    feed.follows = std::any_of(s.clips.begin(), s.clips.end(),
                               [&](const auto& e) { return e.value->track == *feed.source; });
    if (!feed.follows) return;
    std::int32_t nextId = 0;
    for (const TimedEvent& e : feed.events) nextId = std::max(nextId, e.event.noteId + 1);
    std::size_t voices = 0, window = 0;
    const time::TimeMap& tm = *s.timeMap;
    double fastest = 0;
    for (const auto& t : tm.tempo()) fastest = std::max(fastest, t.bpm);
    const double blockBeats = double(p.block) / p.rate * fastest / 60.0;
    for (const auto& e : s.clips) {
        const model::Clip& clip = *e.value;
        if (clip.track != *feed.source || clip.kind != model::ClipKind::Midi) continue;
        ClipPlayer pl;
        pl.clip = e.id;
        pl.once = clip.mode == model::ClipMode::Once;
        pl.muted = clip.mute;
        pl.periodBeats = clip.length;
        const model::Take* take = clip.activeTake ? s.takes.find(*clip.activeTake) : nullptr;
        const model::MidiSource* src =
            take && take->source && take->source->kind == model::Kind::Midi
                ? s.midi.find(*take->source)
                : nullptr;
        if (src) midiEvents(pl, clip, e.id, *take, *src, nextId);
        const auto [v, w] = midiSizing(pl, blockBeats);
        voices = std::max(voices, v);
        window = std::max(window, w);
        feed.players.push_back(std::move(pl));
    }
    feed.clipEvents.reserve(window + 1);
    feed.block.reserve(feed.block.capacity() + window + 1);
}

int playerOf(const NotesFeed& feed, Id clip) SUPPLE_RT {
    for (std::size_t i = 0; i < feed.players.size(); ++i)
        if (feed.players[i].clip == clip) return static_cast<int>(i);
    return -1;
}

void applyFeedLaunch(NotesFeed& feed, LaunchState::Kind kind, Id clip, std::int64_t anchorSample,
                     double anchorBeat) SUPPLE_RT {
    LaunchState& L = feed.launch;
    L.kind = kind;
    L.player = kind == LaunchState::Kind::Clip ? playerOf(feed, clip) : -1;  // -1: audio
    L.anchorSample = anchorSample;
    L.anchorBeat = anchorBeat;
    L.unanchored = false;
    L.fadeFrom = -2;
    feed.releaseHeld = true;  // what the track source holds ends at the switch
}

void feedClipMidi(Plan& p, const Node& rack, NotesFeed& feed, std::int64_t pos,
                  std::uint32_t frames) SUPPLE_RT {
    feed.clipEvents.clear();
    const LaunchState& L = feed.launch;
    if (L.kind != LaunchState::Kind::Clip || L.player < 0) return;
    clipMidi(p, feed.players[static_cast<std::size_t>(L.player)], L, pos - rack.timelineShift,
             frames, feed.clipEvents);
}

}  // namespace supple::engine::detail
