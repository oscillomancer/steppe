// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players (ADR-0097, engine.md §1.4.2): a channel's
// clips compiled into its node, what it plays as transport state, and the
// switch between them. A clip's phase is a pure function of the song
// position and its anchor, so both executors and the offline replay play
// the same samples.
#include <algorithm>
#include <atomic>
#include <cmath>

#include "clip_launcher.h"
#include "pan.h"
#include "plan.h"
#include "rack_runtime.h"
#include "supple/model/source_mapping.h"

namespace supple::engine::detail {

namespace {

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
            a.beat = a.start = at;
            a.origin = {kClipOrigin | clipId.n,
                        clip.activeTake->n,
                        take.source->n,
                        n.start,
                        r,
                        false,
                        static_cast<std::int16_t>(n.pitch)};
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
            ev.beat = ev.start = at;
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

// The tape table the launched players' readers share (PlayerReader).
const std::shared_ptr<const TapeTable>& launchTapeTable() {
    static const auto table = std::make_shared<const TapeTable>();
    return table;
}

// A reader on the playrate path (PlayerReader): its sources are read as
// stereo (playerFrame's channels), by the tape converter, or with the pitch
// preserved by a stretch converter (S3 step D).
void makeReader(PlayerReader& r, const Plan& p) {
    if (!r.state)
        r.state = std::make_unique<RateClipState>(launchTapeTable(), 2, p.rate, p.rateStretch);
    // Room for the previous plan's reading across a trajectory change (S3
    // step D4); its state comes from that plan at adoption.
    if (!r.previous) r.previous = std::make_unique<PlayerReader>();
}

// A following clip's tables (ADR-0098 X4): what placement would follow, by
// tables within the budget, else by its key, else as written. Its player and a
// Notes feed's player of it share them.
ClipFollowBuild followPlayer(Plan& p, const ClipPlayer& pl, const model::Clip& clip,
                             const Session& s) {
    const bool pitched = std::any_of(pl.events.begin(), pl.events.end(), [](const ClipEvent& e) {
        return e.event.kind == host::Event::Kind::NoteOn && e.event.channel != 9;
    });
    if (!p.follow) p.follow = projectFollow(s);
    return buildClipFollow(clip, pitched, *p.follow);
}
void givePlayer(ClipPlayer& pl, ClipFollowBuild built) {
    pl.follow = std::move(built.tables);
    pl.followed = built.outcome;
    pl.followBudget = built.overBudget;
}
}  // namespace

bool clipPlaysPitched(const Session& s, Id clipId) {
    const model::Clip* clip = s.clips.find(clipId);
    if (!clip || clip->kind != model::ClipKind::Midi || !clip->activeTake) return false;
    const model::Take* take = s.takes.find(*clip->activeTake);
    const model::MidiSource* src = take && take->source && take->source->kind == model::Kind::Midi
                                       ? s.midi.find(*take->source)
                                       : nullptr;
    if (!src) return false;
    ClipPlayer pl;
    std::int32_t nextId = 0;
    midiEvents(pl, *clip, clipId, *take, *src, nextId);
    return std::any_of(pl.events.begin(), pl.events.end(), [](const ClipEvent& e) {
        return e.event.kind == host::Event::Kind::NoteOn && e.event.channel != 9;
    });
}

void compileClipPlayers(Plan& p, Node& n, const Session& s) {
    n.players.clear();
    n.clipVoices = n.clipWindow = 0;
    const model::Track* track = s.tracks.find(n.id);
    if (n.rack || !track || track->type != model::StripType::Channel) return;
    std::vector<Id> clips;
    for (const auto& e : s.clips)
        if (e.value->track == n.id) clips.push_back(e.id);
    if (clips.empty() || track->freeze) return;  // a frozen track plays its print
    std::int32_t nextId = 0;
    for (const TimedEvent& e : n.events) nextId = std::max(nextId, e.event.noteId + 1);
    const time::TimeMap& tm = *s.timeMap;
    double fastest = 0;
    for (const auto& t : tm.tempo()) fastest = std::max(fastest, t.bpm);
    // A playrate plan's block covers up to four times the song.
    const double blockBeats =
        double(p.block) / p.rate * fastest / 60.0 * (p.rateEnabled ? model::kMaxPlayrate : 1.0);
    const auto& prepared = p.preparedSources;
    for (const Id id : clips) {
        const model::Clip& clip = *s.clips.find(id);
        // With the pitch preserved, launched audio reads through a stretch
        // converter warmed up before its switch (S3 step D); one with take FX
        // or envelopes, read through its take chain, has no player yet.
        if (p.rateStretch && clip.kind != model::ClipKind::Midi && clipHasTakeChain(s, clip)) {
            p.launchRefusedPlayrate.push_back(id);
            p.warnings.push_back(id.str() +
                                 ": launch.playrate: an audio clip with take FX or envelopes "
                                 "cannot be launched while the master playrate preserves the "
                                 "pitch");
            continue;
        }
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
            // Following live (ADR-0098 X4): what placement would follow, by
            // tables within the budget, else by its key, else as written.
            if (clip.follow != model::ClipFollow::None) {
                ClipFollowBuild built = followPlayer(p, pl, clip, s);
                if (built.overBudget)
                    p.warnings.push_back(
                        id.str() + ": launch.follow_budget: its context and the project's make " +
                        "too many pairs of places to follow " +
                        (built.outcome == FollowOutcome::Key ? "the chords; it follows the key"
                                                             : "live; it plays as written"));
                givePlayer(pl, std::move(built));
            }
            const auto [voices, window] = midiSizing(pl, blockBeats);
            n.clipVoices = std::max(n.clipVoices, voices);
            n.clipWindow = std::max(n.clipWindow, window);
        } else {
            pl.periodFrames = time::samplesAt(clip.length, p.rate);
            pl.loopOn = clip.loop.on && clip.loop.length > 0;
            pl.periodItems = take && !clipLaysAsOneItem(s, clip, *take);
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
                    // Its own fades, shaping each pass (S3 step C).
                    c.fadeIn = time::samplesAt(clip.fadeIn.length, p.rate);
                    c.fadeOut = time::samplesAt(clip.fadeOut.length, p.rate);
                    c.fadeInShape = clip.fadeIn.shape;
                    c.fadeOutShape = clip.fadeOut.shape;
                    c.fadeInCurve = clip.fadeIn.curve;
                    c.fadeOutCurve = clip.fadeOut.curve;
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
    // On a playrate plan, the readers its audio is played through (S3).
    if (!p.rateEnabled) return;
    if (std::any_of(n.players.begin(), n.players.end(),
                    [](const ClipPlayer& pl) { return !pl.midi; })) {
        for (PlayerReader& r : n.launchReaders) makeReader(r, p);
        // A launch reaches it early enough to warm its converter up: one
        // prime per quantum played, over the converter's output lookahead,
        // and a quantum to spare (S3 step D's measure).
        if (p.rateStretch) {
            const int lookahead = n.launchReaders[0].state->stretch->lookahead();
            n.launchWarm =
                (std::int64_t(lookahead + kRateQuantum - 1) / kRateQuantum + 1) * kRateQuantum;
        }
    }
    for (TakeChain& tc : n.takeChains)
        if (tc.clipChain && tc.player >= 0 && !n.players[static_cast<std::size_t>(tc.player)].midi)
            makeReader(tc.clipReader, p);
}

bool clipHasTakeChain(const Session& s, const model::Clip& clip) {
    if (!clip.activeTake) return false;
    const model::Take* take = s.takes.find(*clip.activeTake);
    if (!take) return false;
    if (!take->chain.empty()) return true;
    return std::any_of(s.envelopes.begin(), s.envelopes.end(), [&](const auto& en) {
        return en.value->target.object == *clip.activeTake && en.value->active &&
               !en.value->points.empty();
    });
}

int playerOf(const Node& n, Id clip) SUPPLE_RT {
    for (std::size_t i = 0; i < n.players.size(); ++i)
        if (n.players[i].clip == clip) return static_cast<int>(i);
    return -1;
}

void applyLaunch(Node& n, const LaunchAction& a) SUPPLE_RT {
    // Its readers rotate (S3 step D): what played goes out on reader 1, and
    // a launch warmed up on reader 2 comes in on reader 0 with its
    // converter's history; else reader 0 takes the one that was going out
    // (a tape reader keeps no history, so this changes nothing there).
    {
        auto& r = n.launchReaders;
        std::swap(r[0], r[1]);
        if (a.kind == LaunchState::Kind::Clip && a.player >= 0 && r[2].state &&
            r[2].player == &n.players[static_cast<std::size_t>(a.player)] &&
            r[2].anchor == a.anchorSample && r[2].startQuantum != INT64_MIN) {
            std::swap(r[0], r[2]);
            // What went out before is over (switches are a warm-up apart):
            // reader 2 starts the next warm-up with no previous reading
            // (round 152 F1).
            r[2].previousLive = false;
        } else
            r[0].previousLive = false;  // a reading of its own from here (S3 step D4)
        const LaunchAction& warm = n.launchWarmNext;
        if (n.launchWarmValid && warm.kind == a.kind && warm.player == a.player &&
            warm.anchorSample == a.anchorSample)
            n.launchWarmValid = false;
    }
    LaunchState& L = n.launch;
    L.fadeFrom = L.kind == LaunchState::Kind::Arrangement ? -1
                 : L.kind == LaunchState::Kind::Clip      ? L.player
                                                          : -2;
    L.fadeAnchorSample = L.anchorSample;
    L.fadeAnchorBeat = L.anchorBeat;
    // The crossfade's clock (launchClock): from the anchor's song sample,
    // on a playrate plan too (S3 B).
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
    n.launchFresh = true;
    n.launchSwitchPending = true;
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
    n.launchFresh = old.launchFresh;
    n.launchSwitchAt = old.launchSwitchAt;
    n.launchSwitchPhase = old.launchSwitchPhase;
    n.launchSwitchPending = old.launchSwitchPending;
    // A launch reaching it early (prelaunch), by clip id too.
    n.launchNext = old.launchNext;
    n.launchNextValid = old.launchNextValid && !n.players.empty();
    if (n.launchNextValid && n.launchNext.kind == LaunchState::Kind::Clip) {
        n.launchNext.player = remap(n.launchNext.player);
        n.launchNextValid = n.launchNext.player >= 0;
    }
    // And one warming up (prewarm, S3 step D).
    n.launchWarmNext = old.launchWarmNext;
    n.launchWarmAt = old.launchWarmAt;
    n.launchWarmValid = old.launchWarmValid && n.launchWarm > 0 && !n.players.empty();
    if (n.launchWarmValid && n.launchWarmNext.kind == LaunchState::Kind::Clip) {
        n.launchWarmNext.player = remap(n.launchWarmNext.player);
        n.launchWarmValid = n.launchWarmNext.player >= 0;
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
    n.launchFresh = false;
    if (n.rack)
        for (NotesFeed& feed : n.rack->notes) {
            feed.launch.unanchored = true;
            feed.launchFresh = false;
        }
}

namespace {

// Which pass of `pl`'s period song frame `q` from the anchor falls in.
std::int64_t passOf(const ClipPlayer& pl, std::int64_t q) SUPPLE_RT {
    return q >= 0 ? q / pl.periodFrames : -((-q - 1) / pl.periodFrames) - 1;
}
// The pass a periodItems clip going out reads on in from `hold`, the
// switch's song sample: the one it was in there, so a wrap inside the
// fade-out does not jump to the period's start, as its committed piece
// reads on past the period's end (review round 113).
std::int64_t heldPass(const ClipPlayer& pl, std::int64_t anchor, std::int64_t hold) SUPPLE_RT {
    if (!pl.periodItems || hold == INT64_MAX) return INT64_MIN;
    // A clip with fades switched exactly on a pass's end has begun its next
    // pass: its committed piece is that pass's, the crossfade long, with its
    // own fade-in and the switch's fade-out (S3 step C, Q4, accepted test).
    const std::int64_t q = hold - anchor;
    if (clipFades(pl) && q > 0 && q % pl.periodFrames == 0) return passOf(pl, q);
    return passOf(pl, q - 1);
}

// Q4 rule 3 (S3 step C): an audio clip with a fade-in coming in takes no
// switch fade-in. Every launch starts it at its pass's start, so its own
// fade-in shapes it from the switch.
bool incomingStandsIn(const Node& n) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (L.kind != LaunchState::Kind::Clip || L.player < 0) return false;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
    return !pl.midi && pl.audio.fadeIn > 0;
}

// Q4 rule 4: the song sample where `player`, anchored at `anchor`, going out
// at a switch on song sample `start`, ends. Where the switch's crossfade
// would reach that pass's fade-out (P - Lout - F < phase < P, phase not 0:
// on a pass's end it has begun the next pass) it plays on at weight 1, under
// its own fade-out, to the pass's end and reads nothing after it; else the
// switch's crossfade ends it. The region reaches F before the fade-out
// (review round 135, H1): a switch quantized to the fade-out's start lands a
// sample either side of it as the tempo rounds, and it must play its fade
// out either way, which no 5 ms fade on top of it could be committed as.
std::int64_t outgoingEnd(const Node& n, int player, std::int64_t anchor,
                         std::int64_t start) SUPPLE_RT {
    const std::int64_t crossfade = start + n.launchFadeFrames;
    if (player < 0 || start == kFadeAtNext) return crossfade;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    if (pl.midi || pl.audio.fadeOut <= 0 || pl.periodFrames <= 0) return crossfade;
    const std::int64_t q = start - anchor;
    if (q <= 0) return crossfade;
    const std::int64_t phase = q - passOf(pl, q) * pl.periodFrames;
    if (phase == 0 || phase + n.launchFadeFrames <= pl.periodFrames - pl.audio.fadeOut)
        return crossfade;
    return start + (pl.periodFrames - phase);
}

// An audio player's sample at song sample `t`, anchored at `anchor`, scaled
// by `weight`, added into `into` at frame i. Before the anchor it is silent
// unless `unanchored` (a wrap or relocation since its launch) lets a loop
// clip play there by its phase. `raw`: without the clip's gain and pan (a
// clip chain's input; they apply after the chain).
void playerFrame(const ClipPlayer& pl, std::int64_t anchor, bool unanchored, std::int64_t t,
                 double weight, Stereo& into, std::uint32_t i, bool raw = false,
                 std::int64_t hold = INT64_MAX) SUPPLE_RT {
    const Clip& c = pl.audio;
    if (!c.data || pl.muted || pl.periodFrames <= 0 || weight == 0.0) return;
    const std::int64_t q = t - anchor;
    if ((q < 0 && (pl.once || !unanchored)) || (pl.once && q >= pl.periodFrames)) return;
    const std::int64_t held = t >= hold ? heldPass(pl, anchor, hold) : INT64_MIN;
    const std::int64_t phase = held != INT64_MIN
                                   ? q - held * pl.periodFrames
                                   : (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
    const std::int64_t content = pl.loopOn ? pl.loopStartFrames + phase % pl.loopFrames : phase;
    const auto& ch = c.data->channels;
    const auto frames = static_cast<std::int64_t>(c.data->frames());
    const std::int64_t f = c.offset + content;
    if (ch.empty() || f < 0 || f >= frames) return;
    const auto fi = static_cast<std::size_t>(f);
    const std::size_t right = ch.size() > 1 ? 1 : 0;
    float l = ch[0][fi], r = ch[right][fi];
    // Its own fades by the phase (S3 step C); a clip chain's apply after it.
    if (!raw && clipFades(pl)) weight *= clipFade(pl, double(phase));
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

// --- The playrate path (S3) ---

// What a launched audio clip's tape converter reads. Its frames are
// unwrapped song samples (the rate timeline's phase): each folds over the
// transport loop as the song does, then maps to the clip by playerFrame's
// rule from `anchor`. Before the anchor it is silent unless `unanchored`,
// or folded into a later loop pass than the output's (`pass`), as a clip
// chain's input reading ahead of a wrap is (foldInput).
struct PlayerSource {
    const ClipPlayer& pl;
    const PlayrateTimeline& timeline;
    std::int64_t anchor, pass;
    bool unanchored;
    // A periodItems clip's period read alone (others outside it are silent,
    // as outside its committed item); INT64_MIN: every period.
    std::int64_t period = INT64_MIN;
    // Going out: silent from this song sample, the switch's crossfade end,
    // where the committed piece ends (S3 B).
    std::int64_t until = INT64_MAX;
    // Going out: from this song sample (the switch's) a periodItems clip
    // reads on in the pass it was in (`held`, heldPass).
    std::int64_t hold = INT64_MAX, held = INT64_MIN;
    float sample(std::size_t channel, std::int64_t f) const SUPPLE_RT {
        const double song = double(f);
        const std::int64_t s =
            f + static_cast<std::int64_t>(std::llround(timeline.wrapped(song) - song));
        const bool anywhere = unanchored || timeline.cycle(song) > pass;
        const std::int64_t q = s - anchor;
        if ((q < 0 && (pl.once || !anywhere)) || (pl.once && q >= pl.periodFrames)) return 0;
        const std::int64_t k = held != INT64_MIN && s >= hold ? held : periodOf(q);
        if (period != INT64_MIN && k != period) return 0;
        if (s >= until) return 0;
        const std::int64_t phase = q - k * pl.periodFrames;
        const std::int64_t content = pl.loopOn ? pl.loopStartFrames + phase % pl.loopFrames : phase;
        const auto& ch = pl.audio.data->channels;
        const auto& data = ch[channel > 0 && ch.size() > 1 ? 1 : 0];
        const std::int64_t frame = pl.audio.offset + content;
        if (frame < 0 || frame >= static_cast<std::int64_t>(data.size())) return 0;
        return data[static_cast<std::size_t>(frame)];
    }
    // Which period song frame `q` from the anchor falls in.
    std::int64_t periodOf(std::int64_t q) const SUPPLE_RT { return passOf(pl, q); }
    static const float* read(const void* context, std::size_t channel, std::int64_t first,
                             std::span<float> scratch) noexcept {
        const auto& self = *static_cast<const PlayerSource*>(context);
        for (std::size_t i = 0; i < scratch.size(); ++i)
            scratch[i] = self.sample(channel, first + static_cast<std::int64_t>(i));
        return scratch.data();
    }
};

// The player's output in rate quantum `q` (device frames of `timeline`),
// converted once per quantum and reading.
const RateClipState& playerQuantum(PlayerReader& r, const ClipPlayer& pl, std::int64_t anchor,
                                   bool unanchored, std::int64_t pass,
                                   const PlayrateTimeline& timeline, std::int64_t q,
                                   std::int64_t until, std::int64_t hold) SUPPLE_RT {
    RateClipState& st = *r.state;
    if (st.quantum == q && r.player == &pl && r.anchor == anchor && r.unanchored == unanchored &&
        r.pass == pass && r.until == until && r.hold == hold)
        return st;
    r.player = &pl;
    r.anchor = anchor;
    r.unanchored = unanchored;
    r.pass = pass;
    r.until = until;
    r.hold = hold;
    PlayerSource source{pl, timeline, anchor, pass, unanchored};
    source.until = until;
    source.hold = hold;
    source.held = heldPass(pl, anchor, hold);
    std::array<double, kRateQuantum + 1> positions;
    for (std::size_t j = 0; j < positions.size(); ++j)
        positions[j] = timeline.phase(q + static_cast<std::int64_t>(j));
    // A periodItems clip: each output frame reads its own period alone, as
    // the committed item there would (S3 B). The tape reader keeps no state
    // between calls, so a quantum crossing a period reads it again for the
    // later frames.
    const auto periodAt = [&](std::size_t j) SUPPLE_RT {
        const auto song = static_cast<std::int64_t>(std::floor(timeline.wrapped(positions[j])));
        return source.held != INT64_MIN && song >= hold ? source.held
                                                        : source.periodOf(song - anchor);
    };
    if (pl.periodItems) source.period = periodAt(0);
    bool ok = st.tape.process({&source, PlayerSource::read, 2}, positions, st.pointers);
    if (ok && pl.periodItems) {
        const std::int64_t first = source.period;
        std::array<float*, 2> later{r.later[0].data(), r.later[1].data()};
        for (std::size_t j = 1; j < kRateQuantum; ++j) {
            const std::int64_t here = periodAt(j);
            if (here == first) continue;
            if (here != source.period) {
                source.period = here;
                ok = ok && st.tape.process({&source, PlayerSource::read, 2}, positions, later);
            }
            for (std::size_t c = 0; c < 2; ++c) st.output[c][j] = r.later[c][j];
        }
    }
    if (!ok) {
        st.failed = true;
        for (auto& channel : st.output) channel.fill(0);
    }
    st.quantum = q;
    return st;
}

// --- Stretched readings (a pitch-keeping plan, S3 step D) ---

// What a stretched reading's converter reads: the player's song frames,
// folded as the song is; before the anchor only in a later loop cycle than
// the switch's (`launchCycle`), or anywhere when it began unanchored. A pure
// function of the frame, so its reads do not depend on when they happen.
PlayerSource stretchSource(const PlayerReader& r, const ClipPlayer& pl,
                           const PlayrateTimeline& timeline) SUPPLE_RT {
    return PlayerSource{pl, timeline, r.anchor, r.launchCycle, r.unanchored};
}

// Positions a reading's converter for output from quantum `q`, as primeClip
// does a timeline item's: seeked so its primes cover the output lookahead
// before q. Every position is a function of q and the trajectory, so a
// warm-up spread over the quanta before q gives what one at q would.
// `switchAt`: the device frame of the switch, whose loop cycle it keeps.
// `launchCycle`: the loop cycle it keeps (the switch frame's).
void stretchSeekCycle(PlayerReader& r, const ClipPlayer& pl, std::int64_t anchor, bool unanchored,
                      std::int64_t launchCycle, const PlayrateTimeline& timeline,
                      std::int64_t q) SUPPLE_RT {
    RateClipState& st = *r.state;
    r.player = &pl;
    r.anchor = anchor;
    r.unanchored = unanchored;
    r.startQuantum = q;
    r.launchCycle = launchCycle;
    r.primes = 0;
    r.credit = 0;
    st.primed = false;
    st.quantum = INT64_MIN;
    const PlayerSource source = stretchSource(r, pl, timeline);
    const int latency = st.stretch->lookahead();
    const int quanta = (latency + kRateQuantum - 1) / kRateQuantum;
    const int extra = quanta * kRateQuantum - latency;
    st.stretch->seek({&source, PlayerSource::read, 2}, timeline.phase(q - extra) + extra, 1.);
}

// A fresh reading: it no longer plays the previous plan's (S3 step D4).
void stretchSeek(PlayerReader& r, const ClipPlayer& pl, std::int64_t anchor, bool unanchored,
                 const PlayrateTimeline& timeline, std::int64_t q,
                 std::int64_t switchAt) SUPPLE_RT {
    r.previousLive = false;
    stretchSeekCycle(r, pl, anchor, unanchored, timeline.cycle(timeline.phase(switchAt)), timeline,
                     q);
}

std::atomic<std::uint64_t> gStretchPrimes{0};
std::array<std::atomic<std::int64_t>, 256> gStretchPrimeFrames{};

// Up to `budget` more of its primes; true once it is ready, its output
// running from its start quantum.
bool stretchPrime(PlayerReader& r, const PlayrateTimeline& timeline,
                  std::uint32_t budget) SUPPLE_RT {
    RateClipState& st = *r.state;
    const int latency = st.stretch->lookahead();
    const auto quanta = static_cast<std::uint32_t>((latency + kRateQuantum - 1) / kRateQuantum);
    const PlayerSource source = stretchSource(r, *r.player, timeline);
    while (r.primes < quanta && budget-- > 0) {
        ++r.primes;
        const std::uint64_t counted = gStretchPrimes.fetch_add(1, std::memory_order_relaxed) + 1;
        gStretchPrimeFrames[counted % gStretchPrimeFrames.size()].store(r.startQuantum,
                                                                        std::memory_order_relaxed);
        const std::int64_t at = r.startQuantum - std::int64_t(quanta) * kRateQuantum +
                                std::int64_t(r.primes) * kRateQuantum + latency;
        if (!st.stretch->primeTo({&source, PlayerSource::read, 2},
                                 timeline.phase(at) + st.stretch->analysisLookahead()))
            st.failed = true;
    }
    if (r.primes == quanta && !st.primed) {
        st.primed = true;
        st.quantum = r.startQuantum - kRateQuantum;
    }
    return st.primed;
}

// The reading's output in quantum `q`. Its converter runs every quantum in
// order from its start: a quantum a block skipped (no weight there) is run
// before the next, so its history is the same however blocks fall. A
// reading it was not warmed for (a launch from stop, a relocation, a plan
// that lost its converter) starts all at once: from its switch's quantum
// (`switchQ`, the frame `switchAt`) when that is near, so it gives what a
// warm-up gives, else at q; its loop cycle is the switch's either way
// (round 139 R4), `switchCycle` as recorded at the switch: the timeline's
// ring holds seconds, so a frame long past reads a later one's phase there.
const RateClipState& stretchQuantum(PlayerReader& r, const ClipPlayer& pl, std::int64_t anchor,
                                    bool unanchored, const PlayrateTimeline& timeline,
                                    std::int64_t q, std::int64_t switchAt, std::int64_t switchQ,
                                    std::int64_t switchCycle) SUPPLE_RT {
    RateClipState& st = *r.state;
    constexpr std::int64_t kCatchUp = 32 * kRateQuantum;
    const bool keyed =
        r.player == &pl && r.anchor == anchor && r.startQuantum != INT64_MIN && q >= r.startQuantum;
    if (!keyed || (st.primed && (q < st.quantum || q - st.quantum > kCatchUp))) {
        const bool fromSwitch = switchQ != INT64_MIN && q >= switchQ && q - switchQ <= kCatchUp;
        r.previousLive = false;  // a fresh reading (S3 step D4)
        stretchSeekCycle(r, pl, anchor, unanchored,
                         switchAt != INT64_MIN ? switchCycle : timeline.cycle(timeline.phase(q)),
                         timeline, fromSwitch ? switchQ : q);
    }
    if (!st.primed) stretchPrime(r, timeline, UINT32_MAX);
    const PlayerSource source = stretchSource(r, pl, timeline);
    while (st.quantum < q) {
        const std::int64_t next = st.quantum + kRateQuantum;
        const double end = timeline.phase(next + kRateQuantum + st.stretch->lookahead()) +
                           st.stretch->analysisLookahead();
        if (!st.stretch->process({&source, PlayerSource::read, 2}, end, st.pointers)) {
            st.failed = true;
            for (auto& channel : st.output) channel.fill(0);
        }
        st.quantum = next;
    }
    return st;
}

// A launch warming up on reader 2 (prewarm): its converter seeks at the
// first block that has it and primes once per 64 frames played (a frame
// credit, whatever the block size: round 139 R1), its launchWarm frames
// giving one prime to spare, ready by its switch without a burst (Q5).
void warmLaunch(const Plan& p, Node& n, std::uint32_t frames) SUPPLE_RT {
    if (!n.launchWarmValid) return;
    const LaunchAction& a = n.launchWarmNext;
    if (a.kind != LaunchState::Kind::Clip || a.player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(a.player)];
    PlayerReader& reader = n.launchReaders[2];
    if (pl.midi || pl.chain >= 0 || !pl.audio.data || !reader.state || !reader.state->preserve)
        return;
    // A warm-up switching at or before a trajectory change's switch goes on
    // as the previous plan's reading, on the old trajectory (S3 step D4);
    // one switching later warms on the plan's own (round 152 F1).
    const bool before = reader.previousLive && reader.previous && reader.previous->state &&
                        reader.previous->state->preserve && p.previousTimelines[n.live ? 1 : 0] &&
                        n.launchWarmAt - n.timelineShift <= p.rateSwitchDevice;
    PlayerReader& r = before ? *reader.previous : reader;
    const PlayrateTimeline& timeline =
        before ? *p.previousTimelines[n.live ? 1 : 0] : *p.timelines[n.live ? 1 : 0];
    const std::int64_t device = n.launchWarmAt - n.timelineShift;
    const std::int64_t q = quantumAt(p, device);
    if (r.player != &pl || r.anchor != a.anchorSample || r.startQuantum != q)
        stretchSeek(r, pl, a.anchorSample, false, timeline, q, device);
    r.credit += frames;
    const std::uint32_t budget = r.credit / kRateQuantum;
    r.credit %= kRateQuantum;
    stretchPrime(r, timeline, budget);
}

// playerFrame for a block on a playrate plan: frame i reads the song
// position at the node's device frame `pos + i + ahead` (a clip chain's
// input reads its lead ahead), through `reader`, weighted by `weight(i)`.
template <class Weight>
void playerRate(const Plan& p, const Node& n, PlayerReader& reader, const ClipPlayer& pl,
                std::int64_t anchor, bool unanchored, std::int64_t pos, std::int64_t ahead,
                std::uint32_t frames, Weight&& weight, Stereo& into, bool raw,
                std::int64_t until = INT64_MAX, std::int64_t hold = INT64_MAX,
                std::int64_t switchAt = INT64_MIN, std::int64_t switchCycle = 0) SUPPLE_RT {
    const Clip& c = pl.audio;
    if (!c.data || c.data->channels.empty() || pl.muted || pl.periodFrames <= 0 || !reader.state)
        return;
    const PlayrateTimeline& timeline = *p.timelines[n.live ? 1 : 0];
    // The output's loop pass: a later one read ahead plays before the anchor.
    const std::int64_t pass = timeline.cycle(timeline.phase(pos - n.timelineShift));
    const bool fades = !raw && clipFades(pl);
    const std::int64_t held = heldPass(pl, anchor, hold);
    const std::int64_t switchQ = switchAt == INT64_MIN ? INT64_MIN : quantumAt(p, switchAt);
    // Across a trajectory change (S3 step D4): the previous plan's reading,
    // on the old trajectory, until the switch, then crossfaded into this
    // one over smoothFrames; it alone for a reader with no reading for the
    // switch (one going out), until the old trajectory is let go.
    PlayerReader* const prev = reader.previousLive && reader.previous && reader.previous->state &&
                                       p.previousTimelines[n.live ? 1 : 0] &&
                                       p.rateSwitchDevice != INT64_MIN
                                   ? reader.previous.get()
                                   : nullptr;
    const PlayrateTimeline* const oldTimeline =
        prev ? p.previousTimelines[n.live ? 1 : 0].get() : nullptr;
    const std::int64_t oldPass =
        prev ? oldTimeline->cycle(oldTimeline->phase(pos - n.timelineShift)) : 0;
    const auto reading = [&](PlayerReader& r, const PlayrateTimeline& tl, std::int64_t inPass,
                             std::int64_t q) SUPPLE_RT -> const RateClipState& {
        return r.state->preserve
                   ? stretchQuantum(r, pl, anchor, unanchored, tl, q, switchAt, switchQ,
                                    switchCycle)
                   : playerQuantum(r, pl, anchor, unanchored, inPass, tl, q, until, hold);
    };
    for (std::uint32_t i = 0; i < frames; ++i) {
        double w = weight(i);
        if (w == 0.0) continue;
        const std::int64_t device = pos + i + ahead - n.timelineShift;
        if (fades) {
            // Its own fades at the frame's song position in its pass, as a
            // committed item's (rate_sources.cpp, playRateClip) from the
            // pass's start (S3 step C).
            const double absolute = timeline.absolutePosition(device);
            const auto s = static_cast<std::int64_t>(std::floor(absolute - timeline.origin()));
            const std::int64_t k = held != INT64_MIN && s >= hold ? held : passOf(pl, s - anchor);
            w *=
                clipFade(pl, absolute - (double(anchor + k * pl.periodFrames) + timeline.origin()));
        }
        const std::int64_t q = quantumAt(p, device);
        const auto k = static_cast<std::size_t>(device - q);
        float l = 0, r = 0;
        const bool old = prev && device < p.rateSafeDevice &&
                         (reader.previousOnly || device < p.rateSwitchDevice + p.smoothFrames);
        const bool now = !old || (!reader.previousOnly && device >= p.rateSwitchDevice);
        if (now) {
            const RateClipState& st = reading(reader, timeline, pass, q);
            l = st.output[0][k];
            r = st.output[1][k];
        }
        if (old) {
            const RateClipState& st = reading(*prev, *oldTimeline, oldPass, q);
            const float mix =
                now ? static_cast<float>(double(device - p.rateSwitchDevice) / p.smoothFrames)
                    : 0.0f;
            l = st.output[0][k] + mix * (l - st.output[0][k]);
            r = st.output[1][k] + mix * (r - st.output[1][k]);
        }
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
        into.l[i] += static_cast<float>(static_cast<double>(l) * (raw ? 1.0 : c.gainL) * w);
        into.r[i] += static_cast<float>(static_cast<double>(r) * (raw ? 1.0 : c.gainR) * w);
    }
}

}  // namespace

void launchAudio(Plan& p, Node& n, Stereo& into, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    n.arrangementGain = nullptr;
    if (n.launchSwitchPending) {  // this block starts at the switch
        n.launchSwitchAt = pos - n.timelineShift;
        n.launchSwitchPhase = p.rateEnabled
                                  ? p.timelines[n.live ? 1 : 0]->absolutePhase(n.launchSwitchAt)
                                  : std::numeric_limits<double>::quiet_NaN();
        n.launchSwitchPending = false;
    }
    if (p.rateStretch) warmLaunch(p, n, frames);  // the next launch's converter (S3 step D)
    const std::int64_t song0 = pos - n.timelineShift;
    const double clock0 = launchClock(p, n, pos);
    if (n.launch.fadeFrom == -3 && n.launch.fadeStart == kFadeAtNext)
        n.launch.fadeStart = static_cast<std::int64_t>(std::floor(clock0));
    // A frozen node fades its print in after writing it (retiringIntoPrint).
    if (n.players.empty() && (n.launch.fadeFrom != -3 || n.frozenAudio)) return;
    const LaunchState& L = n.launch;
    const std::int64_t F = n.launchFadeFrames;
    const bool fading = L.fadeFrom != -2 && clock0 < double(L.fadeStart + F);
    const bool arrangement = L.kind == LaunchState::Kind::Arrangement;
    // What goes out ends at the crossfade's end, or at its pass's end where
    // its own fade-out stands in for the switch's (Q4 rule 4).
    const std::int64_t outEnd = L.fadeFrom >= 0
                                    ? outgoingEnd(n, L.fadeFrom, L.fadeAnchorSample, L.fadeStart)
                                    : L.fadeStart + F;
    const bool standsOut = L.fadeFrom >= 0 && outEnd != L.fadeStart + F;
    const bool goingOut = L.fadeFrom >= 0 && clock0 < double(outEnd);
    // The arrangement as the commit cuts it, on the playrate path (S3 B):
    // after a switch away its items read nothing past the crossfade's end,
    // and while a switch back from a clip fades them in, nothing before it.
    std::int64_t gateFrom = INT64_MIN, gateUntil = INT64_MAX;
    if (p.rateEnabled && !arrangement && L.fadeFrom == -1) gateUntil = L.fadeStart + F;
    if (p.rateEnabled && arrangement && fading && L.fadeFrom >= 0) gateFrom = L.fadeStart;
    for (Clip& c : n.clips) {
        c.gateFrom = gateFrom;
        c.gateUntil = gateUntil;
    }
    if (arrangement && !fading && !goingOut) return;  // the arrangement alone: nothing changes
    // x: how far into the crossfade frame i is, 0 to 1 (1 past it), by its
    // song position (launchClock).
    const auto x = [&](std::uint32_t i) {
        if (!fading) return 1.0;
        const double clock = i == 0 ? clock0 : launchClock(p, n, pos + i);
        return std::clamp((clock - double(L.fadeStart)) / double(F), 0.0, 1.0);
    };
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double in = x(i);
        n.arrangementGains[i] = static_cast<float>((arrangement ? in : 0.0) +
                                                   (fading && L.fadeFrom == -1 ? 1.0 - in : 0.0));
    }
    n.arrangementGain = n.arrangementGains.data();
    // A player's block: by song sample, or on a playrate plan through one
    // of the node's readers (0 what plays, 1 what fades out).
    // What fades out reads nothing past the crossfade's end on that path, as
    // its committed piece (S3 B).
    // A clip read by period that fades out at a launch reads on in the pass
    // it was in at the switch (`hold`), as its committed piece does.
    const auto play = [&](const ClipPlayer& pl, std::size_t reader, std::int64_t anchor,
                          bool unanchored, auto&& weight, std::int64_t hold) SUPPLE_RT {
        if (p.rateEnabled)
            playerRate(p, n, n.launchReaders[reader], pl, anchor, unanchored, pos, 0, frames,
                       weight, into, false, reader == 1 ? outEnd : INT64_MAX, hold,
                       reader == 0 ? n.launchSwitchAt : INT64_MIN,
                       switchCycle(n, *p.timelines[n.live ? 1 : 0]));
        else
            for (std::uint32_t i = 0; i < frames; ++i)
                playerFrame(pl, anchor, unanchored, song0 + i, weight(i), into, i, false, hold);
    };
    // (A player with a take chain plays through it: processTake.)
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.player)];
        // Its own fade-in stands in for the switch's (Q4 rule 3).
        const auto whole = [](std::uint32_t) { return 1.0; };
        if (!pl.midi && pl.chain < 0) {
            if (incomingStandsIn(n))
                play(pl, 0, L.anchorSample, L.unanchored, whole, INT64_MAX);
            else
                play(pl, 0, L.anchorSample, L.unanchored, x, INT64_MAX);
        }
    }
    const auto out = [&](std::uint32_t i) { return 1.0 - x(i); };
    if (goingOut) {
        const ClipPlayer& pl = n.players[static_cast<std::size_t>(L.fadeFrom)];
        // Standing in: whole until its pass's end, nothing after (Q4 rule 4).
        const auto stand = [&](std::uint32_t i) {
            const double clock = i == 0 ? clock0 : launchClock(p, n, pos + i);
            return clock < double(outEnd) ? 1.0 : 0.0;
        };
        if (!pl.midi && pl.chain < 0) {
            if (standsOut)
                play(pl, 1, L.fadeAnchorSample, false, stand, L.fadeStart);
            else
                play(pl, 1, L.fadeAnchorSample, false, out, L.fadeStart);
        }
    }
    if (fading && L.fadeFrom == -3) {
        const ClipPlayer& pl = n.retiring[static_cast<std::size_t>(L.fadeRetiring)];
        play(pl, 1, L.fadeAnchorSample, L.fadeUnanchored, out, INT64_MAX);
    }
}

