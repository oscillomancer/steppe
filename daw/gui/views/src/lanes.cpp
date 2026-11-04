// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/lanes.h"
#include "supple/model/audio_warp.h"

#include "supple/views/envelope_lane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

#include "supple/model/fade_shape.h"
#include "supple/model/source_mapping.h"
#include "supple/time/timeline.h"
#include "supple/ui/controls.h"

namespace supple::views {
namespace {

std::string_view typeToken(model::StripType t) {
    switch (t) {
        case model::StripType::Channel:
            return "channel";
        case model::StripType::Buss:
            return "buss";
        case model::StripType::Master:
            return "master";
    }
    return "ink";
}

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

// Item chrome (names, take badges, lock marks) paints after the GL layer (a
// child paints after its parent).
class LaneLabels : public ui::Node {
  public:
    explicit LaneLabels(Lanes& lanes) : Node("lanes-labels"), lanes_(lanes) {}
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const model::Session& s = lanes_.layout_.session();
        // The visible take envelopes by take, in the session's order within
        // one, gathered once: each item then finds its own without a scan.
        takeEnvelopes_.clear();
        for (const auto& e : s.envelopes)
            if (e.value->visible && e.value->target.object.kind == model::Kind::Take)
                takeEnvelopes_.push_back({e.value->target.object, &*e.value});
        std::stable_sort(takeEnvelopes_.begin(), takeEnvelopes_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < lanes_.layout_.rowCount(); ++i) {
            const ui::Rect row = lanes_.rowRect(i).intersect(bounds());
            if (row.empty()) continue;
            for (model::Id id : lanes_.rowItems_[i]) {
                const model::Item* item = s.items.find(id);
                if (!item) continue;
                const ui::Rect r = lanes_.itemRect(i, *item).intersect(bounds());
                if (r.w < 24) continue;
                const model::Take* take =
                    item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
                std::string name = take ? take->name : std::string();
                if (name.empty()) name = id.str();
                if (take && std::abs(take->playrate - 1.0) > 1e-6) {  // a stretched take says so
                    char rate[48];
                    std::snprintf(rate, sizeof rate, "  \u00b7 %.3gx%s", take->playrate,
                                  take->preservePitch ? "" : " varispeed");
                    name += rate;
                }
                const auto* owner = s.tracks.find(item->track);
                const bool frozen = owner && owner->freeze;
                if (frozen) {
                    c.fillRect(r, t.color("panel").withAlpha(.3f));
                    for (float x = r.x + 4; x < r.right(); x += 12)
                        c.fillRect({x, r.bottom() - 3, std::min(6.f, r.right() - x), 2},
                                   t.color("ink-quiet").withAlpha(.5f));
                }
                const bool laneMuted = owner && owner->laneMute;  // a master's prints (ADR-0057)
                const float alpha = item->mute || frozen || laneMuted ? 0.45f : 0.85f;
                // An item that does not follow the beat says what it follows: a
                // tempo change leaves "time" items at their seconds, and "pos"
                // items keep their length in seconds (their start follows).
                const model::Timebase tb = model::itemTimebase(s, *item);
                const char* follows = tb == model::Timebase::Time            ? "time"
                                      : tb == model::Timebase::BeatsPosition ? "pos"
                                                                             : nullptr;
                const float badgeRight = r.right() - (item->takes.size() > 1 ? 22.0f : 4.0f);
                float timebaseW = 0;
                if (follows && r.w > 70) {
                    timebaseW = c.textWidth(follows, ui::Font::Mono, t.type("caption")) + 8;
                    const ui::Rect badge{badgeRight - timebaseW, r.y + 3, timebaseW, 12};
                    c.strokeRect(badge, t.color("ink-quiet").withAlpha(alpha), 1.0f);
                    c.text(badge, follows, ui::Font::Mono, t.type("caption"),
                           t.color("ink-quiet").withAlpha(alpha), ui::kCenter | ui::kMiddle);
                }
                // Keep the label clear of the padlock and the snowflake an item may carry.
                const float markX = item->locked ? r.x + 18.0f : r.x + 4.0f;
                if (frozen)
                    ui::drawSnowflake(c, {markX + 5, r.y + 8}, 5.5f,
                                      t.color("ink").withAlpha(alpha));
                const float textX = frozen ? markX + 14.0f : markX;
                const float textW = std::max(0.0f, r.right() - textX - 26.0f - timebaseW);
                c.text({textX, r.y + 1, textW, 14}, name, ui::Font::Ui, t.type("caption"),
                       t.color("ink").withAlpha(alpha));
                // A badge when the item carries more than one take.
                if (item->takes.size() > 1) {
                    const ui::Rect badge{r.right() - 18, r.y + 3, 14, 12};
                    c.fillRound(badge, t.radius("s"), t.color("raised"));
                    c.text(badge, std::to_string(item->takes.size()), ui::Font::Mono,
                           t.type("caption"), t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
                }
                // A padlock for locked items.
                if (item->locked) {
                    const ui::Color ink = t.color("ink-quiet");
                    c.strokeRect({r.x + 6, r.y + 6, 5, 6}, ink, 1.0f);
                    c.fillRect({r.x + 4, r.y + 12, 9, 7}, ink);
                }
                // Each take lane names its take (the heard one brighter).
                const ui::Rect full = lanes_.itemRect(i, *item);
                if (Lanes::showsTakeLanes(full, item->takes.size()))
                    for (std::size_t k = 0; k < item->takes.size(); ++k) {
                        const ui::Rect lane = Lanes::takeLaneRect(full, item->takes.size(), k);
                        if (lane.h < 18 || lane.bottom() < bounds().y || lane.y > bounds().bottom())
                            continue;
                        const model::Take* tk = s.takes.find(item->takes[k]);
                        std::string label =
                            tk && !tk->name.empty() ? tk->name : "Take " + std::to_string(k + 1);
                        const bool heard = item->activeTake == item->takes[k];
                        c.text({textX, lane.y + 1, textW + 22, 13}, label, ui::Font::Ui,
                               t.type("caption"),
                               t.color(heard ? "ink" : "ink-quiet").withAlpha(alpha));
                    }
                if (take) {
                    const double start = model::itemSpan(s, *item).startSec;
                    auto tick = [&](double local, bool solid) {
                        const float x = lanes_.xAt(start + local);
                        if (x < r.x || x > r.right()) return;
                        const auto color = t.color(solid ? "ink" : "ink-quiet");
                        c.fillRect({x, r.y + 18, 1, std::max(0.f, r.h - 18)},
                                   color.withAlpha(solid ? .8f : .6f));
                        if (solid) c.fillRect({x - 3, r.y + 17, 7, 5}, color);
                    };
                    const model::AudioWarpMap hitMap(*take);
                    if (take->transients)
                        for (double pos : take->transients->positions)
                            tick(item->loop ? (pos - take->startOffset) / model::audioSpeed(*take)
                                            : hitMap.itemAt(pos),
                                 false);
                    for (const auto& marker : take->stretchMarkers) tick(marker.itemPos, true);
                }
                if (item->activeTake) paintTakeEnvelopes(c, i, *item);
            }
        }
    }

