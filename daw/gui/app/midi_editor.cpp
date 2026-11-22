// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_editor.h"
#include "clip_context_edit.h"
#include "midi_event_list.h"
#include "panel_grip.h"
#include "panel_height.h"
#include "plugin_scales.h"
#include "supple/harmony/harmony.h"
#include "supple/ui/shortcut.h"
#include "take_follow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/model/context.h"
#include "supple/model/follow.h"
#include "supple/notation/notation.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/controls.h"
#include "supple/ui/gl.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/chord_field.h"
#include "supple/views/item_body.h"
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

// The same for a following take's sounding view (gui.md §4.7): the written
// pitch steps by degrees of the take's own key (what midi.edit does), and
// the step chosen is the one whose result sounds nearest `target`, the
// row under the pointer. `sounds` is what a written pitch sounds as there.
int nearestSoundingDegrees(int written, const harmony::Key& key, int from, int target,
                           const std::function<int(int)>& sounds) {
    int best = 0, distance = std::abs(target - from);
    for (int d = 1; d <= 84 && distance > 0; ++d)
        for (int step : {d, -d})
            if (const auto moved = harmony::transposeDegrees(written, key, step);
                moved && std::abs(sounds(*moved) - target) < distance) {
                best = step;
                distance = std::abs(sounds(*moved) - target);
            }
    return best;
}

}  // namespace

namespace {
// An audio clip's time ruler (gui.md §4.7): the seconds between labels at
// `pixelsPerSecond`, the first step at least `room` pixels wide, and a
// label as m:ss.d (two decimals for steps under a tenth).
double timeStep(double pixelsPerSecond, double room) {
    for (double step : {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0,
                        120.0, 300.0, 600.0})
        if (step * pixelsPerSecond >= room) return step;
    return 1200.0;
}
std::string timeLabel(double seconds, double step) {
    const double s = std::max(0.0, seconds);
    const int minutes = static_cast<int>(s / 60);
    char text[32];
    std::snprintf(text, sizeof text, step < 0.1 ? "%d:%05.2f" : "%d:%04.1f", minutes,
                  s - minutes * 60.0);
    return text;
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

bool RollState::outsideKey(const RollNote& note) const {
    if (note.channel == 10) return false;  // General MIDI drums
    const auto key = keyAt(itemPosition + note.start);
    if (!key) return false;
    const int pc = ((note.pitch % 12) + 12) % 12;
    const std::vector<int> classes = harmony::keyPitchClasses(*key);
    return std::find(classes.begin(), classes.end(), pc) == classes.end();
}

std::optional<int> RollState::soundingPitch(std::size_t i, const RollNote& n) const {
    if (!following || i >= sounds.size() || i >= notes.size() || sounds[i].pitch < 0)
        return std::nullopt;
    const RollNote& stored = notes[i];
    if (n.pitch == stored.pitch && n.start == stored.start && n.channel == stored.channel)
        return sounds[i].pitch;
    // A preview: the note as it would be, at the place it would start (the start rule).
    if (!soundsAt) return std::nullopt;
    return soundsAt(n.pitch, n.channel, itemPosition + n.start + sounds[i].shift);
}

std::optional<harmony::Key> RollState::degreeKeyAt(std::size_t i, double projectBeat) const {
    if (!following || !writtenKeyAt) return keyAt(projectBeat);
    const double shift = i < sounds.size() && sounds[i].pitch >= 0 ? sounds[i].shift : passShift;
    return writtenKeyAt(projectBeat + shift);
}

int RollState::drawnPitch(std::size_t i, const RollNote& n) const {
    if (!soundingView()) return n.pitch;
    return soundingPitch(i, n).value_or(n.pitch);
}

bool RollState::outsideItem(std::size_t i, const RollNote& n) const {
    if (following && i < sounds.size() && sounds[i].pitch >= 0) return !sounds[i].heard;
    constexpr double kSlack = 1e-9;
    return n.start + n.length <= kSlack || n.start >= itemLength - kSlack;
}

std::optional<int> RollState::outlinePitch(std::size_t i, const RollNote& n) const {
    // A note not heard has no outline (gui.md §4.7): it is drawn dim instead.
    if (i < sounds.size() && !sounds[i].heard) return std::nullopt;
    const auto heard = soundingPitch(i, n);
    if (!heard || *heard == n.pitch) return std::nullopt;
    return soundingView() ? n.pitch : *heard;
}

double RollState::audioGrid() const { return timeStep(pixelsPerBeat, 20); }

double RollState::snapBeat(double itemBeat) const {
    // An audio clip's axis is seconds: its grid is in seconds, set by the
    // zoom (audioGrid), not the project's beats.
    if (snap && audio) return std::round(itemBeat / audioGrid()) * audioGrid();
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
                    if (const auto key = s.degreeKeyAt(i, s.itemPosition + n.start))
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
    // The note at the pitch the view draws it at (a following take's sounding
    // pitch, gui.md §4.7): as it is, for hit tests, and with the drag preview.
    RollNote placed(std::size_t i) const {
        RollNote n = s.notes[i];
        n.pitch = s.drawnPitch(i, n);
        return n;
    }
    RollNote drawn(std::size_t i) const {
        const RollNote n = shown(i);
        RollNote d = n;
        d.pitch = s.drawnPitch(i, n);
        return d;
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
        drawsGl = true;  // an audio clip's waveform
        layout.width = ui::SizeRule::fraction();
        layout.height = ui::SizeRule::fraction();
    }

    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, bounds()};
        const ui::Rect b = bounds();
        theme_ = &t;
        c.save();
        c.clip(b);
        if (s_.audio) {
            paintAudio(c, g);
            c.restore();
            return;
        }
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
        // Outside the item or clip: shaded over the grid, its end marked as an
        // audio clip's is; notes there are drawn dim (outsideItem).
        paintBounds(c, g);
        // Notes (selected on top), names when there is room.
        std::vector<std::size_t> order(s_.notes.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_partition(order.begin(), order.end(),
                              [&](std::size_t i) { return !s_.selected.contains(i); });
        // A following take: each note's other pitch (the written one in the
        // sounding view, the sounding one in the written) as an outline in
        // the notes' colour, under them, where the two differ.
        if (s_.following) {
            const ui::Color outline = (s_.color ? s_.color(t) : t.color("accent")).withAlpha(0.6f);
            for (std::size_t i : order) {
                const RollNote n = g.shown(i);
                const auto other = s_.outlinePitch(i, n);
                if (!other) continue;
                RollNote o = n;
                o.pitch = *other;
                const ui::Rect r = g.rectOf(o);
                if (r.right() < b.x || r.x > b.right() || r.bottom() < b.y || r.y > b.bottom())
                    continue;
                c.strokeRound(r, 2, outline);
            }
        }
        for (std::size_t i : order) {
            const RollNote n = g.drawn(i);
            const ui::Rect r = g.rectOf(n);
            if (r.right() < b.x || r.x > b.right() || r.bottom() < b.y || r.y > b.bottom())
                continue;
            const bool sel = s_.selected.contains(i);
            // Outside the item nothing is heard: the note is drawn dim, over
            // the shading, still where it can be grabbed.
            const bool outside = s_.outsideItem(i, s_.notes[i]);
            auto dim = [outside](ui::Color col) {
                if (outside) col.a *= 0.45f;
                return col;
            };
            c.fillRound(r, 2, dim(velocityColor(t, s_, n.velocity, sel)));
            c.strokeRound(r, 2,
                          dim(sel                ? t.color("ink")
                              : s_.outsideKey(n) ? t.color("meter-hot")
                                                 : t.color("ground").withAlpha(0.7f)));
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
                const ui::Rect r = g.rectOf(g.placed(k));
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
        // An audio clip's waveform is not edited: the wheel zooms and
        // scrolls it, as the roll.
        if (e.type == ui::PointerEvent::Type::Down) {
            s_.envLastLane.reset();  // the editor's keys are the notes' again
            if (!s_.envSel.empty()) {
                s_.envSel.clear();  // a press on the roll takes the lanes' selection away
                editor_.repaintLanes();
            }
        }
        if (s_.audio && e.type != ui::PointerEvent::Type::Wheel) return false;
        const bool handled = handle(e);
        if (s_.audio) return handled;
        noteDrag(e);
        return handled;
    }

    // An audio clip (gui.md §4.7): time lines at the ruler's labels, the
    // clip shaded outside its span, its end marked. The waveform is drawn
    // in paintGl.
    void paintAudio(ui::Canvas& c, const Geometry& g) {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        // The snap grid faint, the ruler's labelled lines over it.
        for (const auto& [step, alpha] :
             {std::pair{s_.audioGrid(), 0.25f}, {timeStep(s_.pixelsPerBeat, 56), 0.6f}}) {
            const double first = std::floor(std::max(0.0, g.beatOf(b.x)) / step) * step;
            for (double at = first; g.xOf(at) <= b.right(); at += step)
                c.fillRect({g.xOf(at), b.y, 1, b.h}, t.color("rule").withAlpha(alpha));
        }
        paintBounds(c, g);
    }

    // Before the start and past the end: shaded; the end a line with "end".
    void paintBounds(ui::Canvas& c, const Geometry& g) {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        const float x0 = g.xOf(0), x1 = g.xOf(s_.itemLength);
        if (x0 > b.x) c.fillRect({b.x, b.y, x0 - b.x, b.h}, t.color("ground").withAlpha(0.6f));
        s_.endMarkX.reset();
        if (x1 < b.right()) {
            const float from = std::max(x1, b.x);
            c.fillRect({from, b.y, b.right() - from, b.h}, t.color("ground").withAlpha(0.6f));
            if (x1 >= b.x) {  // the end itself in view
                c.fillRect({x1, b.y, 1, b.h}, t.color("ink-quiet"));
                c.text({x1 + 4, b.y + 2, 60, 14}, "end", ui::Font::Ui, t.type("caption"),
                       t.color("ink-quiet"), ui::kLeft | ui::kMiddle);
                s_.endMarkX = x1;
            }
        }
    }

    void paintGl(const ui::GlFrame& frame) override {
        s_.waveColumns = 0;
        if (!s_.audio || !s_.wave || glFailed_ || !theme_) return;
        peaks::PeakService* service = editor_.peaks();
        if (!service) return;
        if (!strips_) {
            auto made = ui::StripRenderer::create();
            if (!made) {
                glFailed_ = true;
                return;
            }
            strips_.emplace(std::move(*made));
        }
        const auto pyramid = service->get(s_.wave->hash, s_.wave->path);
        s_.wavePending = !pyramid;
        if (!pyramid) return;  // the service's delivery repaints (peaksReady)
        const ui::Rect b = bounds();
        const Geometry g{s_, b};
        const float x0 = std::max(b.x, g.xOf(0)), x1 = std::min(b.right(), g.xOf(s_.itemLength));
        if (x1 <= x0 || b.h < 16) return;
        const ui::Rect r{x0, b.y + 6, x1 - x0, b.h - 12};
        const views::AudioWindow window{[g](float x) { return g.beatOf(x); }, s_.pixelsPerBeat,
                                        s_.wave->loop};
        const ui::Color color = s_.color ? s_.color(*theme_) : theme_->color("accent");
        s_.waveColumns = views::drawAudioBody(*strips_, frame, r, *pyramid, s_.wave->take, window,
                                              s_.wave->gain, ui::packRgba(color.withAlpha(0.85f)));
        strips_->flush(frame);
    }

    // Edge scroll, as in the arrange view (gui.md §4.3): a note drag or a
    // marquee held within the zone at the roll's left or right edge, or
    // past it, scrolls in time; a move and a marquee also scroll the
    // pitches at the top and bottom. The press point goes with the content
    // and the drag's last event is delivered again, so the notes in hand
    // stay under the pointer.
    bool autoScrolling() const { return edgeVx_ != 0 || edgeVy_ != 0; }
    std::optional<ui::StripRenderer> strips_;  // the waveform's, made on first use
    bool glFailed_ = false;
    const ui::Theme* theme_ = nullptr;  // from the last paint (paintGl has no canvas)
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
                    const double at = s_.snap ? start : std::max(0.0, g.beatOf(e.pos.x));
                    int pitch = std::clamp(g.pitchOf(e.pos.y), 0, 127);
                    // The sounding view draws where notes sound: the note
                    // written is the one that sounds as the row clicked there.
                    if (s_.soundingView()) {
                        const auto written = writtenFor(pitch, at);
                        if (!written) {
                            editor_.refuse("MIDI: nothing written sounds as " +
                                           harmony::spell(pitch, s_.keyAt(s_.itemPosition + at)) +
                                           " here: draw it in Written");
                            s_.drag = RollState::Drag::None;
                            return true;
                        }
                        pitch = *written;
                    }
                    json::Value op = json::Value::object();
                    op["op"] = "add";
                    op["pitch"] = pitch;
                    op["start"] = at;
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
                    const double at = s_.itemPosition + anchor.start + s_.dBeats;
                    if (s_.degrees && s_.dPitch != 0)
                        if (const auto key = s_.degreeKeyAt(anchor_, at)) {
                            if (s_.soundingView()) {
                                // Steps of the written pitch in the take's key,
                                // the one that sounds nearest the pointer's row
                                // where the note lands.
                                RollNote moved = anchor;
                                moved.start += s_.dBeats;
                                const int from = s_.drawnPitch(anchor_, anchor);
                                s_.dDegrees = nearestSoundingDegrees(
                                    anchor.pitch, *key, from, from + s_.dPitch, [&](int w) {
                                        moved.pitch = w;
                                        return s_.soundingPitch(anchor_, moved).value_or(w);
                                    });
                            } else {
                                s_.dDegrees = nearestDegrees(anchor.pitch, *key, s_.dPitch);
                            }
                        }
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
                        const ui::Rect r = g.rectOf(g.placed(i));
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
            // The sounding view, by semitones: where the note in hand lands on
            // a pitch that a neighbouring written pitch also sounds as there,
            // say so (the mapping merges them, gui.md §4.7). A degree step
            // lands in the scale, where a merge is with an alteration only.
            std::string merged;
            if (drag == RollState::Drag::Move && moved_ && !s_.degrees && s_.dPitch != 0 &&
                s_.soundingView()) {
                s_.drag = drag;  // the preview as it was drawn
                const RollNote landed = g.shown(anchor_);
                s_.drag = RollState::Drag::None;
                if (const auto heard = s_.soundingPitch(anchor_, landed))
                    for (const int other : {landed.pitch - 1, landed.pitch + 1}) {
                        RollNote twin = landed;
                        twin.pitch = std::clamp(other, 0, 127);
                        if (twin.pitch == landed.pitch || s_.soundingPitch(anchor_, twin) != heard)
                            continue;
                        // The written pitches in the take's key, the sounding
                        // one in the song's, as each view names them.
                        const double at = s_.itemPosition + landed.start;
                        const auto own = s_.degreeKeyAt(anchor_, at);
                        const int lo = std::min(landed.pitch, twin.pitch);
                        const int hi = std::max(landed.pitch, twin.pitch);
                        merged = harmony::spell(lo, own) + " and " + harmony::spell(hi, own) +
                                 " both sound as " + harmony::spell(*heard, s_.keyAt(at)) + " here";
                        break;
                    }
            }
            s_.dBeats = s_.dLength = s_.dStart = 0;
            s_.dPitch = s_.dDegrees = 0;
            if (ops.size() > 0 && editor_.edit(std::move(ops), s_.gestureVersion) &&
                !merged.empty())
                editor_.notice(merged);
            repaintAll();
            return true;
        }
        return false;
    }