void retiringIntoPrint(const Plan& p, Node& n, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = n.launch;
    // A playrate plan cuts to the print (S3 step 1).
    if (p.rateEnabled || L.fadeFrom != -3 || L.fadeStart == kFadeAtNext) return;
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
            r.audio.fadeIn = pl.audio.fadeIn;  // its own fades (S3 step C)
            r.audio.fadeOut = pl.audio.fadeOut;
            r.audio.fadeInShape = pl.audio.fadeInShape;
            r.audio.fadeOutShape = pl.audio.fadeOutShape;
            r.audio.fadeInCurve = pl.audio.fadeInCurve;
            r.audio.fadeOutCurve = pl.audio.fadeOutCurve;
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
        if (!n.retiring.empty() && to.rateEnabled)
            for (PlayerReader& r : n.launchReaders) makeReader(r, to);
    }
}

namespace {

// A following clip's note (ADR-0098 X4): its key as the tables map it, by
// the clip beat where it began and the song beat where that start sounds
// (`base` the pass's song beat), a note-off as its note-on.
void followEvent(const Plan& p, const ClipPlayer& pl, const ClipEvent& ce, double base,
                 host::Event& e) SUPPLE_RT {
    if (!pl.follow || !p.follow) return;
    if (e.kind != host::Event::Kind::NoteOn && e.kind != host::Event::Kind::NoteOff) return;
    e.key = static_cast<std::int16_t>(
        followedKey(*pl.follow, *p.follow, ce.start, base + ce.start, e.channel, e.key));
}

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
            followEvent(p, pl, *it, base, e);
            e.time = static_cast<std::uint32_t>(at - song0);
            if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
        }
    }
}