  private:
    // The active take's visible envelopes over the item (gap log #22): the
    // curve as the engine plays it, its points; volume, pan and mute each in
    // a colour of their own.
    void paintTakeEnvelopes(ui::Canvas& c, std::size_t row, const model::Item& item) {
        const ui::Theme& t = c.theme();
        const model::Session& s = lanes_.layout_.session();
        const ui::Rect full = lanes_.itemRect(row, item);
        const ui::Rect area = Lanes::takeEnvArea(full);
        if (area.h < 8) return;
        const double start = model::itemSpan(s, item).startBeat;
        const float x0 = std::max(area.x, bounds().x),
                    x1 = std::min(area.right(), bounds().right());
        if (x1 <= x0) return;
        c.save();
        c.clip(full);
        const auto [first, last] = std::equal_range(
            takeEnvelopes_.begin(), takeEnvelopes_.end(), std::pair{*item.activeTake, nullptr},
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto e = first; e != last; ++e) {
            const model::Envelope& env = *e->second;
            const auto scale = envScale(env);
            if (!scale) continue;
            const ui::Color ink = t.color(env.target.param == "volume" ? "accent"
                                          : env.target.param == "pan"  ? "buss"
                                                                       : "armed")
                                      .withAlpha(env.active ? 1.0f : 0.4f);
            auto yOf = [&](double v) {
                return area.bottom() - static_cast<float>(scale->toNorm(v)) * area.h;
            };
            ui::Point prev{};
            bool have = false;
            for (float x = x0; x <= x1 + 3; x += 3) {
                const float px = std::min(x, x1);
                const double beat = s.timeMap->beatAt(std::max(0.0, lanes_.timeAt(px))) - start;
                const ui::Point p{px, yOf(envValueAt(env, beat, scale->reference()))};
                if (have) c.line(prev, p, ink, 1.5f);
                prev = p;
                have = true;
                if (px >= x1) break;
            }
            for (const model::EnvPoint& pt : env.points) {
                const float x = lanes_.xAt(s.timeMap->secondsAt(start + pt.pos));
                if (x < x0 - 3 || x > x1 + 3) continue;
                c.fillRect({x - 3, yOf(pt.value) - 3, 6, 6}, ink);
            }
        }
        c.restore();
    }

