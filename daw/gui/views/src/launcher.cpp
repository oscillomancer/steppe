// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/launcher.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <utility>

#include "supple/harmony/harmony.h"
#include "supple/model/context.h"
#include "supple/model/follow.h"
#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/views/arrange.h"
#include "supple/views/item_body.h"
#include "supple/views/lanes.h"

namespace supple::views {

std::optional<std::size_t> LauncherColumns::columnAt(float px) const {
    if (px < x || px >= scenesRight()) return std::nullopt;
    const float offset = px - x + scroll;
    if (offset < 0) return std::nullopt;
    const auto column = static_cast<std::size_t>(offset / kColumn);
    if (column >= scenes.size()) return std::nullopt;
    return column;
}

LauncherColumns launcherColumns(const model::Session& session) {
    LauncherColumns out;
    std::vector<std::pair<int, model::Id>> order;
    for (const auto& e : session.scenes) order.emplace_back(e.value->order, e.id);
    std::sort(order.begin(), order.end());
    for (const auto& [o, id] : order) out.scenes.push_back(id);
    return out;
}

int barAtOrAfter(const time::TimeMap& map, double beat) {
    const time::BarBeat at = map.barBeat(std::max(0.0, beat));
    return map.beatOfBar(at.bar) < beat - 1e-9 ? at.bar + 1 : at.bar;
}

namespace {

// "+2", "−3": semitones as the status line says them.
std::string semitones(std::int64_t n) {
    return (n < 0 ? "\u2212" : "+") + std::to_string(n < 0 ? -n : n);
}

std::string jsonText(const json::Value* v) {
    return v && v->isString() ? v->asString() : std::string();
}

// A laid clip's name for the status line: its own, else its track's.
std::string laidName(const model::Session& s, const std::string& clipId) {
    const auto id = model::Id::parse(clipId);
    const model::Clip* clip = id ? s.clips.find(*id) : nullptr;
    if (!clip) return "The clip";
    if (!clip->name.empty()) return clip->name;
    if (const model::Track* t = s.tracks.find(clip->track); t && !t->name.empty()) return t->name;
    return "The clip";
}

enum class Laid { Quiet, Followed, Copied };

// One laid clip's phrase, and whether it was mapped or copied in another key.
std::pair<Laid, std::string> laidPhrase(const std::string& name, const json::Value* followed) {
    if (!followed || !followed->isObject()) return {Laid::Quiet, {}};
    const std::string mode = jsonText(followed->find("mode"));
    if (mode == "key" || mode == "chords") {
        // Each run of bars the notes moved over: its key, and how far.
        std::vector<std::string> runs;
        const json::Value* regions = followed->find("regions");
        if (regions && regions->isArray())
            for (const json::Value& r : regions->asArray()) {
                const std::string by = jsonText(r.find("by"));
                // A run already in the key moved nothing: a copy, a
                // transpose by 0, or degrees between one key and itself.
                const json::Value* iv = r.find("interval");
                if (by == "copy" || (by == "transpose" && iv && iv->asInt() == 0) ||
                    (by == "degree" && jsonText(r.find("from_key")) == jsonText(r.find("to_key"))))
                    continue;
                std::string run = jsonText(r.find("to_key"));
                if (run.empty()) run = "the chords";
                if (by == "transpose" && iv)
                    run += ", " + semitones(iv->asInt());
                else if (by == "degree")
                    run += " by degree";
                else if (by == "chords")
                    run += " by chord";
                if (!runs.empty())
                    if (const json::Value* from = r.find("from_bar"))
                        run += " from bar " + std::to_string(from->asInt());
                runs.push_back(std::move(run));
            }
        if (runs.empty()) return {Laid::Quiet, {}};  // already in the key: nothing moved
        std::string text = name + " followed to " + runs[0];
        if (runs.size() > 1) text += ", then " + runs[1];
        if (runs.size() > 2) text += ", and " + std::to_string(runs.size() - 2) + " more";
        if (const json::Value* folded = followed->find("folded"); folded && folded->asInt() > 0)
            text += " (" + std::to_string(folded->asInt()) + " folded into range)";
        return {Laid::Followed, std::move(text)};
    }
    if (const json::Value* m = followed->find("mismatch"); m && m->isObject())
        return {Laid::Copied, name + " copied: its key " + jsonText(m->find("clip_key")) +
                                  " differs from " + jsonText(m->find("key")) + " at bar " +
                                  std::to_string(m->find("bar") ? m->find("bar")->asInt() : 1)};
    const std::string copied = jsonText(followed->find("copied"));
    if (copied == "drums") return {Laid::Copied, name + " copied: drums don't follow"};
    if (copied == "audio") return {Laid::Copied, name + " copied: audio doesn't follow"};
    if (copied == "no_key") return {Laid::Copied, name + " copied: it has no key to follow from"};
    if (copied == "no_notes") return {Laid::Copied, name + " copied: it has no notes to move"};
    return {Laid::Quiet, {}};
}

}  // namespace

std::optional<std::string> followNotice(const model::Session& s, const std::string& command,
                                        const json::Value& args, const json::Value& answer) {
    if (!answer.isObject()) return std::nullopt;
    if (command == "item.add") {
        const std::string clip = jsonText(args.find("clip"));
        if (clip.empty()) return std::nullopt;
        auto [laid, text] = laidPhrase(laidName(s, clip), answer.find("followed"));
        if (laid == Laid::Quiet) return std::nullopt;
        return text;
    }
    std::vector<std::string> phrases, followed, copied;
    std::string head;
    if (command == "launch.commit") {
        // A capture's commit copies what was heard: each launched stretch
        // copied under a key that is not its clip's, as `mismatches` says.
        const json::Value* list = answer.find("mismatches");
        if (!list || !list->isArray()) return std::nullopt;
        for (const json::Value& m : list->asArray()) {
            json::Value followedBlock = json::Value::object();
            followedBlock["mode"] = "none";
            followedBlock["mismatch"] = m;
            const std::string name = laidName(s, jsonText(m.find("clip")));
            auto [laid, text] = laidPhrase(name, &followedBlock);
            if (laid == Laid::Quiet) continue;
            phrases.push_back(std::move(text));
            copied.push_back(name);
        }
        head = "Committed";
    } else if (command == "scene.insert") {
        const json::Value* items = answer.find("items");
        if (!items || !items->isArray()) return std::nullopt;
        for (const json::Value& item : items->asArray()) {
            const std::string name = laidName(s, jsonText(item.find("clip")));
            auto [laid, text] = laidPhrase(name, item.find("followed"));
            if (laid == Laid::Quiet) continue;
            phrases.push_back(std::move(text));
            (laid == Laid::Followed ? followed : copied).push_back(name);
        }
        head = "The scene";
        if (const auto id = model::Id::parse(jsonText(args.find("scene"))))
            if (const model::Scene* sc = s.scenes.find(*id); sc && !sc->name.empty())
                head = sc->name;
        if (const json::Value* bar = args.find("bar"); bar && bar->isNumber())
            head += " at bar " + std::to_string(bar->asInt());
    } else {
        return std::nullopt;
    }
    if (phrases.empty()) return std::nullopt;
    std::string all;
    for (const std::string& p : phrases) all += (all.empty() ? "" : "; ") + p;
    if (all.size() <= 110) return head + ": " + all;
    // Too long for the line: how many followed and how many were copied in
    // another key or for a reason, by name.
    auto names = [](const std::vector<std::string>& v) {
        std::string out;
        for (std::size_t i = 0; i < v.size() && i < 3; ++i) out += (i ? ", " : "") + v[i];
        if (v.size() > 3) out += ", \u2026";
        return out;
    };
    std::string text = head + ":";
    if (!followed.empty())
        text +=
            " " + std::to_string(followed.size()) + " followed the key (" + names(followed) + ")";
    if (!copied.empty())
        text += std::string(followed.empty() ? " " : "; ") + std::to_string(copied.size()) +
                " copied as written (" + names(copied) + ")";
    return text;
}

namespace {

// A key point's words, short (a launcher cell, an item's name bar): "Dm",
// "G", "D dor", "A hm"; "" when the point is missing or unreadable.
std::string shortKey(const model::ContextPoint* p) {
    const auto key = p ? model::parseKey(*p) : std::nullopt;
    if (!key) return {};
    using S = harmony::Scale;
    std::string text = harmony::printPitchClass(key->tonic);
    switch (key->scale) {
        case S::Major:
            break;
        case S::Minor:
            text += "m";
            break;
        case S::HarmonicMinor:
            text += " hm";
            break;
        case S::MelodicMinor:
            text += " mm";
            break;
        case S::MajorPentatonic:
            text += " pent";
            break;
        case S::MinorPentatonic:
            text += "m pent";
            break;
        default:  // dorian, phrygian, ...: the mode's first three letters
            text += " " + std::string(harmony::scaleName(key->scale).substr(0, 3));
    }
    return text;
}

}  // namespace

std::string clipKeyMark(const model::Clip& clip) {
    const std::string text = shortKey(model::keyPointAt(clip, 0));
    if (text.empty()) return {};
    return (clip.follow != model::ClipFollow::None ? std::string(kFollowMark) : std::string()) +
           text;
}

std::string takeFollowMark(const model::Take& take) {
    if (take.follow == model::ClipFollow::None) return {};
    // The take's key where its content starts: its first, before any point.
    const std::string text = shortKey(
        model::contextKeyPointAt(take.context, model::contextBeatOf(take, take.startOffset)));
    return std::string(kFollowMark) + text;
}

bool launchRefused(const model::Session& s, const model::Clip& clip) {
    // launch_runtime.cpp:323: plan->rateStretch && the clip is not MIDI.
    if (clip.kind == model::ClipKind::Midi || !s.project) return false;
    const model::Playrate& rate = s.project->playrate;
    return rate.preservePitch && (rate.rate != 1 || model::activePlayrateEnvelope(s));
}

bool sceneLaunchRefused(const model::Session& s, model::Id scene) {
    for (const auto& e : s.clips) {
        if (e.value->scene != scene) continue;
        const model::Track* track = s.tracks.find(e.value->track);
        if (track && !track->freeze && launchRefused(s, *e.value)) return true;
    }
    return false;
}

int barAfterArrangement(const model::Session& s) {
    double end = 0;
    for (const auto& e : s.items) end = std::max(end, model::itemEndBeat(s, *e.value));
    for (const auto& e : s.regions) {
        const model::Region& rg = *e.value;
        if (!rg.section) continue;
        end = std::max(end,
                       rg.timebase == model::Timebase::Time ? s.timeMap->beatAt(rg.end) : rg.end);
    }
    return barAtOrAfter(*s.timeMap, end);
}

std::int64_t sceneBars(const model::Session& s, model::Id scene, int bar) {
    constexpr std::int64_t kMost = 100000;  // scene.insert's bars
    const time::TimeMap& map = *s.timeMap;
    const double start = map.beatOfBar(bar);
    double end = start;
    for (const auto& e : s.clips) {
        const model::Clip& clip = *e.value;
        if (clip.scene != scene) continue;
        // A MIDI clip's length is in beats, an audio clip's in seconds.
        end = std::max(end, clip.kind == model::ClipKind::Midi
                                ? start + clip.length
                                : map.beatAt(map.secondsAt(start) + clip.length));
    }
    std::int64_t bars = 1;
    while (bars < kMost && map.beatOfBar(bar + static_cast<int>(bars)) < end - 1e-9) ++bars;
    return bars;
}

std::optional<model::Id> clipIn(const model::Session& session, model::Id track, model::Id scene) {
    for (const auto& e : session.clips)
        if (e.value->track == track && e.value->scene == scene) return e.id;
    return std::nullopt;
}

ui::Color clipColor(const ui::Theme& theme, const model::Session& session,
                    const model::Clip& clip) {
    if (const auto own = theme.trackColor(clip.color)) return *own;
    if (const model::Track* t = session.tracks.find(clip.track))
        if (const auto own = theme.trackColor(t->color)) return *own;
    return theme.color("channel");
}

namespace {

// The ▶ of a launch zone, filled, in `box`.
void drawPlay(ui::Canvas& c, const ui::Rect& box, ui::Color color) {
    const float s = std::min(box.w, box.h) * 0.5f;
    const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    c.fillPath(
        {{cx - s * 0.45f, cy - s * 0.55f}, {cx + s * 0.55f, cy}, {cx - s * 0.45f, cy + s * 0.55f}},
        color);
}

// Relative luminance (WCAG 2), and the contrast ratio of two colours.
float luminance(ui::Color c) {
    auto lin = [](float v) {
        return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
float contrast(ui::Color a, ui::Color b) {
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

// The ▶ of something playing: in `playing` on a backing of the theme's ink
// or ground, whichever it reads on better, so it shows at a glance on any
// clip colour and in either theme.
void drawPlaying(ui::Canvas& c, const ui::Rect& box) {
    const ui::Theme& t = c.theme();
    const ui::Color on = t.color("playing");
    const ui::Color ink = t.color("ink"), ground = t.color("ground");
    const ui::Color backing = contrast(on, ink) >= contrast(on, ground) ? ink : ground;
    const float s = std::min({box.w, box.h, 13.0f});
    c.fillRound({std::round(box.x + (box.w - s) / 2), std::round(box.y + (box.h - s) / 2), s, s}, 2,
                backing);
    drawPlay(c, box, on);
}

// A dashed outline (a queued launch): dashes of 4 with gaps of 3.
void strokeDashed(ui::Canvas& c, const ui::Rect& r, ui::Color color) {
    auto run = [&](ui::Point a, ui::Point b) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        for (float d = 0; d < len; d += 7) {
            const float e = std::min(len, d + 4);
            c.line({a.x + (b.x - a.x) * d / len, a.y + (b.y - a.y) * d / len},
                   {a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len}, color, 1.5f);
        }
    };
    run({r.x, r.y}, {r.right(), r.y});
    run({r.right(), r.y}, {r.right(), r.bottom()});
    run({r.right(), r.bottom()}, {r.x, r.bottom()});
    run({r.x, r.bottom()}, {r.x, r.y});
}

}  // namespace

bool hasClips(const model::Session& session, model::Id track) {
    for (const auto& e : session.clips)
        if (e.value->track == track) return true;
    return false;
}

// Whether every clip of a scene plays now (its header is outlined).
bool scenePlaying(const model::Session& session, const LaunchViews& views, model::Id scene) {
    bool any = false;
    for (const auto& e : session.clips) {
        if (e.value->scene != scene) continue;
        const auto it = views.find(e.value->track);
        if (it == views.end() || it->second.plays != LaunchView::Plays::Clip ||
            it->second.clip != e.id)
            return false;
        any = true;
    }
    return any;
}

// ----------------------------------------------------------------- the pane

LauncherPane::LauncherPane(ArrangeView& view, peaks::PeakService& peaks, Resolve resolve)
    : Node("launcher"), view_(view), peaks_(peaks), resolve_(std::move(resolve)) {
    interactive = true;
    clipsChildren = true;
    drawsGl = true;
}

float LauncherPane::keyStrip(const ui::Rect& body, const std::string& keyMark) {
    // A body too low to keep a preview over it shows no key.
    if (keyMark.empty() || body.h < 2 * kKeyStrip) return 0;
    return kKeyStrip;
}

ui::Rect LauncherPane::bodyRect(const ui::Rect& cell) {
    const ui::Rect inner{cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 5};
    const float bar = std::min(Lanes::kNameBar, inner.h);
    return {inner.x + 1, inner.y + bar + 1, inner.w - 2, std::max(0.0f, inner.h - bar - 2)};
}

LauncherColumns LauncherPane::columns() const {
    LauncherColumns cols = launcherColumns(view_.session());
    cols.x = bounds().x;
    cols.w = bounds().w;
    cols.scroll = view_.launcherScroll();
    return cols;
}

ui::Rect LauncherPane::cellRect(std::size_t row, std::size_t column) const {
    const TrackLayout& tracks = view_.trackLayout();
    const LauncherColumns cols = columns();
    return {cols.columnX(column), bounds().y + tracks.yOf(row) - view_.viewport().scrollY,
            LauncherColumns::kColumn, tracks.heightOf(row)};
}

ui::Rect LauncherPane::playZone(const ui::Rect& cell) {
    return {cell.x + 2, cell.y + 2, 16, std::min(Lanes::kNameBar, cell.h - 5)};
}

ui::Rect LauncherPane::stopZone(const ui::Rect& cell) {
    const float k = 13;  // the square is 7; the zone gives it room
    return {cell.x + (cell.w - k) / 2, cell.y + (cell.h - k) / 2, k, k};
}

LauncherPane::Hit LauncherPane::hitAt(ui::Point p) const {
    Hit hit;
    if (!bounds().contains(p)) return hit;
    const TrackLayout& tracks = view_.trackLayout();
    const LauncherColumns cols = columns();
    for (std::size_t i = 0; i < tracks.rowCount(); ++i) {
        const ui::Rect r = arrRect(i);
        if (p.y < r.y || p.y >= r.bottom()) continue;
        const TrackRow& row = tracks.rows()[i];
        if (row.lane() || row.type != model::StripType::Channel) return hit;
        hit.row = i;
        hit.track = row.track;
        if (p.x >= cols.arrX()) {
            hit.kind = Hit::Kind::Arr;
            return hit;
        }
        const auto col = cols.columnAt(p.x);
        if (!col) return hit;
        hit.scene = cols.scenes[*col];
        hit.clip = clipIn(view_.session(), row.track, *hit.scene);
        const ui::Rect cell = cellRect(i, *col);
        if (hit.clip)
            hit.kind = playZone(cell).contains(p) ? Hit::Kind::Play : Hit::Kind::Body;
        else
            hit.kind = stopZone(cell).contains(p) && hasClips(view_.session(), row.track)
                           ? Hit::Kind::Stop
                           : Hit::Kind::Cell;
        return hit;
    }
    return hit;
}

void LauncherPane::showDrop(std::optional<ui::Point> at) {
    if (!at && !dropAt_) return;
    dropAt_ = at;
    invalidate();
}

std::optional<ItemGestures::ClipGhost> LauncherPane::timelineGhost(model::Id clip, ui::Point p,
                                                                   bool free) const {
    const model::Session& s = view_.session();
    const model::Clip* c = s.clips.find(clip);
    if (!c || !view_.lanesRect().contains(p)) return std::nullopt;
    const TrackLayout& tracks = view_.trackLayout();
    for (std::size_t i = 0; i < tracks.rowCount(); ++i) {
        const ui::Rect r = view_.rowRect(i);
        if (p.y < r.y || p.y >= r.bottom()) continue;
        const TrackRow& row = tracks.rows()[i];
        if (row.lane() || row.type != model::StripType::Channel) return std::nullopt;
        const double start = view_.gestures().snappedBeatAt(p.x, free);
        // A MIDI clip's length is in beats, an audio clip's in seconds.
        const double end = c->kind == model::ClipKind::Midi
                               ? start + c->length
                               : s.timeMap->beatAt(s.timeMap->secondsAt(start) + c->length);
        return ItemGestures::ClipGhost{row.track, start, end};
    }
    return std::nullopt;
}

bool LauncherPane::dropAllowed(const Hit& over) const {
    return over.scene && !over.clip && over.kind == Hit::Kind::Cell;
}

ui::Rect LauncherPane::arrRect(std::size_t row) const {
    const TrackLayout& tracks = view_.trackLayout();
    const LauncherColumns cols = columns();
    return {cols.arrX(), bounds().y + tracks.yOf(row) - view_.viewport().scrollY,
            LauncherColumns::kArr, tracks.heightOf(row)};
}

void LauncherPane::paint(ui::Canvas& c) {
    stats = {};
    const ui::Theme& t = c.theme();
    theme_ = &t;
    const ui::Rect b = bounds();
    const model::Session& s = view_.session();
    const TrackLayout& tracks = view_.trackLayout();
    const LauncherColumns cols = columns();
    c.fillRect(b, t.color("ground"));
    const float caption = t.type("caption");
    // A clip the engine would refuse to launch (an audio clip under a
    // pitch-keeping master playrate) draws as a muted one does, and a click's
    // refusal says why in the status line.
    stats.refused = 0;
    c.save();
    c.clip(b);
    for (std::size_t i = 0; i < tracks.rowCount(); ++i) {
        const TrackRow& row = tracks.rows()[i];
        const ui::Rect r = arrRect(i);
        if (r.bottom() <= b.y) continue;
        if (r.y >= b.bottom()) break;
        c.fillRect({b.x, r.bottom() - 1, b.w, 1}, t.color("rule"));
        // Only a channel's own row has cells (clips are on channels only).
        if (row.lane() || row.type != model::StripType::Channel) continue;
        const bool rowHasClips = hasClips(s, row.track);
        c.save();
        c.clip({b.x, r.y, cols.scenesRight() - b.x, r.h});
        for (std::size_t col = 0; col < cols.scenes.size(); ++col) {
            const ui::Rect cell = cellRect(i, col);
            if (cell.right() <= b.x || cell.x >= cols.scenesRight()) continue;
            ++stats.cells;
            const ui::Rect inner{cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 5};
            const auto id = clipIn(s, row.track, cols.scenes[col]);
            const model::Clip* clip = id ? s.clips.find(*id) : nullptr;
            if (!clip) {
                // The stop zone: a small square in quiet ink, nothing else, on
                // a track with clips (one without always follows the
                // arrangement, so there is nothing to stop).
                ++stats.empty;
                const auto& sel = view_.selectedCell();
                if (sel && sel->track == row.track && sel->scene == cols.scenes[col]) {
                    c.strokeRect(inner, t.color("focus"), 2);
                    ++stats.selected;
                }
                if (rowHasClips) {
                    const float k = 7;
                    c.fillRect({cell.x + (cell.w - k) / 2, cell.y + (cell.h - k) / 2, k, k},
                               t.color("ink-quiet").withAlpha(0.6f));
                    ++stats.squares;
                }
                continue;
            }
            ++stats.clips;
            const auto state = view_.launchState().find(row.track);
            const LaunchView* lv = state != view_.launchState().end() ? &state->second : nullptr;
            const bool playing = lv && lv->plays == LaunchView::Plays::Clip && lv->clip == id;
            const bool held = lv && lv->plays == LaunchView::Plays::Held && lv->clip == id;
            const bool queued =
                lv && lv->queued == LaunchView::Queued::Clip && lv->queuedClip == id;
            const ui::Color color = clipColor(t, s, *clip);
            const bool refused = launchRefused(s, *clip);
            stats.refused += refused;
            const float dim = clip->mute || refused ? 0.45f : 1.0f;
            c.fillRound(inner, 2, color.withAlpha(0.24f * dim));
            c.strokeRound(inner, 2, color.withAlpha(0.8f * dim));
            const float bar = std::min(Lanes::kNameBar, inner.h);
            const ui::Rect name{inner.x, inner.y, inner.w, bar};
            c.fillRect({name.x + 1, name.y + 1, name.w - 2, name.h - 1},
                       color.withAlpha(0.38f * dim));
            if (playing)
                drawPlaying(c, {name.x + 1, name.y, 14, name.h});
            else
                drawPlay(c, {name.x + 1, name.y, 14, name.h}, t.color("ink").withAlpha(dim));
            // The clip's own key in a strip at the foot of the body, under the
            // preview (keyStrip), right-aligned, kFollowMark before it when it follows.
            const std::string keyMark = clipKeyMark(*clip);
            const ui::Rect body = bodyRect(cell);
            const float strip = keyStrip(body, keyMark);
            if (strip > 0)
                c.text({body.x + 2, body.bottom() - strip, body.w - 4, strip}, keyMark,
                       ui::Font::Ui, caption,
                       t.color(clip->follow != model::ClipFollow::None ? "ink" : "ink-quiet")
                           .withAlpha(dim),
                       ui::kRight | ui::kMiddle);
            stats.keys.push_back(strip > 0 ? keyMark : std::string());
            std::string label = clip->name.empty() ? id->str() : clip->name;
            const std::string fit =
                c.elide(label, ui::Font::Ui, caption, std::max(0.0f, name.w - 18));
            if (!fit.empty())
                c.text({name.x + 15, name.y, name.w - 16, name.h}, fit, ui::Font::Ui, caption,
                       t.color("ink").withAlpha(dim), ui::kLeft | ui::kMiddle);
            stats.names.push_back(fit);
            // What plays: the outline (and a progress line while it plays) in
            // `playing`; a queued launch dashed until its boundary.
            if (playing || held) {
                c.strokeRound(inner, 2, t.color("playing"), 2);
                ++(playing ? stats.playing : stats.held);
            }
            if (playing && lv->position && clip->length > 0) {
                const double f = std::clamp(*lv->position / clip->length, 0.0, 1.0);
                c.fillRect(
                    {inner.x + 1, inner.bottom() - 3, static_cast<float>(f) * (inner.w - 2), 2},
                    t.color("playing"));
            }
            if (queued) {
                strokeDashed(c, inner.inset(-1), t.color("playing"));
                ++stats.queued;
            }
            const auto& picked = view_.selectedClips();
            if (std::find(picked.begin(), picked.end(), *id) != picked.end()) {
                c.strokeRect(inner.inset(-1), t.color("focus"), 2);
                if (inner.w > 8 && inner.h > 8) c.strokeRect(inner.inset(1), t.color("ground"), 1);
                ++stats.selected;
            }
        }
        c.restore();
        // The Arr dot: lit while the track follows the arrangement.
        const auto state = view_.launchState().find(row.track);
        const bool follows = state == view_.launchState().end() ||
                             state->second.plays == LaunchView::Plays::Arrangement;
        const ui::Point dot{r.x + r.w / 2, r.y + std::min(r.h / 2, 12.0f)};
        if (follows) {
            c.fillCircle(dot, 3.5f, t.color("playing"));
            ++stats.arrLit;
        } else {
            c.strokeCircle(dot, 3.5f, t.color("ink-faint"));
        }
    }
    // A clip in hand, or an item from the lanes: where it would land, or
    // that the cell refuses it.
    std::optional<Hit> over;
    if (drag_ && drag_->moving && drag_->over.scene) over = drag_->over;
    if (dropAt_) over = hitAt(*dropAt_);
    if (over && over->scene) {
        const auto col = std::find(cols.scenes.begin(), cols.scenes.end(), *over->scene);
        if (col != cols.scenes.end()) {
            const ui::Rect cell = cellRect(over->row, col - cols.scenes.begin());
            c.strokeRect(cell.inset(2),
                         dropAllowed(*over) ? t.color("focus") : t.color("meter-clip"), 1.5f);
            ++stats.dropShown;
        }
    }
    // The Arr column's rule, and the pane's edge against the lanes.
    c.fillRect({cols.arrX(), b.y, 1, b.h}, t.color("rule"));
    c.fillRect({b.right() - 1, b.y, 1, b.h}, t.color("rule"));
    c.restore();
}

void LauncherPane::paintGl(const ui::GlFrame& frame) {
    if (glFailed_ || !theme_) return;
    if (!notes_) {
        auto n = ui::RectRenderer::create();
        auto s = ui::StripRenderer::create();
        if (!n || !s) {
            glFailed_ = true;
            return;
        }
        notes_.emplace(std::move(*n));
        strips_.emplace(std::move(*s));
    }
    const ui::Theme& t = *theme_;
    const model::Session& s = view_.session();
    const TrackLayout& tracks = view_.trackLayout();
    const LauncherColumns cols = columns();
    const ui::Rect clip = frame.clip.intersect(bounds());
    stats.notes = stats.columns = stats.pending = 0;
    const std::uint32_t border = ui::packRgba(t.color("ground").withAlpha(0.7f));
    for (std::size_t i = 0; i < tracks.rowCount(); ++i) {
        const TrackRow& row = tracks.rows()[i];
        const ui::Rect rr = arrRect(i);
        if (rr.bottom() <= clip.y) continue;
        if (rr.y >= clip.bottom()) break;
        if (row.lane() || row.type != model::StripType::Channel) continue;
        for (std::size_t col = 0; col < cols.scenes.size(); ++col) {
            ui::Rect body = bodyRect(cellRect(i, col));
            const auto id = clipIn(s, row.track, cols.scenes[col]);
            const model::Clip* c = id ? s.clips.find(*id) : nullptr;
            if (!c || c->length <= 0 || !c->activeTake) continue;
            body.h -= keyStrip(body, clipKeyMark(*c));  // the key's strip, under the preview
            // Cut at the Arr column; a cell scrolled out of view is skipped.
            body.w = std::min(body.right(), cols.scenesRight()) - body.x;
            if (body.w <= 2 || body.h < 4 || body.right() <= bounds().x) continue;
            const model::Take* take = s.takes.find(*c->activeTake);
            if (!take || !take->source) continue;
            const float dim = c->mute || launchRefused(s, *c) ? 0.45f : 1.0f;
            const ui::Color color = clipColor(t, s, *c);
            const ui::Rect full = bodyRect(cellRect(i, col));  // the period's span
            if (const model::MidiSource* src = s.midi.find(*take->source)) {
                MidiWindow window;
                window.xAt = [&](double beat) {
                    return full.x + static_cast<float>(beat / c->length) * full.w;
                };
                window.length = c->length;
                window.from = 0;
                window.to = c->length;
                window.end = body.right();
                window.loop = c->loop.on;
                stats.notes +=
                    drawMidiBody(*notes_, body, *take, *src, window,
                                 ui::packRgba(Lanes::noteColor(t, color).withAlpha(dim)), border);
            } else if (const model::MediaSource* m = s.media.find(*take->source)) {
                auto pyramid = peaks_.get(m->hash, resolve_(*m));
                if (!pyramid) {
                    ++stats.pending;  // the worker's delivery repaints
                    continue;
                }
                const AudioWindow window{
                    [&](float x) { return static_cast<double>((x - full.x) / full.w) * c->length; },
                    static_cast<double>(full.w) / c->length, c->loop.on};
                stats.columns += drawAudioBody(
                    *strips_, frame, body, *pyramid, *take, window,
                    static_cast<float>(std::pow(10.0, (c->gainDb + take->gainDb) / 20.0)),
                    ui::packRgba(t.color("ink").withAlpha(0.8f * dim)));
            }
        }
    }
    strips_->flush(frame);
    notes_->flush(frame);
}

bool LauncherPane::onPointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    if (e.type == T::Wheel) return false;  // the view scrolls the rows
    if (e.type == T::Down && e.button == 1 && e.pos.x >= bounds().right() - kEdge) {
        resizing_ = true;
        pressX_ = e.pos.x;
        pressWidth_ = bounds().w;
        if (root()) root()->capture(this);
        return true;
    }
    if (resizing_) {
        if (e.type == T::Move) view_.dragLauncherWidth(pressWidth_ + e.pos.x - pressX_);
        if (e.type == T::Up) {
            resizing_ = false;
            if (root()) root()->capture(nullptr);
        }
        return true;
    }
    if (drag_) {
        if (e.type == T::Move) {
            if (!drag_->moving &&
                std::hypot(e.pos.x - drag_->press.x, e.pos.y - drag_->press.y) > 4)
                drag_->moving = true;
            if (drag_->moving) {
                drag_->over = hitAt(e.pos);
                view_.gestures().showClipGhost(
                    timelineGhost(drag_->clip, e.pos, (e.mods & ui::kShift) != 0));
                invalidate();
            }
            return true;
        }
        if (e.type == T::Up) {
            const Drag d = *drag_;
            drag_.reset();
            if (root()) root()->capture(nullptr);
            view_.gestures().showClipGhost(std::nullopt);
            invalidate();
            if (!d.moving) return true;
            // Onto the timeline: a copy of the clip's takes as an item there
            // (item.add {clip}), snapped as an item drag (Shift: free).
            if (const auto ghost = timelineGhost(d.clip, e.pos, (e.mods & ui::kShift) != 0)) {
                json::Value args = json::Value::object();
                args["track"] = ghost->track.str();
                args["clip"] = d.clip.str();
                args["position"] = ghost->start;
                (void)view_.runLauncher("item.add", std::move(args));
                return true;
            }
            const Hit over = hitAt(e.pos);
            if (!dropAllowed(over)) return true;  // an occupied cell refuses it
            json::Value args = json::Value::object();
            args["clip"] = d.clip.str();
            args["track"] = over.track.str();
            args["scene"] = over.scene->str();
            const bool copy = (e.mods & ui::kCtrl) != 0;
            if (auto r = view_.runLauncher(copy ? "clip.duplicate" : "clip.move", std::move(args));
                r && copy)
                if (const json::Value* made = r->find("id"); made && made->isString())
                    if (auto id = model::Id::parse(made->asString())) view_.selectClips({*id});
            return true;
        }
        return true;
    }
    if (e.type != T::Down) return e.type == T::Up;  // the lanes' gestures never start here
    const Hit hit = hitAt(e.pos);
    if (e.button == 3) {
        if (hit.clip) {
            const auto& sel = view_.selectedClips();
            if (std::find(sel.begin(), sel.end(), *hit.clip) == sel.end())
                view_.selectClips({*hit.clip});
            openClipMenu(*hit.clip, e.pos);
        }
        return true;
    }
    if (e.button != 1) return true;
    json::Value args = json::Value::object();
    switch (hit.kind) {
        case Hit::Kind::Play:  // launch it; the selection stays
            args["clip"] = hit.clip->str();
            (void)view_.runLauncher("launch.clip", std::move(args));
            break;
        case Hit::Kind::Body:
            if (e.clicks >= 2) {
                view_.selectClips({*hit.clip});
                if (view_.launcherCallbacks().open) view_.launcherCallbacks().open(*hit.clip);
                break;
            }
            if (e.mods & ui::kCtrl) {  // Ctrl adds (or takes back) one
                std::vector<model::Id> sel = view_.selectedClips();
                const auto it = std::find(sel.begin(), sel.end(), *hit.clip);
                if (it != sel.end())
                    sel.erase(it);
                else
                    sel.push_back(*hit.clip);
                view_.selectClips(std::move(sel));
            } else {
                const auto& sel = view_.selectedClips();
                if (std::find(sel.begin(), sel.end(), *hit.clip) == sel.end())
                    view_.selectClips({*hit.clip});
            }
            drag_ = Drag{*hit.clip, e.pos, false, hit};
            if (root()) root()->capture(this);
            break;
        case Hit::Kind::Stop:  // stop the track; nothing else changes
            args["track"] = hit.track.str();
            (void)view_.runLauncher("launch.stop", std::move(args));
            break;
        case Hit::Kind::Cell:
            if (e.clicks >= 2) {
                // An empty one-bar MIDI clip, opened: one undo step.
                args["track"] = hit.track.str();
                args["scene"] = hit.scene->str();
                args["notation"] = "voice midi; 1: r:*";
                if (auto r = view_.runLauncher("clip.add", std::move(args))) {
                    const json::Value* made = r->find("clip");
                    if (auto id = made && made->isString() ? model::Id::parse(made->asString())
                                                           : std::nullopt) {
                        view_.selectClips({*id});
                        if (view_.launcherCallbacks().open) view_.launcherCallbacks().open(*id);
                    }
                }
                break;
            }
            view_.selectCell(ArrangeView::Cell{hit.track, *hit.scene});
            break;
        case Hit::Kind::Arr: {  // back to the arrangement, when it is not
            const auto it = view_.launchState().find(hit.track);
            if (it != view_.launchState().end() &&
                it->second.plays != LaunchView::Plays::Arrangement) {
                args["track"] = hit.track.str();
                (void)view_.runLauncher("launch.arrangement", std::move(args));
            }
            break;
        }
        case Hit::Kind::None:
            break;
    }
    return true;
}

void LauncherPane::renameClip(model::Id clip, ui::Point at) {
    const model::Clip* c = view_.session().clips.find(clip);
    if (!root() || !c) return;
    auto field = std::make_unique<ui::TextField>("launcher-name", c->name);
    field->layout.width = ui::SizeRule::fixed(200);
    field->selectAll();
    field->onSubmit = [this, clip](const std::string& text) {
        json::Value args = json::Value::object();
        args["clip"] = clip.str();
        args["name"] = text;
        if (view_.runLauncher("clip.set", std::move(args)) && root()) root()->closePopup();
    };
    root()->openPopup(std::move(field), {at.x, at.y, 1, 1});
}

namespace {

// Colour rows for a launcher object: the track colours and None.
std::vector<ui::MenuItem> colorItems(const ui::Theme& theme, const std::string& current,
                                     std::function<void(std::string)> pick) {
    std::vector<ui::MenuItem> items;
    for (std::string_view name : model::kTrackColors) {
        ui::MenuItem m;
        m.label = std::string(name);
        m.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(m.label[0])));
        m.checked = current == name;
        m.swatch = theme.trackColor(name);
        m.run = [pick, colour = std::string(name)] { pick(colour); };
        items.push_back(std::move(m));
    }
    items.push_back({"", "", {}, true, true});
    ui::MenuItem none;
    none.label = "None";
    none.checked = current.empty();
    none.run = [pick] { pick(""); };
    items.push_back(std::move(none));
    return items;
}

}  // namespace

