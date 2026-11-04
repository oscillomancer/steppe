#include <supple/model/groups.h>
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/audio_warp.h"
#include "supple/views/item_gestures.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

#include "supple/model/freeze.h"
#include "supple/model/item_edits.h"
#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_lane.h"

namespace supple::views {
namespace {

// An item's start and length in beats, whatever its timebase follows.
double startBeat(const model::Session& s, const model::Item& it) {
    return model::itemSpan(s, it).startBeat;
}
double lengthBeats(const model::Session& s, const model::Item& it) {
    return model::itemLengthBeats(s, it);
}

// Where an envelope's points live on screen: its lane (track, route and
// plugin envelopes) or its item (a take's, gap log #22: positions are beats
// from the item's start).
struct EnvFrame {
    ui::Rect area;
    double offsetBeats = 0;
    EnvScale scale;
    float y(double value) const {
        return area.bottom() - static_cast<float>(scale.toNorm(value)) * area.h;
    }
    double value(float yPos) const {
        return scale.fromNorm(static_cast<double>((area.bottom() - yPos) / area.h));
    }
};
const char* shapeName(model::PointShape shape) {
    switch (shape) {
        case model::PointShape::Linear:
            return "linear";
        case model::PointShape::Square:
            return "square";
        case model::PointShape::Slow:
            return "slow";
        case model::PointShape::FastStart:
            return "fast_start";
        case model::PointShape::FastEnd:
            return "fast_end";
        case model::PointShape::Bezier:
            return "bezier";
    }
    return "linear";
}

std::optional<EnvFrame> envFrame(const ArrangeView& v, model::Id id, const model::Envelope& env) {
    const model::Session& s = v.session();
    if (env.target.object.kind == model::Kind::Take) {
        const auto scale = envScale(env);
        if (!scale) return std::nullopt;
        for (const auto& e : s.items) {
            if (e.value->activeTake != env.target.object) continue;
            const auto row = v.trackLayout().indexOf(e.value->track);
            if (!row) return std::nullopt;
            const ui::Rect item = const_cast<ArrangeView&>(v).lanes().itemRect(*row, *e.value);
            return EnvFrame{Lanes::takeEnvArea(item), model::itemSpan(s, *e.value).startBeat,
                            *scale};
        }
        return std::nullopt;
    }
    const auto row = v.trackLayout().indexOfEnv(id);
    const auto scale = v.trackLayout().scaleOf(env);
    if (!row || !scale) return std::nullopt;
    return EnvFrame{Lanes::envArea(v.rowRect(*row)), 0.0, *scale};
}

constexpr float kEdge = 6;               // px at an item's ends that trim it
constexpr float kHandle = 9;             // the fade handles' square at the top corners
constexpr double kMinLength = 1.0 / 64;  // beats

const model::Item* itemOf(const model::Session& s, model::Id id) { return s.items.find(id); }
const model::Take* activeTake(const model::Session& s, const model::Item& it) {
    return it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
}
bool isMidi(const model::Take* t) { return t && t->source && t->source->kind == model::Kind::Midi; }

// Where a gesture puts an item: its reference in the steps (an id, or
// "$stepN.id" for a copy made by an earlier step), its track and beats.
struct Placed {
    std::string ref;
    std::optional<model::Id> id;
    model::Id track;
    double start = 0, end = 0;
};

// Crossfades for the overlaps the placed items make with the items they do
// not move: the earlier item fades out and the later fades in over the
// overlap (seconds). An item inside another is left alone.
void addCrossfades(const model::Session& s, const std::vector<Placed>& placed,
                   std::vector<EditStep>& steps) {
    std::map<std::string, std::pair<std::optional<double>, std::optional<double>>> fades;
    auto moving = [&](model::Id id) {
        return std::any_of(placed.begin(), placed.end(),
                           [&](const Placed& p) { return p.id == id; });
    };
    auto seconds = [&](double a, double b) {
        return s.timeMap->secondsAt(b) - s.timeMap->secondsAt(a);
    };
    for (const Placed& p : placed)
        for (const auto& e : s.items) {
            const model::Item& o = *e.value;
            if (o.track != p.track || moving(e.id)) continue;
            const double os = startBeat(s, o), oe = startBeat(s, o) + lengthBeats(s, o);
            if (os < p.start && p.start < oe && oe < p.end) {  // o, then p
                const double len = seconds(p.start, oe);
                fades[e.id.str()].second = len;
                fades[p.ref].first = len;
            } else if (p.start < os && os < p.end && p.end < oe) {  // p, then o
                const double len = seconds(os, p.end);
                fades[p.ref].second = len;
                fades[e.id.str()].first = len;
            }
        }
    for (const auto& [ref, fade] : fades) {
        json::Value args = json::Value::object();
        args["item"] = ref;
        if (fade.first) args["fade_in"] = *fade.first;
        if (fade.second) args["fade_out"] = *fade.second;
        steps.push_back({"item.set", std::move(args)});
    }
}

// The ghost and selection layer on the root's overlay: repainted every frame,
// so a drag never damages the cached content.
class Ghost : public ui::Node {
  public:
    explicit Ghost(std::shared_ptr<ItemGestures*> alive)
        : Node("arrange.ghost"), alive_(std::move(alive)) {
        setBounds({0, 0, 1e6f, 1e6f});
    }
    void paint(ui::Canvas& c) override {
        if (!*alive_) return;  // the view went first (the root outlives it)
        const ItemGestures& g = **alive_;
        const ArrangeView& v = g.view();
        const model::Session& s = v.session();
        const ui::Rect lanes = v.lanesRect();
        if (lanes.w <= 0) return;
        const ui::Theme& t = c.theme();
        c.save();
        c.clip(lanes);
        const auto& st = g.state();
        auto rectFor = [&](const model::Item& it, double beatDelta, int rowDelta, double lenDelta,
                           double startDelta) -> std::optional<ui::Rect> {
            auto row = v.trackLayout().indexOf(it.track);
            if (!row) return std::nullopt;
            const auto target = static_cast<std::ptrdiff_t>(*row) + rowDelta;
            if (target < 0 || static_cast<std::size_t>(target) >= v.trackLayout().rowCount())
                return std::nullopt;
            const ui::Rect r = v.rowRect(static_cast<std::size_t>(target));
            const double start = startBeat(s, it) + beatDelta + startDelta;
            const double end = startBeat(s, it) + beatDelta + lengthBeats(s, it) + lenDelta;
            const float x0 = v.xAt(s.timeMap->secondsAt(start)),
                        x1 = v.xAt(s.timeMap->secondsAt(end));
            return ui::Rect{x0, r.y + 1, std::max(2.0f, x1 - x0), r.h - 2};
        };
        // The selection.
        for (model::Id id : g.selection())
            if (const model::Item* it = itemOf(s, id))
                if (auto r = rectFor(*it, 0, 0, 0, 0)) c.strokeRect(*r, t.color("focus"), 2);
        // Selected automation points (filled), unless a drag draws them moved.
        if (st.drag != ItemGestures::Drag::EnvPoint)
            for (model::Id pe : g.pointEnvelopes())
                if (const model::Envelope* env = s.envelopes.find(pe))
                    if (const auto frame = envFrame(v, pe, *env))
                        for (const model::EnvPoint& pt : g.selectedPoints(pe)) {
                            const float x =
                                v.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos));
                            if (x < lanes.x - 4 || x > lanes.right() + 4) continue;
                            c.fillRect({x - 4, frame->y(pt.value) - 4, 8, 8}, t.color("focus"));
                        }
        // The drag's ghost.
        const model::Item* anchor =
            st.anchor.kind == model::Kind::Item ? itemOf(s, st.anchor) : nullptr;
        switch (st.drag) {
            case ItemGestures::Drag::Warp:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, 0)) {
                        const float x =
                            v.xAt(model::itemSpan(s, *anchor).startSec + st.warpPosition);
                        c.fillRect({x - 1, r->y + 17, 2, std::max(0.f, r->h - 17)},
                                   t.color("focus"));
                        c.fillRect({x - 4, r->y + 17, 9, 5}, t.color("focus"));
                    }
                break;
            case ItemGestures::Drag::Move: {
                std::vector<model::Id> moving = g.selection();
                if (anchor && std::find(moving.begin(), moving.end(), st.anchor) == moving.end())
                    moving.push_back(st.anchor);
                for (model::Id id : moving)
                    if (const model::Item* it = itemOf(s, id))
                        if (auto r = rectFor(*it, st.deltaBeats, st.deltaRows, 0, 0)) {
                            c.fillRect(*r, t.color("focus").withAlpha(0.25f));
                            c.strokeRect(*r, t.color("focus"), 1.5f);
                        }
                break;
            }
            case ItemGestures::Drag::TakeOut:
                if (anchor && st.takeLane < anchor->takes.size())
                    if (auto r = rectFor(*anchor, st.deltaBeats, st.deltaRows, 0, 0)) {
                        c.fillRect(*r, t.color("focus").withAlpha(0.25f));
                        c.strokeRect(*r, t.color("focus"), 1.5f);
                        const model::Take* tk = s.takes.find(anchor->takes[st.takeLane]);
                        c.text({r->x + 4, r->y + 1, std::max(0.0f, r->w - 8), 14},
                               tk && !tk->name.empty() ? tk->name : std::string("Take"),
                               ui::Font::Ui, t.type("caption"), t.color("ink"));
                    }
                break;
            case ItemGestures::Drag::TrimLeft:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, st.deltaBeats)) {
                        c.fillRect(*r, t.color("focus").withAlpha(0.2f));
                        c.fillRect({r->x, r->y, 2, r->h}, t.color("focus"));
                    }
                break;
            case ItemGestures::Drag::TrimRight:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, st.deltaBeats, 0)) {
                        c.fillRect(*r, t.color("focus").withAlpha(0.2f));
                        c.fillRect({r->right() - 2, r->y, 2, r->h}, t.color("focus"));
                        const model::Take* tk =
                            anchor->activeTake ? s.takes.find(*anchor->activeTake) : nullptr;
                        if (st.stretch && tk) {  // the rate the take will play at
                            char buf[32];
                            std::snprintf(buf, sizeof buf, "%.3gx",
                                          g.stretchRate(*anchor, *tk, st.deltaBeats));
                            c.text({r->right() - 70, r->y + 2, 64, 14}, buf, ui::Font::Mono,
                                   t.type("caption"), t.color("ink"), ui::kRight | ui::kMiddle);
                        }
                    }
                break;
            case ItemGestures::Drag::Slip:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, 0)) {
                        const float dx =
                            v.xAt(s.timeMap->secondsAt(startBeat(s, *anchor) + st.deltaBeats)) -
                            v.xAt(s.timeMap->secondsAt(startBeat(s, *anchor)));
                        c.strokeRect(*r, t.color("focus"), 1.5f);
                        c.line({r->x + r->w / 2, r->y + r->h / 2},
                               {r->x + r->w / 2 + dx, r->y + r->h / 2}, t.color("focus"), 2);
                    }
                break;
            case ItemGestures::Drag::FadeIn:
            case ItemGestures::Drag::FadeOut:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, 0)) {
                        const float w =
                            static_cast<float>(st.fadeSeconds * v.viewport().pixelsPerSecond);
                        if (st.drag == ItemGestures::Drag::FadeIn)
                            c.line({r->x, r->bottom()}, {r->x + w, r->y}, t.color("focus"), 2);
                        else
                            c.line({r->right() - w, r->y}, {r->right(), r->bottom()},
                                   t.color("focus"), 2);
                    }
                break;
            case ItemGestures::Drag::Marquee:
                c.fillRect(st.marquee, t.color("selection").withAlpha(0.3f));
                c.strokeRect(st.marquee, t.color("focus"));
                break;
            case ItemGestures::Drag::EnvPoint: {
                const model::Envelope* env = st.env ? s.envelopes.find(*st.env) : nullptr;
                auto frame = env ? envFrame(v, *st.env, *env) : std::nullopt;
                if (!frame) break;
                frame->offsetBeats += st.envOffset;
                model::Envelope draft = *env;
                draft.points = st.envDraft;
                float x0 = std::max(lanes.x, frame->area.x),
                      x1 = std::min(lanes.right(), frame->area.right());
                if (const model::AutomationItem* ai =
                        st.autoItem ? s.autoItems.find(*st.autoItem) : nullptr) {
                    x0 = std::max(x0, v.xAt(s.timeMap->secondsAt(st.envOffset)));
                    x1 = std::min(
                        x1, v.xAt(s.timeMap->secondsAt(std::min(st.envOffset + ai->sourceLength,
                                                                ai->position + ai->length))));
                }
                // The drafted line, and the points that move.
                auto curve = [&](const EnvFrame& f, const model::Envelope& e, float from, float to,
                                 const std::vector<model::EnvPoint>& moving) {
                    ui::Point prev{};
                    for (float x = from; x <= to; x += 3) {
                        const double beat =
                            s.timeMap->beatAt(std::max(0.0, v.timeAt(x))) - f.offsetBeats;
                        const ui::Point p{x, f.y(envValueAt(e, beat, 0.0))};
                        if (x > from) c.line(prev, p, t.color("focus"), 2);
                        prev = p;
                    }
                    for (const model::EnvPoint& pt : moving)
                        c.fillRect({v.xAt(s.timeMap->secondsAt(f.offsetBeats + pt.pos)) - 4,
                                    f.y(pt.value) - 4, 8, 8},
                                   t.color("focus"));
                };
                curve(*frame, draft, x0, x1, st.envMoved);
                for (const auto& along : st.envAlong) {  // other lanes' selected points
                    const model::Envelope* other = s.envelopes.find(along.env);
                    const auto f = other ? envFrame(v, along.env, *other) : std::nullopt;
                    if (!f) continue;
                    model::Envelope moved = *other;
                    moved.points = along.draft;
                    curve(*f, moved, std::max(lanes.x, f->area.x),
                          std::min(lanes.right(), f->area.right()), along.moved);
                }
                if (st.envMoved.empty() && st.envIndex < st.envDraft.size()) {
                    const model::EnvPoint& pt = st.envDraft[st.envIndex];
                    const ui::Point at{v.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos)),
                                       frame->y(pt.value)};
                    c.fillRect({at.x - 4, at.y - 4, 8, 8}, t.color("focus"));
                    c.text({at.x + 8, at.y - 16, 120, 14}, frame->scale.text(pt.value),
                           ui::Font::Mono, t.type("caption"), t.color("ink"));
                }
                break;
            }
            case ItemGestures::Drag::AutoItemMove:
            case ItemGestures::Drag::AutoItemLength: {
                const model::AutomationItem* ai =
                    st.autoItem ? s.autoItems.find(*st.autoItem) : nullptr;
                const auto row = ai ? v.trackLayout().indexOfEnv(ai->envelope) : std::nullopt;
                if (!row) break;
                const bool move = st.drag == ItemGestures::Drag::AutoItemMove;
                const double from = ai->position + (move ? st.deltaBeats : 0.0);
                const double to = ai->position + ai->length + st.deltaBeats;
                const ui::Rect lane = v.rowRect(*row);
                const float x0 = v.xAt(s.timeMap->secondsAt(from)),
                            x1 = v.xAt(s.timeMap->secondsAt(to));
                const ui::Rect box{x0, lane.y + 1, std::max(2.0f, x1 - x0), lane.h - 2};
                c.fillRect(box, t.color("selection").withAlpha(0.25f));
                c.strokeRect(box, t.color("focus"));
                break;
            }
            case ItemGestures::Drag::Comp:
                if (anchor && st.compTo > st.compFrom) {
                    const auto row = v.trackLayout().indexOf(anchor->track);
                    if (!row) break;
                    const ui::Rect ir = const_cast<ArrangeView&>(v).lanes().itemRect(*row, *anchor);
                    const ui::Rect lane =
                        Lanes::takeLaneRect(ir, anchor->takes.size(), st.takeLane);
                    const float x0 = v.xAt(s.timeMap->secondsAt(st.compFrom));
                    const float x1 = v.xAt(s.timeMap->secondsAt(st.compTo));
                    c.fillRect({x0, lane.y, x1 - x0, lane.h}, t.color("focus").withAlpha(0.3f));
                    c.strokeRect({x0, lane.y, x1 - x0, lane.h}, t.color("focus"), 1.5f);
                }
                break;
            case ItemGestures::Drag::None:
                break;
        }
        c.restore();
    }

  private:
    std::shared_ptr<ItemGestures*> alive_;
};

}  // namespace