// clipMidi on a playrate plan (S3): the events for frames [0, frames)
// whose song positions are the rate timeline's at devices `dev0 + i` (the
// input side). As rateEvents plays the timeline's: an event plays at the
// first frame whose song position reaches it, and what `stage` holds ends
// where the song enters a new loop pass. `pass`: the output's loop pass (a
// later one, read ahead, plays before the anchor, as foldInput's fold
// does). `pl` null: no notes, only those ends. Within capacity: never
// allocates.
void rateClipMidi(const Plan& p, const PlayrateTimeline& tl, const ClipPlayer* pl,
                  const LaunchState& L, std::int64_t dev0, std::uint32_t frames, std::int64_t pass,
                  ClipWindow from, Stage& stage) SUPPLE_RT {
    if (frames == 0) return;
    auto& out = stage.blockEvents;
    auto& held = stage.pendingVoices;  // scratch (prepareNoteIds refills it)
    held.clear();
    for (const HeldVoice& v : stage.voices)
        if (!v.live && v.sourceId >= 0 && held.size() < held.capacity()) held.push_back(v);
    const auto emit = [&](const host::Event& e) SUPPLE_RT {
        if (out.size() == out.capacity()) {
            ++stage.voiceDropped;
            return;
        }
        out.push_back(e);
        if (e.kind == host::Event::Kind::NoteOn) {
            if (held.size() < held.capacity()) {
                HeldVoice v;
                v.sourceId = e.noteId;
                v.port = e.port;
                v.channel = e.channel;
                v.key = e.key;
                held.push_back(v);
            }
        } else if (e.kind == host::Event::Kind::NoteOff || e.kind == host::Event::Kind::NoteChoke) {
            std::erase_if(held, [&](const HeldVoice& v) { return v.sourceId == e.noteId; });
        }
    };
    const double here = tl.phase(dev0);
    double before = tl.phase(dev0 - 1);
    if (from == ClipWindow::Here) {
        before = here;
    } else if (from == ClipWindow::Anchor) {
        // The anchor in this pass, at most a frame's step behind (the
        // switch is the first frame whose song position reaches it).
        const double anchor = here - (tl.position(dev0) - double(L.anchorSample));
        if (anchor < before && anchor >= here - model::kMaxPlayrate) before = anchor;
    }
    const double last = tl.phase(dev0 + frames - 1);
    const std::int64_t c0 = tl.cycle(before), c1 = tl.cycle(last);
    const double length = tl.looping() ? tl.cycleLength() : 0.0;
    const bool notes = pl && pl->midi && !pl->muted && !pl->events.empty() && pl->periodBeats > 0;
    const time::TimeMap& tm = *p.timeMap;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    constexpr double kSlack = 1e-6;  // beats: the sample decides
    for (std::int64_t c = c0; c <= c1; ++c) {
        std::uint32_t start = 0;
        if (c != c0) {
            // The frame where the song enters pass c: what is held ends there.
            std::uint32_t lo = 0, hi = frames - 1;
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (tl.cycle(tl.phase(dev0 + mid)) < c)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            start = lo;
            while (!held.empty()) {
                const HeldVoice v = held.back();
                held.pop_back();
                host::Event off;
                off.kind = host::Event::Kind::NoteOff;
                off.time = start;
                off.noteId = v.sourceId;
                off.port = v.port;
                off.channel = v.channel;
                off.key = v.key;
                if (out.size() < out.capacity())
                    out.push_back(off);
                else
                    ++stage.voiceDropped;
            }
        }
        if (!notes) continue;
        const double offset = double(c) * length;
        const double a = before - offset, b = last - offset;
        const bool inclusive = from != ClipWindow::After && c == c0;
        // Before the anchor: unanchored, or a later pass than the output's.
        const bool anywhere = (L.unanchored || c > pass) && !pl->once;
        const double b0 = tm.beatAt((a + double(origin)) / p.rate);
        const double b1 = tm.beatAt((b + double(origin)) / p.rate);
        const double period = pl->periodBeats;
        auto kLo = static_cast<std::int64_t>(std::floor((b0 - L.anchorBeat) / period)) - 1;
        auto kHi = static_cast<std::int64_t>(std::floor((b1 - L.anchorBeat) / period)) + 1;
        if (!anywhere) kLo = std::max<std::int64_t>(kLo, 0);
        if (pl->once) kHi = std::min<std::int64_t>(kHi, 0);
        for (std::int64_t k = kLo; k <= kHi; ++k) {
            const double base = L.anchorBeat + double(k) * period;
            auto it = std::lower_bound(pl->events.begin(), pl->events.end(), b0 - base - kSlack,
                                       [](const ClipEvent& e, double x) { return e.beat < x; });
            for (; it != pl->events.end() && it->beat <= b1 - base + kSlack; ++it) {
                const std::int64_t at =
                    time::samplesAt(tm.secondsAt(base + it->beat), p.rate) - origin;
                const double x = double(at);
                if ((inclusive ? x < a : x <= a) || x > b) continue;
                if (!anywhere && at < L.anchorSample) continue;
                if (tl.looping() && (x >= tl.loopEnd() || (c > 0 && x < tl.loopStart()))) continue;
                // Its frame: the first whose song position (in pass c) reaches it.
                std::uint32_t lo = start, hi = frames - 1;
                while (lo < hi) {
                    const auto mid = lo + (hi - lo) / 2;
                    if (tl.phase(dev0 + mid) - offset < x)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                host::Event e = it->event;
                followEvent(p, *pl, *it, base, e);
                e.time = lo;
                emit(e);
            }
        }
    }
}

}  // namespace