void LauncherPane::openClipMenu(model::Id clip, ui::Point at) {
    const model::Session& s = view_.session();
    const model::Clip* c = s.clips.find(clip);
    if (!root() || !c) return;
    ui::Root* r = root();
    const ui::Rect anchor{at.x, at.y, 1, 1};
    auto set = [this, clip](const char* field, json::Value value) {
        json::Value args = json::Value::object();
        args["clip"] = clip.str();
        args[field] = std::move(value);
        (void)view_.runLauncher("clip.set", std::move(args));
    };
    std::vector<ui::MenuItem> items;
    items.push_back({"Rename\u2026", "", [this, clip, at] { renameClip(clip, at); }});
    items.push_back({"Colour\u2026", "", [r, set, anchor, current = c->color] {
                         auto rows = colorItems(r->theme(), current, [set](std::string colour) {
                             set("color", colour);
                         });
                         r->openPopup(ui::makeMenu("launcher.clip.color", std::move(rows),
                                                   [r] { r->closePopup(); }),
                                      anchor, ui::Placement::Center);
                     }});
    items.push_back({"", "", {}, true, true});
    for (const auto& [label, mode] : {std::pair{"Mode: Loop", model::ClipMode::Loop},
                                      std::pair{"Mode: Once", model::ClipMode::Once}}) {
        ui::MenuItem m;
        m.label = label;
        m.checked = c->mode == mode;
        m.run = [set, mode] { set("mode", mode == model::ClipMode::Loop ? "loop" : "once"); };
        items.push_back(std::move(m));
    }
    items.push_back(
        {"Quantize\u2026", "", [r, set, anchor, current = c->quantize] {
             std::vector<ui::MenuItem> rows;
             ui::MenuItem project;
             project.label = "Project";
             project.checked = !current;
             project.run = [set] { set("quantize", json::Value()); };
             rows.push_back(std::move(project));
             static constexpr std::pair<const char*, model::LaunchQuantize> kChoices[] = {
                 {"bar", model::LaunchQuantize::Bar},
                 {"beat", model::LaunchQuantize::Beat},
                 {"1/2", model::LaunchQuantize::Half},
                 {"1/4", model::LaunchQuantize::Quarter},
                 {"1/8", model::LaunchQuantize::Eighth},
                 {"1/16", model::LaunchQuantize::Sixteenth},
                 {"1/32", model::LaunchQuantize::ThirtySecond},
                 {"none", model::LaunchQuantize::None}};
             for (const auto& [name, q] : kChoices) {
                 ui::MenuItem m;
                 m.label = name;
                 m.label[0] =
                     static_cast<char>(std::toupper(static_cast<unsigned char>(m.label[0])));
                 m.checked = current && *current == q;
                 m.run = [set, value = std::string(name)] { set("quantize", value); };
                 rows.push_back(std::move(m));
             }
             r->openPopup(
                 ui::makeMenu("launcher.clip.quantize", std::move(rows), [r] { r->closePopup(); }),
                 anchor, ui::Placement::Center);
         }});
    items.push_back({"", "", {}, true, true});
    items.push_back({"Duplicate", "", [this, clip] {
                         // Into the next empty cell to its right on its track.
                         const model::Session& now = view_.session();
                         const model::Clip* cc = now.clips.find(clip);
                         if (!cc) return;
                         const LauncherColumns cols = launcherColumns(now);
                         auto it = std::find(cols.scenes.begin(), cols.scenes.end(), cc->scene);
                         for (; it != cols.scenes.end(); ++it)
                             if (!clipIn(now, cc->track, *it)) break;
                         if (it == cols.scenes.end()) {
                             if (view_.launcherCallbacks().onError)
                                 view_.launcherCallbacks().onError(
                                     Error("clip.occupied", "no empty cell after this clip",
                                           "/scene", "add a scene, or drag the clip with Ctrl"));
                             return;
                         }
                         json::Value args = json::Value::object();
                         args["clip"] = clip.str();
                         args["track"] = cc->track.str();
                         args["scene"] = it->str();
                         (void)view_.runLauncher("clip.duplicate", std::move(args));
                     }});
    items.push_back({"Lay on timeline at playhead", "", [this, clip] {
                         const model::Session& now = view_.session();
                         const model::Clip* cc = now.clips.find(clip);
                         if (!cc) return;
                         json::Value args = json::Value::object();
                         args["track"] = cc->track.str();
                         args["clip"] = clip.str();
                         args["position"] = now.timeMap->beatAt(view_.playhead());
                         (void)view_.runLauncher("item.add", std::move(args));
                     }});
    // The active take's chain, in the FX window, as an item's take menu has.
    if (c->activeTake && view_.launcherCallbacks().takeFx)
        items.push_back({"Take FX\u2026", "", [this, take = *c->activeTake] {
                             view_.launcherCallbacks().takeFx(take);
                         }});
    items.push_back({"Delete", "", [this] {
                         const std::vector<model::Id> clips = view_.selectedClips();
                         view_.selectClips({});
                         if (view_.launcherCallbacks().remove) {
                             view_.launcherCallbacks().remove(clips);
                             return;
                         }
                         for (model::Id id : clips) {
                             json::Value args = json::Value::object();
                             args["clip"] = id.str();
                             if (!view_.runLauncher("clip.remove", std::move(args))) break;
                         }
                     }});
    r->openPopup(ui::makeMenu("launcher.clip.menu", std::move(items), [r] { r->closePopup(); }),
                 anchor, ui::Placement::Center);
}