ItemGestures::ItemGestures(ArrangeView& view)
    : view_(view), alive_(std::make_shared<ItemGestures*>(this)) {}

// The registration releases the ghost; its token also covers deferred deletion.
ItemGestures::~ItemGestures() { *alive_ = nullptr; }

void ItemGestures::installOverlay(ui::Root& root) {
    root_ = &root;
    if (!overlay_) overlay_ = root.addOverlay(std::make_unique<Ghost>(alive_));
}

void ItemGestures::setSelection(std::vector<model::Id> items) {
    selection_ = std::move(items);
    if (root_) root_->requestAnimationFrame();
}

bool ItemGestures::isSelected(model::Id item) const {
    return std::find(selection_.begin(), selection_.end(), item) != selection_.end();
}

void ItemGestures::changed() {
    const auto seeds = selection_;
    for (auto id : seeds)
        for (auto peer : model::linkedItems(view_.session(), id))
            if (!isSelected(peer)) selection_.push_back(peer);
    if (root_) root_->requestAnimationFrame();
    if (onSelection) onSelection(selection_);
}

Result<model::Id> ItemGestures::newMidiItem(model::Id track, double fromBeat,
                                            std::optional<double> toBeat) {
    if (!run) return Error("gui.unwired", "no command runner");
    const model::Session& s = view_.session();
    const int bar = s.timeMap->barBeat(std::max(0.0, fromBeat)).bar;
    const double start = s.timeMap->beatOfBar(bar);
    const double oneBar = s.timeMap->beatOfBar(bar + 1) - start;
    const double length = std::max(oneBar, toBeat.value_or(0.0) - start);
    json::Value args = json::Value::object();
    args["track"] = track.str();
    args["bar"] = static_cast<std::int64_t>(bar);
    args["length"] = length;
    args["notation"] = "voice midi; 1: r:*";  // an empty MIDI source (a bar of rest)
    args["name"] = "MIDI";
    auto r = run("New MIDI item", {{"item.add", std::move(args)}});
    if (!r) return std::move(r).error();
    const json::Value* item = r->find("item");
    const auto id = item && item->isString() ? model::Id::parse(item->asString()) : std::nullopt;
    if (!id) return Error("gui.new_midi", "item.add returned no item");
    selection_ = {*id};
    changed();
    if (onOpenMidi) onOpenMidi(*id);
    return *id;
}

ItemGestures::Clip ItemGestures::clip() const {
    Clip c;
    c.generation = view_.projectGeneration();
    if (projectGeneration && projectGeneration() != c.generation) return c;
    const model::Session& s = view_.session();
    c.version = s.version;
    if (pinVersion) c.pin = pinVersion(c.version);
    // The rows that hold items, in order: channels and masters' lanes (ADR-0057).
    std::vector<model::Id> channels;
    for (const auto& row : view_.trackLayout().rows())
        if (holdsItem(s, row, nullptr)) channels.push_back(row.track);
    std::ptrdiff_t first = static_cast<std::ptrdiff_t>(channels.size());
    for (auto id : selection_) {
        const auto* item = itemOf(s, id);
        if (!item) continue;
        const auto found = std::find(channels.begin(), channels.end(), item->track);
        if (found == channels.end()) continue;
        const auto index = found - channels.begin();
        first = std::min(first, index);
        c.items.push_back({id, startBeat(s, *item), startBeat(s, *item) + lengthBeats(s, *item),
                           item->track, index, audioItem(s, *item)});
    }
    if (!c.items.empty()) {
        c.anchorTrack = channels[static_cast<std::size_t>(first)];
        for (auto& item : c.items) item.channelOffset -= first;
    }
    return c;
}

void ItemGestures::copySelection() {
    Clip c = clip();
    if (!c.items.empty()) clipboard_ = std::move(c);
}

Status ItemGestures::cutSelection() {
    Clip c = clip();
    if (c.items.empty() || !run) return {};
    std::vector<EditStep> steps;
    for (const auto& e : c.items) {
        json::Value args = json::Value::object();
        args["item"] = e.item.str();
        steps.push_back({"item.remove", std::move(args)});
    }
    auto r = run(c.items.size() == 1 ? "Cut item" : "Cut items", steps);
    if (!r) return std::move(r).error();
    clipboard_ = std::move(c);
    selection_.clear();
    changed();
    return {};
}

Status ItemGestures::paste(double atBeat, std::optional<model::Id> track) {
    if (!canPaste()) clearClipboard();
    return pasteClip(clipboard_, atBeat, track,
                     clipboard_.items.size() == 1 ? "Paste item" : "Paste items");
}

Status ItemGestures::duplicateSelection() {
    Clip c = clip();
    if (c.items.empty()) return {};
    double end = 0;
    for (const auto& e : c.items) end = std::max(end, e.end);
    return pasteClip(c, end, std::nullopt,
                     c.items.size() == 1 ? "Duplicate item" : "Duplicate items");
}

Status ItemGestures::pasteClip(const Clip& c, double atBeat, std::optional<model::Id> track,
                               const std::string& label) {
    if (c.items.empty() || !run) return {};
    std::vector<model::Id> channels;
    std::vector<bool> masters;  // a master's lane takes audio items only (ADR-0057)
    for (const auto& row : view_.trackLayout().rows())
        if (holdsItem(view_.session(), row, nullptr)) {
            channels.push_back(row.track);
            masters.push_back(row.type == model::StripType::Master);
        }
    const auto anchor = std::find(channels.begin(), channels.end(),
                                  track.value_or(c.anchorTrack.value_or(model::Id{})));
    double start = 1e300;
    for (const auto& entry : c.items) start = std::min(start, entry.position);
    json::Value placements = json::Value::array();
    std::size_t dropped = 0;
    for (const auto& e : c.items) {
        const auto target = (anchor - channels.begin()) + e.channelOffset;
        if (anchor == channels.end() || target < 0 ||
            static_cast<std::size_t>(target) >= channels.size() ||
            (!e.audio && masters[static_cast<std::size_t>(target)])) {
            ++dropped;
            continue;
        }
        json::Value p = json::Value::object();
        p["item"] = e.item.str();
        p["track"] = channels[static_cast<std::size_t>(target)].str();
        p["position"] = std::max(0.0, atBeat + (e.position - start));
        placements.push_back(std::move(p));
    }
    json::Value args = json::Value::object();
    args["version"] = static_cast<std::int64_t>(c.version);
    if (placements.asArray().empty())
        return Error("gui.clipboard.targets",
                     "paste dropped " + std::to_string(dropped) + " items: no target channel", {},
                     "select a target channel with enough following channels");
    args["items"] = std::move(placements);
    auto r = run(label, {{"item.paste", std::move(args)}});
    if (!r) return std::move(r).error();
    selection_.clear();
    if (const json::Value* made = r->find("items"); made && made->isArray())
        for (const json::Value& id : made->asArray())
            if (auto parsed = model::Id::parse(id.asString())) selection_.push_back(*parsed);
    changed();
    if (dropped && onError)
        onError(Error("gui.clipboard.targets", "paste dropped " + std::to_string(dropped) +
                                                   " items without a target channel"));
    return {};
}

double ItemGestures::stretchRate(const model::Item& item, const model::Take& take,
                                 double deltaBeats) const {
    const model::Session& s = view_.session();
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    const double start = s.timeMap->secondsAt(startBeat(s, item));
    const double before = s.timeMap->secondsAt(startBeat(s, item) + lengthBeats(s, item)) - start;
    const double after =
        s.timeMap->secondsAt(startBeat(s, item) + lengthBeats(s, item) + deltaBeats) - start;
    return std::clamp(after > 0 ? rate * before / after : rate, 0.01, 100.0);
}

double ItemGestures::gridBeats() const {
    const model::Session& s = view_.session();
    const double secondsPerBeat = std::max(1e-6, s.timeMap->secondsAt(1) - s.timeMap->secondsAt(0));
    const double pxPerBeat = view_.viewport().pixelsPerSecond * secondsPerBeat;
    for (double g : {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0})
        if (g * pxPerBeat >= 10) return g;
    return 32.0;
}

double ItemGestures::beatAtX(float x) const {
    return view_.session().timeMap->beatAt(std::max(0.0, view_.timeAt(x)));
}

double ItemGestures::snap(double beat, bool free) const {
    if (free) return beat;
    const double g = gridBeats();
    return std::round(beat / g) * g;
}

std::optional<std::size_t> ItemGestures::rowAt(float y) const {
    for (std::size_t r = 0; r < view_.trackLayout().rowCount(); ++r) {
        const ui::Rect rr = view_.rowRect(r);
        if (y >= rr.y && y < rr.bottom()) return r;
    }
    return std::nullopt;
}