    Lanes& lanes_;
    std::vector<std::pair<model::Id, const model::Envelope*>> takeEnvelopes_;
};

Lanes::Lanes(peaks::PeakService& peaks, Resolve resolve, const TrackLayout& rowLayout,
             const Viewport& view)
    : Node("lanes", ui::Role::Custom),
      peaks_(peaks),
      resolve_(std::move(resolve)),
      layout_(rowLayout),
      view_(view) {
    drawsGl = true;
    clipsChildren = true;
    emplace<LaneLabels>(*this);
}

void Lanes::rebuild() {
    ++indexStats_.rebuilds;
    const model::Session& s = layout_.session();
    rowItems_.assign(layout_.rowCount(), {});
    indexStats_.items += s.items.size();
    for (const auto& e : s.items) {  // indexOf: the track's own row, never its lanes
        if (auto row = layout_.indexOf(e.value->track)) rowItems_[*row].push_back(e.id);
    }
    for (auto& items : rowItems_)
        std::sort(items.begin(), items.end(), [&](model::Id a, model::Id b) {
            return model::itemSpan(s, *s.items.find(a)).startSec <
                   model::itemSpan(s, *s.items.find(b)).startSec;
        });
    invalidate();
}

void Lanes::updateIndex(const model::Session& before, const std::set<model::Id>& items,
                        const std::set<model::Id>& tracks) {
    const auto& after = layout_.session();
    std::set<std::size_t> dirty;
    for (auto id : items) {
        const auto* old = before.items.find(id);
        const auto* now = after.items.find(id);
        if (old && now && old->track == now->track &&
            model::itemSpan(before, *old).startSec == model::itemSpan(after, *now).startSec)
            continue;
        if (old)
            if (auto row = layout_.indexOf(old->track)) {
                std::erase(rowItems_[*row], id);
                dirty.insert(*row);
            }
        if (now)
            if (auto row = layout_.indexOf(now->track)) {
                rowItems_[*row].push_back(id);
                dirty.insert(*row);
            }
    }
    for (auto track : tracks)
        if (auto row = layout_.indexOf(track)) dirty.insert(*row);
    if (dirty.empty()) return;
    ++indexStats_.rebuilds;
    for (auto row : dirty) {
        auto& ids = rowItems_[row];
        indexStats_.items += ids.size();
        std::sort(ids.begin(), ids.end(), [&](auto a, auto b) {
            return model::itemSpan(after, *after.items.find(a)).startSec <
                   model::itemSpan(after, *after.items.find(b)).startSec;
        });
    }
}

void Lanes::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    setBounds(r);
    for (const auto& c : children()) c->arrange(canvas, r);
}

ui::Rect Lanes::rowRect(std::size_t row) const {
    return {bounds().x, bounds().y + layout_.yOf(row) - view_.scrollY, bounds().w,
            layout_.heightOf(row)};
}

ui::Rect Lanes::itemRect(std::size_t row, const model::Item& item) const {
    const ui::Rect r = rowRect(row);
    const model::ItemSpan span = model::itemSpan(layout_.session(), item);
    const float x0 = xAt(span.startSec), x1 = xAt(span.endSec);
    return {x0, r.y + 1, std::max(1.0f, x1 - x0), r.h - 2};
}

void Lanes::paint(ui::Canvas& c) {
    c.save();
    c.clip(bounds());
    theme_ = &c.theme();
    const ui::Theme& t = c.theme();
    c.fillRect(bounds(), t.color("ground"));
    for (std::size_t i = 0; i < layout_.rowCount(); ++i) {
        const ui::Rect r = rowRect(i);
        if (r.bottom() < bounds().y) continue;
        if (r.y > bounds().bottom()) break;
        if (layout_.rows()[i].env) {
            c.save();
            c.clip(r);  // curve glyphs and text must stay inside their own lane
            drawEnvelope(c, i, r);
            c.restore();
            continue;
        }
        if (i % 2) c.fillRect(r, t.color("panel").withAlpha(0.5f));
        c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.6f));
    }
    c.restore();
}