ui::Cursor LauncherPane::cursorAt(ui::Point p) const {
    if (resizing_ || p.x >= bounds().right() - kEdge) return ui::Cursor::ResizeHorizontal;
    if (drag_ && drag_->moving) {  // a clip in hand: an occupied cell refuses it
        const Hit over = hitAt(p);
        return over.scene && !dropAllowed(over) ? ui::Cursor::NotAllowed : ui::Cursor::Move;
    }
    return ui::Cursor::Arrow;
}

// ----------------------------------------------------------------- the header cell

LauncherHeader::LauncherHeader(ArrangeView& view) : Node("launcher-header"), view_(view) {
    interactive = true;
    clipsChildren = true;
}

ui::Rect LauncherHeader::scenesRow() const {
    const ui::Rect b = bounds();
    const float h = view_.rulerRows().marker;
    return {b.x, b.bottom() - h, b.w, h};
}

ui::Rect LauncherHeader::stopRect() const {
    const ui::Rect b = bounds();
    return {b.x + 8, b.y + 8, 64, 20};
}

ui::Rect LauncherHeader::arrangementRect() const {
    const ui::Rect s = stopRect();
    return {s.right() + 6, s.y, 92, s.h};
}

ui::Rect LauncherHeader::captureRect() const {
    const ui::Rect s = stopRect();
    return {s.x, s.bottom() + 6, s.w, s.h};
}

