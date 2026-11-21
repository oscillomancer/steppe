// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_editor.h"
#include "midi_event_list.h"
#include "panel_grip.h"
#include "panel_height.h"
#include "supple/harmony/harmony.h"
#include "supple/ui/shortcut.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/model/context.h"
#include "supple/notation/notation.h"
#include "supple/ui/controls.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_area.h"
#include "supple/ui/widgets.h"
#include "supple/views/chord_field.h"
#include "supple/views/lanes.h"
#include "supple/views/launcher.h"
#include "supple/views/mouse_map.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kKeysWidth = 46;
constexpr float kEdge = 5;  // px at a note's ends that resize it
constexpr auto kCompileDelay = std::chrono::milliseconds(300);
constexpr float kGripHeight = 6;  // the top border that resizes the panel
// The controllers the Lanes menu offers: mod wheel, volume, pan, expression,
// sustain.
constexpr std::pair<int, const char*> kCcs[] = {
    {1, "mod"}, {7, "volume"}, {10, "pan"}, {11, "expression"}, {64, "sustain"}};

bool blackKey(int pitch) {
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

std::string pitchName(int pitch) { return harmony::spell(pitch); }

// The pitch classes a key's rows show light, as a mask; where no key holds,
// the white keys' (so C major looks as the roll always has).
std::uint16_t scaleMask(const std::optional<harmony::Key>& key) {
    std::uint16_t mask = 0;
    if (!key) {
        for (int pc : {0, 2, 4, 5, 7, 9, 11}) mask = static_cast<std::uint16_t>(mask | 1u << pc);
        return mask;
    }
    for (int pc : harmony::keyPitchClasses(*key))
        mask = static_cast<std::uint16_t>(mask | 1u << pc);
    return mask;
}

constexpr float kChordRowHeight = 18;  // the strip's chord row, under the bar numbers

// The scale degrees that take `pitch` nearest to `pitch + semitones` (the
// row under the pointer): what a degree drag previews and commits. Ties go to
// fewer degrees; none nearer than staying put: 0.
int nearestDegrees(int pitch, const harmony::Key& key, int semitones) {
    const int target = pitch + semitones;
    int best = 0, distance = std::abs(semitones);
    for (int d = 1; d <= 84 && distance > 0; ++d)
        for (int step : {d, -d})
            if (const auto moved = harmony::transposeDegrees(pitch, key, step);
                moved && std::abs(*moved - target) < distance) {
                best = step;
                distance = std::abs(*moved - target);
            }
    return best;
}

}  // namespace

std::optional<harmony::Key> RollState::keyAt(double projectBeat) const {
    std::optional<harmony::Key> key;
    for (const KeySpan& k : keys) {
        if (k.beat > projectBeat) break;
        key = k.key;
    }
    return key;
}

double RollState::snapBeat(double itemBeat) const {
    if (!snap || grid <= 0) return itemBeat;
    const double project = itemPosition + itemBeat;
    return std::round(project / grid) * grid - itemPosition;
}

// ----------------------------------------------------------------- geometry

namespace {

struct Geometry {
    const RollState& s;
    ui::Rect area;  // the roll's bounds (notes area)
    float xOf(double beat) const {
        return area.x + static_cast<float>((beat - s.scrollBeat) * s.pixelsPerBeat);
    }
    double beatOf(float x) const {
        return s.scrollBeat + static_cast<double>(x - area.x) / s.pixelsPerBeat;
    }
    float yOf(int pitch) const {
        return area.y + static_cast<float>(s.topPitch - pitch) * s.rowHeight;
    }
    int pitchOf(float y) const {
        return static_cast<int>(std::ceil(s.topPitch - static_cast<double>(y - area.y) /
                                                           static_cast<double>(s.rowHeight)));
    }
    // A note as drawn, with the drag preview applied when it is selected.
    RollNote shown(std::size_t i) const {
        RollNote n = s.notes[i];
        if (!s.selected.contains(i)) return n;
        switch (s.drag) {
            case RollState::Drag::Move:
                n.start += s.dBeats;
                if (!s.degrees) {
                    n.pitch = std::clamp(n.pitch + s.dPitch, 0, 127);
                } else if (s.dDegrees != 0) {  // by the key where the note lands
                    if (const auto key = s.keyAt(s.itemPosition + n.start))
                        n.pitch =
                            harmony::transposeDegrees(n.pitch, *key, s.dDegrees).value_or(n.pitch);
                }
                break;
            case RollState::Drag::ResizeEnd:
                n.length = std::max(1.0 / 64, n.length + s.dLength);
                break;
            case RollState::Drag::ResizeStart: {
                const double end = n.start + n.length;
                n.start = std::min(end - 1.0 / 64, n.start + s.dStart);
                n.length = end - n.start;
                break;
            }
            case RollState::Drag::Velocity:
                n.velocity = std::clamp(n.velocity + s.dVelocity, 1, 127);
                break;
            default:
                break;
        }
        return n;
    }
    ui::Rect rectOf(const RollNote& n) const {
        const float x = xOf(n.start);
        return {x, yOf(n.pitch) + 1, std::max(3.0f, xOf(n.start + n.length) - x - 1),
                s.rowHeight - 2};
    }
};

// A note's colour: the item's (the selection's when selected), fuller the
// harder the note is played. A soft note keeps 45% of it and is drawn the
// rest of the way towards the ground, so it fades in either theme.
ui::Color velocityColor(const ui::Theme& t, const RollState& s, int velocity, bool selected) {
    const ui::Color base = selected ? t.color("focus") : s.color ? s.color(t) : t.color("accent");
    const ui::Color ground = t.color("ground");
    const float k = 0.45f + 0.55f * static_cast<float>(velocity) / 127.0f;
    return {ground.r + (base.r - ground.r) * k, ground.g + (base.g - ground.g) * k,
            ground.b + (base.b - ground.b) * k, 1};
}

// ----------------------------------------------------------------- the roll

class Roll : public ui::Node {
  public:
    Roll(RollState& s, MidiEditor& editor) : Node("midi.roll"), s_(s), editor_(editor) {
        focusable = true;
        interactive = true;
        layout.width = ui::SizeRule::fraction();
        layout.height = ui::SizeRule::fraction();
    }

    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, bounds()};
        const ui::Rect b = bounds();
        c.save();
        c.clip(b);
        // Rows: those in the scale on the panel, the others on the darker
        // ground, per span of constant key (ADR-0095). Where no key holds,
        // the black keys' rows are the dark ones, as on the keyboard beside
        // them, so C major looks the same. C lines stronger.
        c.fillRect(b, t.color("panel"));
        const int top = g.pitchOf(b.y), bottom = g.pitchOf(b.bottom());
        {
            std::vector<std::pair<float, std::uint16_t>> spans{{b.x, scaleMask(std::nullopt)}};
            for (const RollState::KeySpan& k : s_.keys) {
                const float x = std::max(b.x, g.xOf(k.beat - s_.itemPosition));
                if (x > b.right()) break;
                if (x <= b.x) spans.clear();  // it holds from the left edge
                spans.push_back({x, scaleMask(k.key)});
            }
            for (std::size_t i = 0; i < spans.size(); ++i) {
                const float xa = spans[i].first;
                const float xb = i + 1 < spans.size() ? spans[i + 1].first : b.right();
                if (xb <= xa) continue;
                for (int p = std::min(127, top); p >= std::max(0, bottom); --p)
                    if (!(spans[i].second >> (p % 12) & 1u))
                        c.fillRect({xa, g.yOf(p), xb - xa, s_.rowHeight}, t.color("ground"));
            }
        }
        for (int p = std::min(127, top); p >= std::max(0, bottom); --p)
            if (p % 12 == 0)
                c.fillRect({b.x, g.yOf(p) + s_.rowHeight - 1, b.w, 1}, t.color("rule"));
        // Outside the item: shaded.
        const float x0 = g.xOf(0), x1 = g.xOf(s_.itemLength);
        if (x0 > b.x) c.fillRect({b.x, b.y, x0 - b.x, b.h}, t.color("ground").withAlpha(0.6f));
        if (x1 < b.right())
            c.fillRect({x1, b.y, b.right() - x1, b.h}, t.color("ground").withAlpha(0.6f));
        // Grid: project grid lines, beats, bars.
        const double first = g.beatOf(b.x), last = g.beatOf(b.right());
        if (s_.grid > 0 && s_.grid * s_.pixelsPerBeat >= 6) {
            const double p0 = std::floor((s_.itemPosition + first) / s_.grid) * s_.grid;
            for (double p = p0; p - s_.itemPosition <= last; p += s_.grid) {
                const float x = g.xOf(p - s_.itemPosition);
                const bool beat = std::abs(p - std::round(p)) < 1e-9;
                c.fillRect({x, b.y, 1, b.h}, t.color("rule").withAlpha(beat ? 0.9f : 0.4f));
            }
        }
        for (double bar : s_.barLines) {
            const float x = g.xOf(bar - s_.itemPosition);
            c.fillRect({x, b.y, 1, b.h}, t.color("ink-faint"));
        }
        // Notes (selected on top), names when there is room.
        std::vector<std::size_t> order(s_.notes.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_partition(order.begin(), order.end(),
                              [&](std::size_t i) { return !s_.selected.contains(i); });
        for (std::size_t i : order) {
            const RollNote n = g.shown(i);
            const ui::Rect r = g.rectOf(n);
            if (r.right() < b.x || r.x > b.right() || r.bottom() < b.y || r.y > b.bottom())
                continue;
            const bool sel = s_.selected.contains(i);
            c.fillRound(r, 2, velocityColor(t, s_, n.velocity, sel));
            c.strokeRound(r, 2, sel ? t.color("ink") : t.color("ground").withAlpha(0.7f));
            if (r.w > 34 && s_.rowHeight >= 11)
                c.text({r.x + 3, r.y, r.w - 4, r.h},
                       harmony::spell(n.pitch, s_.keyAt(s_.itemPosition + n.start)), ui::Font::Ui,
                       t.type("caption") - 1, t.color("ground"), ui::kLeft | ui::kMiddle);
        }
        if (s_.drag == RollState::Drag::Marquee) {
            c.fillRect(s_.marquee, t.color("selection").withAlpha(0.35f));
            c.strokeRect(s_.marquee, t.color("focus"));
        }
        c.restore();
    }