std::optional<ItemGestures::Hit> ItemGestures::hit(ui::Point p) const {
    if (!view_.lanesRect().contains(p)) return std::nullopt;
    const auto row = rowAt(p.y);
    if (!row || view_.trackLayout().rows()[*row].env) return std::nullopt;  // a lane has no items
    const model::Session& s = view_.session();
    const model::Id track = view_.trackLayout().rows()[*row].track;
    std::optional<Hit> best;
    double bestStart = -1e300;
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != track) continue;
        Hit h;
        h.item = e.id;
        h.row = *row;
        h.rect = const_cast<ArrangeView&>(view_).lanes().itemRect(*row, it);
        if (!h.rect.contains(p) || startBeat(s, it) < bestStart) continue;
        bestStart = startBeat(s, it);
        const ui::Rect& r = h.rect;
        const bool roomy = r.w > 4 * kHandle;
        if (it.takes.size() > 1 && p.x >= r.right() - 24 && p.x < r.right() - kHandle &&
            p.y < r.y + 16)
            h.takeBadge = true;
        if (roomy && p.y < r.y + kHandle && p.x < r.x + kHandle)
            h.zone = Drag::FadeIn;
        else if (roomy && p.y < r.y + kHandle && p.x > r.right() - kHandle)
            h.zone = Drag::FadeOut;
        else if (r.w > 3 * kEdge && p.x < r.x + kEdge)
            h.zone = Drag::TrimLeft;
        else if (r.w > 3 * kEdge && p.x > r.right() - kEdge)
            h.zone = Drag::TrimRight;
        else
            h.zone = Drag::Move;
        // Below the name bar of an item showing its take lanes: comping.
        if (h.zone == Drag::Move && !h.takeBadge && p.y >= r.y + 15 &&
            Lanes::showsTakeLanes(r, it.takes.size())) {
            const ui::Rect first = Lanes::takeLaneRect(r, it.takes.size(), 0);
            const auto index = static_cast<std::size_t>(std::max(0.0f, (p.y - first.y) / first.h));
            h.takeLane = std::min(index, it.takes.size() - 1);
            h.zone = Drag::Comp;
        }
        best = h;
    }
    return best;
}

bool ItemGestures::pointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    const model::Session& s = view_.session();
    if (e.type == T::Cancel) {
        state_ = State{};
        moved_ = false;
        changed();
        return true;
    }
    if (e.type == T::Down && (e.button == 1 || e.button == 3)) {
        if (!view_.lanesRect().contains(e.pos)) return false;
        press_ = e.pos;
        moved_ = false;
        free_ = (e.mods & ui::kShift) != 0;
        copy_ = false;
        cycleFade_ = false;
        marqueeAdd_ = false;
        mixed_ = false;
        state_ = State{};
        // A marquee takes the items and the automation points inside it; with
        // these modifiers it adds them to what is selected.
        auto startMarquee = [&](bool add) {
            state_.drag = Drag::Marquee;
            state_.marquee = {e.pos.x, e.pos.y, 0, 0};
            marqueeAdd_ = add;
            if (add) {
                state_.marqueeBase = selection_;
                state_.pointBase = pointSel_;
            }
        };
        if (const auto lane = rowAt(e.pos.y); lane && view_.trackLayout().rows()[*lane].env) {
            const model::Id env = *view_.trackLayout().rows()[*lane].env;
            const auto ai = autoItemAt(env, e.pos);
            if (onEnvelopeFocus) onEnvelopeFocus(true);
            if (e.button == 3) {  // a marquee; without a drag, the lane's menu
                startMarquee(mouse.behaviour("lane.drag", e.mods) == "add");
                state_.env = env;
                if (root_) root_->capture(&view_);
                return true;
            }
            if (ai && ai->zone != Drag::EnvPoint) {
                state_.drag = ai->zone;
                state_.autoItem = ai->item;
                state_.autoBase = s.autoItems.entry(ai->item)->value;
                if (ai->zone == Drag::AutoItemLength) {  // its shape stretches, or repeats
                    const std::string_view b = mouse.behaviour("autoitem.edge", e.mods);
                    state_.stretch = b.starts_with("stretch");
                    free_ = b.ends_with("_free");
                }
                if (root_) root_->capture(&view_);
                return true;
            }
            return envPress(env, e, ai);
        }
        const auto h = hit(e.pos);
        if (h && e.button == 1)
            if (const auto owner = model::frozenOwner(s, h->item)) {
                setSelection({h->item});
                if (e.clicks >= 2 && onError) onError(model::frozenError(s, *owner, "/item"));
                return true;
            }
        // A take envelope drawn on the item takes the press near its line.
        if (e.button == 1 && h && e.clicks < 2)
            if (const auto env = takeEnvelopeAt(h->item, e.pos)) {
                if (onEnvelopeFocus) onEnvelopeFocus(true);
                return envPress(*env, e);
            }
        const bool marquee = e.button == 3 || !h;
        const bool add = marquee && mouse.behaviour("lane.drag", e.mods) == "add";
        if (!add) clearPoints();  // a press elsewhere leaves the automation points
        if (onEnvelopeFocus) onEnvelopeFocus(false);
        pressRow_ = rowAt(e.pos.y).value_or(0);
        if (e.button == 1 && e.clicks >= 2 && !h) {  // empty space on a channel: a new MIDI item
            if (const auto row = rowAt(e.pos.y)) {
                const TrackRow& spec = view_.trackLayout().rows()[*row];
                if (!spec.env && spec.type == model::StripType::Channel) {
                    if (auto made = newMidiItem(spec.track, beatAtX(e.pos.x)); !made && onError)
                        onError(made.error());
                    state_ = State{};
                    return true;
                }
            }
        }
        if (marquee) {  // a marquee (right-drag anywhere, or from empty space)
            // A right-click that does not drag opens the item's menu instead.
            if (e.button == 3 && h) state_.anchor = h->item;
            startMarquee(add);
        } else {
            state_.anchor = h->item;
            if (e.clicks >= 2) {
                if (isMidi(activeTake(s, *itemOf(s, h->item))) && onOpenMidi) onOpenMidi(h->item);
                state_ = State{};
                return true;
            }
            // Ctrl below the name bar of an item with several takes: that take
            // comes out on its own (the lane under the pointer, else the heard one).
            const model::Item* pressed = itemOf(s, h->item);
            if ((e.mods & ui::kCtrl) && pressed && pressed->takes.size() > 1 &&
                e.pos.y >= h->rect.y + 15 && (h->zone == Drag::Move || h->zone == Drag::Comp)) {
                state_.drag = Drag::TakeOut;
                std::size_t heard = 0;
                for (std::size_t k = 0; k < pressed->takes.size(); ++k)
                    if (pressed->activeTake == pressed->takes[k]) heard = k;
                state_.takeLane = h->takeLane.value_or(heard);
                if (root_) root_->capture(&view_);
                return true;
            }
            state_.drag = h->zone;
            if (h->takeLane) state_.takeLane = *h->takeLane;
            if (state_.drag == Drag::Move && pressed && !pressed->locked && pressed->activeTake &&
                e.pos.y >= h->rect.y + 17 && !(e.mods & ui::kAlt)) {
                const auto* take = s.takes.find(*pressed->activeTake);
                if (take && take->source && take->source->kind == model::Kind::Media) {
                    const auto span = model::itemSpan(s, *pressed);
                    float distance = 5;
                    auto candidate = [&](double source, double local,
                                         std::optional<std::size_t> index) {
                        const float d = std::abs(view_.xAt(span.startSec + local) - e.pos.x);
                        if (local > 0 && local < span.endSec - span.startSec && d <= distance) {
                            distance = d;
                            state_.drag = Drag::Warp;
                            state_.warpIndex = index;
                            state_.warpSource = source;
                            state_.warpPosition = local;
                            state_.warpVersion = s.version;
                            state_.warpGeneration = view_.projectGeneration();
                        }
                    };
                    if (take->transients)
                        for (double pos : take->transients->positions)
                            candidate(pos, model::audioItemAt(*take, pos), {});
                    for (std::size_t i = 1; i + 1 < take->stretchMarkers.size(); ++i) {
                        const auto& m = take->stretchMarkers[i];
                        candidate(m.sourcePos, m.itemPos, i);
                    }
                }
            }
            // The mouse map says what this press does with these modifiers.
            std::string_view b;
            if (state_.drag == Drag::Move) {
                b = mouse.behaviour("item.drag", e.mods);
                copy_ = b.starts_with("copy");
                if (b.starts_with("slip")) state_.drag = Drag::Slip;
            } else if (state_.drag == Drag::TrimLeft || state_.drag == Drag::TrimRight) {
                b = mouse.behaviour("item.edge", e.mods);
                state_.stretch = state_.drag == Drag::TrimRight && b.starts_with("stretch") &&
                                 !isMidi(activeTake(s, *itemOf(s, h->item)));
            } else if (state_.drag == Drag::FadeIn || state_.drag == Drag::FadeOut) {
                b = mouse.behaviour("fade", e.mods);
                cycleFade_ = b == "cycle_shape";
            }
            if (!b.empty()) free_ = b.ends_with("_free");
            // A press on an unselected item selects it, unless these modifiers
            // make a click toggle or extend the selection, or the drag copies.
            if (!isSelected(h->item) && !copy_ &&
                mouse.behaviour("item.click", e.mods) == "select") {
                selection_ = {h->item};
                changed();
            }
        }
        if (root_) root_->capture(&view_);
        return true;
    }
    if (e.type == T::Move && state_.autoItem && !checkAutoItem()) {
        state_ = State{};
        moved_ = false;
        if (root_) {
            root_->capture(nullptr);
            root_->requestAnimationFrame();
        }
        return true;
    }
    if (e.type == T::Move &&
        (state_.drag == Drag::AutoItemMove || state_.drag == Drag::AutoItemLength)) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) > 3;
        const model::AutomationItem* ai =
            state_.autoItem ? s.autoItems.find(*state_.autoItem) : nullptr;
        if (ai) {
            const double raw = beatAtX(e.pos.x) - beatAtX(press_.x);
            if (state_.drag == Drag::AutoItemMove)
                state_.deltaBeats = std::max(0.0, snap(ai->position + raw, free_)) - ai->position;
            else
                state_.deltaBeats = std::max(ai->position + kMinLength,
                                             snap(ai->position + ai->length + raw, free_)) -
                                    (ai->position + ai->length);
        }
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    if (e.type == T::Move && state_.drag == Drag::EnvPoint) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) + std::abs(e.pos.y - press_.y) > 3;
        envDrag(e);
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    if (e.type == T::Move && state_.drag != Drag::None) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) + std::abs(e.pos.y - press_.y) > 3;
        const double raw = beatAtX(e.pos.x) - beatAtX(press_.x);
        const model::Item* anchor =
            state_.anchor.kind == model::Kind::Item ? itemOf(s, state_.anchor) : nullptr;
        switch (state_.drag) {
            case Drag::Warp:
                if (anchor)
                    state_.warpPosition = std::clamp(
                        s.timeMap->secondsAt(snap(beatAtX(e.pos.x), (e.mods & ui::kShift) != 0)) -
                            model::itemSpan(s, *anchor).startSec,
                        0.,
                        model::itemSpan(s, *anchor).endSec - model::itemSpan(s, *anchor).startSec);
                break;
            case Drag::Move: {
                if (!anchor) break;
                std::vector<model::Id> items = selection_;
                if (!isSelected(state_.anchor)) items.push_back(state_.anchor);
                double d = snap(startBeat(s, *anchor) + raw, free_) - startBeat(s, *anchor);
                for (model::Id id : items)  // nothing before the start
                    if (const model::Item* it = itemOf(s, id)) d = std::max(d, -startBeat(s, *it));
                state_.deltaBeats = d;
                // Rows: every selected item must land on a channel.
                const int dr = static_cast<int>(rowAt(e.pos.y).value_or(pressRow_)) -
                               static_cast<int>(pressRow_);
                bool ok = true;
                for (model::Id id : items) {
                    const model::Item* it = itemOf(s, id);
                    const auto row = it ? view_.trackLayout().indexOf(it->track) : std::nullopt;
                    const auto target = row ? static_cast<std::ptrdiff_t>(*row) + dr : -1;
                    ok = ok && target >= 0 &&
                         static_cast<std::size_t>(target) < view_.trackLayout().rowCount() &&
                         holdsItem(s, view_.trackLayout().rows()[static_cast<std::size_t>(target)],
                                   it);
                }
                state_.deltaRows = ok ? dr : 0;
                break;
            }
            case Drag::TakeOut: {
                if (!anchor) break;
                state_.deltaBeats =
                    std::max(snap(startBeat(s, *anchor) + raw, free_), 0.0) - startBeat(s, *anchor);
                const auto row = view_.trackLayout().indexOf(anchor->track);
                const auto target = rowAt(e.pos.y);
                const auto& rows = view_.trackLayout().rows();
                if (row && target && holdsItem(s, rows[*target], anchor))
                    state_.deltaRows = static_cast<int>(*target) - static_cast<int>(*row);
                break;
            }
            case Drag::TrimLeft:
                if (anchor)  // out to the timeline's start, in to the minimum length
                    state_.deltaBeats =
                        std::clamp(snap(startBeat(s, *anchor) + raw, free_) - startBeat(s, *anchor),
                                   -startBeat(s, *anchor), lengthBeats(s, *anchor) - kMinLength);
                break;
            case Drag::TrimRight:
                if (anchor) {
                    const double end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
                    state_.deltaBeats = std::max(snap(end + raw, free_) - end,
                                                 kMinLength - lengthBeats(s, *anchor));
                }
                break;
            case Drag::Slip:
                state_.deltaBeats = raw;
                break;
            case Drag::FadeIn:
            case Drag::FadeOut:
                if (anchor) {
                    const double startSec = s.timeMap->secondsAt(startBeat(s, *anchor));
                    const double endSec =
                        s.timeMap->secondsAt(startBeat(s, *anchor) + lengthBeats(s, *anchor));
                    const double at = view_.timeAt(e.pos.x);
                    state_.fadeSeconds =
                        std::clamp(state_.drag == Drag::FadeIn ? at - startSec : endSec - at, 0.0,
                                   endSec - startSec);
                }
                break;
            case Drag::Marquee: {
                state_.marquee = {std::min(press_.x, e.pos.x), std::min(press_.y, e.pos.y),
                                  std::abs(e.pos.x - press_.x), std::abs(e.pos.y - press_.y)};
                if (!moved_) break;  // a click: the selection stays (a right-click opens a menu)
                marqueePoints();
                selection_ = state_.marqueeBase;
                for (std::size_t r = 0; r < view_.trackLayout().rowCount(); ++r) {
                    if (view_.trackLayout().rows()[r].env) continue;
                    const model::Id track = view_.trackLayout().rows()[r].track;
                    for (const auto& item : s.items) {
                        if (item.value->track != track) continue;
                        const ui::Rect ir = view_.lanes().itemRect(r, *item.value);
                        const ui::Rect& m = state_.marquee;
                        if (ir.x < m.right() && ir.right() > m.x && ir.y < m.bottom() &&
                            ir.bottom() > m.y && !isSelected(item.id))
                            selection_.push_back(item.id);
                    }
                }
                // Published as it grows: the app re-applies its selection every
                // frame, which would otherwise undo the marquee before release.
                changed();
                break;
            }
            case Drag::Comp:
                if (anchor) {
                    const double start = startBeat(s, *anchor),
                                 end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
                    const double a0 = std::clamp(snap(beatAtX(press_.x), free_), start, end);
                    const double a1 = std::clamp(snap(beatAtX(e.pos.x), free_), start, end);
                    state_.compFrom = std::min(a0, a1);
                    state_.compTo = std::max(a0, a1);
                }
                break;
            case Drag::EnvPoint:
            case Drag::AutoItemMove:
            case Drag::AutoItemLength:
            case Drag::None:
                break;
        }
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    if (e.type == T::Up && state_.drag != Drag::None) {
        if (root_) root_->capture(nullptr);
        commit(e);
        state_ = State{};
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    return false;
}