void rateLaunchMidi(Plan& p, Node& n, std::int64_t pos, std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = n.launch;
    const ClipPlayer* pl = nullptr;
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0) {
        pl = &n.players[static_cast<std::size_t>(L.player)];
        // An instrument in its take chain plays its notes there.
        if (pl->chain >= 0 && n.takeChains[static_cast<std::size_t>(pl->chain)].noteTarget >= 0)
            pl = nullptr;
    }
    const PlayrateTimeline& tl = *p.timelines[n.live ? 1 : 0];
    const std::int64_t dev0 = pos - n.timelineShift;
    const ClipWindow from = n.launchFresh      ? ClipWindow::Anchor
                            : n.rateEventFresh ? ClipWindow::Here
                                               : ClipWindow::After;
    n.launchFresh = false;
    n.rateEventFresh = false;
    rateClipMidi(p, tl, pl, L, dev0, frames, tl.cycle(tl.phase(dev0)), from, n.midiStage);
}

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

void prewarm(Node& n, const LaunchAction& a, std::int64_t at) SUPPLE_RT {
    n.launchWarmNext = a;
    n.launchWarmAt = at;
    n.launchWarmValid = true;
}

namespace {
// `clip`'s player in `n`, a retiring one too; null when it has none.
const ClipPlayer* playerIn(const Node& n, Id clip) SUPPLE_RT {
    if (const int i = playerOf(n, clip); i >= 0) return &n.players[static_cast<std::size_t>(i)];
    for (const ClipPlayer& r : n.retiring)
        if (r.clip == clip) return &r;
    return nullptr;
}

// `from`'s reading handed to `to`, its state swapped (never freed here).
void handReading(PlayerReader& to, PlayerReader& from, const ClipPlayer* pl) SUPPLE_RT {
    to.state.swap(from.state);
    to.player = pl;
    to.until = from.until;
    to.hold = from.hold;
    to.later = from.later;
    to.anchor = from.anchor;
    to.pass = from.pass;
    to.unanchored = from.unanchored;
    to.startQuantum = from.startQuantum;
    to.launchCycle = from.launchCycle;
    to.primes = from.primes;
    to.credit = from.credit;
}
}  // namespace