    std::optional<std::size_t> noteAt(ui::Point p, int* edge) const {
        const Geometry g{s_, bounds()};
        for (bool selected : {true, false})
            for (std::size_t k = s_.notes.size(); k-- > 0;) {
                if (s_.selected.contains(k) != selected) continue;
                const ui::Rect r = g.rectOf(s_.notes[k]);
                if (!r.contains(p)) continue;
                if (edge)
                    *edge = p.x >= r.right() - kEdge && r.w > 2 * kEdge ? 1
                            : p.x <= r.x + kEdge && r.w > 2 * kEdge     ? -1
                                                                        : 0;
                return k;
            }
        return std::nullopt;
    }

    // A note's ends set its length; a note in hand is being moved.
    ui::Cursor cursorAt(ui::Point p) const override {
        switch (s_.drag) {
            case RollState::Drag::ResizeEnd:
            case RollState::Drag::ResizeStart:
                return ui::Cursor::ResizeHorizontal;
            case RollState::Drag::Move:
                return moved_ ? ui::Cursor::Move : ui::Cursor::Arrow;
            case RollState::Drag::Marquee:
            case RollState::Drag::Velocity:
                return ui::Cursor::Arrow;
            case RollState::Drag::None:
                break;
        }
        int edge = 0;
        return noteAt(p, &edge) && edge != 0 ? ui::Cursor::ResizeHorizontal : ui::Cursor::Arrow;
    }

    bool onPointer(const ui::PointerEvent& e) override {
        const bool handled = handle(e);
        noteDrag(e);
        return handled;
    }

    // Edge scroll, as in the arrange view (gui.md §4.3): a note drag or a
    // marquee held within the zone at the roll's left or right edge, or
    // past it, scrolls in time; a move and a marquee also scroll the
    // pitches at the top and bottom. The press point goes with the content
    // and the drag's last event is delivered again, so the notes in hand
    // stay under the pointer.
    bool autoScrolling() const { return edgeVx_ != 0 || edgeVy_ != 0; }
    bool autoScroll(double seconds) {
        if (!autoScrolling()) return false;
        if (!scrollsAtEdge()) {
            edgeVx_ = edgeVy_ = 0;
            return false;
        }
        const double beat = s_.scrollBeat, top = s_.topPitch;
        s_.scrollBeat += static_cast<double>(edgeVx_) * seconds / s_.pixelsPerBeat;
        // Leftwards it stops just before the item's start, where the view opens.
        if (edgeVx_ < 0) s_.scrollBeat = std::max(s_.scrollBeat, std::min(beat, kLeftLimit));
        s_.topPitch = std::clamp(
            top - static_cast<double>(edgeVy_) * seconds / static_cast<double>(s_.rowHeight), 12.0,
            127.0);
        const auto dx = static_cast<float>((s_.scrollBeat - beat) * s_.pixelsPerBeat);
        const auto dy = static_cast<float>((top - s_.topPitch) * static_cast<double>(s_.rowHeight));
        if (dx == 0 && dy == 0) return false;  // at an end of the view's range
        press_.x -= dx;
        press_.y -= dy;
        handle(last_);
        repaintAll();
        return true;
    }

  private:
    static constexpr double kLeftLimit = -0.5;  // beats before the item's start
    bool scrollsAtEdge() const {
        return s_.drag == RollState::Drag::Move || s_.drag == RollState::Drag::ResizeEnd ||
               s_.drag == RollState::Drag::ResizeStart || s_.drag == RollState::Drag::Marquee;
    }
    void noteDrag(const ui::PointerEvent& e) {
        using T = ui::PointerEvent::Type;
        if (!scrollsAtEdge() || e.type != T::Move) {
            if (!scrollsAtEdge() || e.type == T::Up || e.type == T::Cancel) edgeVx_ = edgeVy_ = 0;
            return;
        }
        const ui::Rect b = bounds();
        const bool rows = s_.drag == RollState::Drag::Move || s_.drag == RollState::Drag::Marquee;
        edgeVx_ = ui::edgeScrollSpeed(e.pos.x, b.x, b.right());
        edgeVy_ = rows ? ui::edgeScrollSpeed(e.pos.y, b.y, b.bottom()) : 0.0f;
        last_ = e;
        if (autoScrolling() && root()) root()->requestAnimationFrame();  // sync advances it
    }
    float edgeVx_ = 0, edgeVy_ = 0;  // design units per second
    ui::PointerEvent last_;

  public:
    bool handle(const ui::PointerEvent& e) {
        const Geometry g{s_, bounds()};
        using T = ui::PointerEvent::Type;
        if (e.type == T::Cancel) {
            s_.cancelGesture();
            if (root()) root()->requestAnimationFrame();
            return true;
        }
        if (e.type == T::Down) s_.gestureVersion = editor_.beginGesture();
        if (e.type == T::Wheel) {
            // What each combination does is the mouse map's (midi.wheel).
            const std::string_view behaviour = editor_.wheelBehaviour(e.mods);
            if (behaviour == "zoom" && e.wheelX != 0) {  // a sideways wheel scrolls in time
                s_.scrollBeat -= static_cast<double>(e.wheelX) * 60 / s_.pixelsPerBeat;
            } else if (behaviour == "zoom") {  // zoom in time around the pointer
                const double at = g.beatOf(e.pos.x);
                s_.pixelsPerBeat =
                    std::clamp(s_.pixelsPerBeat * std::pow(1.15, e.wheelY), 8.0, 1200.0);
                s_.scrollBeat = at - static_cast<double>(e.pos.x - bounds().x) / s_.pixelsPerBeat;
            } else if (behaviour == "zoom_rows") {  // zoom the keys and rows around the pointer
                editor_.zoomRows(e.wheelY != 0 ? e.wheelY : e.wheelX, e.pos.y);
            } else if (behaviour == "scroll_time") {
                s_.scrollBeat -= static_cast<double>(e.wheelY + e.wheelX) * 60 / s_.pixelsPerBeat;
            } else {
                s_.topPitch =
                    std::clamp(s_.topPitch + static_cast<double>(e.wheelY) * 3, 12.0, 127.0);
                s_.scrollBeat -= static_cast<double>(e.wheelX) * 60 / s_.pixelsPerBeat;
            }
            repaintAll();
            return true;
        }
        if (e.type == T::Down && e.button == 3) {
            focusSelf();
            if (const auto hit = noteAt(e.pos, nullptr); hit && !s_.selected.contains(*hit))
                s_.selected = {*hit};
            repaintAll();
            editor_.openNoteMenu(e.pos, std::max(0.0, s_.snapBeat(g.beatOf(e.pos.x))));
            return true;
        }
        if (e.type == T::Down && e.button == 1) {
            focusSelf();
            press_ = e.pos;
            moved_ = false;
            int edge = 0;
            const auto hit = noteAt(e.pos, &edge);
            if (e.clicks >= 2) {
                if (hit) {  // double-click a note: delete it
                    s_.selected = {*hit};
                    editor_.removeSelected();
                } else {
                    const double start = std::max(
                        0.0, std::floor((s_.itemPosition + g.beatOf(e.pos.x)) / s_.grid) * s_.grid -
                                 s_.itemPosition);
                    json::Value op = json::Value::object();
                    op["op"] = "add";
                    op["pitch"] = std::clamp(g.pitchOf(e.pos.y), 0, 127);
                    op["start"] = s_.snap ? start : std::max(0.0, g.beatOf(e.pos.x));
                    op["length"] = s_.lastLength;
                    op["velocity"] = s_.lastVelocity;
                    json::Value ops = json::Value::array();
                    ops.push_back(std::move(op));
                    (void)editor_.edit(std::move(ops), s_.gestureVersion);
                }
                s_.drag = RollState::Drag::None;
                return true;
            }
            if (hit) {
                if (e.mods & ui::kShift) {
                    if (!s_.selected.erase(*hit)) s_.selected.insert(*hit);
                } else if (!s_.selected.contains(*hit)) {
                    s_.selected = {*hit};
                }
                anchor_ = *hit;
                s_.drag = edge > 0   ? RollState::Drag::ResizeEnd
                          : edge < 0 ? RollState::Drag::ResizeStart
                                     : RollState::Drag::Move;
                s_.lastLength = s_.notes[*hit].length;
                s_.lastVelocity = s_.notes[*hit].velocity;
            } else {
                if (!(e.mods & ui::kShift)) s_.selected.clear();
                s_.drag = RollState::Drag::Marquee;
                marqueeBase_ = s_.selected;
                s_.marquee = {e.pos.x, e.pos.y, 0, 0};
            }
            s_.dBeats = s_.dLength = s_.dStart = 0;
            s_.dPitch = 0;
            if (Root()) Root()->capture(this);
            repaintAll();
            return true;
        }
        if (e.type == T::Move && s_.drag != RollState::Drag::None) {
            moved_ = moved_ || std::abs(e.pos.x - press_.x) + std::abs(e.pos.y - press_.y) > 3;
            const bool free = (e.mods & ui::kAlt) || !s_.snap;
            const double raw = static_cast<double>(e.pos.x - press_.x) / s_.pixelsPerBeat;
            auto snapped = [&](double anchorBeat) {
                if (free) return raw;
                return s_.snapBeat(anchorBeat + raw) - anchorBeat;
            };
            switch (s_.drag) {
                case RollState::Drag::Move: {
                    s_.dBeats = snapped(s_.notes[anchor_].start);
                    s_.dPitch = static_cast<int>(std::lround((press_.y - e.pos.y) / s_.rowHeight));
                    // Degrees: the note in hand goes to the scale pitch nearest the
                    // pointer's row, in the key where it lands; the others move as
                    // many degrees.
                    s_.dDegrees = 0;
                    const RollNote& anchor = s_.notes[anchor_];
                    if (s_.degrees && s_.dPitch != 0)
                        if (const auto key = s_.keyAt(s_.itemPosition + anchor.start + s_.dBeats))
                            s_.dDegrees = nearestDegrees(anchor.pitch, *key, s_.dPitch);
                    break;
                }
                case RollState::Drag::ResizeEnd:
                    s_.dLength = snapped(s_.notes[anchor_].start + s_.notes[anchor_].length);
                    break;
                case RollState::Drag::ResizeStart:
                    s_.dStart = snapped(s_.notes[anchor_].start);
                    break;
                case RollState::Drag::Marquee: {
                    s_.marquee = {std::min(press_.x, e.pos.x), std::min(press_.y, e.pos.y),
                                  std::abs(e.pos.x - press_.x), std::abs(e.pos.y - press_.y)};
                    s_.selected = marqueeBase_;
                    for (std::size_t i = 0; i < s_.notes.size(); ++i) {
                        const ui::Rect r = g.rectOf(s_.notes[i]);
                        if (r.x < s_.marquee.right() && r.right() > s_.marquee.x &&
                            r.y < s_.marquee.bottom() && r.bottom() > s_.marquee.y)
                            s_.selected.insert(i);
                    }
                    break;
                }
                default:
                    break;
            }
            repaintAll();
            return true;
        }
        if (e.type == T::Up && s_.drag != RollState::Drag::None) {
            if (Root()) Root()->capture(nullptr);
            const auto drag = s_.drag;
            s_.drag = RollState::Drag::None;
            json::Value ops = json::Value::array();
            json::Value sel = json::Value::array();
            for (std::size_t i : s_.selected) sel.push_back(static_cast<std::int64_t>(i));
            json::Value op = json::Value::object();
            op["notes"] = sel;
            const int pitchMove = s_.degrees ? s_.dDegrees : s_.dPitch;
            if (drag == RollState::Drag::Move && moved_ && (s_.dBeats != 0 || pitchMove != 0)) {
                // In time first: a degree op reads the key where the move left the note.
                if (s_.dBeats != 0 || !s_.degrees) {
                    json::Value move = op;
                    move["op"] = "move";
                    if (s_.dBeats != 0) move["beats"] = s_.dBeats;
                    if (!s_.degrees && s_.dPitch != 0) move["semitones"] = s_.dPitch;
                    ops.push_back(std::move(move));
                }
                if (s_.degrees && s_.dDegrees != 0) {
                    op["op"] = "transpose_degrees";
                    op["degrees"] = s_.dDegrees;
                    ops.push_back(std::move(op));
                }
            } else if (drag == RollState::Drag::ResizeEnd && moved_ && s_.dLength != 0) {
                op["op"] = "resize";
                op["length_delta"] = s_.dLength;
                ops.push_back(std::move(op));
                s_.lastLength = std::max(1.0 / 64, s_.notes[anchor_].length + s_.dLength);
            } else if (drag == RollState::Drag::ResizeStart && moved_ && s_.dStart != 0) {
                op["op"] = "resize";
                op["start_delta"] = s_.dStart;
                ops.push_back(std::move(op));
            } else if (drag == RollState::Drag::Move && !moved_ && !(e.mods & ui::kShift)) {
                s_.selected = {anchor_};  // a click on a selected note selects just it
            }
            s_.dBeats = s_.dLength = s_.dStart = 0;
            s_.dPitch = s_.dDegrees = 0;
            if (ops.size() > 0) (void)editor_.edit(std::move(ops), s_.gestureVersion);
            repaintAll();
            return true;
        }
        return false;
    }