Status ItemGestures::deleteSelection() {
    if (selection_.empty() || !run) return {};
    std::vector<EditStep> steps;
    for (model::Id item : selection_) {
        json::Value args = json::Value::object();
        args["item"] = item.str();
        steps.push_back({"item.remove", std::move(args)});
    }
    auto r = run(selection_.size() == 1 ? "Delete item" : "Delete items", steps);
    if (!r) return r.error();
    selection_.clear();
    changed();
    return {};
}

bool ItemGestures::canConsolidate() const {
    return static_cast<bool>(model::consolidationGroups(view_.session(), selection_));
}
bool ItemGestures::canHeal() const {
    return static_cast<bool>(model::healingGroups(view_.session(), selection_));
}
void ItemGestures::consolidateSelection() {
    if (onConsolidate) onConsolidate(selection_);
}
Status ItemGestures::healSelection() {
    if (!run) return {};
    auto args = json::Value::object();
    args["items"] = json::Value::array();
    for (auto id : selection_) args["items"].push_back(id.str());
    auto result = run("Heal splits", {{"item.heal", std::move(args)}});
    if (!result) return result.error();
    selection_.clear();
    if (const auto* items = result->find("items"))
        for (const auto& item : items->asArray())
            if (auto id = model::Id::parse(item.asString())) selection_.push_back(*id);
    changed();
    return {};
}

Status ItemGestures::splitSelection(bool zeroCrossing) {
    if (!run) return {};
    const auto& s = view_.session();
    const double seconds = view_.playhead();
    std::set<model::Id> visited;
    std::vector<EditStep> steps;
    for (auto id : selection_) {
        if (visited.contains(id)) continue;
        const auto* item = s.items.find(id);
        if (!item) continue;
        const auto span = model::itemSpan(s, *item);
        if (!(seconds > span.startSec && seconds < span.endSec)) continue;
        const auto* take = item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
        if (zeroCrossing && (!take || !take->source || take->source->kind != model::Kind::Media))
            continue;
        for (auto peer : model::linkedItems(s, id)) visited.insert(peer);
        json::Value args = json::Value::object();
        args["item"] = id.str();
        args["at"] = s.timeMap->beatAt(seconds);
        if (zeroCrossing) args["snap"] = "zero_crossing";
        steps.push_back({"item.split", std::move(args)});
    }
    if (steps.empty()) return {};
    auto result = run(zeroCrossing ? "Split at prior zero crossing" : "Split at cursor", steps);
    if (!result) return result.error();
    return {};
}

Status ItemGestures::addStretchMarkerAtCursor() {
    if (!run) return {};
    const auto& s = view_.session();
    const double cursor = view_.playhead();
    std::set<model::Id> visited;
    std::vector<EditStep> steps;
    for (auto id : selection_) {
        const auto* item = s.items.find(id);
        const auto* take = item && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
        if (!take || !take->source || take->source->kind != model::Kind::Media ||
            visited.contains(*item->activeTake))
            continue;
        const auto span = model::itemSpan(s, *item);
        if (!(cursor > span.startSec && cursor < span.endSec)) continue;
        visited.insert(*item->activeTake);
        visited.insert(take->warpPeers.begin(), take->warpPeers.end());
        auto args = json::Value::object();
        args["take"] = item->activeTake->str();
        args["item_pos"] = cursor - span.startSec;
        steps.push_back({"take.stretch.add", std::move(args)});
    }
    if (steps.empty()) return {};
    auto result = run("Add stretch marker at cursor", steps);
    if (!result) return result.error();
    return {};
}

ItemGestures::MenuPlace ItemGestures::placeAt(ui::Point at) const {
    MenuPlace place;
    place.at = at;
    // Paste lands where the menu was opened: that beat (on the grid), that channel.
    if (const auto row = rowAt(at.y);
        row && holdsItem(view_.session(), view_.trackLayout().rows()[*row], nullptr))
        place.track = view_.trackLayout().rows()[*row].track;
    place.beat = std::max(0.0, snap(beatAtX(at.x), false));
    return place;
}

std::optional<model::Id> ItemGestures::menuTarget() const {
    if (selection_.empty()) return {};
    if (state_.anchor.kind == model::Kind::Item && isSelected(state_.anchor)) return state_.anchor;
    return selection_.front();
}

void ItemGestures::addEditItems(std::vector<ui::MenuItem>& items, const MenuPlace& place,
                                bool onItem) {
    auto add = [&](std::string label, const char* action, bool enabled,
                   std::function<Status()> act) {
        ui::MenuItem m;
        m.label = std::move(label);
        if (keysFor) m.shortcut = keysFor(action);
        m.enabled = enabled;
        m.run = [this, act = std::move(act)] {
            if (auto r = act(); !r && onError) onError(r.error());
        };
        items.push_back(std::move(m));
    };
    const double beat = place.beat;
    const std::optional<model::Id> track = place.track;
    const std::size_t n = selection_.size();
    const bool any = n > 0;
    const std::string count = n <= 1 ? "" : " " + std::to_string(n) + " items";
    if (onItem) {
        add("Cut" + count, "ui.item.cut", any, [this] { return cutSelection(); });
        add("Copy" + count, "ui.item.copy", any, [this] {
            copySelection();
            return Status{};
        });
    }
    add("Paste", "ui.item.paste", canPaste(), [this, beat, track] { return paste(beat, track); });
    if (!onItem) return;
    add("Duplicate" + count, "ui.item.duplicate", any, [this] { return duplicateSelection(); });
    add("Delete" + count, "edit.delete", any, [this] { return deleteSelection(); });
    // Always listed (greyed when it does not apply), so the rows keep their places.
    add("Consolidate items", "ui.item.consolidate", canConsolidate(), [this] {
        consolidateSelection();
        return Status{};
    });
    add("Heal splits", "ui.item.heal", canHeal(), [this] { return healSelection(); });
    add("Split item at cursor", "item.split:playhead", any, [this] { return splitSelection(); });
    const auto& s = view_.session();
    const bool audio = std::any_of(selection_.begin(), selection_.end(), [&](auto id) {
        const auto* item = s.items.find(id);
        const auto* take = item && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
        return take && take->source && take->source->kind == model::Kind::Media;
    });
    add("Split item at prior zero crossing", "ui.item.split-zero", audio,
        [this] { return splitSelection(true); });
    const std::optional<model::Id> target = menuTarget();
    const model::Item* item = target ? itemOf(s, *target) : nullptr;
    const model::Take* heard = item && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
    add("Mix to new track", "ui.item.mix-to-track", any && onMixToTrack, [this] {
        onMixToTrack(selection_);
        return Status{};
    });
    ui::MenuItem rule;
    rule.separator = true;
    items.push_back(rule);
    add("Edit MIDI", "ui.midi.edit", isMidi(heard) && onOpenMidi, [this, target] {
        onOpenMidi(*target);
        return Status{};
    });
    add("Show or hide the MIDI editor", "ui.midi.toggle", static_cast<bool>(onToggleMidiEditor),
        [this] {
            onToggleMidiEditor();
            return Status{};
        });
}

void ItemGestures::openSpaceMenu(ui::Point at) {
    if (!root_) return;
    std::vector<ui::MenuItem> items;
    addEditItems(items, placeAt(at), false);
    root_->openPopup(
        ui::makeMenu("arrange.space.menu", std::move(items), [this] { root_->closePopup(); }),
        {at.x, at.y, 0, 0}, ui::Placement::Below);
}

void ItemGestures::openItemMenu(ui::Point at) {
    if (!root_ || selection_.empty()) return;
    root_->openPopup(
        ui::makeMenu("item.menu", itemMenu(placeAt(at)), [this] { root_->closePopup(); }),
        {at.x, at.y, 1, 1});
}

std::vector<ui::MenuItem> ItemGestures::itemMenu(const MenuPlace& place) {
    return buildItemMenu(place, false);
}

std::vector<ui::MenuItem> ItemGestures::itemBarMenu() { return buildItemMenu({}, true); }