    std::function<void()> repaintAll = [] {};

  private:
    // The written pitch nearest `sounding` that sounds as it at roll beat
    // `at` (channel 1), within an octave either way; none when no written
    // pitch does (the mapping can merge two pitches and skip one).
    std::optional<int> writtenFor(int sounding, double at) const {
        if (!s_.soundsAt) return std::nullopt;
        for (int d = 0; d <= 12; ++d)
            for (const int w : {sounding - d, sounding + d}) {
                if (w < 0 || w > 127) continue;
                if (s_.soundsAt(w, 1, s_.itemPosition + at + s_.passShift) == sounding) return w;
                if (d == 0) break;
            }
        return std::nullopt;
    }
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
        if (s_.audio) {  // an audio clip: its take's name, as a lane's title
            if (s_.wave)
                c.text({b.x + 6, b.y + 4, b.w - 8, 16}, s_.wave->label, ui::Font::Ui,
                       t.type("caption"), t.color("ink"), ui::kLeft | ui::kMiddle);
            c.fillRect({b.right() - 1, b.y, 1, b.h}, t.color("rule"));
            c.restore();
            return;
        }
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
    struct ChordHit {
        double beat = 0;  // the chord point's project beat
        ui::Rect label;   // its start and name, as the last paint drew them
    };

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
        using T = ui::PointerEvent::Type;
        if (s_.audio) return false;  // an audio clip's ruler: no chords or keys
        const ui::Rect b = bounds();
        const Geometry g{s_, {b.x + kKeysWidth, b.y, b.w - kKeysWidth, b.h}};
        auto snap = [&](double at) {
            at = std::max(0.0, at);
            return s_.snap && s_.grid > 0 ? std::round(at / s_.grid) * s_.grid : at;
        };
        // A clip's chord or key in hand (ADR-0098 X3, as the ruler's): it
        // lands on the grid beat it is dragged to, Alt copies a chord.
        if (held_ != Held::None) {
            if (e.type == T::Cancel) {
                held_ = Held::None;
                invalidate();
                return true;
            }
            if (e.type == T::Move) {
                const double to = snap(from_ + g.beatOf(e.pos.x) - g.beatOf(pressX_));
                moved_ = moved_ || std::abs(e.pos.x - pressX_) >= 3;
                if (to != draft_ || moved_) invalidate();
                draft_ = to;
                return true;
            }
            if (e.type == T::Up) {
                if (ui::Root* r = root()) r->capture(nullptr);
                const Held held = held_;
                held_ = Held::None;
                invalidate();
                if (moved_ && draft_ != from_) {
                    const model::ContextType type =
                        held == Held::Chord ? model::ContextType::Chord : model::ContextType::Key;
                    if (takeHeld_)
                        editor_.moveTakePoint(type, from_, draft_, copy_ && held == Held::Chord);
                    else
                        editor_.moveClipPoint(type, from_, draft_, copy_ && held == Held::Chord);
                }
                return true;
            }
        }
        if (e.type != T::Down || e.pos.x < b.x + kKeysWidth) return false;
        std::optional<double> hit;
        for (const ChordHit& h : chordHits_)
            if (h.label.contains(e.pos)) hit = h.beat;
        std::optional<double> keyHit;
        for (const ChordHit& h : keyHits_)
            if (h.label.contains(e.pos)) keyHit = h.beat;
        auto gridBeat = [&] {
            const double at = std::max(0.0, s_.itemPosition + g.beatOf(e.pos.x));
            return s_.grid > 0 ? std::round(at / s_.grid) * s_.grid : at;
        };
        // A timeline item's take (gui.md §4.7): its own row's chords are
        // dragged, double-clicked and right-clicked as a clip's, each edit
        // one take.set {context}.
        if (!s_.clip) {
            std::optional<double> takeHit;
            for (const ChordHit& h : takeHits_)
                if (h.label.contains(e.pos)) takeHit = h.beat;
            const bool onTakeRow = takeRow_.h > 0 && takeRow_.contains(e.pos);
            // The take's row, or anywhere while the take has no context of
            // its own (no row yet): the song's row is edited on the ruler.
            if (e.button == 3 && (onTakeRow || s_.rowLabel.empty())) {
                editor_.openTakeContextMenu(gridBeat(), takeHit, e.pos);
                return true;
            }
            if (onTakeRow && e.button == 1 && e.clicks == 1 && takeHit) {
                held_ = Held::Chord;
                takeHeld_ = true;
                from_ = draft_ = *takeHit;
                pressX_ = e.pos.x;
                moved_ = false;
                copy_ = (e.mods & ui::kAlt) != 0;
                heldText_ = "r";
                for (const auto* marks : {&s_.chords, &s_.otherChords})
                    if ((marks == &s_.chords) == (s_.rowLabel == "take"))
                        for (const RollState::ChordMark& m : *marks)
                            if (std::abs(m.beat - from_) < 1e-6 && !m.text.empty())
                                heldText_ = m.text;
                if (ui::Root* r = root()) r->capture(this);
                return true;
            }
            if (onTakeRow && e.button == 1 && e.clicks == 2) {
                editor_.editTakeChord(takeHit ? *takeHit : gridBeat(), e.pos);
                return true;
            }
        }
        if (s_.clip && e.button == 1 && e.clicks == 1 && (hit || keyHit)) {
            takeHeld_ = false;
            held_ = hit ? Held::Chord : Held::Key;
            from_ = draft_ = hit ? *hit : *keyHit;
            pressX_ = e.pos.x;
            moved_ = false;
            copy_ = (e.mods & ui::kAlt) != 0;
            heldText_.clear();
            for (const RollState::ChordMark& m : hit ? s_.chords : s_.keyMarks)
                if (std::abs(m.beat - from_) < 1e-6) heldText_ = m.text.empty() ? "r" : m.text;
            if (ui::Root* r = root()) r->capture(this);
            return true;
        }
        // A clip's own context (ADR-0098 X3): its menu on a right-click.
        if (s_.clip && e.button == 3) {
            editor_.openContextMenu(gridBeat(), hit, e.pos);
            return true;
        }
        // A double-click edits the chord there; on a clip with no chords yet,
        // anywhere on the strip starts its first.
        // The first row is the project's (or a clip's own) unless it is the
        // take's, handled above; the second row is the take's or read-only.
        const bool chordRow = !s_.chords.empty() && e.pos.y >= b.y + kHeight &&
                              s_.rowLabel != "take" &&
                              (s_.clip || e.pos.y < b.y + kHeight + kChordRowHeight);
        const bool firstChord = s_.clip && s_.chords.empty();
        if (e.button != 1 || e.clicks != 2 || !(chordRow || firstChord)) return false;
        editor_.editChord(hit ? *hit : gridBeat(), e.pos);
        return true;
    }
    // The strip's height: the bar numbers, and the chord row while chords
    // hold over the roll (ADR-0095).
    float wanted() const {
        return kHeight + (s_.chords.empty() ? 0 : kChordRowHeight) +
               (s_.keyMarks.empty() ? 0 : kChordRowHeight) +
               (s_.otherChords.empty() ? 0 : kChordRowHeight);
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect whole = bounds();
        const ui::Rect b{whole.x, whole.y, whole.w, std::min(whole.h, kHeight)};
        const ui::Rect area{b.x + kKeysWidth, b.y, std::max(0.0f, b.w - kKeysWidth), b.h};
        const Geometry g{s_, area};
        labelled_.clear();
        chordLabels_.clear();
        otherLabels_.clear();
        chordHits_.clear();
        takeHits_.clear();
        takeRow_ = {};
        c.fillRect(whole, t.color("raised"));
        c.fillRect({whole.x, whole.bottom() - 1, whole.w, 1}, t.color("rule"));
        c.fillRect({area.x - 1, whole.y, 1, whole.h}, t.color("rule"));
        keyHits_.clear();
        {
            float y = b.bottom();
            const float chordH = s_.chords.empty() ? 0.0f : kChordRowHeight;
            const Geometry rowG{s_, area};
            // A clip's last chord ends with the clip, a take's with the item
            // (ADR-0098: their points lie inside them); the project's runs on.
            const std::optional<float> end = s_.clip ? std::optional<float>(rowG.xOf(s_.contextEnd))
                                             : s_.chordsEnd
                                                 ? std::optional<float>(rowG.xOf(*s_.chordsEnd))
                                                 : std::nullopt;
            if (chordH > 0 && whole.bottom() > y) {
                const ui::Rect row{area.x, y, area.w, std::min(chordH, whole.bottom() - y)};
                const bool take = s_.rowLabel == "take";
                paintChords(c, row, s_.chords, end, false, take ? &takeHits_ : &chordHits_);
                if (take) takeRow_ = row;
                rowLabel(c, row, s_.rowLabel);
            }
            y += chordH;
            if (!s_.keyMarks.empty() && whole.bottom() > y) {
                paintKeys(c, {area.x, y, area.w, std::min(kChordRowHeight, whole.bottom() - y)});
                y += kChordRowHeight;
            }
            // The context the view does not read (a take's, or the song's in
            // the written view), dim and read-only.
            if (!s_.otherChords.empty() && whole.bottom() > y) {
                const ui::Rect row{area.x, y, area.w,
                                   std::min(kChordRowHeight, whole.bottom() - y)};
                const bool take = s_.otherLabel == "take";
                paintChords(
                    c, row, s_.otherChords,
                    s_.otherEnd ? std::optional<float>(rowG.xOf(*s_.otherEnd)) : std::nullopt, true,
                    take ? &takeHits_ : nullptr);
                if (take) takeRow_ = row;
                rowLabel(c, row, s_.otherLabel);
            }
            // A point in hand: where it lands.
            if (held_ != Held::None && moved_) {
                const float x = chordX(draft_);
                const ui::Rect rows{area.x, b.bottom(), area.w, whole.bottom() - b.bottom()};
                c.save();
                c.clip(rows);
                c.fillRect({x, rows.y, 1.5f, rows.h}, t.color("focus"));
                const float textY = takeHeld_ ? takeRow_.y : held_ == Held::Chord ? b.bottom() : y;
                c.text({x + 4, textY, 160, kChordRowHeight - 1}, heldText_, ui::Font::Ui,
                       t.type("caption"), t.color("focus"), ui::kLeft | ui::kMiddle);
                c.restore();
            }
        }
        if (s_.audio) {
            paintTime(c, b, area);
            return;
        }
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
    // An audio clip's ruler: seconds from its start, labelled m:ss.d at a
    // step that fits the label (timeStep), as the roll's time lines.
    void paintTime(ui::Canvas& c, const ui::Rect& b, const ui::Rect& area) {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, area};
        const float size = t.type("caption");
        const double step = timeStep(s_.pixelsPerBeat, 56);
        c.save();
        c.clip(area);
        const double first = std::floor(std::max(0.0, g.beatOf(area.x)) / step) * step;
        for (double at = first; g.xOf(at) <= area.right(); at += step) {
            const float x = g.xOf(at);
            const std::string text = timeLabel(at, step);
            c.fillRect({x, b.bottom() - 6, 1, 5}, t.color("ink-quiet"));
            c.text({x + 3, b.y, c.textWidth(text, ui::Font::Mono, size) + 4, b.h - 1}, text,
                   ui::Font::Mono, size, t.color("ink"), ui::kLeft | ui::kMiddle);
        }
        c.restore();
    }
    // A row's name in the keyboard column ("song", "take"), when the strip
    // shows two contexts.
    void rowLabel(ui::Canvas& c, const ui::Rect& row, const std::string& text) {
        if (text.empty()) return;
        const ui::Theme& t = c.theme();
        c.text({bounds().x + 4, row.y, kKeysWidth - 6, row.h - 1}, text, ui::Font::Ui,
               t.type("caption") - 1, t.color("ink-quiet"), ui::kLeft | ui::kMiddle);
    }
    // Each chord a cell from its beat to the next chord point, in the
    // ruler's cell and letters; a "no chord" point a short faint tick. The
    // last cell ends at `lastEnd` when given, else runs on. A `dim` row is
    // drawn quieter and counted apart; each cell's start and name go to
    // `hits` when given (the row that takes edits).
    void paintChords(ui::Canvas& c, const ui::Rect& row,
                     const std::vector<RollState::ChordMark>& marks, std::optional<float> lastEnd,
                     bool dim, std::vector<ChordHit>* hits) {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, row};
        const float size = t.type("caption");
        c.save();
        c.clip(row);
        for (std::size_t i = 0; i < marks.size(); ++i) {
            const RollState::ChordMark& m = marks[i];
            const float x = g.xOf(m.beat - s_.itemPosition);
            if (x > row.right()) break;
            const float end = i + 1 < marks.size() ? g.xOf(marks[i + 1].beat - s_.itemPosition)
                              : lastEnd            ? *lastEnd
                                                   : row.right() + 2;
            if (end < row.x) continue;
            // A key change's own cell holds no chord point: nothing to grab.
            std::vector<ChordHit>* cellHits = m.keyOnly ? nullptr : hits;
            if (m.text.empty()) {
                c.fillRect({x, row.y + 4, 1, row.h - 8}, t.color("ink-faint"));
                if (cellHits) cellHits->push_back({m.beat, {x - 4, row.y, 8, row.h}});
                continue;
            }
            const ui::Rect cell{x + 1, row.y + 2, std::max(0.0f, end - x - 2), row.h - 5};
            c.fillRound(cell, 2, t.color("panel"));
            c.strokeRound(cell, 2, dim ? t.color("rule").withAlpha(0.6f) : t.color("rule"));
            const float room = std::max(0.0f, cell.w - 8);
            if (room <= 0) continue;
            // A cell that starts left of the roll keeps its name at the edge.
            const float textX = std::max(cell.x + 4, row.x + 4);
            // A name too long for its cell ends in an ellipsis, or goes.
            const float shown = std::min(room, cell.right() - 4 - textX);
            const std::string fit = c.elide(m.text, ui::Font::Ui, size, std::max(0.0f, shown));
            const float fitW = fit.empty() ? 0.0f : c.textWidth(fit, ui::Font::Ui, size);
            if (cellHits)
                cellHits->push_back(
                    {m.beat, {x - 4, row.y, std::max(8.0f, textX + fitW + 8 - (x - 4)), row.h}});
            if (fit.empty()) continue;
            c.text({textX, row.y, shown + 4, row.h - 1}, fit, ui::Font::Ui, size,
                   t.color(dim       ? "ink-quiet"
                           : m.known ? "ink"
                                     : "ink-quiet"),
                   ui::kLeft | ui::kMiddle);
            (dim ? otherLabels_ : chordLabels_).push_back(fit);
        }
        c.restore();
    }
    // A clip's key points: a tick at each and its name, as the ruler's key
    // row draws them; a name this build cannot read is quiet.
    void paintKeys(ui::Canvas& c, const ui::Rect& row) {
        const ui::Theme& t = c.theme();
        const Geometry g{s_, row};
        const float size = t.type("caption");
        c.save();
        c.clip(row);
        for (std::size_t i = 0; i < s_.keyMarks.size(); ++i) {
            const RollState::ChordMark& m = s_.keyMarks[i];
            const float x = g.xOf(m.beat - s_.itemPosition);
            if (x > row.right()) break;
            const float next = i + 1 < s_.keyMarks.size()
                                   ? g.xOf(s_.keyMarks[i + 1].beat - s_.itemPosition)
                                   : row.right();
            const float room = std::max(0.0f, next - x - 8);
            const std::string fit = c.elide(m.text, ui::Font::Ui, size, room);
            const float w = fit.empty() ? 0.0f : c.textWidth(fit, ui::Font::Ui, size);
            c.fillRect({x, row.y + 3, 1, row.h - 6}, t.color("ink-quiet"));
            if (!fit.empty())
                c.text({x + 4, row.y, w + 4, row.h - 1}, fit, ui::Font::Ui, size,
                       t.color(m.known ? "ink-quiet" : "ink-faint"), ui::kLeft | ui::kMiddle);
            keyHits_.push_back({m.beat, {x - 4, row.y, std::max(8.0f, w + 12), row.h}});
        }
        c.restore();
    }
    enum class Held { None, Chord, Key };
    Held held_ = Held::None;
    double from_ = 0, draft_ = 0;  // the held point's beat, and where it would land
    float pressX_ = 0;
    bool moved_ = false, copy_ = false;
    std::string heldText_;
    std::vector<ChordHit> keyHits_;
    const RollState& s_;
    MidiEditor& editor_;
    std::vector<int>& labelled_;
    std::vector<std::string>& chordLabels_;
    std::vector<ChordHit> chordHits_;
    std::vector<ChordHit> takeHits_;  // the take's row, wherever it is drawn
    ui::Rect takeRow_{};              // that row as last painted; empty without one
    bool takeHeld_ = false;           // the point in hand is the take's

  public:
    std::vector<std::string> otherLabels_;  // the dim row's names, as last painted (tests)
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