    std::function<void()> repaintAll = [] {};

  private:
    ui::Root* Root() { return root(); }
    void focusSelf() {
        if (ui::Root* r = root()) r->focus(this);
    }
    RollState& s_;
    MidiEditor& editor_;
    ui::Point press_{};
    bool moved_ = false;
    std::size_t anchor_ = 0;
    std::set<std::size_t> marqueeBase_;
};

// ----------------------------------------------------------------- keyboard

class Keys : public ui::Node {
  public:
    explicit Keys(RollState& s) : Node("midi.keys"), s_(s) {
        layout.width = ui::SizeRule::fixed(kKeysWidth);
        layout.height = ui::SizeRule::fraction();
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        const Geometry g{s_, b};
        c.save();
        c.clip(b);
        c.fillRect(b, t.color("raised"));
        for (int p = std::min(127, g.pitchOf(b.y)); p >= std::max(0, g.pitchOf(b.bottom())); --p) {
            const float y = g.yOf(p);
            if (blackKey(p)) c.fillRect({b.x, y, b.w * 0.62f, s_.rowHeight}, t.color("ground"));
            c.fillRect({b.x, y + s_.rowHeight - 1, b.w, 1}, t.color("rule").withAlpha(0.5f));
            if (p % 12 == 0 && s_.rowHeight >= 8)
                c.text({b.x, y, b.w - 4, s_.rowHeight}, pitchName(p), ui::Font::Ui,
                       t.type("caption") - 1, t.color("ink-quiet"), ui::kRight | ui::kMiddle);
        }
        c.fillRect({b.right() - 1, b.y, 1, b.h}, t.color("rule"));
        c.restore();
    }

  private:
    RollState& s_;
};

// ----------------------------------------------------------------- bar strip

// The project's bar numbers over the roll, at its bar lines, so a part can be
// placed in the song without the arrangement in view. As in the arrange
// ruler, every bar is numbered, or every 2nd, 4th, 8th... counted from bar 1:
// the first stride whose spacing fits the widest number (three digits at
// least), set by the zoom and the narrowest bar around the item (barLines),
// never by the scroll; no two numbers ever touch.
class BarStrip : public ui::Node {
  public:
    static constexpr float kHeight = 16, kGap = 12;
    BarStrip(const RollState& s, MidiEditor& editor, std::vector<int>& labelled,
             std::vector<std::string>& chords)
        : Node("midi.bars"), s_(s), editor_(editor), labelled_(labelled), chordLabels_(chords) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(kHeight);
    }
    // The chord row's window x of a project beat.
    float chordX(double beat) const {
        const ui::Rect b = bounds();
        return Geometry{s_, {b.x + kKeysWidth, b.y, b.w - kKeysWidth, b.h}}.xOf(beat -
                                                                                s_.itemPosition);
    }
    // A double-click on the chord row edits the chord whose start or name is
    // under it, as the ruler's chord row does; elsewhere it starts a chord at
    // the grid beat there.
    bool onPointer(const ui::PointerEvent& e) override {
        const ui::Rect b = bounds();
        if (e.type != ui::PointerEvent::Type::Down || e.button != 1 || e.clicks != 2 ||
            e.pos.y < b.y + kHeight || e.pos.x < b.x + kKeysWidth || s_.chords.empty())
            return false;
        std::optional<double> beat;
        for (const ChordHit& hit : chordHits_)
            if (hit.label.contains(e.pos)) beat = hit.beat;
        if (!beat) {
            const Geometry g{s_, {b.x + kKeysWidth, b.y, b.w - kKeysWidth, b.h}};
            const double at = std::max(0.0, s_.itemPosition + g.beatOf(e.pos.x));
            beat = s_.grid > 0 ? std::round(at / s_.grid) * s_.grid : at;
        }
        editor_.editChord(*beat, e.pos);
        return true;
    }
    // The strip's height: the bar numbers, and the chord row while chords
    // hold over the roll (ADR-0095).
    float wanted() const { return kHeight + (s_.chords.empty() ? 0 : kChordRowHeight); }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect whole = bounds();
        const ui::Rect b{whole.x, whole.y, whole.w, std::min(whole.h, kHeight)};
        const ui::Rect area{b.x + kKeysWidth, b.y, std::max(0.0f, b.w - kKeysWidth), b.h};
        const Geometry g{s_, area};
        labelled_.clear();
        chordLabels_.clear();
        chordHits_.clear();
        c.fillRect(whole, t.color("raised"));
        c.fillRect({whole.x, whole.bottom() - 1, whole.w, 1}, t.color("rule"));
        c.fillRect({area.x - 1, whole.y, 1, whole.h}, t.color("rule"));
        if (whole.h > kHeight) paintChords(c, {area.x, b.bottom(), area.w, whole.h - kHeight});
        if (s_.barLines.empty()) return;
        const float size = t.type("caption");
        const int last = s_.firstBar + static_cast<int>(s_.barLines.size()) - 1;
        const std::string digits(std::to_string(std::max(last, 100)).size(), '8');
        const float widest = c.textWidth(digits, ui::Font::Mono, size);
        float narrowest = 1e9f;
        for (std::size_t i = 1; i < s_.barLines.size(); ++i)
            narrowest = std::min(
                narrowest,
                static_cast<float>((s_.barLines[i] - s_.barLines[i - 1]) * s_.pixelsPerBeat));
        int stride = 1;
        while (stride < (1 << 20) && static_cast<float>(stride) * narrowest < widest + kGap)
            stride *= 2;
        c.save();
        c.clip(area);
        float lastX = -1e9f, lastW = 0;
        for (std::size_t i = 0; i < s_.barLines.size(); ++i) {
            const int bar = s_.firstBar + static_cast<int>(i);
            const float x = g.xOf(s_.barLines[i] - s_.itemPosition);
            if (x > area.right()) break;
            if (x < area.x - widest - kGap || (bar - 1) % stride != 0) continue;
            const std::string text = std::to_string(bar);
            const float w = c.textWidth(text, ui::Font::Mono, size);
            if (x - lastX < lastW + kGap) continue;
            c.fillRect({x, b.bottom() - 6, 1, 5}, t.color("ink-quiet"));
            c.text({x + 3, b.y, w + 4, b.h - 1}, text, ui::Font::Mono, size, t.color("ink"),
                   ui::kLeft | ui::kMiddle);
            if (x + 3 + w > area.x) labelled_.push_back(bar);
            lastX = x;
            lastW = w;
        }
        c.restore();
    }

  private:
    // Each chord a cell from its beat to the next chord point, in the
    // ruler's cell and letters; a "no chord" point a short faint tick.
    void paintChords(ui::Canvas& c, const ui::Rect& row) {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, row};
        const float size = t.type("caption");
        c.save();
        c.clip(row);
        for (std::size_t i = 0; i < s_.chords.size(); ++i) {
            const RollState::ChordMark& m = s_.chords[i];
            const float x = g.xOf(m.beat - s_.itemPosition);
            if (x > row.right()) break;
            const float end = i + 1 < s_.chords.size()
                                  ? g.xOf(s_.chords[i + 1].beat - s_.itemPosition)
                                  : row.right() + 2;
            if (end < row.x) continue;
            if (m.text.empty()) {
                c.fillRect({x, row.y + 4, 1, row.h - 8}, t.color("ink-faint"));
                chordHits_.push_back({m.beat, {x - 4, row.y, 8, row.h}});
                continue;
            }
            const ui::Rect cell{x + 1, row.y + 2, std::max(0.0f, end - x - 2), row.h - 5};
            c.fillRound(cell, 2, t.color("panel"));
            c.strokeRound(cell, 2, t.color("rule"));
            const float room = std::max(0.0f, cell.w - 8);
            if (room <= 0) continue;
            // A cell that starts left of the roll keeps its name at the edge.
            const float textX = std::max(cell.x + 4, row.x + 4);
            // A name too long for its cell ends in an ellipsis, or goes.
            const float shown = std::min(room, cell.right() - 4 - textX);
            const std::string fit = c.elide(m.text, ui::Font::Ui, size, std::max(0.0f, shown));
            const float fitW = fit.empty() ? 0.0f : c.textWidth(fit, ui::Font::Ui, size);
            chordHits_.push_back(
                {m.beat, {x - 4, row.y, std::max(8.0f, textX + fitW + 8 - (x - 4)), row.h}});
            if (fit.empty()) continue;
            c.text({textX, row.y, shown + 4, row.h - 1}, fit, ui::Font::Ui, size,
                   t.color(m.known ? "ink" : "ink-quiet"), ui::kLeft | ui::kMiddle);
            chordLabels_.push_back(fit);
        }
        c.restore();
    }
    struct ChordHit {
        double beat = 0;  // the chord point's project beat
        ui::Rect label;   // its start and name, as the last paint drew them
    };
    const RollState& s_;
    MidiEditor& editor_;
    std::vector<int>& labelled_;
    std::vector<std::string>& chordLabels_;
    std::vector<ChordHit> chordHits_;
};