std::int64_t switchCycle(const Node& n, const PlayrateTimeline& timeline) SUPPLE_RT {
    // A switch on a plan without a playrate: its frame while the ring still
    // holds it, else 0 (too low a cycle only matters before the anchor in
    // the switch's own cycle, long past by then; too high silences later
    // cycles before the anchor).
    if (std::isnan(n.launchSwitchPhase))
        return n.launchSwitchAt != INT64_MIN &&
                       timeline.hasHistory(n.launchSwitchAt, n.launchSwitchAt)
                   ? timeline.cycle(timeline.phase(n.launchSwitchAt))
                   : 0;
    return timeline.cycle(n.launchSwitchPhase - timeline.origin());
}

std::uint64_t stretchPrimes() noexcept { return gStretchPrimes.load(std::memory_order_relaxed); }
std::int64_t stretchPrimeFrame(std::uint64_t n) noexcept {
    return gStretchPrimeFrames[n % gStretchPrimeFrames.size()].load(std::memory_order_relaxed);
}

void carryLaunchAcross(const Plan& p, Node& old, Node& n) SUPPLE_RT {
    const PlayrateTimeline& timeline = *p.timelines[n.live ? 1 : 0];
    const std::int64_t S = p.rateSwitchDevice;
    if (S == INT64_MIN || !p.previousTimelines[n.live ? 1 : 0]) return;
    const LaunchState& L = old.launch;
    // The warm-up under way in the previous plan, and its switch's frame.
    const bool warming = old.launchWarmValid &&
                         old.launchWarmNext.kind == LaunchState::Kind::Clip &&
                         old.launchWarmNext.player >= 0;
    const std::int64_t warmSwitch =
        warming ? old.launchWarmAt - old.timelineShift : std::numeric_limits<std::int64_t>::max();
    const bool warmFirst = warming && warmSwitch <= S;  // what reads at S is that launch
    // The reading each reader holds in the previous plan: 0 what plays, 1
    // what goes out, 2 the warm-up.
    const ClipPlayer* reads[3] = {nullptr, nullptr, nullptr};
    if (L.kind == LaunchState::Kind::Clip && L.player >= 0)
        reads[0] = &old.players[static_cast<std::size_t>(L.player)];
    if (L.fadeFrom >= 0)
        reads[1] = &old.players[static_cast<std::size_t>(L.fadeFrom)];
    else if (L.fadeFrom == -3 && L.fadeRetiring >= 0 &&
             static_cast<std::size_t>(L.fadeRetiring) < old.retiring.size())
        reads[1] = &old.retiring[static_cast<std::size_t>(L.fadeRetiring)];
    if (warming) reads[2] = &old.players[static_cast<std::size_t>(old.launchWarmNext.player)];
    for (std::size_t k = 0; k < 3; ++k) {
        PlayerReader& from = old.launchReaders[k];
        PlayerReader& to = n.launchReaders[k];
        // A reader that plays only an earlier plan's reading holds none of
        // its own to hand on: rateSafeDevice keeps the next change from
        // being adopted until that reading is over (round 145 A1).
        if (!reads[k] || reads[k]->midi || from.player != reads[k] || !from.state ||
            (from.previousLive && from.previousOnly) || !to.state || !to.previous)
            continue;
        const ClipPlayer* pl = playerIn(n, reads[k]->clip);
        if (!pl) continue;
        const bool stretched = to.state->preserve;
        if (k == 2 && !warmFirst) {
            // It switches after the change: its warm-up starts again on the
            // new trajectory, primed as far as it had come.
            if (stretched && from.state->preserve && from.startQuantum != INT64_MIN) {
                stretchSeek(to, *pl, from.anchor, false, timeline, quantumAt(p, warmSwitch),
                            warmSwitch);
                stretchPrime(to, timeline, from.primes);
                to.credit = from.credit;
            }
            continue;
        }
        const bool reading = from.state->preserve ? from.startQuantum != INT64_MIN : true;
        if (!reading) continue;
        handReading(*to.previous, from, pl);
        to.previousLive = true;
        // What reads at S: reader 0's clip, or the warm-up switching first.
        to.previousOnly = k == 1 || (k == 0 && warmFirst);
        if (to.previousOnly) continue;
        if (stretched) {
            // Seeked here for output from the switch; primed at its first
            // use, a warm-up's primes at most (stretchQuantum).
            // Its loop cycle: the reading's own, or a tape reader's switch's
            // (a tape reader keeps none: round 152 F2). A switch whose block
            // has not run yet starts cold there instead (stretchQuantum).
            // (Unreached as far as known: executors adopt at the top of a
            // slice and apply its launches after. Were it reached, a plain
            // cold start there, as stretchSeek gives a block later.)
            const bool tape = !to.previous->state->preserve;
            if (tape && old.launchSwitchPending) {
                to.previousLive = false;
                continue;
            }
            stretchSeekCycle(to, *pl, to.previous->anchor, to.previous->unanchored,
                             tape ? switchCycle(old, timeline) : to.previous->launchCycle, timeline,
                             quantumAt(p, S));
            to.previousLive = true;
        } else {
            to.player = pl;
            to.anchor = to.previous->anchor;
            to.unanchored = to.previous->unanchored;
            to.until = to.previous->until;
            to.hold = to.previous->hold;
        }
    }
}