// ----------------------------------------------------------------- clip-time lanes

// One of a MIDI clip take's envelopes, or one on its take FX (gui.md §4.7):
// the curve in clip beats on the roll's axis, dimmed past the clip's end,
// where it holds its last value (dashed). A click adds a point and drags it;
// a point drags in beat (snapped to the grid) and value; a right- or
// Alt-click removes one; each is one env.points.set on release. The header
// at the left names the lane and hides it. Clip time takes no writing mode
// and no automation items, so there is no read/write cell.
class EnvLane : public ui::Node {
  public:
    static constexpr float kHeight = 56, kTitle = 14;
    EnvLane(RollState& s, MidiEditor& editor, ui::Node*& roll, model::Id env)
        : Node("midi.env." + env.str()), s_(s), editor_(editor), roll_(roll), env_(env) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(kHeight);
    }
    ui::Rect area() const {
        const ui::Rect b = bounds();
        return {b.x + kKeysWidth, b.y + kTitle, std::max(0.0f, b.w - kKeysWidth),
                std::max(1.0f, b.h - kTitle - 4)};
    }
    // The header column holds only "hide": the lane's title row names it in
    // full ("Volume (take)"), which the narrow column could only cut short.
    ui::Rect hideRect() const {
        const ui::Rect b = bounds();
        return {b.x + 4, b.y + 4, kKeysWidth - 8, 14};
    }
    const RollEnvLane* lane() const {
        for (const RollEnvLane& l : s_.envLanes)
            if (l.env == env_) return &l;
        return nullptr;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        c.fillRect({b.x, b.y, b.w, 1}, t.color("rule"));
        const RollEnvLane* l = lane();
        if (!l) return;
        c.text(hideRect(), "hide", ui::Font::Ui, t.type("caption"), t.color("ink-faint"));
        const ui::Rect a = area();
        c.text({a.x + 4, b.y + 1, std::max(0.0f, a.w - 8), kTitle}, l->title, ui::Font::Ui,
               t.type("caption"), t.color("ink-quiet"));
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        // The points as dragged (in the press's order, as moving_ names
        // them), and the curve through them in position order.
        model::Envelope env = l->envelope;
        if (dragging_) env.points = draft_;
        model::Envelope curve = env;
        std::stable_sort(
            curve.points.begin(), curve.points.end(),
            [](const model::EnvPoint& x, const model::EnvPoint& y) { return x.pos < y.pos; });
        auto yOf = [&](double value) {
            return a.bottom() -
                   static_cast<float>(std::clamp(l->scale.toNorm(value), 0.0, 1.0)) * a.h;
        };
        c.save();
        c.clip(a);
        c.fillRect({a.x, yOf(l->scale.reference()), a.w, 1}, t.color("rule").withAlpha(0.5f));
        const ui::Color ink = t.color("accent").withAlpha(env.active ? 1.0f : 0.4f);
        const float end = g.xOf(s_.itemLength);
        // The curve to the clip's end, then its value there held, dashed.
        float px = a.x;
        float py = yOf(views::envValueAt(curve, std::max(0.0, g.beatOf(px)), l->fallback));
        for (float x = a.x + 2; x <= std::min(a.right(), end) + 2; x += 2) {
            const float cx = std::min(x, end);
            const float y = yOf(views::envValueAt(curve, std::max(0.0, g.beatOf(cx)), l->fallback));
            c.line({px, py}, {cx, y}, ink, 1.5f);
            px = cx;
            py = y;
            if (cx >= end) break;
        }
        if (end < a.right()) {
            const float y = yOf(views::envValueAt(curve, s_.itemLength, l->fallback));
            for (float x = std::max(end, a.x); x < a.right(); x += 7)
                c.line({x, y}, {std::min(x + 4, a.right()), y}, ink.withAlpha(0.45f), 1.2f);
        }
        for (std::size_t i = 0; i < env.points.size(); ++i) {
            const model::EnvPoint& pt = env.points[i];
            const float x = g.xOf(pt.pos), y = yOf(pt.value);
            const bool held = dragging_ && i == index_;
            const bool picked = dragging_ ? moving_.contains(i) : s_.envSelected(env_, pt);
            // Selected: the timeline's filled 8 px square in the focus colour.
            if (held || picked)
                c.fillRect({x - 4, y - 4, 8, 8}, t.color("focus"));
            else
                c.fillRect({x - 3, y - 3, 6, 6}, ink);
            if (held)
                c.text({x + 6, y - 14, 120, 12}, l->scale.text(pt.value), ui::Font::Ui,
                       t.type("caption"), t.color("ink"));
        }
        // A marquee being drawn.
        if (marking_ && marked_) {
            const ui::Rect m = markRect();
            c.fillRect(m, t.color("focus").withAlpha(0.15f));
            c.strokeRect(m, t.color("focus"), 1);
        }
        // Before the clip's start and past its end: not played (as the roll).
        if (const float start = g.xOf(0); start > a.x)
            c.fillRect({a.x, a.y, std::min(start, a.right()) - a.x, a.h},
                       t.color("ground").withAlpha(0.6f));
        if (end < a.right())
            c.fillRect({std::max(end, a.x), a.y, a.right() - std::max(end, a.x), a.h},
                       t.color("ground").withAlpha(0.6f));
        c.restore();
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        const RollEnvLane* l = lane();
        if (e.type == T::Cancel) {
            dragging_ = marking_ = false;
            invalidate();
            return true;
        }
        if (!l) return false;
        const ui::Rect a = area();
        const Geometry g{s_, roll_ ? roll_->bounds() : a};
        auto valueAt = [&](float y) {
            return l->scale.fromNorm(
                static_cast<double>(std::clamp((a.bottom() - y) / a.h, 0.0f, 1.0f)));
        };
        auto beatAt = [&](float x) {
            return std::clamp(s_.snapBeat(g.beatOf(x)), 0.0, s_.itemLength);
        };
        auto hit = [&](ui::Point p) -> std::optional<std::size_t> {
            const auto& pts = l->envelope.points;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const float x = g.xOf(pts[i].pos);
                const float y =
                    a.bottom() -
                    static_cast<float>(std::clamp(l->scale.toNorm(pts[i].value), 0.0, 1.0)) * a.h;
                if (std::abs(p.x - x) <= 5 && std::abs(p.y - y) <= 5) return i;
            }
            return std::nullopt;
        };
        if (e.type == T::Down && e.button == 1 && hideRect().contains(e.pos)) {
            (void)editor_.showEnvelope(l->envelope.target, false);
            return true;
        }
        if (e.type == T::Down && e.pos.x < a.x) return true;  // the header
        if (e.type == T::Down) {
            s_.selected.clear();  // a lane's press leaves the notes
            s_.envLastLane = env_;
        }
        // A marquee, as on the timeline's lanes: a right-drag (Shift or Ctrl
        // adds to the selection), or a Ctrl-drag from empty space (adds). A
        // right-click without a drag opens the lane's menu.
        if (e.type == T::Down &&
            (e.button == 3 || (e.button == 1 && (e.mods & ui::kCtrl) && !hit(e.pos)))) {
            marking_ = true;
            marked_ = false;
            markButton_ = e.button;
            markAdd_ = e.button == 1 || (e.mods & (ui::kShift | ui::kCtrl));
            markFrom_ = markTo_ = e.pos;
            if (ui::Root* r = root()) {
                r->capture(this);
                if (roll_) r->focus(roll_);
            }
            return true;
        }
        if (e.type == T::Move && marking_) {
            markTo_ = e.pos;
            marked_ = marked_ || std::abs(markTo_.x - markFrom_.x) > 3 ||
                      std::abs(markTo_.y - markFrom_.y) > 3;
            invalidate();
            return true;
        }
        if (e.type == T::Up && marking_) {
            if (ui::Root* r = root()) r->capture(nullptr);
            marking_ = false;
            if (marked_) {
                // The lane's points inside it become the selection, or join it.
                const ui::Rect m = markRect();
                if (!markAdd_ || s_.envSelLane != env_) s_.envSel.clear();
                s_.envSelLane = env_;
                for (const model::EnvPoint& pt : l->envelope.points) {
                    const float x = g.xOf(pt.pos);
                    const float y =
                        a.bottom() -
                        static_cast<float>(std::clamp(l->scale.toNorm(pt.value), 0.0, 1.0)) * a.h;
                    if (m.contains({x, y}) && !s_.envSelected(env_, pt))
                        s_.envSel.push_back({pt.pos, pt.value});
                }
                editor_.repaintLanes();
            } else if (markButton_ == 3) {
                editor_.openEnvLaneMenu(env_, e.pos, beatAt(markFrom_.x), valueAt(markFrom_.y));
            }
            invalidate();
            return true;
        }
        // Alt+click deletes the point under it.
        if (e.type == T::Down && e.button == 1 && (e.mods & ui::kAlt)) {
            if (const auto i = hit(e.pos)) {
                const std::uint64_t version = editor_.beginGesture();
                std::vector<model::EnvPoint> pts = l->envelope.points;
                pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(*i));
                (void)editor_.setEnvPoints(env_, pts, version);
            }
            return true;
        }
        // Ctrl+click adds a point to the selection or takes it out.
        if (e.type == T::Down && e.button == 1 && (e.mods & ui::kCtrl)) {
            if (const auto i = hit(e.pos)) {
                const model::EnvPoint& pt = l->envelope.points[*i];
                if (s_.envSelLane != env_) s_.envSel.clear();
                s_.envSelLane = env_;
                if (s_.envSelected(env_, pt))
                    std::erase_if(s_.envSel, [&](const auto& sel) {
                        return std::abs(sel.first - pt.pos) < 1e-9 &&
                               std::abs(sel.second - pt.value) < 1e-9;
                    });
                else
                    s_.envSel.push_back({pt.pos, pt.value});
                invalidate();
            }
            return true;
        }
        if (e.type == T::Down && e.button == 1) {
            version_ = editor_.beginGesture();
            l = lane();  // the gesture starts from the session as it is
            if (!l) return true;
            draft_ = original_ = l->envelope.points;
            moving_.clear();
            if (const auto i = hit(e.pos)) {
                // A point: the selection moves with it (it alone when it was
                // not selected).
                if (!s_.envSelected(env_, original_[*i])) {
                    s_.envSelLane = env_;
                    s_.envSel = {{original_[*i].pos, original_[*i].value}};
                }
                for (std::size_t k = 0; k < original_.size(); ++k)
                    if (s_.envSelected(env_, original_[k])) moving_.insert(k);
                index_ = *i;
                grab_ = {e.pos.x - g.xOf(original_[*i].pos), 0};
                moved_ = false;
            } else {
                // Empty space: a new point there, selected, in hand.
                model::EnvPoint pt;
                pt.pos = beatAt(e.pos.x);
                pt.value = valueAt(e.pos.y);
                const auto at = std::upper_bound(
                    original_.begin(), original_.end(), pt.pos,
                    [](double beat, const model::EnvPoint& p) { return beat < p.pos; });
                index_ = static_cast<std::size_t>(at - original_.begin());
                original_.insert(at, pt);
                draft_ = original_;
                moving_ = {index_};
                s_.envSelLane = env_;
                s_.envSel = {{pt.pos, pt.value}};
                grab_ = {};
                moved_ = true;  // a new point is an edit already
            }
            dragging_ = true;
            if (ui::Root* r = root()) {
                r->capture(this);
                if (roll_) r->focus(roll_);  // the editor's keys: Delete takes the points
            }
            invalidate();
            return true;
        }
        if (e.type == T::Move && dragging_) {
            // Every selected point moves by the anchor's time (on the grid
            // unless Shift) and the same share of the lane's height.
            const model::EnvPoint& anchor = original_[index_];
            const double to = (e.mods & ui::kShift)
                                  ? std::clamp(g.beatOf(e.pos.x - grab_.x), 0.0, s_.itemLength)
                                  : beatAt(e.pos.x - grab_.x);
            double dt = to - anchor.pos;
            for (std::size_t k : moving_)
                dt = std::clamp(dt, -original_[k].pos, s_.itemLength - original_[k].pos);
            const double dn = l->scale.toNorm(valueAt(e.pos.y)) - l->scale.toNorm(anchor.value);
            draft_ = original_;
            for (std::size_t k : moving_) {
                draft_[k].pos = original_[k].pos + dt;
                draft_[k].value = l->scale.fromNorm(
                    std::clamp(l->scale.toNorm(original_[k].value) + dn, 0.0, 1.0));
            }
            moved_ = true;
            invalidate();
            return true;
        }
        if (e.type == T::Up && dragging_) {
            if (ui::Root* r = root()) r->capture(nullptr);
            dragging_ = false;
            if (moved_) {
                // The selection follows its points; the order is by position.
                s_.envSel.clear();
                for (std::size_t k : moving_) s_.envSel.push_back({draft_[k].pos, draft_[k].value});
                std::stable_sort(draft_.begin(), draft_.end(),
                                 [](const model::EnvPoint& x, const model::EnvPoint& y) {
                                     return x.pos < y.pos;
                                 });
                (void)editor_.setEnvPoints(env_, draft_, version_);
            } else if (moving_.size() > 1) {
                // A click on one of several selected points selects it alone.
                s_.envSel = {{original_[index_].pos, original_[index_].value}};
            }
            moving_.clear();
            invalidate();
            return true;
        }
        return false;
    }

  private:
    RollState& s_;
    MidiEditor& editor_;
    ui::Node*& roll_;
    model::Id env_;
    // The marquee: from where it was pressed to the pointer, inside the lane.
    ui::Rect markRect() const {
        const ui::Rect a = area();
        const float x0 = std::clamp(std::min(markFrom_.x, markTo_.x), a.x, a.right());
        const float x1 = std::clamp(std::max(markFrom_.x, markTo_.x), a.x, a.right());
        const float y0 = std::clamp(std::min(markFrom_.y, markTo_.y), a.y, a.bottom());
        const float y1 = std::clamp(std::max(markFrom_.y, markTo_.y), a.y, a.bottom());
        return {x0, y0, x1 - x0, y1 - y0};
    }
    bool marking_ = false, marked_ = false, markAdd_ = false;
    int markButton_ = 0;
    ui::Point markFrom_{}, markTo_{};
    bool dragging_ = false, moved_ = false;
    std::size_t index_ = 0;         // the point in hand, in original_
    std::set<std::size_t> moving_;  // the selected points it moves, in original_
    ui::Point grab_{};
    std::uint64_t version_ = 0;
    std::vector<model::EnvPoint> original_, draft_;  // at the press, and as dragged
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
    // An audio clip opens too (its waveform and lanes); an audio item does
    // not: its take is edited on the timeline.
    if (!o.source && o.clip && o.take && o.take->source) o.audio = s.media.find(*o.take->source);
    if (!o.source && !o.audio) return std::nullopt;
    return o;
}