// ----------------------------------------------------------------- velocity lane

class VelocityLane : public ui::Node {
  public:
    VelocityLane(RollState& s, MidiEditor& editor, ui::Node*& roll)
        : Node("midi.velocity"), s_(s), editor_(editor), roll_(roll) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(60);
    }
    ui::Rect area() const {  // aligned with the roll
        const ui::Rect b = bounds();
        return {b.x + kKeysWidth, b.y + 4, std::max(0.0f, b.w - kKeysWidth), b.h - 8};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        c.fillRect({b.x, b.y, b.w, 1}, t.color("rule"));
        c.text({b.x + 4, b.y, kKeysWidth - 6, b.h}, "Vel", ui::Font::Ui, t.type("caption"),
               t.color("ink-quiet"));
        const ui::Rect a = area();
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        c.save();
        c.clip(a);
        for (std::size_t i = 0; i < s_.notes.size(); ++i) {
            const RollNote n = g.shown(i);
            const float x = g.xOf(n.start);
            if (x < a.x - 4 || x > a.right()) continue;
            const float h = a.h * static_cast<float>(n.velocity) / 127.0f;
            const bool sel = s_.selected.contains(i);
            const ui::Color col = velocityColor(t, s_, n.velocity, sel);
            c.fillRect({x, a.bottom() - h, 2, h}, col);
            c.fillRound({x - 2, a.bottom() - h - 2, 6, 4}, 1, col);
        }
        c.restore();
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Cancel) {
            s_.cancelGesture();
            if (root()) root()->requestAnimationFrame();
            return true;
        }
        if (e.type == T::Down) s_.gestureVersion = editor_.beginGesture();
        const ui::Rect a = area();
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        auto valueAt = [&](float y) {
            return std::clamp(static_cast<int>(std::lround((a.bottom() - y) / a.h * 127.0f)), 1,
                              127);
        };
        if (e.type == T::Down && e.button == 1) {
            // The nearest note start within 6 px, selected notes first.
            std::optional<std::size_t> best;
            float bestD = 7;
            for (std::size_t i = 0; i < s_.notes.size(); ++i) {
                const float d = std::abs(g.xOf(s_.notes[i].start) - e.pos.x) -
                                (s_.selected.contains(i) ? 0.5f : 0.0f);
                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }
            if (!best) return true;
            if (!s_.selected.contains(*best)) s_.selected = {*best};
            s_.velocityNote = best;
            s_.drag = RollState::Drag::Velocity;
            s_.dVelocity = valueAt(e.pos.y) - s_.notes[*best].velocity;
            if (ui::Root* r = root()) r->capture(this);
            repaintAll();
            return true;
        }
        if (e.type == T::Move && s_.drag == RollState::Drag::Velocity && s_.velocityNote) {
            s_.dVelocity = valueAt(e.pos.y) - s_.notes[*s_.velocityNote].velocity;
            repaintAll();
            return true;
        }
        if (e.type == T::Up && s_.drag == RollState::Drag::Velocity) {
            if (ui::Root* r = root()) r->capture(nullptr);
            s_.drag = RollState::Drag::None;
            json::Value op = json::Value::object(), sel = json::Value::array();
            for (std::size_t i : s_.selected) sel.push_back(static_cast<std::int64_t>(i));
            op["op"] = "velocity";
            op["notes"] = std::move(sel);
            if (s_.selected.size() == 1)
                op["velocity"] =
                    std::clamp(s_.notes[*s_.velocityNote].velocity + s_.dVelocity, 1, 127);
            else
                op["velocity_delta"] = std::clamp(s_.dVelocity, -127, 127);
            s_.lastVelocity =
                std::clamp(s_.notes[*s_.velocityNote].velocity + s_.dVelocity, 1, 127);
            s_.dVelocity = 0;
            s_.velocityNote.reset();
            json::Value ops = json::Value::array();
            ops.push_back(std::move(op));
            (void)editor_.edit(std::move(ops), s_.gestureVersion);
            repaintAll();
            return true;
        }
        return false;
    }
    std::function<void()> repaintAll = [] {};

  private:
    RollState& s_;
    MidiEditor& editor_;
    ui::Node*& roll_;
};

// ----------------------------------------------------------------- CC lane

// One controller's values over time: a step line (a CC holds until the next
// value). Drag to draw (a point per grid step), Alt- or right-drag to erase;
// one midi.edit on release.
class CcLane : public ui::Node {
  public:
    CcLane(RollState& s, MidiEditor& editor, ui::Node*& roll)
        : Node("midi.cc"), s_(s), editor_(editor), roll_(roll) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(52);
    }
    ui::Rect area() const {
        const ui::Rect b = bounds();
        return {b.x + kKeysWidth, b.y + 4, std::max(0.0f, b.w - kKeysWidth), b.h - 8};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        c.fillRect({b.x, b.y, b.w, 1}, t.color("rule"));
        c.text({b.x + 4, b.y, kKeysWidth - 6, b.h}, "CC " + std::to_string(s_.ccNumber),
               ui::Font::Ui, t.type("caption"), t.color("ink-quiet"));
        const ui::Rect a = area();
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        auto yOf = [&](int v) { return a.bottom() - a.h * static_cast<float>(v) / 127.0f; };
        // Committed points outside the drag's span, the draft inside it.
        const bool dragging = !s_.ccDraft.empty() || s_.ccErasing;
        std::vector<CcPoint> shown;
        for (const CcPoint& p : s_.cc)
            if (!dragging || p.beat < s_.ccFrom || p.beat >= s_.ccTo) shown.push_back(p);
        if (!s_.ccErasing) shown.insert(shown.end(), s_.ccDraft.begin(), s_.ccDraft.end());
        std::sort(shown.begin(), shown.end(),
                  [](const CcPoint& x, const CcPoint& y) { return x.beat < y.beat; });
        c.save();
        c.clip(a);
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const float x0 = g.xOf(shown[i].beat);
            const float x1 = i + 1 < shown.size() ? g.xOf(shown[i + 1].beat) : a.right();
            const float y = yOf(shown[i].value);
            c.fillRect({x0, y, std::max(1.0f, x1 - x0), a.bottom() - y},
                       t.color("accent").withAlpha(0.25f));
            c.fillRect({x0, y - 1, std::max(1.0f, x1 - x0), 2}, t.color("accent"));
            c.fillRound({x0 - 2, y - 3, 5, 5}, 2, t.color("focus"));
        }
        if (s_.ccErasing)
            c.fillRect({g.xOf(s_.ccFrom), a.y, g.xOf(s_.ccTo) - g.xOf(s_.ccFrom), a.h},
                       t.color("meter-clip").withAlpha(0.2f));
        c.restore();
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Cancel) {
            s_.cancelGesture();
            if (root()) root()->requestAnimationFrame();
            return true;
        }
        if (e.type == T::Down) s_.gestureVersion = editor_.beginGesture();
        const ui::Rect a = area();
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        auto valueAt = [&](float y) {
            return std::clamp(static_cast<int>(std::lround((a.bottom() - y) / a.h * 127.0f)), 0,
                              127);
        };
        const double step = s_.grid > 0 ? s_.grid : 0.25;
        auto stepBeat = [&](float x) {  // the grid step under the pointer
            const double beat = g.beatOf(x);
            if (!s_.snap) return beat;
            return std::floor((s_.itemPosition + beat) / step + 1e-9) * step - s_.itemPosition;
        };
        const bool drawing = !s_.ccDraft.empty() || s_.ccErasing;
        if (e.type == T::Down && (e.button == 1 || e.button == 3)) {
            s_.ccErasing = e.button == 3 || (e.mods & ui::kAlt);
            pressBeat_ = std::max(0.0, stepBeat(e.pos.x));
            s_.ccFrom = pressBeat_;
            s_.ccTo = pressBeat_ + step;
            s_.ccDraft.clear();
            if (!s_.ccErasing) s_.ccDraft.push_back({pressBeat_, valueAt(e.pos.y)});
            if (ui::Root* r = root()) r->capture(this);
            invalidate();
            return true;
        }
        if (e.type == T::Move && drawing) {
            const double beat = std::max(0.0, stepBeat(e.pos.x));
            s_.ccFrom = std::min(pressBeat_, beat);
            s_.ccTo = std::max(pressBeat_, beat) + step;
            if (!s_.ccErasing) {
                std::erase_if(s_.ccDraft,
                              [&](const CcPoint& p) { return std::abs(p.beat - beat) < 1e-9; });
                s_.ccDraft.push_back({beat, valueAt(e.pos.y)});
                std::sort(s_.ccDraft.begin(), s_.ccDraft.end(),
                          [](const CcPoint& x, const CcPoint& y) { return x.beat < y.beat; });
            }
            invalidate();
            return true;
        }
        if (e.type == T::Up && drawing) {
            if (ui::Root* r = root()) r->capture(nullptr);
            json::Value ops = json::Value::array();
            json::Value clear = json::Value::object();
            clear["op"] = "cc.clear";
            clear["number"] = s_.ccNumber;
            clear["channel"] = 1;
            clear["from"] = s_.ccFrom;
            clear["to"] = s_.ccTo;
            ops.push_back(std::move(clear));
            if (!s_.ccErasing) {
                json::Value set = json::Value::object(), pts = json::Value::array();
                set["op"] = "cc.set";
                set["number"] = s_.ccNumber;
                for (const CcPoint& p : s_.ccDraft) {
                    json::Value v = json::Value::object();
                    v["beat"] = p.beat;
                    v["value"] = p.value;
                    pts.push_back(std::move(v));
                }
                set["points"] = std::move(pts);
                ops.push_back(std::move(set));
            }
            s_.ccDraft.clear();
            s_.ccErasing = false;
            (void)editor_.edit(std::move(ops), s_.gestureVersion);
            invalidate();
            return true;
        }
        return false;
    }

  private:
    RollState& s_;
    MidiEditor& editor_;
    ui::Node*& roll_;
    double pressBeat_ = 0;
};