std::vector<ui::MenuItem> ItemGestures::buildItemMenu(const MenuPlace& place, bool bar) {
    std::vector<ui::MenuItem> items;
    ui::MenuItem rule;
    rule.separator = true;
    const model::Session& s = view_.session();
    const std::optional<model::Id> target = menuTarget();
    const model::Item* item = target ? itemOf(s, *target) : nullptr;
    const model::Id itemId = target.value_or(model::Id{});
    // The take under the pointer (a take lane), else the heard one.
    std::optional<model::Id> pointed;
    if (item && item->activeTake) {
        pointed = *item->activeTake;
        if (const auto h = place.at ? hit(*place.at) : std::nullopt;
            h && h->item == itemId && h->takeLane)
            pointed = item->takes[*h->takeLane];
    }
    if (!bar) {
        addEditItems(items, place, true);
        if (!pointed) return items;
        const model::Id take = *pointed;
        items.push_back(rule);
        // Render every selected item through its FX (MIDI through the track's
        // instrument), the clicked one's take under the pointer (heard first
        // when it is not); items without material or locked are left alone.
        // Deleting FX removes the FX the renders went through after them: the
        // replaced takes' go with them, then the tracks' chains.
        std::vector<model::Id> render;
        for (model::Id id : selection_) {
            const model::Item* it = itemOf(s, id);
            if (!it || it->locked) continue;
            const model::Take* tk =
                id == itemId ? s.takes.find(take)
                             : (it->activeTake ? s.takes.find(*it->activeTake) : nullptr);
            if (tk && tk->source) render.push_back(id);
        }
        for (auto [label, mode, dropFx] :
             {std::tuple{"Render with FX, replacing the audio", "replace", false},
              {"Render with FX, deleting FX", "replace", true},
              {"Render with FX as a new take", "take", false}}) {
            ui::MenuItem m;
            m.label = label;
            m.enabled = !render.empty();
            m.run = [this, render, itemId, take, heard = *item->activeTake,
                     modeName = std::string(mode), drop = dropFx] {
                if (!run) return;
                const model::Session& now = view_.session();
                std::vector<EditStep> steps;
                if (take != heard && std::count(render.begin(), render.end(), itemId)) {
                    json::Value pick = json::Value::object();
                    pick["item"] = itemId.str();
                    pick["take"] = take.str();
                    steps.push_back({"take.activate", std::move(pick)});
                }
                std::vector<model::Id> tracks;
                for (model::Id id : render) {
                    json::Value args = json::Value::object();
                    args["item"] = id.str();
                    args["mode"] = modeName;
                    steps.push_back({"item.apply-fx", std::move(args)});
                    if (const model::Item* it = itemOf(now, id);
                        it && std::find(tracks.begin(), tracks.end(), it->track) == tracks.end())
                        tracks.push_back(it->track);
                }
                if (drop)
                    for (model::Id tr : tracks)
                        if (const model::Track* t = now.tracks.find(tr))
                            for (model::Id fx : t->chain) {
                                json::Value args = json::Value::object();
                                args["fx"] = fx.str();
                                steps.push_back({"fx.remove", std::move(args)});
                            }
                if (auto r = run(render.size() == 1 ? "Render item" : "Render items", steps);
                    !r && onError)
                    onError(r.error());
            };
            items.push_back(std::move(m));
        }
        return items;
    }
    // The clicked item's heard take, raw (item.export); greyed for an empty take.
    const model::Take* heard = item && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
    ui::MenuItem save;
    save.label = "Export\u2026";
    if (keysFor) save.shortcut = keysFor("ui.item.export");
    save.enabled = heard && heard->source;
    save.run = [this, target] {
        if (onExport && target) onExport(*target);
    };
    items.push_back(std::move(save));
    // Timebase: what the selection follows when the tempo changes (item.set
    // timebase, one step; the items keep their place now).
    items.push_back(rule);
    const model::Item* first = selection_.empty() ? nullptr : itemOf(s, selection_.front());
    for (auto [label, value, name] :
         {std::tuple{"Follow the beat", model::ItemTimebase::Beats, "beats"},
          {"Follow time", model::ItemTimebase::Time, "time"},
          {"Follow the beat (start only)", model::ItemTimebase::BeatsPosition, "beats_position"},
          {"Follow the track", model::ItemTimebase::Track, "track"}}) {
        ui::MenuItem m;
        m.label = label;
        m.checked = first && first->timebase == value;
        m.enabled = first != nullptr;
        m.run = [this, timebase = std::string(name)] {
            if (!run) return;
            std::vector<EditStep> steps;
            for (model::Id item : selection_) {
                json::Value args = json::Value::object();
                args["item"] = item.str();
                args["timebase"] = timebase;
                steps.push_back({"item.set", std::move(args)});
            }
            if (auto r = run("Item timebase", steps); !r && onError) onError(r.error());
        };
        items.push_back(std::move(m));
    }
    for (const auto& [label, op] : {std::pair{"Detect hits", "detect"},
                                    {"Add stretch marker at cursor", "marker"},
                                    {"Quantize audio", "warp"},
                                    {"Slice at hits", "slice"},
                                    {"Remove warping", "reset"}}) {
        ui::MenuItem menu;
        menu.label = label;
        menu.run = [this, operation = std::string(op)] {
            if (operation == "marker") {
                if (auto result = addStretchMarkerAtCursor(); !result && onError)
                    onError(result.error());
            } else if (onAudioEdit)
                onAudioEdit(selection_, operation, gridBeats());
        };
        menu.enabled = std::any_of(selection_.begin(), selection_.end(), [&](auto id) {
            const auto* it = s.items.find(id);
            const auto* tk = it && it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
            if (!tk || !tk->source || !s.media.contains(*tk->source)) return false;
            if (std::string_view(op) != "marker") return true;
            const auto span = model::itemSpan(s, *it);
            if (it->locked || model::frozenOwner(s, id) || it->loop || !tk->preservePitch ||
                tk->pitchMode != "signalsmith" || !model::validateWarpMap(*tk) ||
                view_.playhead() <= span.startSec || view_.playhead() >= span.endSec)
                return false;
            for (auto peer : tk->warpPeers)
                for (const auto& owner : s.items)
                    if (std::find(owner.value->takes.begin(), owner.value->takes.end(), peer) !=
                            owner.value->takes.end() &&
                        (owner.value->locked || model::frozenOwner(s, owner.id)))
                        return false;
            return true;
        });
        items.push_back(std::move(menu));
    }
    // The active take's own chain and envelopes (gap log #22).
    if (item && item->activeTake) {
        const model::Id take = *item->activeTake;
        items.push_back(rule);
        ui::MenuItem fx;
        fx.label = "Take FX\u2026";
        fx.run = [this, take] {
            if (onTakeFx) onTakeFx(take);
        };
        items.push_back(std::move(fx));
        for (auto [label, param] : {std::pair{"Take volume envelope", "volume"},
                                    {"Take pan envelope", "pan"},
                                    {"Take mute envelope", "mute"}}) {
            std::optional<model::Id> env;
            bool shown = false;
            for (const auto& e : s.envelopes)
                if (e.value->target.object == take && e.value->target.param == param) {
                    env = e.id;
                    shown = e.value->visible;
                }
            ui::MenuItem m;
            m.label = label;
            m.checked = env && shown;
            m.run = [this, take, env, shown, paramName = std::string(param)] {
                if (!run) return;
                json::Value args = json::Value::object();
                std::string command;
                if (env) {  // show or hide it
                    command = "env.set";
                    args["env"] = env->str();
                    args["visible"] = !shown;
                } else {  // a new one, flat at its neutral value from the item's start
                    command = "env.add";
                    args["take"] = take.str();
                    args["param"] = paramName;
                    json::Value points = json::Value::array(), start = json::Value::object();
                    start["beat"] = 0.0;
                    start["value"] = 0.0;
                    points.push_back(std::move(start));
                    args["points"] = std::move(points);
                }
                if (auto r = run(env ? "Take envelope" : "Add take envelope",
                                 {{command, std::move(args)}});
                    !r && onError)
                    onError(r.error());
            };
            items.push_back(std::move(m));
        }
    }
    if (!pointed) return items;
    const model::Id take = *pointed;
    const model::Take* tk = s.takes.find(take);
    const std::string name = tk && !tk->name.empty() ? tk->name : take.str();
    // Every selected item with several takes keeps only its heard one.
    std::vector<EditStep> crop;
    for (model::Id id : selection_)
        if (const model::Item* it = itemOf(s, id); it && it->activeTake)
            for (model::Id other : it->takes)
                if (other != *it->activeTake) {
                    json::Value args = json::Value::object();
                    args["take"] = other.str();
                    crop.push_back({"take.remove", std::move(args)});
                }
    if (item->takes.size() < 2 && crop.empty()) return items;
    items.push_back(rule);
    if (item->takes.size() > 1) {
        ui::MenuItem all;
        all.label = "Show all takes";
        all.checked = view_.trackLayout().takesExpanded(itemId);
        all.run = [this, itemId, on = !all.checked] {
            view_.trackLayout().setTakesExpanded(itemId, on);
        };
        items.push_back(std::move(all));
        for (auto [label, mode] : {std::pair{"Explode takes onto new tracks", "tracks"},
                                   {"Explode takes in sequence on this track", "serial"}}) {
            ui::MenuItem m;
            m.label = label;
            m.run = [this, itemId, modeName = std::string(mode)] {
                if (!run) return;
                json::Value args = json::Value::object();
                args["item"] = itemId.str();
                args["mode"] = modeName;
                if (auto r = run("Explode takes", {{"item.explode-takes", std::move(args)}});
                    !r && onError)
                    onError(r.error());
            };
            items.push_back(std::move(m));
        }
        ui::MenuItem drop;
        drop.label = "Delete take \u201c" + name + "\u201d";
        drop.run = [this, take] {
            if (!run) return;
            json::Value args = json::Value::object();
            args["take"] = take.str();
            if (auto r = run("Delete take", {{"take.remove", std::move(args)}}); !r && onError)
                onError(r.error());
        };
        items.push_back(std::move(drop));
    }
    ui::MenuItem keep;
    keep.label = "Keep only the selected take";
    keep.enabled = !crop.empty();
    keep.run = [this, crop] {
        if (!run) return;
        if (auto r = run("Keep only the selected take", crop); !r && onError) onError(r.error());
    };
    items.push_back(std::move(keep));
    return items;
}