// An automation lane: the envelope's curve sampled every few pixels (so the
// segment shapes show as the engine plays them) and its points.
void Lanes::drawEnvelope(ui::Canvas& c, std::size_t row, const ui::Rect& r) {
    const ui::Theme& t = c.theme();
    const model::Session& s = layout_.session();
    c.fillRect(r, t.color("ground").withAlpha(0.6f));
    c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.4f));
    const model::Envelope* env = s.envelopes.find(*layout_.rows()[row].env);
    const auto scale = env ? layout_.scaleOf(*env) : std::nullopt;
    if (!env || !scale) return;
    double fallback = scale->reference();
    if (const model::Track* tr = s.tracks.find(env->target.object))
        fallback = env->target.param == "playrate" ? s.project->playrate.rate
                   : env->target.param == "volume" ? tr->volumeDb
                   : env->target.param == "pan"    ? tr->pan
                                                   : (tr->mute ? 1.0 : 0.0);
    if (const auto* g = s.groups.find(env->target.object); g && g->vca)
        fallback = env->target.param == "volume" ? g->vca->volumeDb : (g->vca->mute ? 1.0 : 0.0);
    const ui::Rect a = envArea(r);
    auto yOf = [&](double value) {
        return a.bottom() - static_cast<float>(scale->toNorm(value)) * a.h;
    };
    const ui::Color ink =
        t.color(typeToken(layout_.rows()[row].type)).withAlpha(env->active ? 1.0f : 0.35f);
    // The unity / centre line for orientation.
    c.fillRect({a.x, yOf(scale->reference()), a.w, 1}, t.color("rule").withAlpha(0.35f));
    const model::Id envId = *layout_.rows()[row].env;
    // Automation items replace the envelope where they sit: their boxes under
    // the curve, a header band (name or pool), a tick at each loop pass.
    const double visibleFrom = s.timeMap->beatAt(std::max(0.0, timeAt(bounds().x)));
    const double visibleTo = s.timeMap->beatAt(std::max(0.0, timeAt(bounds().right())));
    std::vector<std::pair<double, double>> covered;
    for (const auto& e : s.autoItems) {
        const model::AutomationItem& ai = *e.value;
        if (ai.envelope != envId) continue;
        covered.emplace_back(ai.position, ai.position + ai.length);
        const float ix0 = xAt(s.timeMap->secondsAt(ai.position)),
                    ix1 = xAt(s.timeMap->secondsAt(ai.position + ai.length));
        if (ix1 < bounds().x || ix0 > bounds().right()) continue;
        const ui::Rect box{ix0, r.y + 1, std::max(2.0f, ix1 - ix0), r.h - 2};
        c.fillRect(box, ink.withAlpha(0.10f));
        c.strokeRect(box, ink.withAlpha(0.5f));
        c.fillRect({box.x, box.y, box.w, kAutoHeader}, ink.withAlpha(0.28f));
        std::string label = ai.name.empty() ? "item" : ai.name;
        if (ai.pool > 0) label += "  pool " + std::to_string(ai.pool);
        c.text({box.x + 4, box.y - 1, std::max(0.0f, box.w - 6), kAutoHeader}, label, ui::Font::Ui,
               t.type("caption"), t.color("ink"));
        std::size_t glyphs = 0;
        for (const double base : model::visibleLoopPasses(ai.position, ai.length, ai.sourceLength,
                                                          ai.loop, visibleFrom, visibleTo)) {
            if (ai.loop && base > ai.position) {
                const float x = xAt(s.timeMap->secondsAt(base));
                c.fillRect({x, box.y + kAutoHeader, 1, box.h - kAutoHeader}, ink.withAlpha(0.4f));
            }
            for (const auto& point : ai.points) {
                if (++glyphs > 8192) break;
                const double at = base + point.pos;
                if (point.pos > ai.sourceLength || at >= ai.position + ai.length ||
                    at < visibleFrom || at > visibleTo)
                    continue;
                const float x = xAt(s.timeMap->secondsAt(at));
                c.strokeRect({x - 3, yOf(autoItemValue(ai, point.value)) - 3, 6, 6}, ink);
            }
            if (glyphs > 8192) break;
        }
    }
    const EnvelopeCurve played(s, envId, *scale, visibleFrom, visibleTo);
    const float x0 = std::max(a.x, bounds().x), x1 = std::min(a.right(), bounds().right());
    ui::Point prev{};
    bool have = false;
    for (float x = x0; x <= x1 + 3; x += 3) {
        const float px = std::min(x, x1);
        const double beat = s.timeMap->beatAt(std::max(0.0, timeAt(px)));
        const ui::Point p{px, yOf(played.valueAt(beat, fallback))};
        if (have) c.line(prev, p, ink, 1.5f);
        prev = p;
        have = true;
        if (px >= x1) break;
    }
    for (const model::EnvPoint& pt : env->points) {
        const float x = xAt(s.timeMap->secondsAt(pt.pos));
        if (x < x0 - 4 || x > x1 + 4) continue;
        if (std::any_of(covered.begin(), covered.end(), [&](const auto& span) {
                return pt.pos > span.first && pt.pos < span.second;
            }))
            continue;  // under an item: not what plays
        c.fillRect({x - 3, yOf(pt.value) - 3, 6, 6}, ink);
    }
    c.text({r.x + 6, r.y, 200, 14}, envTitle(*env, s) + (env->active ? "" : " (off)"), ui::Font::Ui,
           t.type("caption"), t.color("ink-quiet"));
}