// ----------------------------------------------------------------- playhead (overlay)

class RollPlayhead : public ui::Node {
  public:
    RollPlayhead(const RollState& s, ui::Node*& roll) : Node("midi.playhead"), s_(s), roll_(roll) {}
    void place(const ui::Rect& r) { setBounds(r); }
    void paint(ui::Canvas& c) override {
        if (!roll_ || !roll_->visible || s_.item.empty() || s_.playheadBeat < 0) return;
        const ui::Rect b = roll_->bounds();
        const Geometry g{s_, b};
        const float x = g.xOf(s_.playheadBeat - s_.itemPosition);
        if (x < b.x || x > b.right()) return;
        c.fillRect({x, b.y, 1.5f, b.h}, c.theme().color("ink"));
    }

  private:
    const RollState& s_;
    ui::Node*& roll_;
};

// ----------------------------------------------------------------- panel

// The docked panel. The wheel anywhere in it that no child used (the keys, the
// lanes, the header) zooms the rows when the mouse map says so for its
// modifiers (Alt by default), the same as over the roll.
class Panel : public ui::Node {
  public:
    explicit Panel(MidiEditor& editor) : Node("midi.editor"), editor_(editor) {
        interactive = true;
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Wheel ||
            editor_.wheelBehaviour(e.mods) != "zoom_rows")
            return false;
        float y = e.pos.y;
        if (const ui::Node* roll = editor_.roll())
            y = std::clamp(y, roll->bounds().y, roll->bounds().bottom());
        editor_.zoomRows(e.wheelY != 0 ? e.wheelY : e.wheelX, y);
        return true;
    }

  private:
    MidiEditor& editor_;
};

}  // namespace

// ----------------------------------------------------------------- MidiEditor

std::optional<MidiOwner> midiOwner(const model::Session& s, const std::string& text) {
    const auto id = model::Id::parse(text);
    if (!id) return std::nullopt;
    MidiOwner o;
    std::optional<model::Id> take;
    if (const model::Item* item = s.items.find(*id)) {
        o.item = item;
        o.track = item->track;
        take = item->activeTake;
    } else if (const model::Clip* clip = s.clips.find(*id)) {
        o.clip = clip;
        o.track = clip->track;
        take = clip->activeTake;
    } else {
        return std::nullopt;
    }
    o.take = take ? s.takes.find(*take) : nullptr;
    o.source = o.take && o.take->source ? s.midi.find(*o.take->source) : nullptr;
    if (!o.source) return std::nullopt;
    return o;
}

MidiEditor::MidiEditor(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui)
    : root_(root), actions_(actions), dispatcher_(dispatcher), app_(app), ui_(ui) {
    // The notes take the colour their item has in the arrangement.
    state_.color = [this](const ui::Theme& theme) {
        const model::Session& s = app_.head();
        if (auto id = model::Id::parse(state_.item)) {
            if (const model::Item* item = s.items.find(*id)) {
                const model::Track* track = s.tracks.find(item->track);
                return views::Lanes::itemColor(theme, *item, track,
                                               track ? track->type : model::StripType::Channel);
            }
            if (const model::Clip* clip = s.clips.find(*id))
                return views::clipColor(theme, s, *clip);  // as its launcher cell
        }
        return theme.color("accent");
    };
    // The selected MIDI item, or the one a caller names.
    actions_.addUiFor("ui.midi.edit", "MIDI: edit selected item", "item",
                      [this](const json::Value& v) -> Status {
                          if (v.isString()) return open(v.asString());
                          for (const std::string& it : ui_.selectedItems)
                              if (open(it)) return {};
                          return Error("gui.midi", "select a MIDI item to edit");
                      });
    actions_.addUi("ui.midi.close", "MIDI: close editor", [this] { close(); });
    // Alt+E: hide the editor, or show it on the item it had last (else the
    // selected clip after a launcher click, the selected MIDI item, the first
    // MIDI item, the first MIDI clip); nothing when there is no MIDI.
    actions_.addUi("ui.midi.toggle", "MIDI: show or hide the editor", [this] {
        if (isOpen()) {
            close();
            return;
        }
        const model::Session& s = app_.head();
        auto isMidi = [&s](const std::string& text) { return midiOwner(s, text).has_value(); };
        std::vector<std::string> candidates;
        if (!lastItem_.empty()) candidates.push_back(lastItem_);
        if (ui_.clipsFocused)  // a launcher cell was clicked last
            for (const std::string& c : ui_.selectedClips) candidates.push_back(c);
        for (const std::string& it : ui_.selectedItems) candidates.push_back(it);
        for (const auto& e : s.items) candidates.push_back(e.id.str());
        for (const auto& e : s.clips) candidates.push_back(e.id.str());  // none: a clip
        for (const std::string& it : candidates)
            if (isMidi(it) && open(it)) return;
    });
    actions_.addUi("ui.midi.quantize", "MIDI: quantize selected notes (or all)",
                   [this] { quantize(); });
    actions_.addUi("ui.midi.notes.delete", "MIDI: delete selected notes (or the event list's)",
                   [this] { removeSelected(); });
    // The arrows: a semitone, or a scale degree while the key button is on.
    actions_.addUi("ui.midi.notes.up",
                   "MIDI: transpose selected notes up a semitone (a degree with the key button on)",
                   [this] { state_.degrees ? transposeDegrees(1) : transpose(1); });
    actions_.addUi(
        "ui.midi.notes.down",
        "MIDI: transpose selected notes down a semitone (a degree with the key button on)",
        [this] { state_.degrees ? transposeDegrees(-1) : transpose(-1); });
    actions_.addUi("ui.midi.notes.degree_up", "MIDI: move selected notes up a scale degree",
                   [this] { transposeDegrees(1); });
    actions_.addUi("ui.midi.notes.degree_down", "MIDI: move selected notes down a scale degree",
                   [this] { transposeDegrees(-1); });
    actions_.addUi("ui.midi.notes.octave_up", "MIDI: transpose selected notes up an octave",
                   [this] { transpose(12); });
    actions_.addUi("ui.midi.notes.octave_down", "MIDI: transpose selected notes down an octave",
                   [this] { transpose(-12); });
    actions_.addUi("ui.midi.notes.earlier", "MIDI: move selected notes a grid step earlier",
                   [this] { nudge(-1); });
    actions_.addUi("ui.midi.notes.later", "MIDI: move selected notes a grid step later",
                   [this] { nudge(1); });
    actions_.addUi("ui.midi.notes.copy", "MIDI: copy selected notes", [this] { copySelected(); });
    actions_.addUi("ui.midi.notes.cut", "MIDI: cut selected notes", [this] { cutSelected(); });
    actions_.addUi("ui.midi.notes.paste", "MIDI: paste notes at the playhead", [this] { paste(); });
    actions_.addUi("ui.midi.notes.duplicate", "MIDI: duplicate selected notes after themselves",
                   [this] { duplicate(); });
    actions_.addUi("ui.midi.notes.select_all", "MIDI: select all notes", [this] { selectAll(); });
    actions_.addUi("ui.midi.notes.select_none", "MIDI: clear the note selection",
                   [this] { selectNone(); });
}

MidiEditor::~MidiEditor() = default;