void ItemGestures::commit(const ui::PointerEvent& e) {
    const model::Session& s = view_.session();
    const model::Item* anchor =
        state_.anchor.kind == model::Kind::Item ? itemOf(s, state_.anchor) : nullptr;
    auto runSteps = [&](const std::string& label, const std::vector<EditStep>& steps) {
        if (steps.empty() || !run) return;
        if (auto r = run(label, steps); !r && onError) onError(r.error());
    };
    if (state_.drag == Drag::Marquee && state_.env && !moved_) {
        if (e.button == 3) openEnvMenu(*state_.env, press_);  // a right-click on a lane
        return;
    }
    if (state_.drag == Drag::EnvPoint) {
        if (moved_ || state_.envCreated) {
            const std::string label = state_.envCreated          ? "Add envelope point"
                                      : !state_.envMoved.empty() ? "Move envelope points"
                                                                 : "Move envelope point";
            commitEnv(label);
        }
        if (!state_.autoItem && state_.env && state_.envIndex < state_.envDraft.size())
            selectPoints(*state_.env, state_.envCreated
                                          ? std::vector{state_.envDraft[state_.envIndex]}
                                      : !moved_ ? std::vector{state_.envBase[state_.envIndex]}
                                      : !state_.envMoved.empty()
                                          ? state_.envMoved
                                          : std::vector{state_.envDraft[state_.envIndex]});
        if (moved_)  // the other lanes' points stay selected where they moved
            for (const State::Along& along : state_.envAlong)
                if (!along.moved.empty()) pointSel_[along.env] = along.moved;
        return;
    }
    if ((state_.drag == Drag::AutoItemMove || state_.drag == Drag::AutoItemLength) &&
        state_.autoItem && moved_ && std::abs(state_.deltaBeats) > 1e-9) {
        if (!checkAutoItem()) return;
        if (const model::AutomationItem* ai = s.autoItems.find(*state_.autoItem)) {
            json::Value args = json::Value::object();
            args["item"] = state_.autoItem->str();
            std::vector<EditStep> steps;
            if (state_.drag == Drag::AutoItemMove) {
                args["position"] = ai->position + state_.deltaBeats;
            } else {
                const double length = ai->length + state_.deltaBeats;
                args["length"] = length;
                if (state_.stretch && ai->length > 0) {
                    // The shape stretches with the item: its points and the
                    // span they repeat over scale (for its whole pool, whose
                    // items keep their share of it).
                    const double k = length / ai->length;
                    args["source_length"] = ai->sourceLength * k;
                    json::Value points = json::Value::array();
                    for (const model::EnvPoint& pt : ai->points) {
                        json::Value p = json::Value::object();
                        p["beat"] = pt.pos * k;
                        p["value"] = pt.value;
                        p["shape"] = shapeName(pt.shape);
                        if (pt.tension != 0) p["tension"] = pt.tension;
                        points.push_back(std::move(p));
                    }
                    args["points"] = std::move(points);
                    if (ai->pool > 0)
                        for (const auto& other : s.autoItems)
                            if (other.id != *state_.autoItem && other.value->pool == ai->pool) {
                                json::Value o = json::Value::object();
                                o["item"] = other.id.str();
                                o["length"] = other.value->length * k;
                                steps.push_back({"autoitem.set", std::move(o)});
                            }
                }
            }
            steps.insert(steps.begin(), {"autoitem.set", std::move(args)});
            runSteps(state_.drag == Drag::AutoItemMove ? "Move automation item"
                     : state_.stretch                  ? "Stretch automation item"
                                                       : "Resize automation item",
                     steps);
        }
        return;
    }
    if (state_.drag == Drag::Comp && anchor && !anchor->locked) {
        // A click hears that take for the whole item; a swipe splits the item
        // at the range and hears that take there (the piece keeps the take
        // order, so the take is chosen by index).
        json::Value activate = json::Value::object();
        activate["index"] = static_cast<std::int64_t>(state_.takeLane);
        const double start = startBeat(s, *anchor),
                     end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
        const bool whole = !moved_ || state_.compTo - state_.compFrom < kMinLength ||
                           (state_.compFrom <= start + 1e-9 && state_.compTo >= end - 1e-9);
        if (whole) {
            if (anchor->activeTake && state_.takeLane < anchor->takes.size() &&
                anchor->takes[state_.takeLane] == *anchor->activeTake)
                return;
            activate["item"] = state_.anchor.str();
            runSteps("Switch take", {{"take.activate", std::move(activate)}});
            return;
        }
        std::vector<EditStep> steps;
        std::string target = state_.anchor.str();
        if (state_.compFrom > start + 1e-9) {
            json::Value split = json::Value::object();
            split["item"] = target;
            split["at"] = state_.compFrom;
            steps.push_back({"item.split", std::move(split)});
            target = "$step" + std::to_string(steps.size() - 1) + ".right";
        }
        if (state_.compTo < end - 1e-9) {
            json::Value split = json::Value::object();
            split["item"] = target;
            split["at"] = state_.compTo;
            steps.push_back({"item.split", std::move(split)});
            target = "$step" + std::to_string(steps.size() - 1) + ".left";
        }
        activate["item"] = target;
        steps.push_back({"take.activate", std::move(activate)});
        runSteps("Comp take", steps);
        return;
    }
    if (state_.drag == Drag::TakeOut) {
        if (!moved_ || !anchor || anchor->locked || state_.takeLane >= anchor->takes.size() || !run)
            return;
        const auto row = view_.trackLayout().indexOf(anchor->track);
        if (!row) return;
        const auto target =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*row) + state_.deltaRows);
        if (target >= view_.trackLayout().rowCount()) return;
        json::Value args = json::Value::object();
        args["take"] = anchor->takes[state_.takeLane].str();
        args["track"] = view_.trackLayout().rows()[target].track.str();
        args["position"] = startBeat(s, *anchor) + state_.deltaBeats;
        auto r = run("Take out take", {{"take.extract", std::move(args)}});
        if (!r) {
            if (onError) onError(r.error());
            return;
        }
        if (const json::Value* made = r->find("item"); made && made->isString())
            if (auto id = model::Id::parse(made->asString())) {
                selection_ = {*id};
                changed();
            }
        return;
    }
    // A click on a fade handle with the cycle_shape modifier (Ctrl) cycles its shape.
    if (!moved_ && anchor && !anchor->locked && cycleFade_ &&
        (state_.drag == Drag::FadeIn || state_.drag == Drag::FadeOut)) {
        static constexpr const char* kShapes[] = {"linear",    "slow", "fast", "slow_fast",
                                                  "fast_slow", "cos",  "exp"};
        const bool in = state_.drag == Drag::FadeIn;
        const auto now =
            static_cast<std::size_t>(in ? anchor->fadeIn.shape : anchor->fadeOut.shape);
        json::Value args = json::Value::object();
        args["item"] = state_.anchor.str();
        args[in ? "fade_in_shape" : "fade_out_shape"] = kShapes[(now + 1) % 7];
        runSteps(in ? "Fade-in shape" : "Fade-out shape", {{"item.set", std::move(args)}});
        return;
    }
    if (state_.drag == Drag::Warp) {
        if (!moved_ || !anchor || !anchor->activeTake) return;
        if (state_.warpVersion != s.version || state_.warpGeneration != view_.projectGeneration() ||
            (projectGeneration && projectGeneration() != state_.warpGeneration)) {
            if (onError)
                onError(Error("warp.changed", "audio changed during marker drag", {},
                              "drag the marker again"));
            return;
        }
        auto args = json::Value::object();
        args["take"] = anchor->activeTake->str();
        args["item_pos"] = state_.warpPosition;
        if (state_.warpIndex)
            args["index"] = static_cast<std::uint64_t>(*state_.warpIndex);
        else
            args["source_pos"] = state_.warpSource;
        runSteps("Warp audio",
                 {{state_.warpIndex ? "take.stretch.move" : "take.stretch.add", args}});
        return;
    }
    if (!moved_) {  // a click
        if (state_.drag == Drag::Marquee && e.button == 3 &&
            state_.anchor.kind == model::Kind::Item) {
            if (!isSelected(state_.anchor)) {
                selection_ = {state_.anchor};
                changed();
            }
            openItemMenu(e.pos);
            return;
        }
        if (state_.drag == Drag::Marquee && e.button == 3 && view_.lanesRect().contains(e.pos)) {
            openSpaceMenu(e.pos);  // empty space
            return;
        }
        if (state_.drag == Drag::Marquee) {
            if (e.button == 1 && !marqueeAdd_) {
                selection_.clear();
                changed();
                if (onSeek) onSeek(std::max(0.0, view_.timeAt(press_.x)));
            }
            return;
        }
        if (const auto h = hit(press_); h && h->takeBadge && anchor && anchor->activeTake) {
            const auto& takes = anchor->takes;
            auto at = std::find(takes.begin(), takes.end(), *anchor->activeTake);
            const model::Id next =
                (at == takes.end() || at + 1 == takes.end()) ? takes.front() : *(at + 1);
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["take"] = next.str();
            runSteps("Switch take", {{"take.activate", std::move(args)}});
            return;
        }
        const std::string_view click = mouse.behaviour("item.click", e.mods);
        if (click == "toggle") {
            if (isSelected(state_.anchor)) {
                for (auto peer : model::linkedItems(view_.session(), state_.anchor))
                    std::erase(selection_, peer);
            } else
                selection_.push_back(state_.anchor);
        } else if (click == "extend") {
            if (!isSelected(state_.anchor)) selection_.push_back(state_.anchor);
        } else {
            selection_ = {state_.anchor};
        }
        changed();
        return;
    }
    switch (state_.drag) {
        case Drag::Warp:
            return;  // committed above, with version guard
        case Drag::Marquee:
            // Points selected: the keys act on them (with items too: deleteAll).
            mixed_ = !selection_.empty() && !pointSel_.empty();
            if (onEnvelopeFocus) onEnvelopeFocus(!pointSel_.empty());
            changed();
            return;
        case Drag::Move: {
            if (state_.deltaBeats == 0 && state_.deltaRows == 0) return;
            // The anchor joins the set even if it was not selected (a press
            // whose modifiers copy, toggle or extend does not select it).
            std::vector<model::Id> items = selection_;
            if (!isSelected(state_.anchor)) items.push_back(state_.anchor);
            std::vector<EditStep> steps;
            std::vector<Placed> placed;
            for (model::Id id : items) {
                const model::Item* it = itemOf(s, id);
                if (!it || (it->locked && !copy_)) continue;
                std::optional<model::Id> track;
                if (state_.deltaRows != 0) {
                    const auto row = view_.trackLayout().indexOf(it->track);
                    if (!row) continue;
                    const auto target = static_cast<std::ptrdiff_t>(*row) + state_.deltaRows;
                    const auto& rows = view_.trackLayout().rows();
                    if (target < 0 || static_cast<std::size_t>(target) >= rows.size() ||
                        !holdsItem(s, rows[static_cast<std::size_t>(target)], it))
                        return;
                    track = rows[static_cast<std::size_t>(target)].track;
                }
                if (copy_) {
                    json::Value dup = json::Value::object();
                    dup["item"] = id.str();
                    dup["to"] = startBeat(s, *it) + state_.deltaBeats;
                    steps.push_back({"item.duplicate", std::move(dup)});
                    placed.push_back({"$step" + std::to_string(steps.size() - 1) + ".id",
                                      std::nullopt, track.value_or(it->track),
                                      startBeat(s, *it) + state_.deltaBeats,
                                      startBeat(s, *it) + state_.deltaBeats + lengthBeats(s, *it)});
                    if (!track) continue;
                    json::Value mv = json::Value::object();
                    mv["item"] = "$step" + std::to_string(steps.size() - 1) + ".id";
                    mv["track"] = track->str();
                    steps.push_back({"item.move", std::move(mv)});
                    continue;
                }
                json::Value args = json::Value::object();
                args["item"] = id.str();
                args["position"] = startBeat(s, *it) + state_.deltaBeats;
                if (track) args["track"] = track->str();
                steps.push_back({"item.move", std::move(args)});
                placed.push_back({id.str(), id, track.value_or(it->track),
                                  startBeat(s, *it) + state_.deltaBeats,
                                  startBeat(s, *it) + state_.deltaBeats + lengthBeats(s, *it)});
            }
            const std::size_t edits = steps.size();
            if (autoCrossfade) addCrossfades(s, placed, steps);
            if (copy_)
                runSteps(items.size() == 1 ? "Copy item" : "Copy items", steps);
            else
                runSteps(edits == 1 ? "Move item" : "Move items", steps);
            return;
        }
        case Drag::TrimLeft: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            const double d = state_.deltaBeats;
            auto offsetFor = [&](const model::Take& take, double delta) {
                const double rate = take.playrate > 0 ? take.playrate : 1.0;
                return take.startOffset +
                       (isMidi(&take) ? delta
                                      : s.timeMap->secondsAt(startBeat(s, *anchor) + delta) -
                                            s.timeMap->secondsAt(startBeat(s, *anchor))) *
                           rate;
            };
            // Extending before a take's material starts gives it silence
            // first (a negative offset), as slipping does.
            std::vector<EditStep> steps;
            for (auto id : anchor->takes)
                if (const auto* take = s.takes.find(id)) {
                    json::Value targs = json::Value::object();
                    targs["take"] = id.str();
                    if (isMidi(take))
                        targs["start_offset"] = offsetFor(*take, d);
                    else
                        targs["rebase_seconds"] = s.timeMap->secondsAt(startBeat(s, *anchor) + d) -
                                                  s.timeMap->secondsAt(startBeat(s, *anchor));
                    steps.push_back({"take.set", std::move(targs)});
                }
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["position"] = startBeat(s, *anchor) + d;
            args["length"] = lengthBeats(s, *anchor) - d;
            steps.insert(steps.begin(), {"item.set", std::move(args)});
            if (autoCrossfade)
                addCrossfades(
                    s,
                    {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor) + d,
                      startBeat(s, *anchor) + lengthBeats(s, *anchor)}},
                    steps);
            runSteps("Trim item start", steps);
            return;
        }
        case Drag::TrimRight: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["length"] = lengthBeats(s, *anchor) + state_.deltaBeats;
            const model::Take* take = activeTake(s, *anchor);
            if (state_.stretch && take) {
                json::Value rate = json::Value::object();
                rate["take"] = anchor->activeTake->str();
                rate["playrate"] = stretchRate(*anchor, *take, state_.deltaBeats);
                std::vector<EditStep> steps = {{"item.set", std::move(args)},
                                               {"take.set", std::move(rate)}};
                if (autoCrossfade)
                    addCrossfades(
                        s,
                        {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor),
                          startBeat(s, *anchor) + lengthBeats(s, *anchor) + state_.deltaBeats}},
                        steps);
                runSteps("Stretch item", steps);
                return;
            }
            std::vector<EditStep> steps = {{"item.set", std::move(args)}};
            if (autoCrossfade)
                addCrossfades(
                    s,
                    {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor),
                      startBeat(s, *anchor) + lengthBeats(s, *anchor) + state_.deltaBeats}},
                    steps);
            runSteps("Trim item end", steps);
            return;
        }
        case Drag::Slip: {
            const model::Take* take = anchor ? activeTake(s, *anchor) : nullptr;
            if (!take || anchor->locked || state_.deltaBeats == 0) return;
            const double rate = take->playrate > 0 ? take->playrate : 1.0;
            const double shift =
                isMidi(take) ? state_.deltaBeats * rate
                             : (s.timeMap->secondsAt(startBeat(s, *anchor) + state_.deltaBeats) -
                                s.timeMap->secondsAt(startBeat(s, *anchor))) *
                                   rate;
            json::Value args = json::Value::object();
            args["take"] = anchor->activeTake->str();
            // Past the source's start is silence first (a negative offset).
            args["start_offset"] = take->startOffset - shift;
            runSteps("Slip item", {{"take.set", std::move(args)}});
            return;
        }
        case Drag::FadeIn:
        case Drag::FadeOut: {
            if (!anchor || anchor->locked) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args[state_.drag == Drag::FadeIn ? "fade_in" : "fade_out"] = state_.fadeSeconds;
            runSteps(state_.drag == Drag::FadeIn ? "Fade in" : "Fade out",
                     {{"item.set", std::move(args)}});
            return;
        }
        case Drag::Comp:
        case Drag::TakeOut:
        case Drag::EnvPoint:
        case Drag::AutoItemMove:
        case Drag::AutoItemLength:
        case Drag::None:
            return;
    }
}