ui::Rect LauncherHeader::addRect() const {
    LauncherColumns cols = launcherColumns(view_.session());
    cols.x = bounds().x;
    cols.w = bounds().w;
    cols.scroll = view_.launcherScroll();
    const ui::Rect row = scenesRow();
    return {cols.columnX(cols.scenes.size()), row.y, LauncherColumns::kArr, row.h};
}

void LauncherHeader::paint(ui::Canvas& c) {
    const ui::Theme& t = c.theme();
    const ui::Rect b = bounds();
    const model::Session& s = view_.session();
    stats.refused = 0;
    c.fillRect(b, t.color("panel"));
    c.fillRect({b.x, b.bottom() - 1, b.w, 1}, t.color("rule"));
    c.fillRect({b.right() - 1, b.y, 1, b.h}, t.color("rule"));
    const float caption = t.type("caption");
    // Stop and arrangement, for every track: the header's uniform cells.
    for (const auto& [r, label] :
         {std::pair{stopRect(), "Stop all"}, std::pair{arrangementRect(), "Arrangement"}}) {
        if (r.right() > b.right()) continue;
        c.fillRound(r, 3, t.color("raised"));
        c.strokeRound(r, 3, t.color("rule"));
        c.text(r, label, ui::Font::Ui, caption, t.color("ink"), ui::kCenter | ui::kMiddle);
    }
    // Capture, as the transport's record button: a red dot, the cell lit
    // while armed.
    stats.captureLit = view_.captureArmed();
    if (const ui::Rect r = captureRect(); r.right() <= b.right() && r.bottom() <= scenesRow().y) {
        const ui::Color red = t.color("armed");
        if (stats.captureLit) {
            c.fillRound(r, 3, red.withAlpha(0.2f));
            c.strokeRound(r, 3, red.withAlpha(0.7f));
        } else {
            c.fillRound(r, 3, t.color("raised"));
            c.strokeRound(r, 3, t.color("rule"));
        }
        c.fillCircle({r.x + 10, r.y + r.h / 2}, 3.5f,
                     stats.captureLit ? red : t.color("ink-quiet"));
        c.text({r.x + 16, r.y, r.w - 18, r.h}, "Capture", ui::Font::Ui, caption, t.color("ink"),
               ui::kCenter | ui::kMiddle);
    }
    // The scene headers: a ▶ and the name, in the marker row's height.
    const ui::Rect row = scenesRow();
    LauncherColumns cols = launcherColumns(s);
    cols.x = b.x;
    cols.w = b.w;
    cols.scroll = view_.launcherScroll();
    c.fillRect({b.x, row.y, b.w, 1}, t.color("rule"));
    c.save();
    c.clip({b.x, row.y, cols.scenesRight() - b.x, row.h});
    for (std::size_t col = 0; col < cols.scenes.size(); ++col) {
        const model::Scene* scene = s.scenes.find(cols.scenes[col]);
        if (!scene) continue;
        const ui::Rect cell{cols.columnX(col), row.y, LauncherColumns::kColumn, row.h};
        c.fillRect({cell.right() - 1, cell.y + 3, 1, cell.h - 6}, t.color("rule"));
        const bool playing = scenePlaying(s, view_.launchState(), cols.scenes[col]);
        const bool refused = sceneLaunchRefused(s, cols.scenes[col]);
        stats.refused += refused;
        if (playing)
            drawPlaying(c, {cell.x + 1, cell.y, 14, cell.h});
        else
            drawPlay(c, {cell.x + 1, cell.y, 14, cell.h},
                     t.color("ink-quiet").withAlpha(refused ? 0.45f : 1.0f));
        if (playing) c.strokeRect(cell.inset(1), t.color("playing"), 1.5f);
        const std::string name = scene->name.empty() ? cols.scenes[col].str() : scene->name;
        const std::string fit = c.elide(name, ui::Font::Ui, caption, cell.w - 19);
        if (!fit.empty())
            c.text({cell.x + 15, cell.y, cell.w - 16, cell.h}, fit, ui::Font::Ui, caption,
                   t.color("ink"), ui::kLeft | ui::kMiddle);
    }
    const ui::Rect add = addRect();
    c.text(add, "+", ui::Font::UiBold, caption, t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
    c.restore();
    // The Arr column's head.
    const ui::Rect arr{cols.arrX(), row.y, LauncherColumns::kArr, row.h};
    c.fillRect({arr.x, row.y, 1, row.h}, t.color("rule"));
    c.text(arr, "Arr", ui::Font::Ui, t.type("caption") - 1, t.color("ink-quiet"),
           ui::kCenter | ui::kMiddle);
}

std::optional<std::size_t> LauncherHeader::sceneAt(ui::Point p) const {
    const ui::Rect row = scenesRow();
    if (p.y < row.y || p.y >= row.bottom()) return std::nullopt;
    LauncherColumns cols = launcherColumns(view_.session());
    cols.x = bounds().x;
    cols.w = bounds().w;
    cols.scroll = view_.launcherScroll();
    return cols.columnAt(p.x);
}

ui::Rect LauncherHeader::sceneRect(std::size_t column) const {
    LauncherColumns cols = launcherColumns(view_.session());
    cols.x = bounds().x;
    cols.w = bounds().w;
    cols.scroll = view_.launcherScroll();
    const ui::Rect row = scenesRow();
    return {cols.columnX(column), row.y, LauncherColumns::kColumn, row.h};
}

bool LauncherHeader::onPointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    if (e.type == T::Wheel) return false;
    const LauncherColumns cols = [&] {
        LauncherColumns c = launcherColumns(view_.session());
        c.x = bounds().x;
        c.w = bounds().w;
        c.scroll = view_.launcherScroll();
        return c;
    }();
    if (drag_) {
        if (e.type == T::Move && !drag_->moving &&
            std::hypot(e.pos.x - drag_->press.x, e.pos.y - drag_->press.y) > 4)
            drag_->moving = true;
        if (e.type == T::Move && drag_->moving) {
            // Over the lanes: where the scene would lie, from the bar under
            // the pointer for its longest clip's whole bars.
            std::optional<ItemGestures::SceneGhost> ghost;
            if (const auto bar = dropBar(e.pos)) {
                const model::Session& s = view_.session();
                ItemGestures::SceneGhost g;
                g.start = s.timeMap->beatOfBar(*bar);
                g.end =
                    s.timeMap->beatOfBar(*bar + static_cast<int>(sceneBars(s, drag_->scene, *bar)));
                for (const auto& entry : s.clips)
                    if (entry.value->scene == drag_->scene) g.tracks.push_back(entry.value->track);
                ghost = std::move(g);
            }
            view_.gestures().showSceneGhost(std::move(ghost));
        }
        if (e.type == T::Up) {
            const Drag d = *drag_;
            drag_.reset();
            if (root()) root()->capture(nullptr);
            view_.gestures().showSceneGhost(std::nullopt);
            if (d.moving) {
                if (const auto bar = dropBar(e.pos)) {  // onto the timeline
                    (void)insertScene(d.scene, *bar);
                    return true;
                }
                // The column it is dropped on; past the last, the end.
                const float offset = e.pos.x - cols.x + cols.scroll;
                const auto to = static_cast<std::int64_t>(
                    std::clamp(offset / LauncherColumns::kColumn, 0.0f,
                               static_cast<float>(cols.scenes.size()) - 1.0f));
                const auto from = std::find(cols.scenes.begin(), cols.scenes.end(), d.scene);
                if (from != cols.scenes.end() && from - cols.scenes.begin() == to)
                    return true;  // back on its own column: nothing to move
                json::Value args = json::Value::object();
                args["scene"] = d.scene.str();
                args["to"] = to;
                (void)view_.runLauncher("scene.move", std::move(args));
            }
        }
        return true;
    }
    if (e.type != T::Down) return e.type == T::Up;
    json::Value args = json::Value::object();
    if (e.button == 1 && stopRect().contains(e.pos)) {
        (void)view_.runLauncher("launch.stop", std::move(args));
        return true;
    }
    if (e.button == 1 && arrangementRect().contains(e.pos)) {
        (void)view_.runLauncher("launch.arrangement", std::move(args));
        return true;
    }
    if (e.button == 1 && captureRect().contains(e.pos)) {
        if (view_.launcherCallbacks().capture)
            view_.launcherCallbacks().capture(!view_.captureArmed());
        return true;
    }
    if (e.button == 1 && addRect().contains(e.pos)) {
        (void)view_.runLauncher("scene.add", std::move(args));
        return true;
    }
    const auto column = sceneAt(e.pos);
    if (!column) return true;
    const model::Id scene = cols.scenes[*column];
    const ui::Rect cell = sceneRect(*column);
    if (e.button == 3) {
        openSceneMenu(scene, e.pos);
        return true;
    }
    if (e.button != 1) return true;
    if (e.pos.x < cell.x + 16) {  // its ▶ launches it
        args["scene"] = scene.str();
        (void)view_.runLauncher("launch.scene", std::move(args));
    } else if (e.clicks >= 2) {
        renameScene(scene, e.pos);
    } else {
        drag_ = Drag{scene, e.pos, false};
        if (root()) root()->capture(this);
    }
    return true;
}