std::unique_ptr<ui::Node> MidiEditor::build() {
    auto panel = std::make_unique<Panel>(*this);
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.height = ui::SizeRule::fixed(height_);
    panel->visible = false;
    auto& grip = panel->emplace<PanelGrip>("midi.grip", kGripHeight);
    grip.height = [this] { return panel_->layout.height.value; };
    grip.resize = [this](float h) { setHeight(h); };
    grip.reset = [this] { setHeight(kDefaultHeight); };
    auto& header = panel->emplace<ui::Node>("midi.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(32);
    header.layout.padding = ui::Insets{4, 8, 4, 8};
    header.layout.gap = 6;
    header.layout.align = ui::Layout::Align::Center;
    title_ = &header.emplace<ui::Label>("midi.title", "MIDI", ui::Font::UiBold, "small");
    title_->layout.width = ui::SizeRule::fraction();
    for (auto [label, grid] : {std::pair{"1/4", 1.0},
                               {"1/8", 0.5},
                               {"1/16", 0.25},
                               {"1/32", 0.125},
                               {"1/8T", 1.0 / 3}}) {
        const double gv = grid;
        header.emplace<ui::Button>(std::string("midi.grid.") + label, label, [this, gv] {
            state_.grid = gv;
            state_.lastLength = gv;
            refreshTitle();
            if (roll_) roll_->invalidate();
        });
    }
    header.emplace<ui::Button>("midi.snap", "Snap", [this] {
        state_.snap = !state_.snap;
        refreshTitle();
    });
    header.emplace<ui::Button>("midi.quantize", "Quantize", [this] { quantize(); });
    // The key at the item's start; lit, pitch drags and arrows go by degrees.
    keyButton_ = &header.emplace<ui::Toggle>("midi.key", "No key", "accent",
                                             [this](bool on) { state_.degrees = on; });
    keyButton_->layout.width = ui::SizeRule::content();
    keyButton_->textSize = "small";  // as the header's buttons
    keyButton_->offInk = "ink";
    keyButton_->enabled = false;
    // One menu owns every lane under the roll (gui.md §4.7).
    lanesButton_ = &header.emplace<ui::Button>("midi.lanes", "Lanes", [this] { openLanesMenu(); });
    header.emplace<ui::Button>("midi.notation.toggle", "Notation", [this] {
        if (!notationPane_) return;
        notationPane_->visible = !notationPane_->visible;
        if (!notationPane_->visible) root_.releaseWithin(notationPane_);
        root_.relayout();
        root_.damageAll();
    });
    header.emplace<ui::Button>("midi.list.toggle", "List", [this] {
        if (!listPane_ || !rollPane_) return;
        listPane_->visible = !listPane_->visible;
        rollPane_->visible = !listPane_->visible;
        roll_->visible = !listPane_->visible;
        eventList_->activate(listPane_->visible);
        if (!listPane_->visible) root_.focus(roll_);
        root_.relayout();
        root_.damageAll();
    });
    header.emplace<ui::Button>("midi.close", "Close", [this] { close(); });

    auto& body = panel->emplace<ui::Node>("midi.body");
    body.layout.dir = ui::Layout::Dir::Row;
    body.layout.height = ui::SizeRule::fraction();
    auto& left = body.emplace<ui::Node>("midi.left");
    rollPane_ = &left;
    left.layout.dir = ui::Layout::Dir::Column;
    left.layout.width = ui::SizeRule::fraction();
    bars_ = &left.emplace<BarStrip>(state_, *this, barLabels_, chordLabels_);
    auto& rollRow = left.emplace<ui::Node>("midi.rollrow");
    rollRow.layout.dir = ui::Layout::Dir::Row;
    rollRow.layout.height = ui::SizeRule::fraction();
    auto& keys = rollRow.emplace<Keys>(state_);
    keys_ = &keys;
    auto& roll = rollRow.emplace<Roll>(state_, *this);
    roll_ = &roll;
    auto& vel = left.emplace<VelocityLane>(state_, *this, roll_);
    velocity_ = &vel;
    ccLane_ = &left.emplace<CcLane>(state_, *this, roll_);
    auto repaint = [this] {
        if (roll_) roll_->invalidate();
        if (bars_) bars_->invalidate();
        if (keys_) keys_->invalidate();
        if (velocity_) velocity_->invalidate();
        if (ccLane_) ccLane_->invalidate();
    };
    roll.repaintAll = repaint;
    vel.repaintAll = repaint;
    applyLanes();

    eventList_ = std::make_unique<MidiEventList>(root_, *this, app_);
    auto listPane = eventList_->build();
    listPane_ = listPane.get();
    body.add(std::move(listPane));

    auto& pane = body.emplace<ui::Node>("midi.notation");
    notationPane_ = &pane;
    pane.layout.dir = ui::Layout::Dir::Column;
    pane.layout.width = ui::SizeRule::fixed(380);
    pane.layout.padding = ui::Insets{4, 8, 6, 8};
    pane.layout.gap = 4;
    pane.emplace<ui::Label>("midi.notation.title", "Notation", ui::Font::UiBold, "caption",
                            "ink-quiet");
    auto& text = pane.emplace<ui::TextArea>("midi.notation.text");
    text_ = &text;
    text.layout.height = ui::SizeRule::fraction();
    text.onChange = [this](const std::string&) {
        textDirty_ = true;
        compilePending_ = true;
        typedAt_.reset();  // the next pass starts the pause again
    };
    text.onSubmit = [this](const std::string&) { compileText(true, true); };
    text.onFocusChange = [this](bool focused) {
        if (focused) ++typingSession_;
        if (!focused && textDirty_) compileText(true);
    };
    diagnostics_ = &pane.emplace<ui::TextBlock>("midi.notation.diagnostics", "", ui::Font::Mono,
                                                "caption", "ink-quiet");
    diagnostics_->layout.height = ui::SizeRule::content();

    auto overlay = std::make_unique<RollPlayhead>(state_, roll_);
    playhead_ = overlay.get();
    playheadHandle_ = root_.addOverlay(std::move(overlay));
    return panel;
}

Status MidiEditor::open(const std::string& item) {
    const std::string previous = state_.item;
    state_.item = item;
    state_.selected.clear();
    if (auto r = reload(); !r) {
        state_.item = previous;
        return r;
    }
    lastItem_ = item;
    // Center the view on the notes (or middle C).
    int lo = 127, hi = 0;
    for (const RollNote& n : state_.notes) {
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
    }
    const double center = state_.notes.empty() ? 60 : (lo + hi) / 2.0;
    const float rows = roll_ && roll_->bounds().h > 0 ? roll_->bounds().h / state_.rowHeight : 22;
    state_.topPitch =
        std::clamp(center + static_cast<double>(rows) / 2.0, static_cast<double>(rows), 127.0);
    state_.scrollBeat = -0.25;
    if (roll_ && roll_->bounds().w > 0 && state_.itemLength > 0)
        state_.pixelsPerBeat = std::clamp(
            static_cast<double>(roll_->bounds().w) / (state_.itemLength + 0.5), 20.0, 400.0);
    textDirty_ = compilePending_ = false;
    reloadText();
    if (panel_ && !panel_->visible) {
        panel_->visible = true;
        root_.relayout();
    }
    root_.damageAll();
    if (listPane_ && listPane_->visible)
        eventList_->activate(true);
    else if (roll_)
        root_.focus(roll_);
    return {};
}

void MidiEditor::close() {
    if (eventList_) eventList_->activate(false);
    if (textDirty_) compileText(true);
    state_.item.clear();
    state_.notes.clear();
    state_.selected.clear();
    state_.drag = RollState::Drag::None;
    state_.velocityNote.reset();
    state_.ccDraft.clear();
    state_.ccErasing = false;
    if (eventList_) eventList_->sync();
    if (panel_ && panel_->visible) {
        root_.releaseWithin(panel_);
        panel_->visible = false;
        root_.relayout();
        root_.damageAll();
    }
}

float MidiEditor::maxHeight() const { return maxPanelHeight(panel_, kMinHeight); }

void MidiEditor::applyHeight() {
    if (!panel_) return;
    const float shown = std::clamp(height_, kMinHeight, maxHeight());
    if (panel_->layout.height.value == shown) return;
    panel_->layout.height = ui::SizeRule::fixed(shown);
    root_.relayout();
    root_.damageAll();
}

void MidiEditor::setHeight(float height) {
    height_ = std::clamp(height, kMinHeight, maxHeight());
    applyHeight();
}

json::Value MidiEditor::saveLayout() const {
    json::Value v = json::Value::object();
    v["height"] = static_cast<double>(height_);
    v["velocity"] = state_.velocityShown;
    v["cc"] = state_.ccShown ? state_.ccNumber : 0;
    return v;
}

void MidiEditor::loadLayout(const json::Value& v) {
    // The saved preference as is: the window may be smaller at load than when
    // it was saved, and applyHeight() only shrinks what is laid out.
    if (const json::Value* h = v.find("height");
        h && h->isNumber() && std::isfinite(h->asDouble())) {
        height_ = std::clamp(static_cast<float>(h->asDouble()), kMinHeight, 4000.0f);
        applyHeight();
    }
    if (const json::Value* vel = v.find("velocity"); vel && vel->isBool())
        state_.velocityShown = vel->asBool();
    // A controller the menu offers, or 0 for none; anything else is ignored.
    if (const json::Value* cc = v.find("cc"); cc && cc->isNumber()) {
        const double n = cc->asDouble();
        if (n == 0) state_.ccShown = false;
        for (const auto& [number, name] : kCcs)
            if (n == number) {
                state_.ccShown = true;
                state_.ccNumber = number;
            }
    }
    applyLanes();
}

void MidiEditor::applyLanes() {
    bool changed = false;
    auto show = [&](ui::Node* lane, bool shown) {
        if (!lane || lane->visible == shown) return;
        if (!shown) root_.releaseWithin(lane);  // a drag in it ends
        lane->visible = shown;
        changed = true;
    };
    show(velocity_, state_.velocityShown);
    show(ccLane_, state_.ccShown);
    if (!changed) return;
    root_.relayout();
    root_.damageAll();
}

void MidiEditor::openLanesMenu() {
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, bool checked, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.checked = checked;
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    add("Velocity", state_.velocityShown, [this] {
        state_.velocityShown = !state_.velocityShown;
        applyLanes();
    });
    // The CC lane shows one controller: picking another switches it, picking
    // the shown one hides the lane.
    for (const auto& [number, name] : kCcs) {
        const int n = number;
        add("CC " + std::to_string(n) + " " + name, state_.ccShown && state_.ccNumber == n,
            [this, n] {
                if (state_.ccShown && state_.ccNumber == n) {
                    state_.ccShown = false;
                } else {
                    state_.ccShown = true;
                    state_.ccNumber = n;
                    if (isOpen()) (void)reload();
                }
                applyLanes();
            });
    }
    const ui::Rect b = lanesButton_ ? lanesButton_->bounds() : ui::Rect{};
    root_.openPopup(
        ui::makeMenu("midi.lanes.menu", std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{b.x, b.bottom(), 0, 0}, ui::Placement::Below);
}

std::string_view MidiEditor::wheelBehaviour(unsigned mods) const {
    static const views::MouseMap defaults;
    return (mouse_ ? *mouse_ : defaults).behaviour("midi.wheel", mods);
}

void MidiEditor::zoomRows(float wheel, float anchorY) {
    if (!roll_) return;
    const double offset = static_cast<double>(anchorY - roll_->bounds().y);
    const double at = state_.topPitch - offset / static_cast<double>(state_.rowHeight);
    state_.rowHeight = std::clamp(static_cast<float>(static_cast<double>(state_.rowHeight) *
                                                     std::pow(1.15, static_cast<double>(wheel))),
                                  kMinRowHeight, kMaxRowHeight);
    state_.topPitch = std::clamp(at + offset / static_cast<double>(state_.rowHeight), 12.0, 127.0);
    roll_->invalidate();
    if (keys_) keys_->invalidate();
}

Status MidiEditor::reload() {
    json::Value args = json::Value::object();
    args["item"] = state_.item;
    auto r = dispatcher_.execute({"midi.notes", std::move(args), kOrigin});
    if (!r) return std::move(r).error();
    // A reload replaces note indices and CC data; an old preview cannot be
    // committed against the new sequence (including undo and item switches).
    state_.cancelGesture();
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner) return Error("gui.midi", "no MIDI source", "/item");
    const model::Take* take = owner->take;
    const model::MidiSource* source = owner->source;
    // A clip sits at beat 0 for the roll: its bars count from its start.
    state_.clip = owner->clip != nullptr;
    state_.itemPosition = owner->item ? model::itemSpan(app_.head(), *owner->item).startBeat : 0.0;
    state_.itemLength =
        owner->item ? model::itemLengthBeats(app_.head(), *owner->item) : owner->clip->length;
    const double rate = take->playrate > 0 ? take->playrate : 1.0;
    state_.notes.clear();
    for (const auto& n : source->sequence.notes) {
        RollNote note;
        note.pitch = n.pitch;
        note.start =
            (static_cast<double>(n.start) / source->sequence.ppq - take->startOffset) / rate;
        note.length = static_cast<double>(n.length) / (source->sequence.ppq * rate);
        note.velocity = n.velocity;
        note.channel = n.channel + 1;
        state_.notes.push_back(note);
    }
    std::erase_if(state_.selected, [&](std::size_t i) { return i >= state_.notes.size(); });
    state_.cc.clear();
    for (const json::Value& c : (*r)["cc"].asArray()) {
        if (c.find("number")->asInt() != state_.ccNumber || c.find("channel"))
            continue;  // channel 1
        state_.cc.push_back(
            {c.find("beat")->asDouble(), static_cast<int>(c.find("value")->asInt())});
    }
    // Bars in a generous window around the item.
    state_.barLines.clear();
    const auto& map = *app_.head().timeMap;
    const int firstBar = std::max(1, map.barBeat(std::max(0.0, state_.itemPosition - 64)).bar);
    state_.firstBar = firstBar;
    for (int bar = firstBar; bar < firstBar + 512; ++bar) {
        const double at = map.beatOfBar(bar);
        if (at > state_.itemPosition + state_.itemLength + 64) break;
        state_.barLines.push_back(at);
    }
    // The context in the same window (ADR-0095): what holds at its start and
    // the changes in it. A clip has no place in time: it reads the key at the
    // project's start, as midi.edit does, and no chords.
    state_.keys.clear();
    state_.chords.clear();
    if (state_.clip) {
        std::optional<harmony::Key> key;
        if (const auto id = model::keyPointAt(app_.head(), 0.0))
            if (const model::ContextPoint* p = app_.head().context.find(*id))
                key = model::parseKey(*p);
        state_.keys.push_back({0.0, key});
    } else {
        const model::Session& head = app_.head();
        const double lo = state_.itemPosition - 64;
        const double hi = state_.itemPosition + state_.itemLength + 64;
        for (const auto& k : head.contextView->keys) {
            if (k.beat > hi) break;
            const model::ContextPoint* p = head.context.find(k.id);
            if (!p) continue;
            if (k.beat <= lo) state_.keys.clear();
            state_.keys.push_back({k.beat, model::parseKey(*p)});
        }
        for (const auto& k : head.contextView->chords) {
            if (k.beat > hi) break;
            const model::ContextPoint* p = head.context.find(k.id);
            if (!p) continue;
            if (k.beat <= lo) state_.chords.clear();
            const bool known = p->symbol.empty() || model::parseChordPoint(*p).has_value();
            state_.chords.push_back({k.beat, p->symbol + (known ? "" : "?"), known});
        }
    }
    refreshKeyButton();
    if (bars_) {
        const float h = static_cast<BarStrip*>(bars_)->wanted();
        if (bars_->layout.height.value != h) {
            bars_->layout.height = ui::SizeRule::fixed(h);
            root_.relayout();
            root_.damageAll();
        }
    }
    projectGeneration_ = app_.projectGeneration();
    version_ = app_.head().version;
    if (title_) {
        std::string name = state_.item;
        if (owner->clip) {
            const model::Track* t = app_.head().tracks.find(owner->track);
            name = (t && !t->name.empty() ? t->name : owner->track.str()) + " — " +
                   (owner->clip->name.empty() ? state_.item : owner->clip->name) + " · clip";
        }
        if (auto id = model::Id::parse(state_.item))
            if (const model::Item* it = app_.head().items.find(*id))
                if (const model::Track* t = app_.head().tracks.find(it->track)) {
                    // The item as the arrangement names it; its id when it
                    // has no name there.
                    const std::string label = views::Lanes::itemLabel(app_.head(), *it);
                    name = (t->name.empty() ? it->track.str() : t->name) + " — " +
                           (label.empty() ? state_.item : label);
                }
        titleBase_ = "MIDI · " + name + " · " + std::to_string(state_.notes.size()) + " notes";
        refreshTitle();
    }
    if (roll_) roll_->invalidate();
    if (bars_) bars_->invalidate();
    if (keys_) keys_->invalidate();
    if (velocity_) velocity_->invalidate();
    if (ccLane_) ccLane_->invalidate();
    return {};
}

void MidiEditor::refreshTitle() {
    if (!title_) return;
    const double g = state_.grid;
    std::string grid = g >= 1                         ? "1/4"
                       : g >= 0.5                     ? "1/8"
                       : std::abs(g - 1.0 / 3) < 1e-9 ? "1/8T"
                       : g >= 0.25                    ? "1/16"
                                                      : "1/32";
    title_->setText(titleBase_ + " · grid " + grid + (state_.snap ? "" : " · snap off"));
}

namespace {
std::function<midi::Meter(int)> meterAtItem(const model::Session& session, double beat) {
    const auto map = session.timeMap;
    const int first = map->barBeat(std::max(0.0, beat)).bar;
    return [map, first](int bar) {
        const auto meter = map->meterAt(std::max(1, first + bar - 1));
        return midi::Meter{meter.numerator, meter.denominator};
    };
}
}  // namespace

std::optional<std::string> MidiEditor::modelText() const {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner) return std::nullopt;
    notation::DecompileOptions options;
    options.meterAt = meterAtItem(
        app_.head(), owner->item ? model::itemSpan(app_.head(), *owner->item).startBeat : 0.0);
    auto text = notation::decompile(owner->source->sequence, options);
    if (!text) return std::nullopt;
    return std::move(*text);
}