void carryLaunchReaders(Node& old, Node& n) SUPPLE_RT {
    for (std::size_t k = 0; k < n.launchReaders.size(); ++k) {
        PlayerReader& to = n.launchReaders[k];
        PlayerReader& from = old.launchReaders[k];
        // Only a stretch converter has history to carry.
        if (!to.state || !from.state || !to.state->preserve || !from.state->preserve) continue;
        to.state.swap(from.state);
        // A previous plan's reading still heard (S3 step D4) goes along.
        to.previous.swap(from.previous);
        to.previousLive = from.previousLive;
        to.previousOnly = from.previousOnly;
        if (to.previous && to.previous->player) {
            const Id was = to.previous->player->clip;
            to.previous->player = nullptr;
            if (const int i = playerOf(n, was); i >= 0)
                to.previous->player = &n.players[static_cast<std::size_t>(i)];
            else
                for (const ClipPlayer& r : n.retiring)
                    if (r.clip == was) to.previous->player = &r;
        }
        to.until = from.until;
        to.hold = from.hold;
        to.anchor = from.anchor;
        to.pass = from.pass;
        to.unanchored = from.unanchored;
        to.startQuantum = from.startQuantum;
        to.launchCycle = from.launchCycle;
        to.primes = from.primes;
        to.credit = from.credit;
        // Its player in the new plan, by clip (a retiring one too).
        to.player = nullptr;
        if (!from.player) continue;
        const Id clip = from.player->clip;
        if (const int i = playerOf(n, clip); i >= 0)
            to.player = &n.players[static_cast<std::size_t>(i)];
        else
            for (const ClipPlayer& r : n.retiring)
                if (r.clip == clip) to.player = &r;
    }
}