MidiEditor::MidiEditor(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui)
    : root_(root),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      scales_(std::make_unique<PluginScales>(dispatcher, &app)) {
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
        // A MIDI take; an audio clip only when it was open last or picked in
        // the launcher.
        std::set<std::string> picked;
        if (!lastItem_.empty()) picked.insert(lastItem_);
        auto isMidi = [&](const std::string& text) {
            const auto o = midiOwner(s, text);
            return o && (o->source || picked.contains(text));
        };
        std::vector<std::string> candidates;
        if (!lastItem_.empty()) candidates.push_back(lastItem_);
        if (ui_.clipsFocused)  // a launcher cell was clicked last
            for (const std::string& c : ui_.selectedClips) {
                candidates.push_back(c);
                picked.insert(c);
            }
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
        midiOnly_.push_back(
            &header.emplace<ui::Button>(std::string("midi.grid.") + label, label, [this, gv] {
                state_.grid = gv;
                state_.lastLength = gv;
                refreshTitle();
                if (roll_) roll_->invalidate();
            }));
    }
    // Snap stays for an audio clip: its lanes snap to a seconds grid.
    header.emplace<ui::Button>("midi.snap", "Snap", [this] {
        state_.snap = !state_.snap;
        refreshTitle();
    });
    midiOnly_.push_back(
        &header.emplace<ui::Button>("midi.quantize", "Quantize", [this] { quantize(); }));
    // A following take (gui.md §4.7): Written shows the written pitches, the
    // sounding ones outlined; Pass picks which pass of a looped item whose
    // passes sound differently the roll shows. Each only while it applies.
    writtenButton_ =
        &header.emplace<ui::Toggle>("midi.written", "Written", "accent", [this](bool on) {
            state_.written = on;
            (void)reload();
        });
    writtenButton_->layout.width = ui::SizeRule::content();
    writtenButton_->textSize = "small";
    writtenButton_->offInk = "ink";
    writtenButton_->visible = false;
    passButton_ = &header.emplace<ui::Button>("midi.pass", "Pass 1", [this] { openPassMenu(); });
    passButton_->layout.width = ui::SizeRule::content();
    passButton_->visible = false;
    // The key at the item's start; lit, pitch drags and arrows go by degrees.
    // An audio clip's is its one key point, set from the button's menu.
    keyButton_ = &header.emplace<ui::Toggle>("midi.key", "No key", "accent", [this](bool on) {
        if (state_.audio) {
            keyButton_->setOn(false);
            openAudioKeyMenu();
            return;
        }
        state_.degrees = on;
    });
    keyButton_->layout.width = ui::SizeRule::content();
    keyButton_->textSize = "small";  // as the header's buttons
    keyButton_->offInk = "ink";
    keyButton_->enabled = false;
    // One menu owns every lane under the roll (gui.md §4.7).
    lanesButton_ = &header.emplace<ui::Button>("midi.lanes", "Lanes", [this] { openLanesMenu(); });
    midiOnly_.push_back(&header.emplace<ui::Button>("midi.notation.toggle", "Notation", [this] {
        if (!notationPane_) return;
        notationPane_->visible = !notationPane_->visible;
        if (!notationPane_->visible) root_.releaseWithin(notationPane_);
        root_.relayout();
        root_.damageAll();
    }));
    midiOnly_.push_back(&header.emplace<ui::Button>("midi.list.toggle", "List", [this] {
        if (!listPane_ || !rollPane_) return;
        listPane_->visible = !listPane_->visible;
        rollPane_->visible = !listPane_->visible;
        roll_->visible = !listPane_->visible;
        eventList_->activate(listPane_->visible);
        if (!listPane_->visible) root_.focus(roll_);
        root_.relayout();
        root_.damageAll();
    }));
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
    // A MIDI clip's take envelopes and take FX envelopes (gui.md §4.7).
    envColumn_ = &left.emplace<ui::Node>("midi.envlanes");
    envColumn_->layout.dir = ui::Layout::Dir::Column;
    envColumn_->layout.height = ui::SizeRule::content();
    auto repaint = [this] {
        if (roll_) roll_->invalidate();
        if (bars_) bars_->invalidate();
        if (keys_) keys_->invalidate();
        if (velocity_) velocity_->invalidate();
        if (ccLane_) ccLane_->invalidate();
        if (envColumn_)
            for (const auto& lane : envColumn_->children()) lane->invalidate();
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
    notationTitle_ = &pane.emplace<ui::Label>("midi.notation.title", "Notation", ui::Font::UiBold,
                                              "caption", "ink-quiet");
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
    // Center the view on the notes (or middle C): a following take's as
    // drawn and as outlined, so both show.
    int lo = 127, hi = 0;
    for (std::size_t i = 0; i < state_.notes.size(); ++i) {
        const RollNote& n = state_.notes[i];
        for (const int p : {n.pitch, state_.soundingPitch(i, n).value_or(n.pitch)}) {
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
    }
    const double center = state_.notes.empty() ? 60 : (lo + hi) / 2.0;
    // The roll's height as the panel will lay it out for this item: the
    // panel's, less its grip, header, bar strip (which may have grown a
    // chord or key row) and the lanes shown under the roll, all fixed. Its
    // bounds may be stale or, on a first open, empty.
    float rollH = (panel_ ? panel_->layout.height.value : height_) - kGripHeight - 32;
    if (bars_) rollH -= bars_->layout.height.value;
    for (const ui::Node* lane : {velocity_, ccLane_})
        if (lane && lane->visible) rollH -= lane->layout.height.value;
    if (envColumn_)
        for (const auto& lane : envColumn_->children()) rollH -= lane->layout.height.value;
    const float rows = rollH > state_.rowHeight ? rollH / state_.rowHeight : 22;
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
    state_.envLanes.clear();
    refreshEnvLanes();
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
    v["written"] = state_.written;
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
    if (const json::Value* w = v.find("written"); w && w->isBool()) state_.written = w->asBool();
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
    show(velocity_, state_.velocityShown && !state_.audio);
    show(ccLane_, state_.ccShown && !state_.audio);
    if (!changed) return;
    root_.relayout();
    root_.damageAll();
}

void MidiEditor::applyMode() {
    // An audio clip has no notes: no grid, Snap, Quantize, notation, event
    // list, velocity or CC lane. Its pane keeps the title, the key button,
    // Lanes, Close, the waveform and its clip-time lanes.
    const bool audio = state_.audio;
    bool changed = false;
    auto show = [&](ui::Node* node, bool shown) {
        if (!node || node->visible == shown) return;
        if (!shown) root_.releaseWithin(node);
        node->visible = shown;
        changed = true;
    };
    for (ui::Node* n : midiOnly_) show(n, !audio);
    if (audio) {
        if (notationPane_ && notationPane_->visible) notationWasShown_ = true;
        if (notationPane_ && !notationPane_->visible && !audioShown_) notationWasShown_ = false;
        show(notationPane_, false);
        if (listPane_ && listPane_->visible) {
            eventList_->activate(false);
            show(listPane_, false);
            show(rollPane_, true);
            show(roll_, true);
        }
    } else if (audioShown_) {
        show(notationPane_, notationWasShown_);
    }
    audioShown_ = audio;
    applyLanes();
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
    // An audio clip has no notes: no velocity or CC lane.
    if (!state_.audio)
        add("Velocity", state_.velocityShown, [this] {
            state_.velocityShown = !state_.velocityShown;
            applyLanes();
        });
    // The CC lane shows one controller: picking another switches it, picking
    // the shown one hides the lane.
    for (const auto& [number, name] : kCcs) {
        if (state_.audio) break;
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
    // A MIDI clip's take envelopes and its take FX's parameters, in clip time.
    const auto owner = isOpen() ? midiOwner(app_.head(), state_.item) : std::nullopt;
    const ui::Rect b = lanesButton_ ? lanesButton_->bounds() : ui::Rect{};
    if (owner && owner->clip && owner->clip->activeTake) {
        const model::Session& head = app_.head();
        const model::Id take = *owner->clip->activeTake;
        auto shown = [&](const model::EnvTarget& target) {
            for (const auto& e : head.envelopes)
                if (e.value->target == target) return e.value->visible;
            return false;
        };
        // A MIDI take's envelope shapes what an instrument in its take FX
        // plays (env.take_midi otherwise); an audio take's shapes its audio.
        bool instrument = owner->audio != nullptr;
        for (model::Id fx : owner->take->chain)
            if (const model::FxInstance* f = head.fx.find(fx))
                if (auto info = app_.findPlugin(f->plugin.id)) instrument |= info->instrument;
        ui::MenuItem rule;
        rule.separator = true;
        if (!items.empty()) items.push_back(rule);
        for (const auto& [param, label] :
             {std::pair{"volume", "Take volume"}, {"pan", "Take pan"}, {"mute", "Take mute"}}) {
            const model::EnvTarget target{take, param};
            const bool on = shown(target);
            add(label, on, [this, target, on] { (void)showEnvelope(target, !on); });
            items.back().enabled = instrument;
        }
        if (!instrument) {
            ui::MenuItem why;
            why.label = "Take envelopes need an instrument in Take FX";
            why.enabled = false;
            items.push_back(why);
        }
        for (model::Id fx : owner->take->chain) {
            const model::FxInstance* f = head.fx.find(fx);
            if (!f) continue;
            const std::string effect = f->label.empty() ? f->plugin.name : f->label;
            items.push_back(rule);
            // Its envelopes, then the whole parameter list.
            for (const auto& e : head.envelopes) {
                if (e.value->target.object != fx) continue;
                const auto scale = (*scales_)(*e.value);
                const std::string name = scale && scale->param && !scale->param->name.empty()
                                             ? scale->param->name
                                             : e.value->target.param;
                const model::EnvTarget target = e.value->target;
                const bool on = e.value->visible;
                add(effect + " \u00b7 " + name, on,
                    [this, target, on] { (void)showEnvelope(target, !on); });
            }
            const ui::Point at{b.x, b.bottom()};
            add(effect + ": parameters\u2026", false, [this, fx, at] { openFxParamsMenu(fx, at); });
        }
    }
    root_.openPopup(
        ui::makeMenu("midi.lanes.menu", std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{b.x, b.bottom(), 0, 0}, ui::Placement::Below);
}

void MidiEditor::openFxParamsMenu(model::Id fx, ui::Point at) {
    json::Value args = json::Value::object();
    args["fx"] = fx.str();
    args["full"] = true;
    auto described = dispatcher_.execute({"fx.describe", std::move(args), kOrigin});
    if (!described) {
        if (actions_.onError) actions_.onError("Lanes: " + described.error().message);
        return;
    }
    const model::Session& head = app_.head();
    std::vector<ui::MenuItem> items;
    if (const json::Value* ps = described->find("params"); ps && ps->isObject())
        for (const auto& m : ps->asObject()) {
            auto d = params::fromDescription(m.key, m.value);
            if (!d || ((*d)->spec.flags & (params::kReadonly | params::kHidden))) continue;
            const model::EnvTarget target{fx, m.key};
            bool on = false;
            for (const auto& e : head.envelopes)
                if (e.value->target == target) on = e.value->visible;
            ui::MenuItem item;
            item.label = (*d)->name.empty() ? m.key : (*d)->name;
            item.checked = on;
            item.run = [this, target, on] { (void)showEnvelope(target, !on); };
            items.push_back(std::move(item));
        }
    if (items.empty()) {
        ui::MenuItem none;
        none.label = "No described parameters";
        none.enabled = false;
        items.push_back(std::move(none));
    }
    root_.openPopup(
        ui::makeMenu("midi.lanes.params", std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

void MidiEditor::refreshEnvLanes() {
    if (!envColumn_) return;
    std::vector<model::Id> wanted;
    for (const RollEnvLane& l : state_.envLanes) wanted.push_back(l.env);
    if (wanted == envNodes_) {
        for (const auto& lane : envColumn_->children()) lane->invalidate();
        return;
    }
    root_.releaseWithin(envColumn_);
    envColumn_->clear();
    for (model::Id env : wanted) envColumn_->emplace<EnvLane>(state_, *this, roll_, env);
    envNodes_ = std::move(wanted);
    root_.relayout();
    root_.damageAll();
}

Status MidiEditor::command(std::string_view name, json::Value args,
                           std::optional<std::uint64_t> expectedVersion, std::string_view what) {
    if (expectedVersion && (*expectedVersion != app_.head().version ||
                            gestureGeneration_ != app_.projectGeneration())) {
        follow({});
        return Error("gui.midi.stale", "the clip changed during the gesture", "/env",
                     "start the gesture again against the current clip");
    }
    auto r = dispatcher_.execute({std::string(name), std::move(args), kOrigin});
    if (!r) {
        if (actions_.onError) actions_.onError(std::string(what) + ": " + r.error().message);
        return std::move(r).error();
    }
    return reload();
}

Status MidiEditor::setEnvPoints(model::Id env, const std::vector<model::EnvPoint>& points,
                                std::optional<std::uint64_t> expectedVersion) {
    json::Value args = json::Value::object(), list = json::Value::array();
    args["env"] = env.str();
    for (const model::EnvPoint& pt : points) {
        json::Value p = json::Value::object();
        p[state_.audio ? "seconds" : "beat"] = pt.pos;  // the clip's unit
        p["value"] = pt.value;
        p["shape"] = shapeName(pt.shape);
        if (pt.tension != 0) p["tension"] = pt.tension;
        list.push_back(std::move(p));
    }
    args["points"] = std::move(list);
    return command("env.points.set", std::move(args), expectedVersion);
}

Status MidiEditor::showEnvelope(const model::EnvTarget& target, bool shown) {
    const model::Session& head = app_.head();
    for (const auto& e : head.envelopes) {
        if (e.value->target != target) continue;
        if (e.value->visible == shown) return {};
        json::Value args = json::Value::object();
        args["env"] = e.id.str();
        args["visible"] = shown;
        return command("env.set", std::move(args));
    }
    if (!shown) return {};
    // None yet: one point at the clip's start, at the parameter's value now.
    model::Envelope probe;
    probe.target = target;
    const auto scale =
        target.object.kind == model::Kind::Fx ? (*scales_)(probe) : views::envScale(probe);
    if (!scale) {
        Error error("gui.lanes", "the parameter has no lane", "/param",
                    "pick a parameter the plug-in describes");
        if (actions_.onError) actions_.onError("Lanes: " + error.message);
        return error;
    }
    json::Value args = json::Value::object(), points = json::Value::array(),
                point = json::Value::object();
    args[target.object.kind == model::Kind::Fx ? "fx" : "take"] = target.object.str();
    args["param"] = target.param;
    point[state_.audio ? "seconds" : "beat"] = 0.0;  // the clip's unit
    point["value"] = views::targetValue(head, target, *scale);
    points.push_back(std::move(point));
    args["points"] = std::move(points);
    return command("env.add", std::move(args));
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

// A clip's take lanes, in its unit (beats, or seconds for an audio clip);
// an item's are on the timeline.
void MidiEditor::collectEnvLanes(const MidiOwner& owner) {
    state_.envLanes.clear();
    if (owner.clip && owner.clip->activeTake) {
        const model::Session& head = app_.head();
        const model::Id takeId = *owner.clip->activeTake;
        std::vector<std::pair<std::pair<int, std::uint32_t>, RollEnvLane>> lanes;
        for (const auto& e : head.envelopes) {
            const model::Envelope& env = *e.value;
            if (!env.visible) continue;
            int order = 0;
            std::optional<views::EnvScale> scale;
            if (env.target.object == takeId) {
                scale = views::envScale(env);
                order = views::envOrder(env);
            } else if (env.target.object.kind == model::Kind::Fx) {
                const auto at = std::find(owner.take->chain.begin(), owner.take->chain.end(),
                                          env.target.object);
                if (at == owner.take->chain.end()) continue;
                scale = (*scales_)(env);
                order = 100 + static_cast<int>(at - owner.take->chain.begin());
            }
            if (!scale) continue;
            RollEnvLane lane;
            lane.env = e.id;
            lane.envelope = env;
            lane.scale = *scale;
            lane.fallback = views::targetValue(head, env.target, *scale);
            lane.title = env.target.object == takeId ? views::envTitle(env) + " (take)"
                                                     : views::envTitle(env, head, &*scale);
            lanes.push_back({{order, e.id.n}, std::move(lane)});
        }
        std::sort(lanes.begin(), lanes.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        for (auto& l : lanes) state_.envLanes.push_back(std::move(l.second));
    }
}

namespace {

RollState::ChordMark chordMarkOf(double beat, const model::ContextPoint& p) {
    const bool known = p.symbol.empty() || model::parseChordPoint(p).has_value();
    return {beat, p.symbol + (known ? "" : "?"), known};
}

// A take's own context on the roll's axis (gui.md §4.7): its points at roll
// beats (item time, the content's first pass) in [lo, hi), each at every
// repeat of its context beat, and the key and chord holding at lo carried in
// there; as project beats (itemPosition + roll beat), as the strip and keyAt
// read them. A note at roll beat t starts at unwrapped source beat
// start_offset + t × playrate, whose context beat is the phase plus that
// (model::contextBeatOf), so a point at context beat c falls at
// (c + k × period − phase − start_offset) / playrate.
void takeContextMarks(const model::Take& take, double itemPosition, double lo, double hi,
                      std::vector<RollState::ChordMark>& chords,
                      std::vector<RollState::KeySpan>& keys) {
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    struct Mark {
        double at;
        const model::ContextPoint* point;
    };
    std::vector<Mark> marks;
    for (const model::ContextPoint& p : take.context) {
        const double base = (p.beat - take.contextPhase - take.startOffset) / rate;
        if (!take.contextPeriod || !(*take.contextPeriod > 0)) {
            if (base >= lo && base < hi) marks.push_back({base, &p});
            continue;
        }
        const double step = *take.contextPeriod / rate;
        std::size_t n = 0;
        for (double t = base + std::ceil((lo - base) / step) * step; t < hi && n < 4096;
             t += step, ++n)
            marks.push_back({t, &p});
    }
    std::stable_sort(marks.begin(), marks.end(),
                     [](const Mark& a, const Mark& b) { return a.at < b.at; });
    // What holds at lo is carried in there; a mark that repeats the point
    // already holding (the first point, which holds before itself) adds no
    // cell, so it is drawn once, from the left.
    const double from = model::contextBeatOf(take, take.startOffset + lo * rate);
    const model::ContextPoint* lastKey = model::contextKeyPointAt(take.context, from);
    const model::ContextPoint* lastChord = model::contextChordPointAt(take.context, from);
    if (lastKey) keys.push_back({itemPosition + lo, model::parseKey(*lastKey)});
    if (lastChord) chords.push_back(chordMarkOf(itemPosition + lo, *lastChord));
    std::vector<std::pair<double, std::string>> keyChanges;
    for (const Mark& m : marks) {
        if (m.point->type == model::ContextType::Key) {
            if (m.point == lastKey) continue;
            lastKey = m.point;
            keys.push_back({itemPosition + m.at, model::parseKey(*m.point)});
            keyChanges.emplace_back(itemPosition + m.at, model::keyWords(*m.point));
        } else {
            if (m.point == lastChord) continue;
            lastChord = m.point;
            chords.push_back(chordMarkOf(itemPosition + m.at, *m.point));
        }
    }
    // Each key change after the one holding at lo is named in the row (gui.md
    // §4.7): on the cell of a chord at the same beat, else on a cell of its
    // own over the chord holding there.
    const std::vector<RollState::ChordMark> plain = chords;  // the chords alone, unnamed
    for (const auto& [beat, words] : keyChanges) {
        auto at = std::find_if(chords.begin(), chords.end(), [&](const RollState::ChordMark& c) {
            return c.beat >= beat - model::kPlaceSlack;
        });
        if (at != chords.end() && at->beat <= beat + model::kPlaceSlack) {
            at->text = at->text.empty() ? words : words + ": " + at->text;
            continue;
        }
        RollState::ChordMark own{beat, words, true, true};
        const auto holding = std::find_if(
            plain.rbegin(), plain.rend(),
            [&](const RollState::ChordMark& c) { return c.beat < beat - model::kPlaceSlack; });
        if (holding != plain.rend()) {
            if (!holding->text.empty()) own.text += ": " + holding->text;
            own.known = holding->known;
        }
        chords.insert(at, std::move(own));
    }
}

}  // namespace

Status MidiEditor::reload() {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner) return Error("gui.midi", "no MIDI source", "/item");
    if (owner->audio) {
        // An audio clip neither follows nor has a written view.
        state_.following = false;
        state_.sounds.clear();
        state_.soundsAt = nullptr;
        state_.writtenKeyAt = nullptr;
        state_.otherChords.clear();
        state_.rowLabel.clear();
        state_.otherLabel.clear();
        if (writtenButton_) writtenButton_->visible = false;
        if (passButton_) passButton_->visible = false;
        return reloadAudio();
    }
    json::Value args = json::Value::object();
    args["item"] = state_.item;
    auto r = dispatcher_.execute({"midi.notes", std::move(args), kOrigin});
    if (!r) return std::move(r).error();
    // A reload replaces note indices and CC data; an old preview cannot be
    // committed against the new sequence (including undo and item switches).
    state_.cancelGesture();
    state_.audio = false;
    state_.wave.reset();
    applyMode();
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
    // A following take (ADR-0098 amended, gui.md §4.7): what each note sounds
    // as, from model::soundingNotes as the engine compiles it, in the pass
    // shown; and a reading for previews and the pencil (model::placeAt). A
    // clip has no place: its roll is as written.
    state_.following = false;
    state_.sounds.clear();
    state_.soundsAt = nullptr;
    state_.writtenKeyAt = nullptr;
    state_.passes = 1;
    state_.passShift = 0;
    state_.passesDiffer = false;
    const auto itemId = owner->item ? model::Id::parse(state_.item) : std::nullopt;
    if (itemId && take->follow != model::ClipFollow::None) {
        state_.following = true;
        const model::Session& head = app_.head();
        const auto heard = model::soundingNotes(head, *itemId, state_.itemPosition,
                                                state_.itemPosition + state_.itemLength);
        int last = 0;
        std::vector<int> first(state_.notes.size(), -1);
        for (const model::SoundingNote& o : heard) {
            last = std::max(last, static_cast<int>(o.pass));
            if (o.pass < 0 || o.note >= first.size()) continue;
            int& f = first[o.note];
            if (f < 0)
                f = o.sounds.pitch;
            else if (f != o.sounds.pitch)
                state_.passesDiffer = true;
        }
        state_.passes = last + 1;
        state_.pass = std::clamp(state_.pass, 0, state_.passes - 1);
        state_.sounds.assign(state_.notes.size(), {});
        bool shiftSet = false;
        for (const model::SoundingNote& o : heard) {
            if (o.pass != state_.pass || o.note >= state_.sounds.size()) continue;
            const double shift = o.songBeat - (state_.itemPosition + state_.notes[o.note].start);
            state_.sounds[o.note] = {o.sounds.pitch, shift};
            if (!shiftSet && state_.notes[o.note].start >= 0) {  // not a chased note
                state_.passShift = shift;
                shiftSet = true;
            }
        }
        // A note not heard in this pass (before the item's start, past its
        // end): what it would sound as there, by the place rule past the
        // edges (model::notePlace at its unwrapped source beat, then
        // soundingKey), not its written pitch.
        for (std::size_t i = 0; i < state_.notes.size(); ++i) {
            if (state_.sounds[i].pitch >= 0) continue;
            const RollNote& n = state_.notes[i];
            const model::NotePlace place =
                model::notePlace(*take, state_.itemPosition,
                                 take->startOffset + (n.start + state_.passShift) * rate);
            // Before the song's start no item can sound, and the song has no
            // key there: its context at beat 0 is read, as its first point
            // holds before itself.
            const harmony::Followed f = model::soundingKey(
                take->context, take->follow, *head.contextView, place.contextBeat,
                std::max(0.0, place.songBeat), n.channel - 1, n.pitch);
            state_.sounds[i] = {f.pitch, state_.passShift, false};
        }
        state_.soundsAt = [this, id = *itemId](int pitch, int channel, double beat) {
            const auto place = model::placeAt(app_.head(), id, beat);
            if (!place) return pitch;
            return model::soundsAt(*place, channel - 1, pitch).pitch;
        };
        state_.writtenKeyAt = [this, id = *itemId](double beat) -> std::optional<harmony::Key> {
            const auto place = model::placeAt(app_.head(), id, beat);
            return place ? place->take.key : std::nullopt;
        };
        // Where each pass starts sounding: its first heard note's song beat.
        passStarts_.assign(static_cast<std::size_t>(state_.passes), -1.0);
        for (const model::SoundingNote& o : heard) {
            if (o.pass < 0) continue;
            double& at = passStarts_[static_cast<std::size_t>(o.pass)];
            if (at < 0 || o.songBeat < at) at = o.songBeat;
        }
    } else {
        state_.pass = 0;
        passStarts_.clear();
    }
    {
        // The header's Written and Pass cells, only while they apply.
        const bool written = state_.following, pass = state_.following && state_.passesDiffer;
        bool moved = false;
        if (writtenButton_) {
            moved = moved || writtenButton_->visible != written;
            writtenButton_->visible = written;
            writtenButton_->setOn(state_.written);
        }
        if (passButton_) {
            moved = moved || passButton_->visible != pass;
            passButton_->visible = pass;
            const std::string text =
                "Pass " + std::to_string(state_.pass + 1) + " of " + std::to_string(state_.passes);
            if (passButton_->valueText() != text) {
                passButton_->setText(text);
                moved = true;
            }
        }
        // The notation and the list are the take's text: written notes,
        // said while the roll may show others (gui.md §4.7).
        if (notationTitle_) {
            const std::string title = state_.following ? "Notation \u00b7 written" : "Notation";
            if (notationTitle_->text() != title) notationTitle_->setText(title);
        }
        if (moved) root_.relayout();
    }
    state_.cc.clear();
    for (const json::Value& c : (*r)["cc"].asArray()) {
        if (c.find("number")->asInt() != state_.ccNumber || c.find("channel"))
            continue;  // channel 1
        state_.cc.push_back(
            {c.find("beat")->asDouble(), static_cast<int>(c.find("value")->asInt())});
    }
    collectEnvLanes(*owner);
    refreshEnvLanes();
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
    // the changes in it. A clip has no place in time: it reads its own
    // context (ADR-0098), in clip beats, as midi.edit does, so the shading,
    // the spelling and a degree drag's preview agree with what the engine
    // commits. Without one no key holds.
    state_.keys.clear();
    state_.chords.clear();
    state_.keyMarks.clear();
    state_.contextShift = 0;
    state_.contextEnd = 0;
    if (state_.clip) {
        // The roll's axis is the take's content; a looped clip's context is
        // read through its loop window, as midi.edit reads it (review round
        // 77, L1). The strip shows the first pass.
        const model::Clip& clip = *owner->clip;
        const ClipContextAxis axis = clipContextAxis(clip);
        for (const auto& [beat, key] : clipKeySpans(clip)) state_.keys.push_back({beat, key});
        state_.contextShift = axis.from;
        state_.contextEnd = axis.from + axis.shownEnd();
        for (const model::ContextPoint& p : clip.context) {
            if (p.beat >= axis.shownEnd() - 1e-9) continue;  // a later pass
            const double at = p.beat + axis.from;
            if (p.type == model::ContextType::Key) {
                state_.keyMarks.push_back({at, model::keyWords(p), model::parseKey(p).has_value()});
                continue;
            }
            const bool known = p.symbol.empty() || model::parseChordPoint(p).has_value();
            state_.chords.push_back({at, p.symbol + (known ? "" : "?"), known});
        }
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
    // A take's own context (gui.md §4.7): the strip's second row, dim; in the
    // written view of a following take its first, the song's second, and the
    // shading, the spelling and the key button read the take's.
    state_.otherChords.clear();
    state_.rowLabel.clear();
    state_.otherLabel.clear();
    state_.chordsEnd.reset();
    state_.otherEnd.reset();
    if (!state_.clip && !take->context.empty()) {
        std::vector<RollState::ChordMark> takeChords;
        std::vector<RollState::KeySpan> takeKeys;
        takeContextMarks(*take, state_.itemPosition, -64.0, state_.itemLength, takeChords,
                         takeKeys);
        if (state_.following && state_.written) {
            state_.otherChords = std::move(state_.chords);
            state_.chords = std::move(takeChords);
            state_.keys = std::move(takeKeys);
            state_.rowLabel = "take";
            state_.otherLabel = "song";
            state_.chordsEnd = state_.itemLength;
        } else {
            state_.otherChords = std::move(takeChords);
            state_.rowLabel = "song";
            state_.otherLabel = "take";
            state_.otherEnd = state_.itemLength;
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
        // A following take says how: "follows D major → E minor" (its own key
        // at the item's start, the song's there), or "written in D major".
        if (state_.following) {
            const model::ContextPoint* own = model::contextKeyPointAt(
                take->context, model::contextBeatOf(*take, take->startOffset));
            const std::string written = own ? model::keyWords(*own) : "no key";
            if (state_.written) {
                titleBase_ += " · written in " + written;
            } else {
                const auto songId = model::keyPointAt(app_.head(), state_.itemPosition);
                const model::ContextPoint* song =
                    songId ? app_.head().context.find(*songId) : nullptr;
                titleBase_ += " · follows " + written + " " + std::string(views::kFollowMark) +
                              " " + (song ? model::keyWords(*song) : std::string("no key"));
            }
        }
        // Named when one key holds over every such note: "3 outside A minor".
        // The notes as the view draws them (a following take's sounding ones).
        std::size_t outside = 0;
        std::optional<harmony::Key> which;
        bool one = true;
        for (std::size_t i = 0; i < state_.notes.size(); ++i) {
            RollNote n = state_.notes[i];
            n.pitch = state_.drawnPitch(i, n);
            if (!state_.outsideKey(n)) continue;
            const auto key = state_.keyAt(state_.itemPosition + n.start);
            one = one && (!which || which == key);
            which = key;
            ++outside;
        }
        if (outside > 0) {
            std::string scale(harmony::scaleName(which->scale));
            std::replace(scale.begin(), scale.end(), '_', ' ');
            titleBase_ += " · " + std::to_string(outside) + " outside " +
                          (one ? harmony::printPitchClass(which->tonic) + " " + scale
                               : std::string("the key"));
        }
        refreshTitle();
    }
    if (roll_) roll_->invalidate();
    if (bars_) bars_->invalidate();
    if (keys_) keys_->invalidate();
    if (velocity_) velocity_->invalidate();
    if (ccLane_) ccLane_->invalidate();
    return {};
}

// An audio clip (gui.md §4.7, mock 93): its active take's waveform in
// seconds of played time from the clip's start, the clip's end marked, the
// key button its one key point; no notes, chords, grid or notation.
Status MidiEditor::reloadAudio() {
    const model::Session& head = app_.head();
    const auto owner = midiOwner(head, state_.item);
    if (!owner || !owner->audio || !owner->clip) return Error("gui.midi", "no audio", "/item");
    state_.cancelGesture();
    state_.audio = true;
    state_.clip = true;
    state_.itemPosition = 0;
    state_.itemLength = owner->clip->length;  // seconds: an audio clip's unit
    state_.notes.clear();
    state_.selected.clear();
    state_.cc.clear();
    state_.barLines.clear();
    state_.keys.clear();
    state_.chords.clear();
    state_.keyMarks.clear();
    state_.contextShift = 0;
    state_.contextEnd = 0;
    state_.degrees = false;
    if (keyButton_) keyButton_->setOn(false);
    RollState::Wave wave;
    wave.take = *owner->take;
    wave.hash = owner->audio->hash;
    const std::filesystem::path path(owner->audio->path);
    wave.path = path.is_absolute() ? path : app_.mediaDir() / path;
    wave.gain =
        static_cast<float>(std::pow(10.0, (owner->clip->gainDb + owner->take->gainDb) / 20));
    wave.loop = owner->clip->loop.on;
    wave.label = owner->take->name;
    if (wave.label.empty()) {
        const auto& takes = owner->clip->takes;
        const auto at = std::find(takes.begin(), takes.end(), *owner->clip->activeTake);
        wave.label = "Take " + std::to_string(at - takes.begin() + 1);
    }
    state_.wave = std::move(wave);
    collectEnvLanes(*owner);  // in seconds, the clip's unit
    refreshEnvLanes();
    applyMode();
    refreshKeyButton();
    if (bars_ && bars_->layout.height.value != static_cast<BarStrip*>(bars_)->wanted()) {
        bars_->layout.height = ui::SizeRule::fixed(static_cast<BarStrip*>(bars_)->wanted());
        root_.relayout();
    }
    projectGeneration_ = app_.projectGeneration();
    version_ = head.version;
    if (title_) {
        const model::Track* t = head.tracks.find(owner->track);
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, "%.2f s", owner->clip->length);
        titleBase_ = "Audio \u00b7 " + (t && !t->name.empty() ? t->name : owner->track.str()) +
                     " \u2014 " + (owner->clip->name.empty() ? state_.item : owner->clip->name) +
                     " \u00b7 clip \u00b7 " + seconds;
        refreshTitle();
    }
    root_.damageAll();
    return {};
}

void MidiEditor::peaksReady() {
    if (state_.audio && roll_) roll_->invalidate();
}

void MidiEditor::refreshTitle() {
    if (!title_) return;
    if (state_.audio) {
        title_->setText(titleBase_ + (state_.snap ? "" : " \u00b7 snap off"));
        return;
    }
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
    if (!owner || !owner->source) return std::nullopt;
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

void MidiEditor::openPassMenu() {
    if (!passButton_ || state_.passes < 2) return;
    std::vector<ui::MenuItem> items;
    const auto& map = *app_.head().timeMap;
    for (int k = 0; k < state_.passes; ++k) {
        std::string text = "Pass " + std::to_string(k + 1);
        const auto i = static_cast<std::size_t>(k);
        if (i < passStarts_.size() && passStarts_[i] >= 0)
            text += " \u00b7 bar " + std::to_string(map.barBeat(passStarts_[i]).bar);
        items.push_back({std::move(text), "", [this, k] {
                             state_.pass = k;
                             (void)reload();
                         }});
    }
    root_.openPopup(ui::makeMenu("midi.pass.menu", items, [this] { root_.closePopup(); }),
                    passButton_->bounds(), ui::Placement::Below);
}

const std::vector<std::string>& MidiEditor::otherChordLabels() const {
    static const std::vector<std::string> none;
    return bars_ ? static_cast<const BarStrip*>(bars_)->otherLabels_ : none;
}

void MidiEditor::refuse(const std::string& message) {
    if (actions_.onError) actions_.onError(message);
}

void MidiEditor::notice(const std::string& message) {
    if (actions_.onNotice) actions_.onNotice(message);
}

std::uint64_t MidiEditor::beginGesture() {
    follow({});
    gestureGeneration_ = app_.projectGeneration();
    return app_.head().version;
}

Status MidiEditor::edit(json::Value ops, std::optional<std::uint64_t> expectedVersion) {
    if (state_.audio)  // an audio clip has no notes; the note actions do nothing
        return Error("gui.midi.audio", "an audio clip has no notes", "/item",
                     "open a MIDI item or clip to edit notes");
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
    views::ChordFieldHost host{
        [this]() -> const model::Session& { return app_.head(); },
        [this](const std::string& command, json::Value args) {
            auto r = dispatcher_.execute({command, std::move(args), kOrigin});
            if (!r && actions_.onError) actions_.onError("Chord: " + r.error().message);
            return static_cast<bool>(r);
        },
        [this](double b) { return bars_ ? static_cast<BarStrip*>(bars_)->chordX(b) : 0.0f; }};
    if (state_.clip) {
        // The clip's own chord at that clip beat: chord.set {at, symbol}
        // becomes a point of the clip's context.
        const std::string item = state_.item;
        // The field works in roll beats; the clip's points are in clip beats.
        const double shift = state_.contextShift;
        host.symbolAt = [this, item, shift](double b) {
            const auto owner = midiOwner(app_.head(), item);
            if (!owner || !owner->clip) return std::string();
            for (const model::ContextPoint& p : owner->clip->context)
                if (p.type == model::ContextType::Chord && std::abs(p.beat + shift - b) < 1e-6)
                    return p.symbol;
            return std::string();
        };
        host.run = [this, item](const std::string& command, json::Value args) {
            const auto owner = midiOwner(app_.head(), item);
            if (command != "chord.set" || !owner || !owner->clip) return false;
            const auto clipAt = clipBeatAt(args["at"].asDouble());
            if (!clipAt) return false;
            model::ContextPoint p;
            p.type = model::ContextType::Chord;
            p.beat = *clipAt;
            p.symbol = args["symbol"].asString();
            return static_cast<bool>(setClipContext(withContextPoint(owner->clip->context, p)));
        };
    }
    views::openChordField(root_, std::move(host), beat, at);
}

void MidiEditor::editClipKey(double beat, ui::Point at) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip) return;
    // It starts from the clip's key there, else the project's at its start,
    // else C major.
    std::string text = "C major";
    if (const model::ContextPoint* p = model::keyPointAt(*owner->clip, beat))
        text = model::keyWords(*p);
    else if (const auto id = model::keyPointAt(app_.head(), 0.0))
        if (const model::ContextPoint* project = app_.head().context.find(*id))
            text = model::keyWords(*project);
    auto field = std::make_unique<ui::TextField>("midi-key-field", text);
    field->layout.width = ui::SizeRule::fixed(200);
    field->selectAll();
    const std::string item = state_.item;
    field->onSubmit = [this, item, beat](const std::string& typed) {
        const auto parts = views::splitKeyText(typed);
        if (!parts) {
            if (actions_.onError)
                actions_.onError("Key: '" + typed +
                                 "' is not a key: a tonic and a scale, "
                                 "Eb major or F# dorian");
            return;
        }
        const auto clip = midiOwner(app_.head(), item);
        if (!clip || !clip->clip) return;
        model::ContextPoint p;
        p.type = model::ContextType::Key;
        p.beat = beat;
        p.tonic = parts->first;
        p.scale = parts->second;
        if (setClipContext(withContextPoint(clip->clip->context, p))) root_.closePopup();
    };
    root_.openPopup(std::move(field), {at.x, at.y, 1, 1});
}

void MidiEditor::openContextMenu(double rollBeat, std::optional<double> rollChord, ui::Point at) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip) return;
    const model::Clip& clip = *owner->clip;
    const time::TimeMap& tm = *app_.head().timeMap;
    // The menu's beats are the clip's; the strip's are the roll's.
    const double beat = rollBeat - state_.contextShift;
    const std::optional<double> chord =
        rollChord ? std::optional<double>(*rollChord - state_.contextShift) : std::nullopt;
    // A key goes at the start of the bar under the pointer, inside the part
    // of the clip the strip shows.
    double barStart = tm.beatOfBar(tm.barBeat(std::max(0.0, beat)).bar);
    if (barStart >= clipContextAxis(clip).shownEnd()) barStart = 0;
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, bool enabled, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.enabled = enabled;
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    const int bar = tm.barBeat(barStart).bar;
    add(barStart == 0 ? "Key\u2026" : "Key at bar " + std::to_string(bar) + "\u2026", true,
        [this, barStart, at] { editClipKey(barStart, at); });
    add("Chord\u2026", true, [this, rollBeat, at] { editChord(rollBeat, at); });
    const model::ContextPoint* key = nullptr;
    for (const model::ContextPoint& p : clip.context)
        if (p.type == model::ContextType::Key && std::abs(p.beat - barStart) < 1e-6) key = &p;
    if (key) {
        const model::ContextPoint gone = *key;
        add("Remove key " + model::keyWords(gone), true, [this, gone] {
            if (const auto o = midiOwner(app_.head(), state_.item); o && o->clip)
                (void)setClipContext(withContextPoint(o->clip->context, gone, true));
        });
    }
    if (chord) {
        model::ContextPoint gone;
        gone.type = model::ContextType::Chord;
        gone.beat = *chord;
        for (const model::ContextPoint& p : clip.context)
            if (p.type == model::ContextType::Chord && std::abs(p.beat - *chord) < 1e-6)
                gone.symbol = p.symbol;
        add("Remove chord" + (gone.symbol.empty() ? std::string() : " " + gone.symbol), true,
            [this, gone] {
                if (const auto o = midiOwner(app_.head(), state_.item); o && o->clip)
                    (void)setClipContext(withContextPoint(o->clip->context, gone, true));
            });
    }
    ui::MenuItem rule;
    rule.separator = true;
    items.push_back(rule);
    // The project's key and chords from the playhead, for the clip's length.
    const auto t = app_.transport("get");
    const double from = t ? t->beat : 0.0;
    add("Take from the project at bar " + std::to_string(tm.barBeat(from).bar), true, [this, from] {
        json::Value args = json::Value::object(), place = json::Value::object();
        args["clip"] = state_.item;
        place["at"] = std::max(0.0, from);
        args["context_at"] = std::move(place);
        (void)command("clip.set", std::move(args), {}, "Context");
    });
    add("Clear context", !clip.context.empty(), [this] { (void)setClipContext({}); });
    root_.openPopup(
        ui::makeMenu("midi.context.menu", std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

void MidiEditor::openAudioKeyMenu() {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip || !keyButton_) return;
    // An audio clip holds at most one point, a key at 0 (ADR-0098): set it,
    // remove it, or take the project's at the playhead.
    const ui::Rect b = keyButton_->bounds();
    const ui::Point at{b.x, b.bottom()};
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    add("Key\u2026", [this, at] { editClipKey(0.0, at); });
    if (const model::ContextPoint* key = model::keyPointAt(*owner->clip, 0.0)) {
        const model::ContextPoint gone = *key;
        add("Remove key " + model::keyWords(gone), [this, gone] {
            if (const auto o = midiOwner(app_.head(), state_.item); o && o->clip)
                (void)setClipContext(withContextPoint(o->clip->context, gone, true));
        });
    }
    const auto t = app_.transport("get");
    const double from = t ? t->beat : 0.0;
    const time::TimeMap& tm = *app_.head().timeMap;
    add("Take from the project at bar " + std::to_string(tm.barBeat(std::max(0.0, from)).bar),
        [this, from] {
            json::Value args = json::Value::object(), place = json::Value::object();
            args["clip"] = state_.item;
            place["at"] = std::max(0.0, from);
            args["context_at"] = std::move(place);
            (void)command("clip.set", std::move(args), {}, "Context");
        });
    root_.openPopup(ui::makeMenu("midi.key.menu", std::move(items), [this] { root_.closePopup(); }),
                    ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

void MidiEditor::openEnvLaneMenu(model::Id env, ui::Point at, double pos, double value) {
    const RollEnvLane* lane = nullptr;
    for (const RollEnvLane& l : state_.envLanes)
        if (l.env == env) lane = &l;
    if (!lane) return;
    const bool picked = state_.envSelLane == env && !state_.envSel.empty();
    const std::vector<model::EnvPoint> points = lane->envelope.points;
    const model::EnvTarget target = lane->envelope.target;
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, std::function<void()> run, bool enabled = true,
                   std::string shortcut = {}) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.run = std::move(run);
        m.enabled = enabled;
        m.shortcut = std::move(shortcut);
        items.push_back(std::move(m));
    };
    auto rule = [&] {
        ui::MenuItem m;
        m.separator = true;
        items.push_back(std::move(m));
    };
    add("Insert point", [this, env, points, pos, value] {
        model::EnvPoint pt;
        pt.pos = pos;
        pt.value = value;
        std::vector<model::EnvPoint> next = points;
        next.insert(
            std::upper_bound(next.begin(), next.end(), pos,
                             [](double beat, const model::EnvPoint& p) { return beat < p.pos; }),
            pt);
        state_.envSelLane = env;
        state_.envSel = {{pos, value}};
        (void)setEnvPoints(env, next);
    });
    rule();
    add(
        "Select all points",
        [this, env, points] {
            state_.envSelLane = env;
            state_.envSel.clear();
            for (const model::EnvPoint& p : points) state_.envSel.push_back({p.pos, p.value});
            refreshEnvLanes();
        },
        !points.empty());
    add(
        "Copy points", [this] { copyEnvSelection(); }, picked,
        actions_.keysFor("ui.midi.notes.copy"));
    add(
        "Cut points",
        [this] {
            copyEnvSelection();
            deleteEnvSelection();
        },
        picked, actions_.keysFor("ui.midi.notes.cut"));
    add(
        "Paste points", [this, env, pos] { (void)pasteEnvPoints(env, pos); }, canPasteEnv(),
        actions_.keysFor("ui.midi.notes.paste"));
    add(
        "Delete points", [this] { deleteEnvSelection(); }, picked,
        actions_.keysFor("ui.midi.notes.delete"));
    add(
        "Point shape\u2026",
        [this, at] {
            std::vector<ui::MenuItem> shapes;
            const RollEnvLane* l = nullptr;
            for (const RollEnvLane& x : state_.envLanes)
                if (x.env == state_.envSelLane) l = &x;
            for (auto [label, shape] : {std::pair{"Linear", model::PointShape::Linear},
                                        {"Square", model::PointShape::Square},
                                        {"Slow start/end", model::PointShape::Slow},
                                        {"Fast start", model::PointShape::FastStart},
                                        {"Fast end", model::PointShape::FastEnd},
                                        {"Bezier", model::PointShape::Bezier}}) {
                ui::MenuItem m;
                m.label = label;
                bool all = l != nullptr, any = false;
                if (l)
                    for (const model::EnvPoint& p : l->envelope.points)
                        if (state_.envSelected(l->env, p)) {
                            any = true;
                            all = all && p.shape == shape;
                        }
                m.checked = any && all;
                const model::PointShape s = shape;
                m.run = [this, s] { shapeEnvSelection(s); };
                shapes.push_back(std::move(m));
            }
            // Opened from the lane's menu as it closes.
            root_.openPopup(ui::makeMenu("midi.env.shape.menu", std::move(shapes),
                                         [this] { root_.closePopup(); }),
                            {at.x, at.y, 1, 1});
        },
        picked);
    rule();
    add("Hide", [this, target] { (void)showEnvelope(target, false); });
    root_.openPopup(ui::makeMenu("midi.env.menu", std::move(items), [this] { root_.closePopup(); }),
                    ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

void MidiEditor::repaintLanes() {
    if (envColumn_)
        for (const auto& lane : envColumn_->children()) lane->invalidate();
}

void MidiEditor::copyEnvSelection() {
    for (const RollEnvLane& l : state_.envLanes) {
        if (l.env != state_.envSelLane) continue;
        std::vector<model::EnvPoint> picked;
        for (const model::EnvPoint& p : l.envelope.points)
            if (state_.envSelected(l.env, p)) picked.push_back(p);
        if (picked.empty()) return;
        envClip_ = views::copyPoints(l.envelope.points, picked, l.scale,
                                     views::envUnit(l.envelope, l.scale));
        envClipAt_ = picked.front().pos;
        return;
    }
}

bool MidiEditor::pasteEnvPoints(model::Id env, std::optional<double> pos) {
    if (envClip_.empty()) return false;
    for (const RollEnvLane& l : state_.envLanes) {
        if (l.env != env) continue;
        const views::EnvEdit edit =
            views::pastePoints(l.envelope.points, envClip_, pos.value_or(envClipAt_), l.scale,
                               views::envUnit(l.envelope, l.scale));
        state_.envSelLane = env;
        state_.envSel.clear();
        for (const model::EnvPoint& p : edit.selected) state_.envSel.push_back({p.pos, p.value});
        (void)setEnvPoints(env, edit.points);
        return true;
    }
    return false;
}

void MidiEditor::deleteEnvSelection() {
    for (const RollEnvLane& l : state_.envLanes) {
        if (l.env != state_.envSelLane) continue;
        std::vector<model::EnvPoint> next;
        for (const model::EnvPoint& p : l.envelope.points)
            if (!state_.envSelected(l.env, p)) next.push_back(p);
        state_.envSel.clear();
        if (next.size() != l.envelope.points.size()) (void)setEnvPoints(l.env, next);
        return;
    }
}

void MidiEditor::shapeEnvSelection(model::PointShape shape) {
    for (const RollEnvLane& l : state_.envLanes) {
        if (l.env != state_.envSelLane) continue;
        std::vector<model::EnvPoint> next = l.envelope.points;
        bool changed = false;
        for (model::EnvPoint& p : next)
            if (state_.envSelected(l.env, p) && p.shape != shape) {
                p.shape = shape;
                changed = true;
            }
        if (changed) (void)setEnvPoints(l.env, next);
        return;
    }
}

std::optional<double> MidiEditor::clipBeatAt(double rollBeat) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip) return std::nullopt;
    const ClipContextAxis axis = clipContextAxis(*owner->clip);
    const double beat = rollBeat - axis.from;
    if (axis.looping && (beat < -1e-9 || beat >= axis.shownEnd() - 1e-9)) {
        if (actions_.onError)
            actions_.onError(
                "Context: a looped clip's key and chords go inside its loop, "
                "the part of the roll they are shown over");
        return std::nullopt;
    }
    return std::max(0.0, beat);
}

void MidiEditor::moveClipPoint(model::ContextType type, double rollFrom, double rollTo, bool copy) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip) return;
    const double from = rollFrom - state_.contextShift;
    const auto landing = clipBeatAt(rollTo);
    if (!landing) return;
    const double to = *landing;
    std::optional<model::ContextPoint> held;
    for (const model::ContextPoint& p : owner->clip->context)
        if (p.type == type && std::abs(p.beat - from) < 1e-6) held = p;
    if (!held) return;
    std::vector<model::ContextPoint> points =
        copy ? owner->clip->context : withContextPoint(owner->clip->context, *held, true);
    model::ContextPoint moved = *held;
    moved.beat = to;
    (void)setClipContext(withContextPoint(std::move(points), moved));
}

Status MidiEditor::setClipContext(const std::vector<model::ContextPoint>& points) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->clip) return Error("gui.midi", "no clip is open", "/clip");
    json::Value args = json::Value::object();
    args["clip"] = state_.item;
    // An audio clip's length is in seconds: its text is bar 1 only, a key
    // and holds (ADR-0098).
    args["context"] =
        clipLeadSheet(points, *app_.head().timeMap, state_.audio ? 0.0 : owner->clip->length);
    return command("clip.set", std::move(args), {}, "Context");
}

std::optional<double> MidiEditor::takeBeatAt(double projectBeat) const {
    const auto owner = midiOwner(app_.head(), state_.item);
    const auto id = owner && owner->item ? model::Id::parse(state_.item) : std::nullopt;
    if (!id || !owner->take) return std::nullopt;
    const auto place = model::placeAt(app_.head(), *id, projectBeat);
    if (!place) return std::nullopt;
    double beat = std::max(0.0, std::round(place->contextBeat * 960.0) / 960.0);
    if (owner->take->contextPeriod && beat >= *owner->take->contextPeriod - 1e-9) beat = 0;
    return beat;
}

Status MidiEditor::setTakeContext(const std::vector<model::ContextPoint>& points) {
    const model::Session& head = app_.head();
    const auto owner = midiOwner(head, state_.item);
    if (!owner || !owner->item || !owner->take || !owner->item->activeTake)
        return Error("gui.midi", "no item take is open", "/take");
    json::Value args = json::Value::object();
    args["take"] = owner->item->activeTake->str();
    args["context"] = takeLeadSheet(head, *owner->item, *owner->take, points);
    return command("take.set", std::move(args), {}, "Context");
}

void MidiEditor::editTakeChord(double projectBeat, ui::Point at) {
    views::ChordFieldHost host{
        [this]() -> const model::Session& { return app_.head(); },
        [](const std::string&, json::Value) { return false; },
        [this](double b) { return bars_ ? static_cast<BarStrip*>(bars_)->chordX(b) : 0.0f; }};
    // The take's chord holding at that place: the field starts from it.
    host.symbolAt = [this](double b) {
        const auto owner = midiOwner(app_.head(), state_.item);
        const auto beat = takeBeatAt(b);
        if (!owner || !owner->take || !beat) return std::string();
        const model::ContextPoint* p = model::contextChordPointAt(owner->take->context, *beat);
        return p ? p->symbol : std::string();
    };
    host.run = [this](const std::string& command, json::Value args) {
        const auto owner = midiOwner(app_.head(), state_.item);
        if (command != "chord.set" || !owner || !owner->take) return false;
        const auto beat = takeBeatAt(args["at"].asDouble());
        if (!beat) return false;
        // The chord the field was opened on is replaced where it stands.
        model::ContextPoint p;
        p.type = model::ContextType::Chord;
        p.beat = *beat;
        if (const model::ContextPoint* held =
                model::contextChordPointAt(owner->take->context, *beat);
            held && std::abs(held->beat - *beat) < 1.0 / 960.0)
            p.beat = held->beat;
        p.symbol = args["symbol"].asString();
        return static_cast<bool>(setTakeContext(withContextPoint(owner->take->context, p)));
    };
    views::openChordField(root_, std::move(host), projectBeat, at);
}

void MidiEditor::editTakeKey(double projectBeat, ui::Point at) {
    const model::Session& head = app_.head();
    const auto owner = midiOwner(head, state_.item);
    const auto beat = takeBeatAt(projectBeat);
    if (!owner || !owner->take || !beat) return;
    // It starts from the take's key there, else the song's, else C major.
    std::string text = "C major";
    if (const model::ContextPoint* p = model::contextKeyPointAt(owner->take->context, *beat))
        text = model::keyWords(*p);
    else if (const auto id = model::keyPointAt(head, projectBeat))
        if (const model::ContextPoint* song = head.context.find(*id)) text = model::keyWords(*song);
    auto field = std::make_unique<ui::TextField>("midi-key-field", text);
    field->layout.width = ui::SizeRule::fixed(200);
    field->selectAll();
    const double contextBeat = *beat;
    field->onSubmit = [this, contextBeat](const std::string& typed) {
        const auto parts = views::splitKeyText(typed);
        if (!parts) {
            if (actions_.onError)
                actions_.onError("Key: '" + typed +
                                 "' is not a key: a tonic and a scale, "
                                 "Eb major or F# dorian");
            return;
        }
        const auto o = midiOwner(app_.head(), state_.item);
        if (!o || !o->take) return;
        model::ContextPoint p;
        p.type = model::ContextType::Key;
        p.beat = contextBeat;
        p.tonic = parts->first;
        p.scale = parts->second;
        if (setTakeContext(withContextPoint(o->take->context, p))) root_.closePopup();
    };
    root_.openPopup(std::move(field), {at.x, at.y, 1, 1});
}

void MidiEditor::openTakeContextMenu(double projectBeat, std::optional<double> chord,
                                     ui::Point at) {
    const model::Session& head = app_.head();
    const auto owner = midiOwner(head, state_.item);
    if (!owner || !owner->item || !owner->take || !model::takeIsMidi(*owner->take)) return;
    const model::Take& take = *owner->take;
    const time::TimeMap& tm = *head.timeMap;
    // A key goes at the start of the bar under the pointer, or at the item's
    // start when that bar began before it.
    const double barStart =
        std::max(state_.itemPosition, tm.beatOfBar(tm.barBeat(std::max(0.0, projectBeat)).bar));
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, bool enabled, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.enabled = enabled;
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    const bool atStart = barStart <= state_.itemPosition + 1e-9;
    add(atStart ? "Key\u2026" : "Key at bar " + std::to_string(tm.barBeat(barStart).bar) + "\u2026",
        true, [this, barStart, at] { editTakeKey(barStart, at); });
    add("Chord\u2026", true,
        [this, at, beat = chord.value_or(projectBeat)] { editTakeChord(beat, at); });
    const auto keyBeat = takeBeatAt(barStart);
    for (const model::ContextPoint& p : take.context)
        if (keyBeat && p.type == model::ContextType::Key &&
            std::abs(p.beat - *keyBeat) < 1.0 / 960.0) {
            const model::ContextPoint gone = p;
            add("Remove key " + model::keyWords(gone), true, [this, gone] {
                if (const auto o = midiOwner(app_.head(), state_.item); o && o->take)
                    (void)setTakeContext(withContextPoint(o->take->context, gone, true));
            });
        }
    if (chord)
        if (const auto beat = takeBeatAt(*chord))
            if (const model::ContextPoint* p = model::contextChordPointAt(take.context, *beat)) {
                const model::ContextPoint gone = *p;
                add("Remove chord" + (gone.symbol.empty() ? std::string() : " " + gone.symbol),
                    true, [this, gone] {
                        if (const auto o = midiOwner(app_.head(), state_.item); o && o->take)
                            (void)setTakeContext(withContextPoint(o->take->context, gone, true));
                    });
            }
    ui::MenuItem rule;
    rule.separator = true;
    items.push_back(rule);
    // The song's key and chords under the item: what a part written on the
    // timeline was written against.
    model::Id itemId = *model::Id::parse(state_.item);
    add("From the song's key and chords", true,
        [this, itemId] { (void)setTakeContext(songContextUnder(app_.head(), itemId)); });
    add("Clear the take's context", !take.context.empty(), [this] { (void)setTakeContext({}); });
    root_.openPopup(
        ui::makeMenu("midi.take.context.menu", std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

void MidiEditor::moveTakePoint(model::ContextType type, double fromProject, double toProject,
                               bool copy) {
    const auto owner = midiOwner(app_.head(), state_.item);
    if (!owner || !owner->take) return;
    const auto from = takeBeatAt(fromProject);
    const auto to = takeBeatAt(std::max(state_.itemPosition, toProject));
    if (!from || !to) return;
    const std::vector<model::ContextPoint>& context = owner->take->context;
    const model::ContextPoint* held = type == model::ContextType::Chord
                                          ? model::contextChordPointAt(context, *from)
                                          : model::contextKeyPointAt(context, *from);
    if (!held) return;
    std::vector<model::ContextPoint> points =
        copy ? context : withContextPoint(context, *held, true);
    model::ContextPoint moved = *held;
    moved.beat = *to;
    (void)setTakeContext(withContextPoint(std::move(points), moved));
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
    // An item's key at its start; a clip's own at its start (ADR-0098); a
    // following take's own in the written view (gui.md §4.7).
    const model::ContextPoint* point = nullptr;
    const auto owner =
        state_.clip || state_.following ? midiOwner(head, state_.item) : std::optional<MidiOwner>{};
    if (owner && owner->clip) {
        point = model::keyPointAt(*owner->clip, 0.0);
    } else if (owner && owner->take && state_.following && state_.written) {
        point = model::contextKeyPointAt(
            owner->take->context, model::contextBeatOf(*owner->take, owner->take->startOffset));
    } else if (const auto id = model::keyPointAt(head, state_.itemPosition)) {
        point = head.context.find(*id);
    }
    const bool known = point && model::parseKey(*point);
    std::string text = point ? model::keyWords(*point) : "No key";
    // An audio clip's button sets its key (it has no degrees), with or
    // without one. On a following take degrees count in the take's own key
    // (midi.edit reads it, X4d); the sounding view picks the step that
    // sounds nearest the pointer.
    keyButton_->enabled = known || state_.audio;
    if (!known && state_.degrees) {  // nothing to count degrees in
        state_.degrees = false;
        keyButton_->setOn(false);
        // midi.edit refuses degrees on a clip without a key (midi.no_key).
        if (state_.clip && actions_.onError)
            actions_.onError(
                "MIDI: degree mode is off: the clip has no key; give it a "
                "context with a key at its start");
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
    if (!state_.envSel.empty()) {  // a clip-time lane was clicked last
        deleteEnvSelection();
        return;
    }
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
    if (!isOpen()) return;
    if (!state_.envSel.empty()) {  // a clip-time lane's points were picked last
        copyEnvSelection();
        return;
    }
    if (state_.selected.empty()) return;
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
    if (!isOpen()) return;
    // After a press on a clip-time lane its copied points paste there, at the
    // clip time they were copied from.
    if (!itemBeat && state_.envLastLane && pasteEnvPoints(*state_.envLastLane)) return;
    if (clipboard_.empty()) return;
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