bool Lanes::showsTakeLanes(const ui::Rect& item, std::size_t takes) {
    return takes >= 2 && (item.h - 17.0f) / static_cast<float>(takes) >= 14.0f;
}

ui::Rect Lanes::takeLaneRect(const ui::Rect& item, std::size_t takes, std::size_t index) {
    const float h =
        std::max(0.0f, item.h - 17.0f) / static_cast<float>(std::max<std::size_t>(1, takes));
    return {item.x, item.y + 15.0f + h * static_cast<float>(index), item.w, h};
}

ui::Rect Lanes::takeEnvArea(const ui::Rect& item) {
    return {item.x, item.y + 16, item.w, std::max(1.0f, item.h - 19)};
}

ui::Rect Lanes::envArea(const ui::Rect& row) {
    return {row.x, row.y + 4, row.w, std::max(1.0f, row.h - 8)};
}

void Lanes::paintGl(const ui::GlFrame& frame) {
    if (glFailed_ || !theme_) return;
    if (!bodies_) {
        auto b = ui::RectRenderer::create();
        auto n = ui::RectRenderer::create();
        auto s = ui::StripRenderer::create();
        if (!b || !n || !s) {
            std::fprintf(stderr, "lanes: GL layers unavailable: %s\n",
                         (!b   ? b.error()
                          : !n ? n.error()
                               : s.error())
                             .toString()
                             .c_str());
            glFailed_ = true;
            return;
        }
        bodies_.emplace(std::move(*b));
        notes_.emplace(std::move(*n));
        strips_.emplace(std::move(*s));
    }
    const model::Session& s = layout_.session();
    const ui::Theme& t = *theme_;
    stats_ = {};
    const ui::Rect clip = frame.clip.intersect(bounds());
    const double t0 = timeAt(clip.x), t1 = timeAt(clip.right());
    const std::uint32_t noteBorder = ui::packRgba(t.color("ground").withAlpha(0.7f));
    for (std::size_t i = 0; i < layout_.rowCount(); ++i) {
        const ui::Rect row = rowRect(i);
        if (row.bottom() <= clip.y) continue;
        if (row.y >= clip.bottom()) break;
        ++stats_.rows;
        const ui::Color type = t.color(typeToken(layout_.rows()[i].type));
        const std::uint32_t note = ui::packRgba(type);
        for (model::Id id : rowItems_[i]) {
            const model::Item* item = s.items.find(id);
            if (!item) continue;
            const model::ItemSpan span = model::itemSpan(s, *item);
            if (span.startSec >= t1) break;  // sorted by start
            if (span.endSec <= t0) continue;
            ++stats_.items;
            const model::Track* owner = s.tracks.find(item->track);
            const bool dim = item->mute || (owner && owner->laneMute);
            const std::uint32_t body = ui::packRgba(type.withAlpha(dim ? 0.10f : 0.28f));
            const std::uint32_t edge = ui::packRgba(type.withAlpha(dim ? 0.35f : 0.85f));
            const std::uint32_t wave = ui::packRgba(t.color("ink").withAlpha(dim ? 0.3f : 0.8f));
            const ui::Rect r = itemRect(i, *item);
            bodies_->add(r, body, edge);
            const model::Take* take = item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
            if (!take || !take->source) continue;
            if (showsTakeLanes(r, item->takes.size())) {  // every take in its lane
                for (std::size_t ti = 0; ti < item->takes.size(); ++ti) {
                    const model::Take* tk = s.takes.find(item->takes[ti]);
                    if (!tk || !tk->source) continue;
                    const ui::Rect lane = takeLaneRect(r, item->takes.size(), ti);
                    const bool heard = item->activeTake == item->takes[ti];
                    if (heard)
                        bodies_->add(lane, ui::packRgba(type.withAlpha(0.22f)),
                                     ui::packRgba(type.withAlpha(0.7f)));
                    const std::uint32_t ink =
                        heard ? wave : ui::packRgba(t.color("ink").withAlpha(dim ? 0.15f : 0.35f));
                    if (const model::MediaSource* m = s.media.find(*tk->source))
                        drawAudio(frame, lane, *item, *tk, *m, ink);
                    else if (const model::MidiSource* src = s.midi.find(*tk->source))
                        drawMidi(lane, *item, *tk, *src, heard ? note : ink, noteBorder);
                }
            } else {
                const ui::Rect inner{r.x, r.y + 15, r.w,
                                     std::max(0.0f, r.h - 17)};  // under the label
                if (const model::MediaSource* m = s.media.find(*take->source))
                    drawAudio(frame, inner, *item, *take, *m, wave);
                else if (const model::MidiSource* src = s.midi.find(*take->source))
                    drawMidi(inner, *item, *take, *src, note, noteBorder);
            }
            drawFades(r, *item, wave);
        }
    }
    bodies_->flush(frame);
    strips_->flush(frame);
    notes_->flush(frame);
}