bool ItemGestures::envPress(model::Id envId, const ui::PointerEvent& e,
                            std::optional<AutoHit> autoItem) {
    const model::Session& s = view_.session();
    const model::Envelope* env = s.envelopes.find(envId);
    auto frame = env ? envFrame(view_, envId, *env) : std::nullopt;
    if (!frame) return true;
    state_.drag = Drag::EnvPoint;
    state_.env = envId;
    state_.envDraft = state_.envBase = env->points;
    // Inside an automation item the press edits the item's points (as played,
    // in the loop pass under the pointer); elsewhere the envelope's own.
    if (const model::AutomationItem* ai = autoItem ? s.autoItems.find(autoItem->item) : nullptr) {
        if (std::abs(ai->amplitude) <= 1e-9) {
            state_ = State{};
            if (onError)
                onError(Error("autoitem.amplitude", "restore the amplitude before editing points",
                              {},
                              "set the automation item's amplitude magnitude above 0.000000001"));
            return true;
        }
        state_.autoItem = autoItem->item;
        state_.autoBase = s.autoItems.entry(autoItem->item)->value;
        state_.envOffset = autoItem->passStart;
        frame->offsetBeats += autoItem->passStart;
        state_.envDraft = state_.envBase = ai->points;
        for (model::EnvPoint& pt : state_.envDraft) pt.value = Lanes::autoItemValue(*ai, pt.value);
    }
    std::optional<std::size_t> nearest;
    float reach = 7;
    for (std::size_t i = 0; i < state_.envDraft.size(); ++i) {
        const model::EnvPoint& pt = state_.envDraft[i];
        const float x = view_.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos));
        const float y = frame->y(pt.value);
        const float d = std::max(std::abs(x - e.pos.x), std::abs(y - e.pos.y));
        if (d < reach) {
            reach = d;
            nearest = i;
        }
    }
    const std::string_view press = mouse.behaviour("envelope", e.mods);
    free_ = press == "edit_free";
    if (press == "toggle" && !state_.autoItem) {
        if (nearest) {  // in or out of the selection (other lanes' points stay)
            std::vector<model::EnvPoint> picked = selectedPoints(envId);
            const model::EnvPoint& pt = state_.envDraft[*nearest];
            const auto was =
                std::find_if(picked.begin(), picked.end(),
                             [&](const model::EnvPoint& q) { return samePoint(q, pt); });
            if (was != picked.end())
                picked.erase(was);
            else
                picked.push_back(pt);
            pointEnv_ = envId;
            if (picked.empty())
                pointSel_.erase(envId);
            else
                pointSel_[envId] = std::move(picked);
            if (root_) root_->requestAnimationFrame();
            state_ = State{};
            return true;
        }
        // From empty space: a marquee that adds to the selection.
        state_ = State{};
        state_.drag = Drag::Marquee;
        state_.env = envId;
        state_.marquee = {e.pos.x, e.pos.y, 0, 0};
        state_.marqueeBase = selection_;
        state_.pointBase = pointSel_;
        marqueeAdd_ = true;
        if (root_) root_->capture(&view_);
        return true;
    }
    if (nearest && press == "delete") {
        state_.envDraft.erase(state_.envDraft.begin() + static_cast<std::ptrdiff_t>(*nearest));
        commitEnv("Delete envelope point");
        state_ = State{};
        return true;
    }
    if (nearest) {
        state_.envIndex = *nearest;
        if (!state_.autoItem) {
            // A point outside the selection becomes it; one in it drags them
            // all, in every lane.
            const model::EnvPoint& pt = state_.envDraft[*nearest];
            std::vector<model::EnvPoint> picked = selectedPoints(envId);
            if (selectedIndices(picked, {pt}).empty()) {
                picked = {pt};
                selectPoints(envId, picked);
            }
            pointEnv_ = envId;
            for (model::Id other : pointEnvelopes()) {
                const model::Envelope* en = other == envId ? nullptr : s.envelopes.find(other);
                if (!en) continue;
                State::Along along;
                along.env = other;
                along.base = along.draft = en->points;
                along.picked = along.moved = selectedPoints(other);
                state_.envAlong.push_back(std::move(along));
            }
            if (picked.size() > 1 || !state_.envAlong.empty()) {
                state_.envPicked = picked;
                state_.envMoved = std::move(picked);
            }
        }
    } else {  // a new point under the pointer
        model::EnvPoint pt;
        pt.pos = std::max(0.0, snap(beatAtX(e.pos.x), free_) - frame->offsetBeats);
        if (state_.autoItem)
            pt.pos = std::min(pt.pos, s.autoItems.find(*state_.autoItem)->sourceLength);
        pt.value = frame->value(e.pos.y);
        const auto at =
            std::upper_bound(state_.envDraft.begin(), state_.envDraft.end(), pt.pos,
                             [](double b, const model::EnvPoint& p) { return b < p.pos; });
        state_.envIndex = static_cast<std::size_t>(at - state_.envDraft.begin());
        state_.envDraft.insert(at, pt);
        state_.envCreated = true;
    }
    state_.envOrigPos = state_.envDraft[state_.envIndex].pos;
    if (root_) {
        root_->capture(&view_);
        root_->requestAnimationFrame();
    }
    return true;
}

void ItemGestures::envDrag(const ui::PointerEvent& e) {
    const model::Session& s = view_.session();
    const model::Envelope* env = state_.env ? s.envelopes.find(*state_.env) : nullptr;
    auto frame = env ? envFrame(view_, *state_.env, *env) : std::nullopt;
    if (!frame || state_.envIndex >= state_.envDraft.size()) return;
    frame->offsetBeats += state_.envOffset;
    const model::AutomationItem* ai =
        state_.autoItem ? s.autoItems.find(*state_.autoItem) : nullptr;
    if (!state_.envMoved.empty()) {  // the selected points move together
        const model::EnvPoint& anchor = state_.envBase[state_.envIndex];
        double beats = 0;
        if (std::abs(e.pos.x - press_.x) >= 4) {
            const double raw =
                frame->offsetBeats + anchor.pos + beatAtX(e.pos.x) - beatAtX(press_.x);
            beats = snap(raw, free_) - frame->offsetBeats - anchor.pos;
        }
        const double norm = static_cast<double>((press_.y - e.pos.y) / frame->area.h);
        const EnvEdit moved =
            movePoints(state_.envBase, state_.envPicked, beats, norm, frame->scale);
        state_.envDraft = moved.points;
        state_.envMoved = moved.selected;
        // Other lanes' points by the same time and the same share of their lane.
        for (State::Along& along : state_.envAlong) {
            const model::Envelope* other = s.envelopes.find(along.env);
            const auto f = other ? envFrame(view_, along.env, *other) : std::nullopt;
            if (!f) continue;
            const EnvEdit m = movePoints(along.base, along.picked, beats, norm, f->scale);
            along.draft = m.points;
            along.moved = m.selected;
        }
        return;
    }
    auto& pts = state_.envDraft;
    model::EnvPoint& pt = pts[state_.envIndex];
    // A mostly vertical drag keeps the point's time (snapping would move it).
    if (std::abs(e.pos.x - press_.x) >= 4) {
        const double lo = state_.envIndex > 0 ? pts[state_.envIndex - 1].pos : 0.0;
        const double hi = state_.envIndex + 1 < pts.size() ? pts[state_.envIndex + 1].pos
                          : ai                             ? ai->sourceLength
                                                           : 1e12;
        const double raw =
            frame->offsetBeats + state_.envOrigPos + beatAtX(e.pos.x) - beatAtX(press_.x);
        pt.pos = std::clamp(snap(raw, free_) - frame->offsetBeats, lo, hi);
    } else {
        pt.pos = state_.envOrigPos;
    }
    pt.value = frame->value(e.pos.y);
}

std::optional<model::Id> ItemGestures::takeEnvelopeAt(model::Id item, ui::Point p) const {
    const model::Session& s = view_.session();
    const model::Item* it = itemOf(s, item);
    if (!it || !it->activeTake) return std::nullopt;
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        if (env.target.object != *it->activeTake || !env.visible) continue;
        const auto frame = envFrame(view_, e.id, env);
        if (!frame || !frame->area.contains(p)) continue;
        // Near one of its points, or near its line at the pointer's time.
        for (const model::EnvPoint& pt : env.points) {
            const float x = view_.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos));
            if (std::abs(x - p.x) < 7 && std::abs(frame->y(pt.value) - p.y) < 7) return e.id;
        }
        const double beat = beatAtX(p.x) - frame->offsetBeats;
        if (std::abs(frame->y(envValueAt(env, beat, frame->scale.reference())) - p.y) < 5)
            return e.id;
    }
    return std::nullopt;
}

std::optional<ItemGestures::AutoHit> ItemGestures::autoItemAt(model::Id env, ui::Point p) const {
    const model::Session& s = view_.session();
    const auto row = view_.trackLayout().indexOfEnv(env);
    if (!row) return std::nullopt;
    const ui::Rect lane = view_.rowRect(*row);
    if (!lane.contains(p)) return std::nullopt;
    const double beat = beatAtX(p.x);
    std::optional<AutoHit> found;
    double foundStart = -1;
    for (const auto& e : s.autoItems) {
        const model::AutomationItem& ai = *e.value;
        if (ai.envelope != env) continue;
        const float x0 = view_.xAt(s.timeMap->secondsAt(ai.position)),
                    x1 = view_.xAt(s.timeMap->secondsAt(ai.position + ai.length));
        if (p.x < x0 || p.x > x1 + 3) continue;
        if (ai.position < foundStart) continue;  // a later item covers an earlier one
        foundStart = ai.position;
        AutoHit h;
        h.item = e.id;
        h.zone = p.x >= x1 - kEdge                       ? Drag::AutoItemLength
                 : p.y < lane.y + 1 + Lanes::kAutoHeader ? Drag::AutoItemMove
                                                         : Drag::EnvPoint;
        const double pass = ai.loop && ai.sourceLength > 0
                                ? std::floor(std::max(0.0, beat - ai.position) / ai.sourceLength)
                                : 0.0;
        h.passStart = ai.position + pass * ai.sourceLength;
        found = h;
    }
    return found;
}

void ItemGestures::openEnvMenu(model::Id env, ui::Point at) {
    if (!root_) return;
    const model::Session& s = view_.session();
    const model::Envelope* en = s.envelopes.find(env);
    if (!en) return;
    const auto hitItem = autoItemAt(env, at);
    const model::AutomationItem* ai = hitItem ? s.autoItems.find(hitItem->item) : nullptr;
    const bool picked = !selectedPoints().empty();  // in any lane
    std::vector<ui::MenuItem> items;
    auto report = [this](const Status& st) {
        if (!st && onError) onError(st.error());
    };
    auto step = [this](std::string label, std::string command, json::Value args) {
        return [this, label = std::move(label), command = std::move(command),
                args = std::move(args)]() mutable {
            if (!run) return;
            if (auto r = run(label, {{command, std::move(args)}}); !r && onError)
                onError(r.error());
        };
    };
    auto add = [&](std::string label, std::function<void()> fn, bool enabled = true,
                   std::string shortcut = {}) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.run = std::move(fn);
        m.enabled = enabled;
        m.shortcut = std::move(shortcut);
        items.push_back(std::move(m));
    };
    auto rule = [&] {
        ui::MenuItem m;
        m.separator = true;
        items.push_back(std::move(m));
    };
    const double beat = std::max(0.0, snap(beatAtX(at.x), free_));
    // Points: a press there adds one (the same as a click).
    add("Insert point", [this, env, at] {
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Down;
        e.pos = at;
        e.button = 1;
        press_ = at;
        moved_ = false;
        envPress(env, e, std::nullopt);
        if (state_.drag == Drag::EnvPoint) commit(e);
        state_ = State{};
        if (root_) root_->capture(nullptr);
    });
    rule();
    add("Select all points", [this, env] { selectAllPoints(env); }, !en->points.empty());
    add("Copy points", [this] { copyPoints(); }, picked, "Ctrl+C");
    add("Cut points", [this, report] { report(cutPoints()); }, picked, "Ctrl+X");
    add(
        "Paste points", [this, env, beat, report] { report(pastePoints(beat, env)); },
        canPastePoints(), "Ctrl+V");
    add("Delete points", [this, report] { report(deletePoints()); }, picked, "Delete");
    add("Point shape…", [this, at] { openShapeMenu(at); }, picked);
    rule();
    // Automation items: one under the pointer, else a new one there.
    if (ai) {
        const std::string id = hitItem->item.str();
        auto ref = [&id] {
            json::Value a = json::Value::object();
            a["item"] = id;
            return a;
        };
        add("Duplicate automation item (pooled)",
            step("Duplicate automation item", "autoitem.duplicate", ref()));
        json::Value unpooledCopy = ref();
        unpooledCopy["pooled"] = false;
        add("Duplicate automation item (copy)",
            step("Duplicate automation item", "autoitem.duplicate", std::move(unpooledCopy)));
        if (ai->pool > 0)
            add("Unpool automation item", step("Unpool automation item", "autoitem.unpool", ref()));
        json::Value loop = ref();
        loop["loop"] = !ai->loop;
        add("Loop automation item", step("Automation item loop", "autoitem.set", std::move(loop)));
        items.back().checked = ai->loop;
        add("Delete automation item", step("Delete automation item", "autoitem.remove", ref()));
    } else {
        json::Value args = json::Value::object();
        args["env"] = env.str();
        args["position"] = beat;
        args["length"] = 4.0;
        add("Insert automation item",
            step("Insert automation item", "autoitem.add", std::move(args)));
    }
    rule();
    // The envelope.
    json::Value bypass = json::Value::object();
    bypass["env"] = env.str();
    bypass["active"] = !en->active;
    add("Bypass envelope",
        step(en->active ? "Bypass envelope" : "Enable envelope", "env.set", std::move(bypass)));
    items.back().checked = !en->active;
    json::Value hide = json::Value::object();
    hide["env"] = env.str();
    hide["visible"] = false;
    add("Hide envelope", step("Hide envelope", "env.set", std::move(hide)));
    json::Value remove = json::Value::object();
    remove["env"] = env.str();
    add("Remove envelope", step("Remove envelope", "env.remove", std::move(remove)));
    root_->openPopup(ui::makeMenu("env.menu", std::move(items), [this] { root_->closePopup(); }),
                     {at.x, at.y, 1, 1});
}

void ItemGestures::openShapeMenu(ui::Point at) {
    if (!root_) return;
    const auto picked = selectedPoints();
    std::vector<ui::MenuItem> items;
    for (auto [label, shape] : {std::pair{"Linear", model::PointShape::Linear},
                                {"Square", model::PointShape::Square},
                                {"Slow start/end", model::PointShape::Slow},
                                {"Fast start", model::PointShape::FastStart},
                                {"Fast end", model::PointShape::FastEnd},
                                {"Bezier", model::PointShape::Bezier}}) {
        ui::MenuItem m;
        m.label = label;
        m.checked = !picked.empty() &&
                    std::all_of(picked.begin(), picked.end(),
                                [shape](const model::EnvPoint& p) { return p.shape == shape; });
        m.run = [this, shape] {
            if (auto st = shapePoints(shape); !st && onError) onError(st.error());
        };
        items.push_back(std::move(m));
    }
    // Opened from the lane's menu as it closes.
    root_->openPopup(
        ui::makeMenu("env.shape.menu", std::move(items), [this] { root_->closePopup(); }),
        {at.x, at.y, 1, 1});
}