std::optional<int> LauncherHeader::dropBar(ui::Point p) const {
    if (!view_.lanesRect().contains(p) || view_.launcherFull()) return std::nullopt;
    const model::Session& s = view_.session();
    const double beat = s.timeMap->beatAt(std::max(0.0, view_.timeAt(p.x)));
    return s.timeMap->barBeat(beat).bar;
}

bool LauncherHeader::insertScene(model::Id scene, int bar) {
    json::Value args = json::Value::object();
    args["scene"] = scene.str();
    args["bar"] = static_cast<std::int64_t>(bar);
    args["bars"] = sceneBars(view_.session(), scene, bar);
    return static_cast<bool>(view_.runLauncher("scene.insert", std::move(args)));
}

void LauncherHeader::renameScene(model::Id scene, ui::Point at) {
    const model::Scene* sc = view_.session().scenes.find(scene);
    if (!root() || !sc) return;
    auto field = std::make_unique<ui::TextField>("launcher-name", sc->name);
    field->layout.width = ui::SizeRule::fixed(200);
    field->selectAll();
    field->onSubmit = [this, scene](const std::string& text) {
        json::Value args = json::Value::object();
        args["scene"] = scene.str();
        args["name"] = text;
        if (view_.runLauncher("scene.set", std::move(args)) && root()) root()->closePopup();
    };
    root()->openPopup(std::move(field), {at.x, at.y, 1, 1});
}