void Lanes::drawFades(const ui::Rect& r, const model::Item& item, std::uint32_t rgba) {
    const model::ItemSpan span = model::itemSpan(layout_.session(), item);
    auto curve = [&](double fromSec, double toSec, float t0, float t1, model::FadeShape shape,
                     double bend) {
        const float x0 = xAt(fromSec), x1 = xAt(toSec);
        if (x1 <= r.x || x0 >= r.right() || x1 - x0 < 0.5f) return;
        const double secondsWide = std::abs((toSec - fromSec) * view_.pixelsPerSecond);
        const int steps = static_cast<int>(std::clamp(std::ceil(secondsWide / 1.5), 2.0, 256.0));
        strips_->beginRun();
        for (int k = 0; k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            const float gain = static_cast<float>(
                model::fadeGain(shape, bend, static_cast<double>(t0 + (t1 - t0) * t)));
            const float x = x0 + (x1 - x0) * t;
            const float y = (r.bottom() - 1) - gain * (r.h - 2);
            strips_->pair(x, y - 0.75f, y + 0.75f, rgba);
        }
        strips_->endRun();
        ++stats_.fades;
    };
    if (item.fadeIn.length > 0)
        curve(span.startSec, span.startSec + item.fadeIn.length, 0.0f, 1.0f, item.fadeIn.shape,
              item.fadeIn.curve);
    if (item.fadeOut.length > 0)
        curve(span.endSec - item.fadeOut.length, span.endSec, 1.0f, 0.0f, item.fadeOut.shape,
              item.fadeOut.curve);
}