ClipChainInput clipChainInput(const Node& n, int player) SUPPLE_RT {
    ClipChainInput in;
    if (player < 0) return in;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    const LaunchState& L = n.launch;
    const bool current = L.kind == LaunchState::Kind::Clip && L.player == player;
    std::int64_t until = INT64_MAX, hold = INT64_MAX;
    if (n.launchNextValid) {
        const LaunchAction& a = n.launchNext;
        // Coming in (or relaunched): from its new anchor.
        if (a.kind == LaunchState::Kind::Clip && a.player == player)
            return {true, a.anchorSample, a.anchorBeat, false};
        // Going out: its notes end where the switch is heard; its audio reads
        // on through the switch's crossfade, as launchAudio's does, and on the
        // playrate path nothing past its end (S3 B).
        if (pl.midi) return in;
        until = current ? outgoingEnd(n, player, L.anchorSample, a.anchorSample)
                        : a.anchorSample + n.launchFadeFrames;
        hold = a.anchorSample;
    }
    if (current) return {true, L.anchorSample, L.anchorBeat, L.unanchored, until, hold};
    if (!pl.midi && L.fadeFrom == player)
        return {true,
                L.fadeAnchorSample,
                L.fadeAnchorBeat,
                false,
                outgoingEnd(n, player, L.fadeAnchorSample, L.fadeStart),
                L.fadeStart};
    return in;
}

bool clipChainHeard(const Node& n, int player, double clock) SUPPLE_RT {
    const LaunchState& L = n.launch;
    if (player < 0) return false;
    if (L.kind == LaunchState::Kind::Clip && L.player == player) return true;
    return L.fadeFrom == player &&
           clock < double(outgoingEnd(n, player, L.fadeAnchorSample, L.fadeStart));
}