void LauncherHeader::openSceneMenu(model::Id scene, ui::Point at) {
    const model::Scene* sc = view_.session().scenes.find(scene);
    if (!root() || !sc) return;
    ui::Root* r = root();
    const ui::Rect anchor{at.x, at.y, 1, 1};
    std::vector<ui::MenuItem> items;
    items.push_back({"Rename\u2026", "", [this, scene, at] { renameScene(scene, at); }});
    items.push_back({"Colour\u2026", "", [this, r, scene, anchor, current = sc->color] {
                         auto rows =
                             colorItems(r->theme(), current, [this, scene](std::string colour) {
                                 json::Value args = json::Value::object();
                                 args["scene"] = scene.str();
                                 args["color"] = colour;
                                 (void)view_.runLauncher("scene.set", std::move(args));
                             });
                         r->openPopup(ui::makeMenu("launcher.scene.color", std::move(rows),
                                                   [r] { r->closePopup(); }),
                                      anchor, ui::Placement::Center);
                     }});
    // Laid on the timeline for its longest clip's whole bars: from the
    // first bar line at or after the playhead, or after the arrangement.
    items.push_back({"Insert at playhead", "", [this, scene] {
                         const model::Session& s = view_.session();
                         (void)insertScene(
                             scene, barAtOrAfter(*s.timeMap, s.timeMap->beatAt(view_.playhead())));
                     }});
    items.push_back({"Insert at end", "", [this, scene] {
                         (void)insertScene(scene, barAfterArrangement(view_.session()));
                     }});
    items.push_back({"", "", {}, true, true});
    items.push_back({"Delete", "", [this, scene] {
                         json::Value args = json::Value::object();
                         args["scene"] = scene.str();
                         (void)view_.runLauncher("scene.remove", std::move(args));
                     }});
    r->openPopup(ui::makeMenu("launcher.scene.menu", std::move(items), [r] { r->closePopup(); }),
                 anchor, ui::Placement::Center);
}

}  // namespace supple::views