void Lanes::drawAudio(const ui::GlFrame& frame, const ui::Rect& r, const model::Item& item,
                      const model::Take& take, const model::MediaSource& source,
                      std::uint32_t rgba) {
    auto pyramid = peaks_.get(source.hash, resolve_(source));
    if (!pyramid) {
        ++stats_.pending;  // a placeholder until the worker delivers (onReady repaints)
        return;
    }
    const int channels = std::max(1, pyramid->channels());
    const ui::Rect vis = r.intersect(frame.clip);
    if (vis.empty() || r.h < 4) return;
    const float px = 1.0f / frame.ratio;  // one device pixel in design units
    const double rate = pyramid->sampleRate();
    const double sourceSeconds = static_cast<double>(pyramid->frames()) / rate;
    const model::AudioWarpMap timeMap(take);
    const double speed = model::audioSpeed(take);  // pitch unpreserved: faster or slower
    const double framesPerUnit = rate * speed / view_.pixelsPerSecond;
    const peaks::Level& level = pyramid->pick(framesPerUnit * static_cast<double>(px));
    const float gain = static_cast<float>(dbToGain(item.gainDb + take.gainDb));
    const float laneH = r.h / static_cast<float>(channels);
    const double itemStart = model::itemSpan(layout_.session(), item).startSec;
    for (int c = 0; c < channels; ++c) {
        const float mid = r.y + laneH * (static_cast<float>(c) + 0.5f), half = laneH * 0.5f - 1;
        strips_->beginRun();
        for (float x = std::floor(vis.x * frame.ratio) / frame.ratio; x < vis.right(); x += px) {
            // The waveform and playback share original-source coordinates.
            double s0 = item.loop ? take.startOffset + (timeAt(x) - itemStart) * speed
                                  : timeMap.sourceAt(timeAt(x) - itemStart);
            double s1 = item.loop ? take.startOffset + (timeAt(x + px) - itemStart) * speed
                                  : timeMap.sourceAt(timeAt(x + px) - itemStart);
            if (item.loop && sourceSeconds > 0) {
                const double wrap = std::floor(s0 / sourceSeconds) * sourceSeconds;
                s0 -= wrap;
                s1 -= wrap;
            }
            const peaks::Span sp = pyramid->range(level, c, s0 * rate, s1 * rate);
            float top = mid - std::clamp(sp.max * gain, -1.0f, 1.0f) * half;
            float bottom = mid - std::clamp(sp.min * gain, -1.0f, 1.0f) * half;
            if (bottom - top < px) {  // silence still shows a hairline
                const float centre = (top + bottom) / 2;
                top = centre - px / 2;
                bottom = centre + px / 2;
            }
            strips_->pair(x, top, bottom, rgba);
            ++stats_.columns;
        }
        strips_->endRun();
    }
}