std::vector<model::EnvPoint> ItemGestures::selectedPoints(model::Id envId) const {
    const auto sel = pointSel_.find(envId);
    const model::Envelope* env =
        sel != pointSel_.end() ? view_.session().envelopes.find(envId) : nullptr;
    if (!env) return {};
    std::vector<model::EnvPoint> out;
    for (std::size_t i : selectedIndices(env->points, sel->second)) out.push_back(env->points[i]);
    return out;
}

std::vector<model::EnvPoint> ItemGestures::selectedPoints() const {
    std::vector<model::EnvPoint> out;
    for (const auto& entry : pointSel_) {
        const auto picked = selectedPoints(entry.first);
        out.insert(out.end(), picked.begin(), picked.end());
    }
    return out;
}

std::vector<model::Id> ItemGestures::pointEnvelopes() const {
    std::vector<model::Id> out;
    for (const auto& entry : pointSel_)
        if (!selectedPoints(entry.first).empty()) out.push_back(entry.first);
    return out;
}

void ItemGestures::selectPoints(model::Id env, std::vector<model::EnvPoint> points) {
    pointEnv_ = env;
    pointSel_.clear();
    if (!points.empty()) pointSel_[env] = std::move(points);
    mixed_ = false;
    if (root_) root_->requestAnimationFrame();
}

void ItemGestures::selectAllPoints(model::Id env) {
    if (const model::Envelope* en = view_.session().envelopes.find(env))
        selectPoints(env, en->points);
}

void ItemGestures::clearPoints() {
    if (!pointEnv_ && pointSel_.empty()) return;
    pointEnv_.reset();
    pointSel_.clear();
    mixed_ = false;
    if (root_) root_->requestAnimationFrame();
}

std::vector<model::Id> ItemGestures::shownEnvelopes() const {
    const model::Session& s = view_.session();
    std::vector<model::Id> out;
    for (const TrackRow& row : view_.trackLayout().rows())
        if (row.env) out.push_back(*row.env);
    for (const auto& e : s.envelopes)  // drawn on their items
        if (e.value->target.object.kind == model::Kind::Take && e.value->visible)
            out.push_back(e.id);
    return out;
}

void ItemGestures::marqueePoints() {
    const model::Session& s = view_.session();
    const ui::Rect& m = state_.marquee;
    pointSel_ = state_.pointBase;
    std::optional<model::Id> first;
    for (model::Id id : shownEnvelopes()) {
        const model::Envelope* env = s.envelopes.find(id);
        const auto frame = env ? envFrame(view_, id, *env) : std::nullopt;
        if (!frame) continue;
        const bool onItem = env->target.object.kind == model::Kind::Take;
        std::vector<model::EnvPoint>& picked = pointSel_[id];
        for (const model::EnvPoint& pt : env->points) {
            const ui::Point at{view_.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos)),
                               frame->y(pt.value)};
            // A take envelope's points beyond its item are not drawn.
            if (onItem && (at.x < frame->area.x || at.x > frame->area.right())) continue;
            if (m.contains(at) && selectedIndices(picked, {pt}).empty()) picked.push_back(pt);
        }
        if (picked.empty())
            pointSel_.erase(id);
        else if (!first)
            first = id;
    }
    // Pasting goes to the topmost lane it took points in.
    if (first)
        pointEnv_ = first;
    else if (pointSel_.empty())
        pointEnv_.reset();
}

std::vector<EditStep> ItemGestures::pointSteps(const EnvEdits& edits) const {
    std::vector<EditStep> steps;
    for (const auto& [env, edit] : edits) {
        json::Value args = json::Value::object(), points = json::Value::array();
        args["env"] = env.str();
        for (const model::EnvPoint& pt : edit.points) {
            json::Value p = json::Value::object();
            p["beat"] = pt.pos;
            p["value"] = pt.value;
            p["shape"] = shapeName(pt.shape);
            if (pt.tension != 0) p["tension"] = pt.tension;
            points.push_back(std::move(p));
        }
        args["points"] = std::move(points);
        steps.push_back({"env.points.set", std::move(args)});
    }
    return steps;
}

void ItemGestures::adoptPoints(const EnvEdits& edits) {
    const std::optional<model::Id> last = pointEnv_;
    pointSel_.clear();
    mixed_ = false;
    for (const auto& [env, edit] : edits)
        if (!edit.selected.empty()) pointSel_[env] = edit.selected;
    const bool kept = std::any_of(edits.begin(), edits.end(),
                                  [&](const auto& edit) { return edit.first == last; });
    pointEnv_ = kept || edits.empty() ? last : std::optional(edits.front().first);
    if (root_) root_->requestAnimationFrame();
}

Status ItemGestures::setPoints(const EnvEdits& edits, const std::string& label) {
    if (!run) return Error("gui.unwired", "no command runner");
    if (edits.empty()) return {};
    if (auto r = run(label, pointSteps(edits)); !r) return r.error();
    adoptPoints(edits);
    return {};
}

ItemGestures::EnvEdits ItemGestures::pointDeletions(std::size_t* count) const {
    EnvEdits edits;
    *count = 0;
    for (model::Id id : pointEnvelopes()) {
        const auto picked = selectedPoints(id);
        *count += picked.size();
        edits.push_back(
            {id, views::deletePoints(view_.session().envelopes.find(id)->points, picked)});
    }
    return edits;
}

Status ItemGestures::deletePoints() {
    std::size_t count = 0;
    const EnvEdits edits = pointDeletions(&count);
    return setPoints(edits, count == 1 ? "Delete envelope point" : "Delete envelope points");
}

Status ItemGestures::deleteAll() {
    if (!run) return Error("gui.unwired", "no command runner");
    std::size_t count = 0;
    const EnvEdits edits = pointDeletions(&count);
    // The points first: a take envelope goes with its item.
    std::vector<EditStep> steps = pointSteps(edits);
    for (model::Id item : selection_) {
        json::Value args = json::Value::object();
        args["item"] = item.str();
        steps.push_back({"item.remove", std::move(args)});
    }
    if (steps.empty()) return {};
    if (auto r = run("Delete items and envelope points", steps); !r) return r.error();
    adoptPoints(edits);
    selection_.clear();
    changed();
    return {};
}

void ItemGestures::copyPoints() {
    PointClip clip;
    double earliest = 0;
    for (model::Id id : pointEnvelopes()) {
        const model::Envelope* env = view_.session().envelopes.find(id);
        const auto frame = envFrame(view_, id, *env);
        const auto picked = selectedPoints(id);
        if (!frame || picked.empty()) continue;
        const double at = frame->offsetBeats + picked.front().pos;  // in time order
        earliest = clip.parts.empty() ? at : std::min(earliest, at);
        clip.parts.push_back(
            {id, views::copyPoints(env->points, picked, frame->scale, envUnit(*env, frame->scale)),
             at});
    }
    if (clip.parts.empty()) return;
    for (auto& part : clip.parts) part.at -= earliest;
    pointClip_ = std::move(clip);
}

Status ItemGestures::cutPoints() {
    copyPoints();
    return deletePoints();
}

Status ItemGestures::pastePoints(double atBeat, std::optional<model::Id> envId) {
    const bool one = pointClip_.parts.size() == 1;
    EnvEdits edits;
    std::size_t count = 0;
    for (const auto& part : pointClip_.parts) {
        const std::optional<model::Id> target =
            one ? (envId ? envId : pointEnv_) : std::optional(part.env);
        const model::Envelope* env = target ? view_.session().envelopes.find(*target) : nullptr;
        const auto frame = env ? envFrame(view_, *target, *env) : std::nullopt;
        if (!frame) continue;
        // Points are beats from the envelope's start (a take envelope: its item's).
        edits.push_back({*target, views::pastePoints(env->points, part.clip,
                                                     atBeat + part.at - frame->offsetBeats,
                                                     frame->scale, envUnit(*env, frame->scale))});
        count += part.clip.points.size();
    }
    return setPoints(edits, count == 1 ? "Paste envelope point" : "Paste envelope points");
}

Status ItemGestures::shapePoints(model::PointShape shape) {
    EnvEdits edits;
    for (model::Id id : pointEnvelopes())
        edits.push_back({id, views::shapePoints(view_.session().envelopes.find(id)->points,
                                                selectedPoints(id), shape)});
    return setPoints(edits, "Envelope point shape");
}

bool ItemGestures::checkAutoItem() const {
    const auto* now = state_.autoItem ? view_.session().autoItems.find(*state_.autoItem) : nullptr;
    const auto* before = state_.autoBase.get();
    if (!now || !before) return false;
    if (now->amplitude == before->amplitude && now->baseline == before->baseline &&
        now->position == before->position && now->length == before->length &&
        now->sourceLength == before->sourceLength && now->loop == before->loop &&
        now->envelope == before->envelope && now->pool == before->pool)
        return true;
    if (onError)
        onError(Error("env.changed", "the automation changed while you were dragging", {},
                      "drag the item or point again"));
    return false;
}

void ItemGestures::commitEnv(const std::string& label) {
    if (!run || !state_.env) return;
    if (!view_.session().envelopes.contains(*state_.env)) return;
    json::Value args = json::Value::object(), points = json::Value::array();
    const model::AutomationItem* ai =
        state_.autoItem ? view_.session().autoItems.find(*state_.autoItem) : nullptr;
    if (state_.autoItem && !checkAutoItem()) return;
    // The draft replaces every point: if another client changed them since the
    // press, applying it would undo that change, so the drag is dropped.
    auto same = [](const std::vector<model::EnvPoint>& x, const std::vector<model::EnvPoint>& y) {
        return std::equal(x.begin(), x.end(), y.begin(), y.end(),
                          [](const model::EnvPoint& a, const model::EnvPoint& b) {
                              return a.pos == b.pos && a.value == b.value && a.shape == b.shape &&
                                     a.tension == b.tension;
                          });
    };
    const auto& now = ai ? ai->points : view_.session().envelopes.find(*state_.env)->points;
    bool unchanged = same(now, state_.envBase);
    EnvEdits along;  // other lanes' points that moved with these
    for (const State::Along& a : state_.envAlong) {
        const model::Envelope* other = view_.session().envelopes.find(a.env);
        if (!other) continue;
        unchanged = unchanged && same(other->points, a.base);
        along.push_back({a.env, EnvEdit{a.draft, a.moved}});
    }
    if (!unchanged) {
        if (onError)
            onError(Error("env.changed", "the automation changed while you were dragging", {},
                          "drag the point again"));
        return;
    }
    if (ai)
        args["item"] = state_.autoItem->str();
    else
        args["env"] = state_.env->str();
    for (const model::EnvPoint& pt : state_.envDraft) {
        json::Value p = json::Value::object();
        p["beat"] = pt.pos;
        // Back from the played value to the item's own (amplitude scales it).
        p["value"] = ai && std::abs(ai->amplitude) > 1e-9
                         ? ai->baseline + (pt.value - ai->baseline) / ai->amplitude
                         : pt.value;
        p["shape"] = shapeName(pt.shape);
        if (pt.tension != 0) p["tension"] = pt.tension;
        points.push_back(std::move(p));
    }
    args["points"] = std::move(points);
    std::vector<EditStep> steps{{ai ? "autoitem.set" : "env.points.set", std::move(args)}};
    for (EditStep& step : pointSteps(along)) steps.push_back(std::move(step));
    if (auto r = run(label, steps); !r && onError) onError(r.error());
}

const char* importArg(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mid" || ext == ".midi" ? "midi_file" : "audio_file";
}

Status ItemGestures::drop(const std::vector<std::filesystem::path>& files, ui::Point at) {
    if (files.empty() || !run) return {};
    const model::Session& s = view_.session();
    const double position =
        view_.lanesRect().contains(at) ? std::max(0.0, snap(beatAtX(at.x), false)) : 0.0;
    const auto row = rowAt(at.y);
    std::vector<EditStep> steps;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string track;
        if (i == 0 && row &&
            holdsItem(view_.session(), view_.trackLayout().rows()[*row], nullptr)) {
            track = view_.trackLayout().rows()[*row].track.str();
        } else {
            json::Value add = json::Value::object();
            add["name"] = files[i].stem().string();
            steps.push_back({"track.add", std::move(add)});
            track =
                "$step" + std::to_string(steps.size() - 1) + ".track";  // the runner substitutes it
        }
        json::Value item = json::Value::object();
        item["track"] = track;
        item["position"] = position;
        item[importArg(files[i])] = files[i].string();
        steps.push_back({"item.add", std::move(item)});
    }
    (void)s;
    auto r = run(files.size() == 1 ? "Drop file" : "Drop files", steps);
    if (!r) return std::move(r).error();
    return {};
}

}  // namespace supple::views