void MidiEditor::reloadText() {
    if (!text_) return;
    auto text = modelText();
    if (!text) {
        showDiagnostics({}, "notation unavailable: the item has no MIDI it can show");
        return;
    }
    appliedText_ = baseText_ = std::move(*text);
    text_->setText(appliedText_);
    textDirty_ = textConflict_ = false;
    showDiagnostics({}, "");
}

void MidiEditor::showDiagnostics(const std::vector<ui::TextArea::Mark>& marks,
                                 const std::string& summary) {
    if (text_) text_->setMarks(marks);
    if (diagnostics_) diagnostics_->setText(summary);
    root_.relayout();
}

void MidiEditor::compileText(bool write, bool submit) {
    if (!text_ || state_.item.empty()) return;
    const std::string text = text_->text();
    notation::CompileContext cc;
    cc.ppq = app_.head().project->ppq;
    cc.meterAt = meterAtItem(app_.head(), state_.itemPosition);
    auto compiled = notation::compile(text, cc);
    if (!compiled) {
        const Error& e = compiled.error();
        std::vector<ui::TextArea::Mark> marks;
        std::string summary;
        auto addOne = [&](const Error& x) {
            if (x.loc) marks.push_back({x.loc->line, x.loc->column, x.message, true});
            if (!summary.empty()) summary += "\n";
            summary +=
                (x.loc ? std::to_string(x.loc->line) + ":" + std::to_string(x.loc->column) + "  "
                       : "") +
                x.message + (x.hint.empty() ? "" : " — " + x.hint);
        };
        if (e.details.empty()) addOne(e);
        for (const Error& d : e.details) addOne(d);
        showDiagnostics(marks, summary);
        return;
    }
    std::string summary;
    for (const auto& w : compiled->warnings) summary += (summary.empty() ? "" : "\n") + w.message;
    showDiagnostics(
        {}, summary.empty() ? std::to_string(compiled->sequence.notes.size()) + " notes" : summary);
    if (!write || text == appliedText_) {
        textDirty_ = text != appliedText_;
        return;
    }
    // The draft replaces the whole sequence: if the MIDI changed elsewhere
    // since it was loaded, only an explicit Enter overwrites that change.
    if (const auto current = modelText(); current && *current != baseText_ && !submit) {
        textConflict_ = true;
        textDirty_ = true;
        showDiagnostics(
            {}, ui::shortcutLabel("the MIDI changed elsewhere while you typed: Ctrl+Enter applies "
                                  "your text over that change"));
        return;
    }
    if (textConflict_ && !submit) return;
    json::Value args = json::Value::object();
    args["item"] = state_.item;
    args["notation"] = text;
    cmd::Origin origin = kOrigin;
    origin.gesture =
        "notation:" + state_.item + ":" + std::to_string(typingSession_);  // one per focus
    auto r = dispatcher_.execute({"midi.write", std::move(args), origin});
    if (!r) {
        showDiagnostics({}, r.error().message);
        return;
    }
    appliedText_ = text;
    baseText_ = modelText().value_or(std::string());
    textDirty_ = textConflict_ = false;
    (void)reload();
}

std::uint64_t MidiEditor::beginGesture() {
    follow({});
    gestureGeneration_ = app_.projectGeneration();
    return app_.head().version;
}

Status MidiEditor::edit(json::Value ops, std::optional<std::uint64_t> expectedVersion) {
    if (expectedVersion && (*expectedVersion != app_.head().version ||
                            gestureGeneration_ != app_.projectGeneration())) {
        follow({});
        return Error("gui.midi.stale", "the MIDI item changed during the gesture", "/item",
                     "start the gesture again against the current item");
    }
    json::Value args = json::Value::object();
    args["item"] = state_.item;
    args["ops"] = std::move(ops);
    auto r = dispatcher_.execute({"midi.edit", std::move(args), kOrigin});
    if (!r) {
        if (actions_.onError) actions_.onError("MIDI: " + r.error().message);
        return std::move(r).error();
    }
    SUPPLE_TRY(reload());
    state_.selected.clear();
    for (const json::Value& i : (*r)["notes"].asArray())
        if (i.asInt() >= 0 && static_cast<std::size_t>(i.asInt()) < state_.notes.size())
            state_.selected.insert(static_cast<std::size_t>(i.asInt()));
    if (!text_ || !text_->focused()) reloadText();
    return {};
}