void Lanes::drawMidi(const ui::Rect& r, const model::Item& item, const model::Take& take,
                     const model::MidiSource& source, std::uint32_t rgba, std::uint32_t border) {
    const midi::Sequence& seq = source.sequence;
    if (seq.notes.empty() || r.h < 4) return;
    const time::TimeMap& tm = *layout_.session().timeMap;
    const model::ItemSpan span = model::itemSpan(layout_.session(), item);
    const double itemBeat = span.startBeat;
    const double ppq = seq.ppq > 0 ? seq.ppq : 960;
    const model::SourceBeatMapping mapping{take.startOffset,
                                           take.playrate > 0 ? take.playrate : 1.0,
                                           static_cast<double>(seq.length) / ppq};
    const bool looping = item.loop && mapping.sourceLength > 0;
    int lo = 127, hi = 0;
    double end = 0;
    bool hasNotes = false;
    for (const midi::Note& n : seq.notes) {
        if (looping && n.start >= seq.length) continue;
        hasNotes = true;
        lo = std::min(lo, static_cast<int>(n.pitch));
        hi = std::max(hi, static_cast<int>(n.pitch));
        end = std::max(end, (static_cast<double>(n.start) + static_cast<double>(n.length)) / ppq /
                                mapping.rate);
    }
    if (!hasNotes) return;
    const double tail = std::max(0.0, end - mapping.period());
    const int pitches = hi - lo + 3;  // a free row above and below
    const float noteH = std::max(1.0f, std::min(8.0f, r.h / static_cast<float>(pitches)));
    const float usedH = noteH * static_cast<float>(pitches);
    const float top = r.y + (r.h - usedH) / 2;
    const double visibleFrom =
        std::max(0.0, tm.beatAt(std::max(0.0, timeAt(bounds().x))) - itemBeat);
    const double visibleTo =
        std::min(tm.beatAt(span.endSec) - itemBeat,
                 tm.beatAt(std::max(0.0, timeAt(bounds().right()))) - itemBeat);
    std::size_t glyphs = 0;
    for (const double base : mapping.visibleOrigins(tm.beatAt(span.endSec) - itemBeat, looping,
                                                    visibleFrom, visibleTo, tail))
        for (const midi::Note& n : seq.notes) {
            if (looping && n.start >= seq.length) continue;
            const double b0 = base + static_cast<double>(n.start) / ppq / mapping.rate;
            const double b1 = b0 + static_cast<double>(n.length) / ppq / mapping.rate;
            if (b1 <= visibleFrom || b0 >= visibleTo) continue;
            if (++glyphs > 8192) return;
            const double s0 = tm.secondsAt(itemBeat + std::max(0.0, b0));
            const double s1 = std::min(tm.secondsAt(itemBeat + b1), span.endSec);
            const float y = top + static_cast<float>(hi + 1 - n.pitch) * noteH;
            notes_->add({xAt(s0), y, std::max(1.0f, xAt(s1) - xAt(s0)), noteH}, rgba, border);
            ++stats_.notes;
        }
}

}  // namespace supple::views
