// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/launcher.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "supple/ui/canvas.h"
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

}  // namespace

// ----------------------------------------------------------------- the pane

LauncherPane::LauncherPane(ArrangeView& view, peaks::PeakService& peaks, Resolve resolve)
    : Node("launcher"), view_(view), peaks_(peaks), resolve_(std::move(resolve)) {
    interactive = true;
    clipsChildren = true;
    drawsGl = true;
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
    const TrackLayout& layout = view_.trackLayout();
    const LauncherColumns cols = columns();
    return {cols.columnX(column), bounds().y + layout.yOf(row) - view_.viewport().scrollY,
            LauncherColumns::kColumn, layout.heightOf(row)};
}

ui::Rect LauncherPane::arrRect(std::size_t row) const {
    const TrackLayout& layout = view_.trackLayout();
    const LauncherColumns cols = columns();
    return {cols.arrX(), bounds().y + layout.yOf(row) - view_.viewport().scrollY,
            LauncherColumns::kArr, layout.heightOf(row)};
}

void LauncherPane::paint(ui::Canvas& c) {
    stats = {};
    const ui::Theme& t = c.theme();
    theme_ = &t;
    const ui::Rect b = bounds();
    const model::Session& s = view_.session();
    const TrackLayout& layout = view_.trackLayout();
    const LauncherColumns cols = columns();
    c.fillRect(b, t.color("ground"));
    const float caption = t.type("caption");
    c.save();
    c.clip(b);
    for (std::size_t i = 0; i < layout.rowCount(); ++i) {
        const TrackRow& row = layout.rows()[i];
        const ui::Rect r = arrRect(i);
        if (r.bottom() <= b.y) continue;
        if (r.y >= b.bottom()) break;
        c.fillRect({b.x, r.bottom() - 1, b.w, 1}, t.color("rule"));
        // Only a channel's own row has cells (clips are on channels only).
        if (row.lane() || row.type != model::StripType::Channel) continue;
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
                // The stop zone: a small square in quiet ink, nothing else.
                ++stats.empty;
                const float k = 7;
                c.fillRect({cell.x + (cell.w - k) / 2, cell.y + (cell.h - k) / 2, k, k},
                           t.color("ink-quiet").withAlpha(0.6f));
                continue;
            }
            ++stats.clips;
            const ui::Color color = clipColor(t, s, *clip);
            const float dim = clip->mute ? 0.45f : 1.0f;
            c.fillRound(inner, 2, color.withAlpha(0.24f * dim));
            c.strokeRound(inner, 2, color.withAlpha(0.8f * dim));
            const float bar = std::min(Lanes::kNameBar, inner.h);
            const ui::Rect name{inner.x, inner.y, inner.w, bar};
            c.fillRect({name.x + 1, name.y + 1, name.w - 2, name.h - 1},
                       color.withAlpha(0.38f * dim));
            drawPlay(c, {name.x + 1, name.y, 14, name.h}, t.color("ink").withAlpha(dim));
            std::string label = clip->name.empty() ? id->str() : clip->name;
            const std::string fit =
                c.elide(label, ui::Font::Ui, caption, std::max(0.0f, name.w - 18));
            if (!fit.empty())
                c.text({name.x + 15, name.y, name.w - 16, name.h}, fit, ui::Font::Ui, caption,
                       t.color("ink").withAlpha(dim), ui::kLeft | ui::kMiddle);
            stats.names.push_back(fit);
        }
        c.restore();
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
    const TrackLayout& layout = view_.trackLayout();
    const LauncherColumns cols = columns();
    const ui::Rect clip = frame.clip.intersect(bounds());
    stats.notes = stats.columns = stats.pending = 0;
    const std::uint32_t border = ui::packRgba(t.color("ground").withAlpha(0.7f));
    for (std::size_t i = 0; i < layout.rowCount(); ++i) {
        const TrackRow& row = layout.rows()[i];
        const ui::Rect rr = arrRect(i);
        if (rr.bottom() <= clip.y) continue;
        if (rr.y >= clip.bottom()) break;
        if (row.lane() || row.type != model::StripType::Channel) continue;
        for (std::size_t col = 0; col < cols.scenes.size(); ++col) {
            ui::Rect body = bodyRect(cellRect(i, col));
            // Cut at the Arr column; a cell scrolled out of view is skipped.
            body.w = std::min(body.right(), cols.scenesRight()) - body.x;
            if (body.w <= 2 || body.h < 4 || body.right() <= bounds().x) continue;
            const auto id = clipIn(s, row.track, cols.scenes[col]);
            const model::Clip* c = id ? s.clips.find(*id) : nullptr;
            if (!c || c->length <= 0 || !c->activeTake) continue;
            const model::Take* take = s.takes.find(*c->activeTake);
            if (!take || !take->source) continue;
            const float dim = c->mute ? 0.45f : 1.0f;
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
    return e.type == T::Down || e.type == T::Up;  // the lanes' gestures never start here
}

ui::Cursor LauncherPane::cursorAt(ui::Point p) const {
    if (resizing_ || p.x >= bounds().right() - kEdge) return ui::Cursor::ResizeHorizontal;
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
        drawPlay(c, {cell.x + 1, cell.y, 14, cell.h}, t.color("ink-quiet"));
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

bool LauncherHeader::onPointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    if (e.type == T::Wheel) return false;
    return e.type == T::Down || e.type == T::Up;
}

}  // namespace supple::views