void MidiEditor::editChord(double beat, ui::Point at) {
    views::openChordField(
        root_,
        {[this]() -> const model::Session& { return app_.head(); },
         [this](const std::string& command, json::Value args) {
             auto r = dispatcher_.execute({command, std::move(args), kOrigin});
             if (!r && actions_.onError) actions_.onError("Chord: " + r.error().message);
             return static_cast<bool>(r);
         },
         [this](double b) { return bars_ ? static_cast<BarStrip*>(bars_)->chordX(b) : 0.0f; }},
        beat, at);
}

namespace {
json::Value selectionOf(const RollState& s) {
    json::Value sel = json::Value::array();
    for (std::size_t i : s.selected) sel.push_back(static_cast<std::int64_t>(i));
    return sel;
}
}  // namespace

void MidiEditor::selectAll() {
    state_.selected.clear();
    for (std::size_t i = 0; i < state_.notes.size(); ++i) state_.selected.insert(i);
    if (roll_) roll_->invalidate();
    if (velocity_) velocity_->invalidate();
}

void MidiEditor::selectNone() {
    state_.selected.clear();
    if (roll_) roll_->invalidate();
    if (velocity_) velocity_->invalidate();
}

void MidiEditor::quantize() {
    if (!isOpen()) return;
    json::Value op = json::Value::object();
    op["op"] = "quantize";
    op["grid"] = state_.grid;
    if (!state_.selected.empty()) op["notes"] = selectionOf(state_);
    json::Value ops = json::Value::array();
    ops.push_back(std::move(op));
    (void)edit(std::move(ops));
}

void MidiEditor::transpose(int semitones) {
    if (!isOpen() || state_.selected.empty()) return;
    json::Value op = json::Value::object();
    op["op"] = "move";
    op["notes"] = selectionOf(state_);
    op["semitones"] = semitones;
    json::Value ops = json::Value::array();
    ops.push_back(std::move(op));
    (void)edit(std::move(ops));
}

void MidiEditor::transposeDegrees(int degrees) {
    if (!isOpen() || state_.selected.empty()) return;
    json::Value op = json::Value::object();
    op["op"] = "transpose_degrees";
    op["notes"] = selectionOf(state_);
    op["degrees"] = degrees;
    json::Value ops = json::Value::array();
    ops.push_back(std::move(op));
    (void)edit(std::move(ops));
}

void MidiEditor::refreshKeyButton() {
    if (!keyButton_) return;
    const model::Session& head = app_.head();
    const auto id = model::keyPointAt(head, state_.itemPosition);
    const model::ContextPoint* point = id ? head.context.find(*id) : nullptr;
    const bool known = point && model::parseKey(*point);
    std::string text = point ? model::keyWords(*point) : "No key";
    keyButton_->enabled = known;
    if (!known && state_.degrees) {  // nothing to count degrees in
        state_.degrees = false;
        keyButton_->setOn(false);
    }
    if (text == keyText_) return;
    keyText_ = text;
    keyButton_->setText(std::move(text));
    root_.relayout();
}

void MidiEditor::nudge(int steps) {
    if (!isOpen() || state_.selected.empty()) return;
    json::Value op = json::Value::object();
    op["op"] = "move";
    op["notes"] = selectionOf(state_);
    op["beats"] = steps * state_.grid;
    json::Value ops = json::Value::array();
    ops.push_back(std::move(op));
    (void)edit(std::move(ops));
}

void MidiEditor::removeSelected() {
    if (!isOpen()) return;
    if (eventList_ && listPane_ && listPane_->visible) {  // its notes and controls
        eventList_->removeSelected();
        return;
    }
    if (state_.selected.empty()) return;
    json::Value op = json::Value::object();
    op["op"] = "remove";
    op["notes"] = selectionOf(state_);
    json::Value ops = json::Value::array();
    ops.push_back(std::move(op));
    (void)edit(std::move(ops));
}

void MidiEditor::duplicate() {
    if (!isOpen() || state_.selected.empty()) return;
    double first = 1e18, end = -1e18;
    for (std::size_t i : state_.selected) {
        first = std::min(first, state_.notes[i].start);
        end = std::max(end, state_.notes[i].start + state_.notes[i].length);
    }
    // After the selection, on the grid.
    const double span =
        std::max(state_.grid, std::ceil((end - first) / state_.grid - 1e-9) * state_.grid);
    json::Value ops = json::Value::array();
    for (std::size_t i : state_.selected) {
        const RollNote& n = state_.notes[i];
        json::Value op = json::Value::object();
        op["op"] = "add";
        op["pitch"] = n.pitch;
        op["start"] = n.start + span;
        op["length"] = n.length;
        op["velocity"] = n.velocity;
        if (n.channel != 1) op["channel"] = n.channel;
        ops.push_back(std::move(op));
    }
    (void)edit(std::move(ops));
}

void MidiEditor::copySelected() {
    if (!isOpen() || state_.selected.empty()) return;
    double first = 1e18;
    for (std::size_t i : state_.selected) first = std::min(first, state_.notes[i].start);
    clipboard_.clear();
    for (std::size_t i : state_.selected) {
        RollNote n = state_.notes[i];
        n.start -= first;
        clipboard_.push_back(n);
    }
}

void MidiEditor::cutSelected() {
    copySelected();
    removeSelected();
}

void MidiEditor::paste(std::optional<double> itemBeat) {
    if (!isOpen() || clipboard_.empty()) return;
    const double at = itemBeat.value_or(
        state_.clip ? std::max(0.0, state_.playheadBeat)
                    : std::max(0.0, actions_.context().playheadBeats - state_.itemPosition));
    json::Value ops = json::Value::array();
    for (const RollNote& n : clipboard_) {
        json::Value op = json::Value::object();
        op["op"] = "add";
        op["pitch"] = n.pitch;
        op["start"] = at + n.start;
        op["length"] = n.length;
        op["velocity"] = n.velocity;
        if (n.channel != 1) op["channel"] = n.channel;
        ops.push_back(std::move(op));
    }
    (void)edit(std::move(ops));
}

void MidiEditor::openNoteMenu(ui::Point at, double itemBeat) {
    const bool any = !state_.selected.empty();
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, const char* action, bool enabled, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.shortcut = actions_.keysFor(action);
        m.enabled = enabled;
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    add("Delete", "ui.midi.notes.delete", any, [this] { removeSelected(); });
    add("Copy", "ui.midi.notes.copy", any, [this] { copySelected(); });
    add("Cut", "ui.midi.notes.cut", any, [this] { cutSelected(); });
    add("Paste", "ui.midi.notes.paste", canPaste(), [this, itemBeat] { paste(itemBeat); });
    add("Duplicate", "ui.midi.notes.duplicate", any, [this] { duplicate(); });
    root_.openPopup(ui::makeMenu("midi.menu", std::move(items), [this] { root_.closePopup(); }),
                    ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

bool MidiEditor::autoScrolling() const {
    return roll_ && static_cast<const Roll*>(roll_)->autoScrolling();
}

bool MidiEditor::autoScroll(double seconds) {
    if (!roll_ || !static_cast<Roll*>(roll_)->autoScroll(seconds)) return false;
    if (keys_) keys_->invalidate();
    if (velocity_) velocity_->invalidate();
    return true;
}

void MidiEditor::sync(std::chrono::steady_clock::time_point now) {
    if (!isOpen()) return;
    // A drag held at the roll's edge scrolls by the time since the last pass
    // (capped, so a stalled frame is not a jump).
    if (autoScrolling()) {
        const double seconds = edgeClock_.step(now);
        if (autoScroll(seconds) || seconds == 0) root_.requestAnimationFrame();
    } else {
        edgeClock_.reset();
    }
    follow(now);
}

// The editor follows the window, the session, the text's pending compile and
// the playhead. A gesture calls it too, to start from the session as it is:
// with no time, since it is not a pass, and the pause after typing is not
// counted there.
void MidiEditor::follow(std::optional<std::chrono::steady_clock::time_point> pass) {
    if (!isOpen()) return;
    applyHeight();  // a smaller window or a taller neighbour shrinks the panel
    if (playhead_ && roll_) static_cast<RollPlayhead*>(playhead_)->place(roll_->bounds());
    const bool replaced = app_.projectGeneration() != projectGeneration_;
    if (replaced || app_.head().version != version_) {
        if (replaced) {
            textDirty_ = false;
            compilePending_ = false;
            root_.releaseWithin(panel_);
        }
        // The item went away (undo, delete): close; otherwise reload both views.
        if (!midiOwner(app_.head(), state_.item)) {
            close();
            return;
        }
        state_.selected.clear();
        if (!reload()) {
            close();
            return;
        }
        if (replaced || !text_ || !text_->focused() || !textDirty_) reloadText();
    }
    // The pause after typing is counted in the passes' time, from the first
    // pass after the last keystroke (in the app, the pass that keystroke woke).
    if (compilePending_ && pass) {
        if (!typedAt_) {
            typedAt_ = *pass;
        } else if (*pass - *typedAt_ >= kCompileDelay) {
            compilePending_ = false;  // a failed compile waits for the next edit
            compileText(true);
        }
    }
    if (eventList_ && listPane_ && listPane_->visible) eventList_->sync();
    if (state_.clip) {  // a clip's playhead: where the launcher plays it
        double beat = -1;
        const auto id = model::Id::parse(state_.item);
        for (const auto& t : app_.launchState())
            if (id && t.clip == *id && t.plays == engine::Engine::TrackLaunch::Plays::Clip &&
                t.position)
                beat = *t.position;
        if (beat != state_.playheadBeat) {
            state_.playheadBeat = beat;
            root_.requestAnimationFrame();
        }
    } else if (auto t = app_.transport("get")) {
        const double beat = t->playing ? t->beat : -1;
        if (beat != state_.playheadBeat) {
            state_.playheadBeat = beat;
            root_.requestAnimationFrame();
        }
    }
}

std::optional<std::chrono::steady_clock::time_point> MidiEditor::nextDeadline() const {
    if (!isOpen() || !compilePending_) return {};
    // Typed in since the last pass: a pass is due at once, to start the pause.
    return typedAt_ ? *typedAt_ + kCompileDelay : std::chrono::steady_clock::time_point{};
}

}  // namespace supple::gui