double launchWeight(const Node& n, int player, double clock) SUPPLE_RT {
    const LaunchState& L = n.launch;
    const std::int64_t F = n.launchFadeFrames;
    const bool fading = L.fadeFrom != -2 && clock < double(L.fadeStart + F);
    const double x = fading ? std::clamp((clock - double(L.fadeStart)) / double(F), 0.0, 1.0) : 1.0;
    double w = 0;
    // Coming in with a fade-in, or going out inside its fade-out, the clip's
    // own fade stands in for the switch's (Q4 rules 3 and 4).
    if (L.kind == LaunchState::Kind::Clip && L.player == player) w += incomingStandsIn(n) ? 1.0 : x;
    if (L.fadeFrom == player && player >= 0) {
        const std::int64_t end = outgoingEnd(n, player, L.fadeAnchorSample, L.fadeStart);
        if (end != L.fadeStart + F)
            w += clock < double(end) ? 1.0 : 0.0;
        else if (fading)
            w += 1.0 - x;
    }
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

void clipChainAudio(const Plan& p, const Node& n, TakeChain& tc, const ClipChainInput& in,
                    std::int64_t pos, std::int64_t lead, const LoopRange* loop,
                    std::uint32_t frames, Stereo& into) SUPPLE_RT {
    if (!in.on || tc.player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(tc.player)];
    if (p.rateEnabled) {  // `lead` device frames ahead, through its reader (S3)
        playerRate(
            p, n, tc.clipReader, pl, in.anchorSample, in.unanchored, pos, lead, frames,
            [](std::uint32_t) { return 1.0; }, into, true, in.until, in.hold);
        return;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::int64_t t = 0;
        const bool folded = foldInput(loop, pos, lead, i, t);
        playerFrame(pl, in.anchorSample, in.unanchored || folded, t - n.timelineShift, 1.0, into, i,
                    true, in.hold);
    }
}

void clipChainMidi(const Plan& p, const Node& n, int player, const ClipChainInput& in,
                   std::int64_t pos, std::int64_t lead, const LoopRange* loop, std::uint32_t frames,
                   Stage& target, ClipWindow from) SUPPLE_RT {
    if (!in.on || player < 0) return;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    auto& events = target.blockEvents;
    LaunchState L;
    L.kind = LaunchState::Kind::Clip;
    L.player = player;
    L.anchorSample = in.anchorSample;
    L.anchorBeat = in.anchorBeat;
    L.unanchored = in.unanchored;
    if (p.rateEnabled) {  // the input side, `lead` device frames ahead (S3)
        const PlayrateTimeline& tl = *p.timelines[n.live ? 1 : 0];
        rateClipMidi(p, tl, &pl, L, pos + lead - n.timelineShift, frames,
                     tl.cycle(tl.phase(pos - n.timelineShift)), from, target);
        return;
    }
    // Where the input folds over the loop within this block (frames: none).
    std::uint32_t fold = frames;
    if (loop && loop->active() && pos < loop->end && pos + frames + lead > loop->end)
        fold = static_cast<std::uint32_t>(std::max<std::int64_t>(0, loop->end - lead - pos));
    const auto window = [&](std::uint32_t first, std::uint32_t to, bool folded) SUPPLE_RT {
        if (to <= first) return;
        std::int64_t t = 0;
        foldInput(loop, pos, lead, first, t);
        L.unanchored = in.unanchored || folded;
        const std::size_t before = events.size();
        clipMidi(p, pl, L, t - n.timelineShift, to - first, events);
        for (std::size_t k = before; k < events.size(); ++k) events[k].time += first;
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

bool clipChainTicks(const Plan& p, const Node& n, int player, double song,
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
        const auto q = static_cast<std::int64_t>(std::floor(song - double(anchorSample)));
        ticks = (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
        return true;
    }
    if (!(pl.periodBeats > 0)) return false;
    const time::TimeMap& tm = *p.timeMap;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const double beat = tm.beatAt((song + double(origin)) / p.rate);
    const double base =
        anchorBeat + std::floor((beat - anchorBeat) / pl.periodBeats) * pl.periodBeats;
    ticks = std::llround((beat - base) * kClipBeatTicks);
    return true;
}

double clipChainFade(const Node& n, int player, double song) SUPPLE_RT {
    if (player < 0) return 1.0;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(player)];
    if (!clipFades(pl) || pl.periodFrames <= 0) return 1.0;
    const LaunchState& L = n.launch;
    std::int64_t anchor = 0;
    if (L.kind == LaunchState::Kind::Clip && L.player == player)
        anchor = L.anchorSample;
    else if (L.fadeFrom == player)
        anchor = L.fadeAnchorSample;
    else
        return 1.0;
    const auto q = static_cast<std::int64_t>(std::floor(song - double(anchor)));
    return clipFade(pl, song - double(anchor + passOf(pl, q) * pl.periodFrames));
}

bool clipLanePosition(const ClipLaneSource& c, std::uint32_t frame, std::int64_t& at) SUPPLE_RT {
    if (!c.in.on || c.player < 0) return false;
    const Node& n = *c.node;
    const ClipPlayer& pl = n.players[static_cast<std::size_t>(c.player)];
    const Plan& p = *c.plan;
    double song = 0;
    bool folded = false;
    if (p.rateEnabled) {
        // The song position its input reads (S3), folded when that is a
        // later loop pass than the output's.
        const PlayrateTimeline& tl = *p.timelines[n.live ? 1 : 0];
        const std::int64_t device = c.pos + frame + c.ahead - n.timelineShift;
        song = tl.position(device);
        folded = tl.cycle(tl.phase(device)) > tl.cycle(tl.phase(c.pos + frame - n.timelineShift));
    } else {
        std::int64_t t = 0;
        folded = foldInput(c.loop, c.pos, c.ahead, frame, t);
        song = double(t - n.timelineShift);
    }
    const bool before = (c.in.unanchored || folded) && !pl.once;
    if (!pl.midi) {
        if (pl.periodFrames <= 0) return false;
        const auto q = static_cast<std::int64_t>(std::floor(song - double(c.in.anchorSample)));
        if (q < 0 && !before) return false;
        at = (q % pl.periodFrames + pl.periodFrames) % pl.periodFrames;
        return true;
    }
    if (!(pl.periodBeats > 0)) return false;
    const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
    const double beat = p.timeMap->beatAt((song + double(origin)) / p.rate);
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
    if (!p.clipPlayers || !feed.source) return;
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
    const double blockBeats =
        double(p.block) / p.rate * fastest / 60.0 * (p.rateEnabled ? model::kMaxPlayrate : 1.0);
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
        // The feed takes the notes as the track sounds them (review round 86
        // H2): the same tables as its player (compileClipPlayers, which warns
        // of the budget).
        if (clip.follow != model::ClipFollow::None) givePlayer(pl, followPlayer(p, pl, clip, s));
        const auto [v, w] = midiSizing(pl, blockBeats);
        voices = std::max(voices, v);
        window = std::max(window, w);
        feed.players.push_back(std::move(pl));
    }
    feed.clipEvents.reserve(window + 1);
    feed.block.reserve(feed.block.capacity() + window + 1);
    if (p.rateEnabled) {  // the playrate path plays them through its stage (feedRateMidi)
        Stage& st = feed.midiStage;
        st.blockEvents.reserve(st.blockEvents.capacity() + window + voices + 1);
        st.voices.reserve(st.voices.capacity() + voices);
        st.pendingVoices.reserve(st.pendingVoices.capacity() + voices);
    }
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
    feed.launchFresh = true;
}

void feedRateMidi(Plan& p, const Node& rack, NotesFeed& feed, std::int64_t pos,
                  std::uint32_t frames) SUPPLE_RT {
    const LaunchState& L = feed.launch;
    const ClipPlayer* pl = L.kind == LaunchState::Kind::Clip && L.player >= 0
                               ? &feed.players[static_cast<std::size_t>(L.player)]
                               : nullptr;
    const PlayrateTimeline& tl = *p.timelines[rack.live ? 1 : 0];
    const std::int64_t dev0 = pos - rack.timelineShift;
    const ClipWindow from = feed.launchFresh ? ClipWindow::Anchor
                            : feed.rateFresh ? ClipWindow::Here
                                             : ClipWindow::After;
    feed.launchFresh = false;
    feed.rateFresh = false;
    rateClipMidi(p, tl, pl, L, dev0, frames, tl.cycle(tl.phase(dev0)), from, feed.midiStage);
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
