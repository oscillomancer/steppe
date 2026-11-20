// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/arrange.h"

#include "supple/views/chord_field.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/session_changes.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "supple/model/context.h"
#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"

namespace supple::views {
namespace {

constexpr float kTcpWidth = 220;
constexpr float kRangeEdge = 5;  // px beside a loop or punch range's end that drag it

constexpr float kMinBeatLabelPx = 62;
// The breathing space every pair of ruler labels keeps (design units).
constexpr float kLabelGap = 12;

// A stored position in its timebase as seconds (Timebase::Time is already
// seconds).
double atSeconds(const time::TimeMap& tm, double pos, model::Timebase tb) {
    return tb == model::Timebase::Time ? pos : tm.secondsAt(pos);
}

// m:ss (decimals 0) or m:ss.s (decimals 1): the format coarsens with the
// step, so a label always states exactly where its tick is.
std::string formatTime(double seconds, int decimals) {
    if (seconds < 0) seconds = 0;
    const auto totalMs = std::llround(seconds * 1000.0);
    const int minutes = static_cast<int>(totalMs / 60000);
    const int rest = static_cast<int>(totalMs % 60000);
    char buf[32];
    if (decimals <= 0)
        std::snprintf(buf, sizeof buf, "%d:%02d", minutes, rest / 1000);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d.%d", minutes, rest / 1000, (rest % 1000) / 100);
    return buf;
}

std::string formatBpm(double bpm) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.4g", bpm);
    return buf;
}

std::string meterText(const time::MeterChange& m) {
    return std::to_string(m.numerator) + "/" + std::to_string(m.denominator);
}

// "Eb major", "C# harmonic minor": the ruler's key field as key.set's tonic
// and scale (the scale's spaces become underscores). None without both.
std::optional<std::pair<std::string, std::string>> splitKeyText(std::string_view text) {
    auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
            v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
        return v;
    };
    text = trim(text);
    const auto space = text.find_first_of(" \t");
    if (space == std::string_view::npos) return std::nullopt;
    const std::string_view rest = trim(text.substr(space + 1));
    if (rest.empty()) return std::nullopt;
    std::string scale;
    for (char ch : rest) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!scale.empty() && scale.back() != '_') scale += '_';
        } else {
            scale += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return std::pair{std::string(text.substr(0, space)), scale};
}

ui::Color parseColor(const std::string& text, ui::Color fallback) {
    return ui::parseColor(text).value_or(fallback);
}

// The finest nice time interval whose labels clear the previous one by the
// label's own width plus the gap at `pps`.
double timeStepFor(const ui::Canvas& c, double pps, float fontSize) {
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
    for (double s : steps) {
        const float w = c.textWidth(formatTime(0, s >= 1.0 ? 0 : 1), ui::Font::Mono, fontSize);
        if (s * pps >= static_cast<double>(w + kLabelGap)) return s;
    }
    return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

}  // namespace

// --- the ruler -------------------------------------------------------------------

class Ruler : public ui::Node {
  public:
    explicit Ruler(ArrangeView& view) : Node("ruler", ui::Role::Custom), view_(view) {
        interactive = true;
        focusable = true;
    }
    RulerCallbacks callbacks;
    void addMarker() {
        json::Value args = json::Value::object();
        args["pos"] = view_.session().timeMap->beatAt(view_.playhead());
        execute("marker.add", std::move(args));
    }
    void addRegion() {
        if (view_.loopEnd() <= view_.loopStart()) return;
        json::Value args = json::Value::object();
        args["start"] = view_.session().timeMap->beatAt(view_.loopStart());
        args["end"] = view_.session().timeMap->beatAt(view_.loopEnd());
        execute("region.add", std::move(args));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        const bool handled = handle(e);
        view_.noteDrag(e, true);  // a drag held at the lanes' edge scrolls in time
        return handled;
    }
    bool handle(const ui::PointerEvent& e);
    bool dragging() const { return drag_ != Drag::None; }
    void scrolled(float dx) { press_.x -= dx; }  // the press goes with the content
    // The ends of the loop range (bars row) and the punch range (time row)
    // are dragged; so is a range, a region or a marker once it moves.
    ui::Cursor cursorAt(ui::Point p) const override {
        switch (drag_) {
            case Drag::LoopLeft:
            case Drag::LoopRight:
            case Drag::RegionLeft:
            case Drag::RegionRight:
                return ui::Cursor::ResizeHorizontal;
            case Drag::LoopMove:
            case Drag::RegionMove:
            case Drag::Marker:
            case Drag::Chord:
                return moved_ ? ui::Cursor::Move : ui::Cursor::Arrow;
            case Drag::LoopNew:
                return ui::Cursor::Arrow;
            case Drag::None:
                break;
        }
        const RulerRows& rows = view_.rulerRows();
        const float y = p.y - bounds().y;
        if (y < 0 || y >= rows.tempoTop()) return ui::Cursor::Arrow;
        const bool punch = y >= rows.beat;
        const double start = punch ? view_.punchStart() : view_.loopStart();
        const double end = punch ? view_.punchEnd() : view_.loopEnd();
        if (end <= start) return ui::Cursor::Arrow;
        return std::abs(p.x - view_.xAt(start)) <= kRangeEdge ||
                       std::abs(p.x - view_.xAt(end)) <= kRangeEdge
                   ? ui::Cursor::ResizeHorizontal
                   : ui::Cursor::Arrow;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key != SDLK_ESCAPE || drag_ == Drag::None) return false;
        drag_ = Drag::None;
        if (root()) root()->capture(nullptr);
        invalidate();
        return true;
    }
    RulerStats stats;
    void paint(ui::Canvas& c) override {
        stats = {};
        const ui::Theme& t = c.theme();
        const model::Session& s = view_.session();
        const time::TimeMap& tm = *s.timeMap;
        const double pps = view_.viewport().pixelsPerSecond;
        const float x0 = bounds().x, x1 = bounds().right();
        const RulerRows& rows = view_.rulerRows();
        const ui::Rect beatRow{x0, bounds().y, bounds().w, rows.beat};
        const ui::Rect timeRow{x0, bounds().y + rows.beat, bounds().w, rows.time};
        const ui::Rect tempoRow{x0, timeRow.bottom(), bounds().w, rows.tempo};
        const ui::Rect regionRow{x0, tempoRow.bottom(), bounds().w, rows.region};
        const ui::Rect chordRow{x0, regionRow.bottom(), bounds().w, rows.chord};
        const ui::Rect markRow{x0, chordRow.bottom(), bounds().w,
                               std::max(0.0f, bounds().bottom() - chordRow.bottom() - 1)};
        c.fillRect(bounds(), t.color("panel"));
        c.fillRect({x0, bounds().bottom() - 1, bounds().w, 1}, t.color("rule"));
        for (const ui::Rect& r : {beatRow, timeRow, tempoRow})
            c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.6f));
        // Labels near the right edge run past it: keep them out of the panel beside.
        c.save();
        c.clip(bounds());

        const ui::Color ink = t.color("ink");
        const ui::Color quiet = t.color("ink-quiet");
        const ui::Color faint = t.color("ink-faint");
        const double t0 = view_.timeAt(x0), t1 = view_.timeAt(x1);

        // The loop range shades the whole ruler, with a bar along the top.
        const bool loopDrag = isLoop();
        const bool loopGhost = loopDrag && !punch_ && moved_;
        const double loopStart = loopGhost ? tm.secondsAt(draftStart_) : view_.loopStart();
        const double loopEnd = loopGhost ? tm.secondsAt(draftEnd_) : view_.loopEnd();
        if (loopEnd > loopStart) {
            const float lx0 = std::max(x0, view_.xAt(loopStart));
            const float lx1 = std::min(x1, view_.xAt(loopEnd));
            if (lx1 > lx0) {
                c.fillRect({lx0, bounds().y, lx1 - lx0, bounds().h},
                           t.color("accent").withAlpha(view_.loopEnabled() ? 0.12f : 0.06f));
                c.fillRect({lx0, bounds().y, lx1 - lx0, 3}, t.color("accent"));
            }
        }

        // The punch range: a record-coloured band in the time row, with a bar
        // along its foot (drag in the time row to set it).
        const bool punchDrag = loopDrag && punch_;
        const double punchStart =
            punchDrag && moved_ ? tm.secondsAt(draftStart_) : view_.punchStart();
        const double punchEnd = punchDrag && moved_ ? tm.secondsAt(draftEnd_) : view_.punchEnd();
        if (punchEnd > punchStart) {
            const float px0 = std::max(x0, view_.xAt(punchStart));
            const float px1 = std::min(x1, view_.xAt(punchEnd));
            if (px1 > px0) {
                const ui::Color rec = t.color("armed");
                c.fillRect({px0, timeRow.y, px1 - px0, timeRow.h - 1},
                           rec.withAlpha(view_.punchEnabled() ? 0.22f : 0.08f));
                c.fillRect({px0, timeRow.bottom() - 3, px1 - px0, 2},
                           rec.withAlpha(view_.punchEnabled() ? 1.0f : 0.45f));
            }
        }

        // Bars and beats. Bar and beat labels share one row, so they share one
        // "last label" tracker: a label is placed only when the previous one's
        // width plus the gap has passed (never two labels touching).
        const float captionSize = t.type("caption");
        const double pxPerBeat = 60.0 / tm.bpmAt(tm.beatAt(t0)) * pps;
        const int subdivisions = pxPerBeat >= 110 ? 4 : pxPerBeat >= 52 ? 2 : 1;
        const bool beatLabels = pxPerBeat >= static_cast<double>(kMinBeatLabelPx);
        float lastLabelX = -1e9f, lastLabelW = 0;
        // Which bars carry a label depends on the zoom alone: every bar, or
        // every 2nd, 4th, 8th… counted from bar 1, the first stride whose
        // spacing fits the widest label in view, sized for at least three
        // digits so bars 9 and 10 coming into view leave it. Choosing them by what fits
        // after the last label shown depended on the first bar on screen, so
        // the labels flickered as the view scrolled. The tracker below stays
        // as the guarantee that no two labels touch.
        const int firstBar = tm.barBeat(tm.beatAt(std::max(0.0, t0))).bar;
        int stride = 1;
        {
            float narrowest = 1e9f;
            int lastBar = std::max(1, firstBar);
            float prevX = view_.xAt(tm.secondsAt(tm.beatOfBar(lastBar)));
            for (int b = lastBar + 1; b < lastBar + 20000; ++b) {
                const float x = view_.xAt(tm.secondsAt(tm.beatOfBar(b)));
                if (prevX > x1 + 80) break;
                narrowest = std::min(narrowest, x - prevX);
                prevX = x;
                lastBar = b;
            }
            // Mono digits: the last bar's label is the widest.
            const std::string digits = std::to_string(std::max(lastBar, 100));
            const float widest =
                c.textWidth(std::string(digits.size(), '8') + ".1", ui::Font::Mono, captionSize);
            while (stride < (1 << 20) &&
                   static_cast<float>(stride) * narrowest < widest + kLabelGap)
                stride *= 2;
        }
        int bar = std::max(1, firstBar);  // the bar being drawn
        auto placeLabel = [&](float x, const std::string& text, ui::Color colour, bool isBar) {
            const float w = c.textWidth(text, ui::Font::Mono, captionSize);
            if (lastLabelX > -1e8f && x - lastLabelX < lastLabelW + kLabelGap) return;
            c.text({x + 3, beatRow.y, w + 4, beatRow.h}, text, ui::Font::Mono, captionSize, colour,
                   ui::kLeft | ui::kMiddle);
            if (lastLabelX > -1e8f) {
                const float gap = x - lastLabelX;
                stats.minBarLabelGap = std::min(stats.minBarLabelGap, gap);
                stats.minBarSlack = std::min(stats.minBarSlack, gap - lastLabelW - kLabelGap);
            }
            ++(isBar ? stats.barLabels : stats.beatLabels);
            if (isBar) stats.labelledBars.push_back(bar);
            lastLabelX = x;
            lastLabelW = w;
        };
        for (int guard = 0; guard < 20000; ++guard, ++bar) {
            const time::MeterChange meter = tm.meterAt(bar);
            const double barBeat = tm.beatOfBar(bar);
            const double beatQuarters = 4.0 / static_cast<double>(meter.denominator);
            const float barX = view_.xAt(tm.secondsAt(barBeat));
            if (barX > x1 + 80) break;
            if (barX >= x0 - 80 && (bar - 1) % stride == 0) {
                const time::BarBeat bb = tm.barBeat(barBeat);
                placeLabel(barX, std::to_string(bb.bar) + "." + std::to_string(bb.beat), ink, true);
            }
            for (int b = 0; b < meter.numerator; ++b) {
                const double beat = barBeat + b * beatQuarters;
                const float x = view_.xAt(tm.secondsAt(beat));
                if (x < x0 - 1) continue;
                if (x > x1) break;
                const float th = b == 0 ? 10.0f : 6.0f;
                c.fillRect({x, beatRow.bottom() - th, 1, th}, b == 0 ? ink : quiet);
                if (beatLabels && stride == 1 && b > 0) {
                    const time::BarBeat bb = tm.barBeat(beat);
                    placeLabel(x, std::to_string(bb.bar) + "." + std::to_string(bb.beat), quiet,
                               false);
                }
                for (int k = 1; k < subdivisions; ++k) {
                    const double sub = beat + beatQuarters * k / subdivisions;
                    const float sx = view_.xAt(tm.secondsAt(sub));
                    if (sx < x0 || sx > x1) continue;
                    c.fillRect({sx, beatRow.bottom() - 4, 1, 4}, faint.withAlpha(0.7f));
                    ++stats.subTicks;
                }
            }
        }

        // Time. The step coarsens as the zoom goes out and the format follows
        // it (m:ss from 1 s up, m:ss.s below), so no two labels can touch.
        const double step = timeStepFor(c, pps, captionSize);
        const int decimals = step >= 1.0 ? 0 : 1;
        float lastTimeX = -1e9f, lastTimeW = 0;
        for (double ts = std::floor(t0 / step) * step; ts <= t1; ts += step) {
            const float x = view_.xAt(ts);
            if (x < x0 - 1) continue;
            if (x > x1) break;
            const std::string label = formatTime(ts, decimals);
            const float w = c.textWidth(label, ui::Font::Mono, captionSize);
            if (lastTimeX > -1e8f && x - lastTimeX < lastTimeW + kLabelGap) continue;
            c.fillRect({x, timeRow.bottom() - 5, 1, 5}, quiet);
            c.text({x + 3, timeRow.y, w + 4, timeRow.h}, label, ui::Font::Mono, captionSize, quiet,
                   ui::kLeft | ui::kMiddle);
            ++stats.timeLabels;
            if (lastTimeX > -1e8f) {
                const float gap = x - lastTimeX;
                stats.minTimeLabelGap = std::min(stats.minTimeLabelGap, gap);
                stats.minTimeSlack = std::min(stats.minTimeSlack, gap - lastTimeW - kLabelGap);
            }
            lastTimeX = x;
            lastTimeW = w;
        }

        // Tempo points and meter changes as flags.
        for (const time::TempoPoint& p : tm.tempo()) {
            const float x = view_.xAt(tm.secondsAt(p.beat));
            if (x < x0 - 1 || x > x1) continue;
            c.fillRect({x, tempoRow.y + 1, 1, tempoRow.h / 2.0f}, t.color("buss"));
            c.text({x + 3, tempoRow.y, 54, tempoRow.h / 2.0f}, formatBpm(p.bpm), ui::Font::Mono,
                   t.type("caption"), t.color("buss"), ui::kLeft | ui::kMiddle);
        }
        for (const time::MeterChange& m : tm.meter()) {
            const float x = view_.xAt(tm.secondsAt(tm.beatOfBar(m.bar)));
            if (x < x0 - 1 || x > x1) continue;
            const float y = tempoRow.y + tempoRow.h / 2.0f;
            c.fillRect({x, y, 1, tempoRow.h / 2.0f - 1}, t.color("master"));
            c.text({x + 3, y, 54, tempoRow.h / 2.0f}, meterText(m), ui::Font::Mono,
                   t.type("caption"), t.color("master"), ui::kLeft | ui::kMiddle);
        }
        paintKeys(c, tempoRow);

        // Regions on their own row, markers below it.
        for (const auto& e : s.regions) {
            const model::Region& rg = *e.value;
            const bool ghost = isRegion() && moved_ && id_ == e.id;
            const double a = atSeconds(tm, ghost ? draftStart_ : rg.start, rg.timebase);
            const double b = atSeconds(tm, ghost ? draftEnd_ : rg.end, rg.timebase);
            const float rx0 = std::max(x0, view_.xAt(a)), rx1 = std::min(x1, view_.xAt(b));
            if (rx1 <= rx0) continue;
            const ui::Color col =
                parseColor(rg.color, t.color("selection")).withAlpha(ghost ? 0.6f : 1.0f);
            c.fillRect({rx0, regionRow.y, rx1 - rx0, regionRow.h}, col.withAlpha(0.35f));
            c.fillRect({rx0, regionRow.bottom() - 2, rx1 - rx0, 2}, col);
            if (rx1 - rx0 > 24)
                c.text({rx0 + 4, regionRow.y, rx1 - rx0 - 6, regionRow.h}, rg.name, ui::Font::Ui,
                       t.type("caption"), ink, ui::kLeft | ui::kMiddle);
        }
        if (rows.chord > 0) paintChords(c, chordRow);
        for (const auto& e : s.markers) {
            const model::Marker& mk = *e.value;
            const bool ghost = drag_ == Drag::Marker && moved_ && id_ == e.id;
            const float x = view_.xAt(atSeconds(tm, ghost ? draftStart_ : mk.pos, mk.timebase));
            if (x < x0 - 1 || x > x1) continue;
            const ui::Color col =
                parseColor(mk.color, t.color("accent")).withAlpha(ghost ? 0.6f : 1.0f);
            c.fillRect({x, markRow.y + 1, 1, markRow.h - 2}, col);
            c.fillRect({x, markRow.y + 1, 6, 6}, col);
            c.text({x + 8, markRow.y, 90, markRow.h}, mk.name, ui::Font::Ui, t.type("caption"),
                   quiet, ui::kLeft | ui::kMiddle);
        }
        c.restore();
    }

    // Key changes (ADR-0095) in the meter half of the tempo row, in quiet
    // ink: a key that starts on a meter change's beat follows its label;
    // any other has its own tick. Each label stops short of the next flag.
    struct KeyFlag {
        double beat = 0;
        std::string text;  // "Eb major", as written ("?") when this build cannot read it
        ui::Rect rect;     // what a press on it hits
    };
    std::vector<KeyFlag> keyFlags;  // the last paint's, left to right
    void paintKeys(ui::Canvas& c, const ui::Rect& tempoRow) {
        keyFlags.clear();
        const ui::Theme& t = c.theme();
        const model::Session& s = view_.session();
        const time::TimeMap& tm = *s.timeMap;
        const auto& keys = s.contextView->keys;
        const float y = tempoRow.y + tempoRow.h / 2.0f, h = tempoRow.h / 2.0f - 1;
        const float size = t.type("caption");
        const ui::Color quiet = t.color("ink-quiet");
        const float x1 = bounds().right();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const model::ContextPoint* p = s.context.find(keys[i].id);
            if (!p) continue;
            const float x = view_.xAt(tm.secondsAt(keys[i].beat));
            if (x > x1) break;
            float textX = x + 3, limit = x1;
            bool shared = false;
            for (const time::MeterChange& m : tm.meter()) {
                const float mx = view_.xAt(tm.secondsAt(tm.beatOfBar(m.bar)));
                if (std::abs(mx - x) < 1) {
                    textX += c.textWidth(meterText(m), ui::Font::Mono, size) + 6;
                    shared = true;
                } else if (mx > x) {
                    limit = std::min(limit, mx - 3);
                }
            }
            if (i + 1 < keys.size())
                limit = std::min(limit, view_.xAt(tm.secondsAt(keys[i + 1].beat)) - 3);
            std::string text = model::keyWords(*p);
            // Words too long for the room end in an ellipsis, or go.
            const std::string fit =
                c.elide(text, ui::Font::Ui, size, std::max(0.0f, limit - textX));
            const float shown = fit.empty() ? 0.0f : c.textWidth(fit, ui::Font::Ui, size);
            if (!shared) c.fillRect({x, y, 1, h}, quiet);
            if (!fit.empty())
                c.text({textX, y, shown + 4, h}, fit, ui::Font::Ui, size, quiet,
                       ui::kLeft | ui::kMiddle);
            stats.keyLabels.push_back(text);
            const float left = shared ? textX - 2 : x - 5;
            keyFlags.push_back({keys[i].beat,
                                std::move(text),
                                {left, y, textX + std::max(shown, 8.0f) - left, h}});
        }
    }
    const KeyFlag* keyFlagAt(ui::Point p) const {
        for (const KeyFlag& k : keyFlags)
            if (k.rect.contains(p)) return &k;
        return nullptr;
    }
    // The beat under `x`, on the grid.
    double gridBeat(float x) const {
        const auto& tm = *view_.session().timeMap;
        const double grid = view_.gestures().gridBeats();
        return std::round(tm.beatAt(std::max(0.0, view_.timeAt(x))) / grid) * grid;
    }
    // The key field: "Eb major" into key.set at `beat`.
    void keyField(double beat, const std::string& text, ui::Point at) {
        if (!root()) return;
        auto field = std::make_unique<ui::TextField>("ruler-key", text);
        field->layout.width = ui::SizeRule::fixed(200);
        field->selectAll();
        field->onSubmit = [this, beat](const std::string& typed) {
            const auto parts = splitKeyText(typed);
            if (!parts) {
                if (callbacks.onError)
                    callbacks.onError(Error("key.text", "'" + typed + "' is not a key", "",
                                            "a tonic and a scale: Eb major, F# dorian"));
                return;
            }
            json::Value args = json::Value::object();
            args["at"] = beat;
            args["tonic"] = parts->first;
            args["scale"] = parts->second;
            if (execute("key.set", std::move(args))) root()->closePopup();
        };
        root()->openPopup(std::move(field), {at.x, at.y, 1, 1});
    }
    // What a new key at `beat` starts from: the key holding there, else C major.
    std::string keyTextAt(double beat) const {
        const model::Session& s = view_.session();
        if (const auto id = model::keyPointAt(s, beat))
            if (const model::ContextPoint* p = s.context.find(*id)) return model::keyWords(*p);
        return "C major";
    }

    // The chord row (ADR-0095): each chord a cell from its beat to the next
    // chord point, named in letters as the notation writes it ("?" after a
    // symbol this build cannot read); a "no chord" point ends the cell before
    // it and draws a short tick. A chord being dragged is drawn where it would
    // land.
    struct ChordCell {
        double beat = 0;
        std::string symbol;  // "" for no chord
        ui::Rect label;      // what a press on it hits: its start and its name
    };
    std::vector<ChordCell> chordCells;  // the last paint's, left to right
    void paintChords(ui::Canvas& c, const ui::Rect& row) {
        chordCells.clear();
        const ui::Theme& t = c.theme();
        const model::Session& s = view_.session();
        const time::TimeMap& tm = *s.timeMap;
        struct Shown {
            double beat;
            const model::ContextPoint* point;
            bool ghost;
        };
        std::vector<Shown> shown;
        for (const auto& entry : s.contextView->chords)
            if (const model::ContextPoint* p = s.context.find(entry.id)) {
                const bool ghost = drag_ == Drag::Chord && moved_ && entry.beat == originalStart_;
                shown.push_back({ghost ? draftStart_ : entry.beat, p, ghost});
            }
        std::stable_sort(shown.begin(), shown.end(),
                         [](const Shown& a, const Shown& b) { return a.beat < b.beat; });
        const float size = t.type("caption");
        const float x0 = bounds().x, x1 = bounds().right();
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const float x = view_.xAt(tm.secondsAt(shown[i].beat));
            if (x > x1) break;
            const float end =
                i + 1 < shown.size() ? view_.xAt(tm.secondsAt(shown[i + 1].beat)) : x1 + 2;
            const model::ContextPoint& p = *shown[i].point;
            const float alpha = shown[i].ghost ? 0.6f : 1.0f;
            if (p.symbol.empty()) {  // no chord from here
                c.fillRect({x, row.y + 4, 1, row.h - 8}, t.color("ink-faint").withAlpha(alpha));
                chordCells.push_back({shown[i].beat, {}, {x - 4, row.y, 8, row.h}});
                continue;
            }
            const ui::Rect cell{x + 1, row.y + 2, std::max(0.0f, end - x - 2), row.h - 4};
            if (end >= x0) {
                c.fillRound(cell, 2, t.color("raised").withAlpha(alpha));
                c.strokeRound(cell, 2, t.color("rule").withAlpha(alpha));
            }
            const bool known = model::parseChordPoint(p).has_value();
            const std::string text = p.symbol + (known ? "" : "?");
            const float w = c.textWidth(text, ui::Font::Ui, size);
            const float room = std::max(0.0f, cell.w - 8);
            // A name too long for its cell ends in an ellipsis, or goes.
            const std::string fit = end >= x0 ? c.elide(text, ui::Font::Ui, size, room) : "";
            if (!fit.empty()) {
                c.text({cell.x + 4, row.y, room + 4, row.h}, fit, ui::Font::Ui, size,
                       t.color(known ? "ink" : "ink-quiet").withAlpha(alpha),
                       ui::kLeft | ui::kMiddle);
                stats.chordLabels.push_back(fit);
            }
            chordCells.push_back(
                {shown[i].beat,
                 p.symbol,
                 {x - 4, row.y, std::min(w + 12, std::max(8.0f, end - x)) + 4, row.h}});
        }
    }
    const ChordCell* chordAt(ui::Point p) const {
        for (const ChordCell& cell : chordCells)
            if (cell.label.contains(p)) return &cell;
        return nullptr;
    }
    // The chord field (views/chord_field.h) for the chord point at `beat`.
    void chordField(double beat, ui::Point at) {
        if (!root()) return;
        openChordField(
            *root(),
            {[this]() -> const model::Session& { return view_.session(); },
             [this](const std::string& command, json::Value args) {
                 return execute(command, std::move(args));
             },
             [this](double b) { return view_.xAt(view_.session().timeMap->secondsAt(b)); }},
            beat, at);
    }

  private:
    enum class Drag {
        None,
        LoopNew,
        LoopMove,
        LoopLeft,
        LoopRight,
        Marker,
        RegionMove,
        RegionLeft,
        RegionRight,
        Chord
    };
    bool isLoop() const {
        return drag_ == Drag::LoopNew || drag_ == Drag::LoopMove || drag_ == Drag::LoopLeft ||
               drag_ == Drag::LoopRight;
    }
    bool isRegion() const {
        return drag_ == Drag::RegionMove || drag_ == Drag::RegionLeft || drag_ == Drag::RegionRight;
    }
    double position(float x, bool free) const {
        const auto& tm = *view_.session().timeMap;
        double beat = tm.beatAt(std::max(0.0, view_.timeAt(x)));
        if (!free) {
            const double grid = view_.gestures().gridBeats();
            beat = std::round(beat / grid) * grid;
        }
        return timebase_ == model::Timebase::Time ? tm.secondsAt(beat) : beat;
    }
    bool execute(const std::string& command, json::Value args) {
        if (!callbacks.run) return false;
        auto result = callbacks.run(command, std::move(args));
        if (!result && callbacks.onError) callbacks.onError(result.error());
        return static_cast<bool>(result);
    }
    void rename(const std::string& stem, model::Id id, const std::string& name, ui::Point at) {
        if (!root()) return;
        auto field = std::make_unique<ui::TextField>("ruler-name", name);
        field->layout.width = ui::SizeRule::fixed(240);
        field->selectAll();
        field->onSubmit = [this, stem, id](const std::string& text) {
            json::Value args = json::Value::object();
            args["id"] = id.str();
            args["name"] = text;
            if (execute(stem + ".set", std::move(args))) root()->closePopup();
        };
        root()->openPopup(std::move(field), {at.x, at.y, 1, 1});
    }
    // A right-click on a tempo point, meter change, region or marker: its menu
    // (Delete; Rename for regions and markers). The first tempo point and bar
    // 1's meter stay.
    bool contextMenu(ui::Point at) {
        if (!root()) return true;
        const auto& s = view_.session();
        const auto& tm = *s.timeMap;
        const float y = at.y - bounds().y;
        std::vector<ui::MenuItem> items;
        auto add = [&](std::string label, std::function<void()> run, bool allowed = true) {
            ui::MenuItem m;
            m.label = std::move(label);
            m.run = std::move(run);
            m.enabled = allowed;
            items.push_back(std::move(m));
        };
        const RulerRows& rows = view_.rulerRows();
        const float tempoTop = rows.tempoTop();
        if (y < tempoTop) {  // the loop (bars) and punch (time) rows
            const double ls = view_.loopStart(), le = view_.loopEnd();
            const double ps = view_.punchStart(), pe = view_.punchEnd();
            add(
                view_.loopEnabled() ? "Loop off" : "Loop on",
                [this, on = !view_.loopEnabled()] { setRange(false, std::nullopt, on); }, le > ls);
            add(
                view_.punchEnabled() ? "Punch off" : "Punch on",
                [this, on = !view_.punchEnabled()] { setRange(true, std::nullopt, on); }, pe > ps);
            add(
                "Punch range = loop range",
                [this, ls, le] { setRange(true, std::pair{ls, le}, true); }, le > ls);
            add(
                "Loop range = punch range",
                [this, ps, pe] { setRange(false, std::pair{ps, pe}, view_.loopEnabled()); },
                pe > ps);
            add("Delete loop", [this] { clearRange(false); }, le > ls);
            add("Delete punch", [this] { clearRange(true); }, pe > ps);
            add(
                "Delete loop and punch",
                [this] {
                    clearRange(false);
                    clearRange(true);
                },
                le > ls || pe > ps);
        }
        if (y >= tempoTop && y < tempoTop + rows.tempo / 2.0f) {
            const time::TempoPoint* hit = nullptr;
            float distance = 1e9f;
            for (const auto& p : tm.tempo()) {
                const float x = view_.xAt(tm.secondsAt(p.beat));
                const float d = std::abs(at.x - x);
                if (at.x >= x - 5 && at.x <= x + 57 && d < distance) {
                    hit = &p;
                    distance = d;
                }
            }
            if (hit)
                add(
                    "Delete tempo change", [this, beat = hit->beat] { removeTempo(beat); },
                    hit->beat > 0);
        } else if (y >= tempoTop && y < tempoTop + rows.tempo) {
            // The meter half: a key change's flag, else a meter change's; and a
            // key change can start at the beat under the pointer.
            const KeyFlag* key = keyFlagAt(at);
            if (key) {
                add("Change key\u2026",
                    [this, beat = key->beat, text = key->text, at] { keyField(beat, text, at); });
                add("Delete key change", [this, beat = key->beat] {
                    json::Value args = json::Value::object();
                    args["at"] = beat;
                    execute("key.remove", std::move(args));
                });
            }
            for (const time::MeterChange& m : tm.meter()) {
                const float x = view_.xAt(tm.secondsAt(tm.beatOfBar(m.bar)));
                if (key || at.x < x - 5 || at.x > x + 40) continue;
                add(
                    "Delete meter change",
                    [this, bar = m.bar] {
                        json::Value args = json::Value::object();
                        args["bar"] = static_cast<std::int64_t>(bar);
                        execute("timesig.remove", std::move(args));
                    },
                    m.bar > 1);
                break;
            }
            if (!key)
                add("Key change here\u2026",
                    [this, beat = gridBeat(at.x), at] { keyField(beat, keyTextAt(beat), at); });
        } else if (y >= rows.chordTop() && y < rows.markerTop()) {
            // The chord row: the chord under the pointer (its cell runs to the
            // next chord point), and a chord can start at the grid beat there.
            const ChordCell* hit = nullptr;
            for (const ChordCell& cell : chordCells)
                if (cell.label.x <= at.x) hit = &cell;
            if (hit && hit->label.contains(at)) {
                add("Edit chord\u2026", [this, beat = hit->beat, at] { chordField(beat, at); });
                add("Delete chord", [this, beat = hit->beat] {
                    json::Value args = json::Value::object();
                    args["at"] = beat;
                    execute("chord.remove", std::move(args));
                });
            } else {
                add("Add chord here\u2026",
                    [this, beat = gridBeat(at.x), at] { chordField(beat, at); });
                if (hit && !hit->symbol.empty())
                    add("No chord from here", [this, beat = gridBeat(at.x)] {
                        json::Value args = json::Value::object();
                        args["at"] = beat;
                        args["symbol"] = "";
                        execute("chord.set", std::move(args));
                    });
            }
            add("Key change here\u2026",
                [this, beat = hit && hit->label.contains(at) ? hit->beat : gridBeat(at.x), at] {
                    keyField(beat, keyTextAt(beat), at);
                });
        } else if (y >= rows.regionTop() && y < rows.chordTop()) {
            for (const auto& entry : s.regions) {
                const auto& rg = *entry.value;
                const float left = view_.xAt(atSeconds(tm, rg.start, rg.timebase));
                const float right = view_.xAt(atSeconds(tm, rg.end, rg.timebase));
                if (at.x < left - 5 || at.x > right + 5) continue;
                add("Rename region\u2026",
                    [this, id = entry.id, name = rg.name, at] { rename("region", id, name, at); });
                add("Delete region", [this, id = entry.id] { remove("region", id); });
                // A section's items as a new launcher scene (scene.capture);
                // a refusal names what is in the way.
                if (rg.section)
                    add("Capture as scene", [this, id = entry.id] {
                        json::Value args = json::Value::object();
                        args["region"] = id.str();
                        execute("scene.capture", std::move(args));
                    });
                break;
            }
        } else if (y >= rows.markerTop()) {
            std::optional<model::Id> hit;
            std::string name;
            float distance = 1e9f;
            for (const auto& entry : s.markers) {
                const auto& mk = *entry.value;
                const float x = view_.xAt(atSeconds(tm, mk.pos, mk.timebase));
                const float d = std::abs(at.x - x);
                if (at.x >= x - 5 && at.x <= x + 98 && d < distance) {
                    hit = entry.id;
                    name = mk.name;
                    distance = d;
                }
            }
            if (hit) {
                add("Rename marker\u2026",
                    [this, id = *hit, name, at] { rename("marker", id, name, at); });
                add("Delete marker", [this, id = *hit] { remove("marker", id); });
            }
        }
        if (items.empty()) return true;
        root()->openPopup(ui::makeMenu("ruler.menu", std::move(items),
                                       [this] {
                                           if (root()) root()->closePopup();
                                       }),
                          {at.x, at.y, 1, 1});
        return true;
    }
    // transport.loop / transport.punch: a new range (seconds) and/or the switch.
    void setRange(bool punch, std::optional<std::pair<double, double>> range, bool on) {
        json::Value args = json::Value::object();
        if (range) {
            args["start_seconds"] = range->first;
            args["end_seconds"] = range->second;
        }
        args["enabled"] = on;
        if (!execute(punch ? "transport.punch" : "transport.loop", std::move(args))) return;
        const double start = range ? range->first : punch ? view_.punchStart() : view_.loopStart();
        const double end = range ? range->second : punch ? view_.punchEnd() : view_.loopEnd();
        if (punch)
            view_.setPunchRange(start, end, on);
        else
            view_.setLoopRange(start, end, on);
    }
    // Removes the loop or punch range, which also turns it off.
    void clearRange(bool punch) {
        json::Value args = json::Value::object();
        args["clear"] = true;
        if (!execute(punch ? "transport.punch" : "transport.loop", std::move(args))) return;
        if (punch)
            view_.setPunchRange(0, 0, false);
        else
            view_.setLoopRange(0, 0, false);
    }
    void remove(const std::string& stem, model::Id id) {
        json::Value args = json::Value::object();
        args["id"] = id.str();
        execute(stem + ".remove", std::move(args));
    }
    bool removeTempo(double beat) {
        json::Value points = json::Value::array();
        for (const auto& p : view_.session().timeMap->tempo()) {
            if (p.beat == beat) continue;
            json::Value point = json::Value::object();
            point["beat"] = p.beat;
            point["bpm"] = p.bpm;
            point["ramp"] = p.shape == time::TempoShape::Ramp;
            points.push_back(std::move(point));
        }
        json::Value args = json::Value::object();
        args["points"] = std::move(points);
        return execute("tempo.set", std::move(args));
    }
    bool setRamp(double beat, bool ramp) {
        json::Value points = json::Value::array();
        for (const auto& p : view_.session().timeMap->tempo()) {
            json::Value point = json::Value::object();
            point["beat"] = p.beat;
            point["bpm"] = p.bpm;
            point["ramp"] = p.beat == beat ? ramp : p.shape == time::TempoShape::Ramp;
            points.push_back(std::move(point));
        }
        json::Value args = json::Value::object();
        args["points"] = std::move(points);
        return execute("tempo.set", std::move(args));
    }
    bool tempo(double beat, double bpm) {
        // Inserting into a ramp splits it without changing its trajectory.
        bool ramp = false;
        for (const auto& p : view_.session().timeMap->tempo()) {
            if (p.beat > beat) break;
            ramp = p.shape == time::TempoShape::Ramp;
        }
        json::Value points = json::Value::array();
        bool inserted = false;
        for (const auto& p : view_.session().timeMap->tempo()) {
            if (!inserted && p.beat >= beat) {
                json::Value point = json::Value::object();
                point["beat"] = beat;
                point["bpm"] = bpm;
                point["ramp"] = ramp;
                points.push_back(std::move(point));
                inserted = true;
            }
            if (p.beat == beat) continue;
            json::Value point = json::Value::object();
            point["beat"] = p.beat;
            point["bpm"] = p.bpm;
            point["ramp"] = p.shape == time::TempoShape::Ramp;
            points.push_back(std::move(point));
        }
        if (!inserted) {
            json::Value point = json::Value::object();
            point["beat"] = beat;
            point["bpm"] = bpm;
            point["ramp"] = ramp;
            points.push_back(std::move(point));
        }
        json::Value args = json::Value::object();
        args["points"] = std::move(points);
        return execute("tempo.set", std::move(args));
    }
    ArrangeView& view_;
    Drag drag_ = Drag::None;
    model::Id id_{};
    model::Timebase timebase_ = model::Timebase::Beats;
    ui::Point press_{};
    double originalStart_ = 0, originalEnd_ = 0, draftStart_ = 0, draftEnd_ = 0;
    bool moved_ = false, loopEnabled_ = false;
    bool punch_ = false;       // the range gesture edits the punch range (time row), not the loop
    bool markerFree_ = false;  // a marker/region drag the mouse map frees from the grid
    std::string_view markerMods(unsigned mods) {
        const std::string_view b = view_.gestures().mouse.behaviour("ruler.marker", mods);
        markerFree_ = b == "move_free";
        return b;
    }
};

bool Ruler::handle(const ui::PointerEvent& e) {
    if (e.type == ui::PointerEvent::Type::Cancel) {
        drag_ = Drag::None;
        moved_ = false;
        invalidate();
        return true;
    }
    if (e.type == ui::PointerEvent::Type::Wheel) return false;
    if (e.type == ui::PointerEvent::Type::Down && e.button == 3 && bounds().contains(e.pos))
        return contextMenu(e.pos);
    if (e.type == ui::PointerEvent::Type::Down) {
        if (e.button != 1 || !bounds().contains(e.pos)) return false;
        press_ = e.pos;
        moved_ = false;
        drag_ = Drag::None;
        timebase_ = model::Timebase::Beats;
        const auto& s = view_.session();
        const auto& tm = *s.timeMap;
        const float y = e.pos.y - bounds().y;
        const RulerRows& rows = view_.rulerRows();
        const bool free = (e.mods & ui::kShift) != 0;
        punch_ = false;
        (void)markerMods(e.mods);  // a marker or region press drags by these modifiers
        if (y < rows.tempoTop()) {
            // The bars row sets the loop range, the time row the punch range.
            punch_ = y >= rows.beat;
            const double rangeStart = punch_ ? view_.punchStart() : view_.loopStart();
            const double rangeEnd = punch_ ? view_.punchEnd() : view_.loopEnd();
            originalStart_ = tm.beatAt(rangeStart);
            originalEnd_ = tm.beatAt(rangeEnd);
            loopEnabled_ = punch_ ? view_.punchEnabled() : view_.loopEnabled();
            const float left = view_.xAt(rangeStart), right = view_.xAt(rangeEnd);
            drag_ = Drag::LoopNew;
            if (originalEnd_ > originalStart_) {
                if (std::abs(e.pos.x - left) <= kRangeEdge)
                    drag_ = Drag::LoopLeft;
                else if (std::abs(e.pos.x - right) <= kRangeEdge)
                    drag_ = Drag::LoopRight;
                else if (e.pos.x > left && e.pos.x < right)
                    drag_ = Drag::LoopMove;
            }
            if (drag_ == Drag::LoopNew) originalStart_ = originalEnd_ = position(e.pos.x, free);
        } else if (y < rows.regionTop() && y >= rows.tempoTop() + rows.tempo / 2.0f) {
            // The meter half: a double-click edits the key change or the meter
            // change under the pointer, or starts a meter change at the bar
            // under it (timesig.set).
            if (e.clicks != 2 || !root()) return true;
            if (const KeyFlag* key = keyFlagAt(e.pos)) {
                keyField(key->beat, key->text, e.pos);
                return true;
            }
            int bar = tm.barBeat(std::max(0.0, tm.beatAt(view_.timeAt(e.pos.x)))).bar;
            for (const time::MeterChange& m : tm.meter()) {
                const float x = view_.xAt(tm.secondsAt(tm.beatOfBar(m.bar)));
                if (e.pos.x >= x - 5 && e.pos.x <= x + 40) bar = m.bar;
            }
            const time::MeterChange now = tm.meterAt(bar);
            auto field =
                std::make_unique<ui::TextField>("ruler-meter", std::to_string(now.numerator) + "/" +
                                                                   std::to_string(now.denominator));
            field->layout.width = ui::SizeRule::fixed(100);
            field->selectAll();
            field->onSubmit = [this, bar](const std::string& text) {
                json::Value args = json::Value::object();
                args["bar"] = static_cast<std::int64_t>(bar);
                args["meter"] = text;
                if (execute("timesig.set", std::move(args))) root()->closePopup();
            };
            root()->openPopup(std::move(field), {e.pos.x, e.pos.y, 1, 1});
            return true;
        } else if (y < rows.regionTop()) {
            const bool toggleRamp = e.clicks == 1 && view_.gestures().mouse.behaviour(
                                                         "ruler.tempo", e.mods) == "toggle_ramp";
            if (e.clicks != 2 && !toggleRamp) return true;
            const time::TempoPoint* hit = nullptr;
            float distance = 1e9f;
            for (const auto& p : tm.tempo()) {
                const float x = view_.xAt(tm.secondsAt(p.beat));
                const float d = std::abs(e.pos.x - x);
                if (e.pos.x >= x - 5 && e.pos.x <= x + 57 && d < distance) {
                    hit = &p;
                    distance = d;
                }
            }
            if (toggleRamp) {  // Alt+click: the point steps or ramps to the next one
                if (hit) setRamp(hit->beat, hit->shape != time::TempoShape::Ramp);
                return true;
            }
            if (hit) {
                if (!root()) return true;
                auto field =
                    std::make_unique<ui::TextField>("ruler-bpm", json::formatDouble(hit->bpm));
                field->layout.width = ui::SizeRule::fixed(140);
                field->selectAll();
                field->onSubmit = [this, beat = hit->beat](const std::string& text) {
                    char* end = nullptr;
                    const double bpm = std::strtod(text.c_str(), &end);
                    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
                    if (end == text.c_str() || (end && *end) || !std::isfinite(bpm) || bpm < 1 ||
                        bpm > 1000) {
                        if (callbacks.onError)
                            callbacks.onError(Error("tempo.bpm", "BPM must be between 1 and 1000",
                                                    "/bpm", "enter a number"));
                        return;
                    }
                    if (tempo(beat, bpm)) root()->closePopup();
                };
                root()->openPopup(std::move(field), {e.pos.x, e.pos.y, 1, 1});
                return true;
            }
            const double beat = position(e.pos.x, free);
            tempo(beat, tm.bpmAt(beat));
            return true;
        } else if (y >= rows.chordTop() && y < rows.markerTop()) {
            // A chord's start or name: a double-click edits it, a drag moves it;
            // a double-click elsewhere starts a chord at the grid beat there.
            if (const ChordCell* cell = chordAt(e.pos)) {
                if (e.clicks == 2) {
                    chordField(cell->beat, e.pos);
                    return true;
                }
                originalStart_ = originalEnd_ = cell->beat;
                drag_ = Drag::Chord;
            } else if (e.clicks == 2) {
                chordField(gridBeat(e.pos.x), e.pos);
                return true;
            }
        } else if (y < rows.chordTop()) {
            for (const auto& entry : s.regions) {
                const auto& rg = *entry.value;
                const float left = view_.xAt(atSeconds(tm, rg.start, rg.timebase));
                const float right = view_.xAt(atSeconds(tm, rg.end, rg.timebase));
                if (e.pos.x < left - 5 || e.pos.x > right + 5) continue;
                id_ = entry.id;
                if (markerMods(e.mods) == "remove") {
                    json::Value args = json::Value::object();
                    args["id"] = id_.str();
                    execute("region.remove", std::move(args));
                    return true;
                }
                if (e.clicks == 2) {
                    rename("region", id_, rg.name, e.pos);
                    return true;
                }
                timebase_ = rg.timebase;
                originalStart_ = rg.start;
                originalEnd_ = rg.end;
                drag_ = std::abs(e.pos.x - left) <= 5    ? Drag::RegionLeft
                        : std::abs(e.pos.x - right) <= 5 ? Drag::RegionRight
                                                         : Drag::RegionMove;
                break;
            }
        } else {
            const model::Marker* hit = nullptr;
            float distance = 1e9f;
            for (const auto& entry : s.markers) {
                const auto& mk = *entry.value;
                const float x = view_.xAt(atSeconds(tm, mk.pos, mk.timebase));
                const float d = std::abs(e.pos.x - x);
                if (e.pos.x >= x - 5 && e.pos.x <= x + 98 && d < distance) {
                    id_ = entry.id;
                    hit = &mk;
                    distance = d;
                }
            }
            if (hit) {
                const auto& mk = *hit;
                if (markerMods(e.mods) == "remove") {
                    json::Value args = json::Value::object();
                    args["id"] = id_.str();
                    execute("marker.remove", std::move(args));
                    return true;
                }
                if (e.clicks == 2) {
                    rename("marker", id_, mk.name, e.pos);
                    return true;
                }
                timebase_ = mk.timebase;
                originalStart_ = originalEnd_ = mk.pos;
                drag_ = Drag::Marker;
            }
        }
        draftStart_ = originalStart_;
        draftEnd_ = originalEnd_;
        return true;
    }
    if (drag_ == Drag::None) return false;
    if (e.type == ui::PointerEvent::Type::Move || e.type == ui::PointerEvent::Type::Up) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) >= 3;
        if (moved_) {
            const bool ranged = drag_ == Drag::LoopNew || drag_ == Drag::LoopLeft ||
                                drag_ == Drag::LoopRight || drag_ == Drag::LoopMove;
            const bool free = ranged ? (e.mods & ui::kShift) != 0 : markerFree_;
            const double at = position(e.pos.x, free);
            const double raw = position(e.pos.x, true) - position(press_.x, true);
            draftStart_ = originalStart_;
            draftEnd_ = originalEnd_;
            if (drag_ == Drag::LoopNew) {
                draftStart_ = std::min(originalStart_, at);
                draftEnd_ = std::max(originalStart_, at);
            } else if (drag_ == Drag::LoopLeft) {  // the edges may meet: no range
                draftStart_ = std::min(at, originalEnd_);
            } else if (drag_ == Drag::LoopRight) {
                draftEnd_ = std::max(at, originalStart_);
            } else if (drag_ == Drag::RegionLeft) {
                draftStart_ = std::min(at, originalEnd_ - std::min(1e-6, originalEnd_ / 2));
            } else if (drag_ == Drag::RegionRight) {
                draftEnd_ = std::max(at, originalStart_ + 1e-6);
            } else {
                const auto& tm = *view_.session().timeMap;
                double start = std::max(0.0, originalStart_ + raw);
                if (!free) {
                    const double grid = view_.gestures().gridBeats();
                    double beat = timebase_ == model::Timebase::Time ? tm.beatAt(start) : start;
                    beat = std::round(beat / grid) * grid;
                    start = timebase_ == model::Timebase::Time ? tm.secondsAt(beat) : beat;
                }
                draftStart_ = start;
                draftEnd_ = start + originalEnd_ - originalStart_;
            }
            invalidate();
        }
        if (e.type == ui::PointerEvent::Type::Up && e.button == 1) {
            json::Value args = json::Value::object();
            if (!moved_ && isLoop()) {
                args["seconds"] = std::max(0.0, view_.timeAt(e.pos.x));
                execute("transport.seek", std::move(args));
            } else if (moved_ && isLoop() && draftEnd_ > draftStart_) {
                const auto& tm = *view_.session().timeMap;
                const double start = tm.secondsAt(draftStart_), end = tm.secondsAt(draftEnd_);
                args["start_seconds"] = start;
                args["end_seconds"] = end;
                // A new punch range turns punch on: it is set to be used.
                const bool on = punch_ ? (loopEnabled_ || drag_ == Drag::LoopNew) : loopEnabled_;
                args["enabled"] = on;
                if (execute(punch_ ? "transport.punch" : "transport.loop", std::move(args))) {
                    if (punch_)
                        view_.setPunchRange(start, end, on);
                    else
                        view_.setLoopRange(start, end, on);
                }
            } else if (moved_ && (drag_ == Drag::LoopLeft || drag_ == Drag::LoopRight)) {
                clearRange(punch_);  // an edge dragged onto the other: the range is gone
            } else if (moved_ && drag_ == Drag::Chord && draftStart_ != originalStart_) {
                args["at"] = originalStart_;
                args["to"] = draftStart_;
                execute("chord.set", std::move(args));
            } else if (moved_ && (drag_ == Drag::Marker || isRegion()) &&
                       (draftStart_ != originalStart_ || draftEnd_ != originalEnd_)) {
                args["id"] = id_.str();
                if (drag_ == Drag::Marker)
                    args["pos"] = draftStart_;
                else {
                    args["start"] = draftStart_;
                    args["end"] = draftEnd_;
                }
                execute(drag_ == Drag::Marker ? "marker.set" : "region.set", std::move(args));
            }
            drag_ = Drag::None;
            invalidate();
        }
        return true;
    }
    return false;
}

// --- the track control panel (placeholder: name and type badge) ---------------------

class TcpHeader : public ui::Node {
  public:
    explicit TcpHeader(ArrangeView& view) : Node("tcp-header"), view_(view) {}
    // Channels and busses: not the master, and not the automation lanes
    // that are rows under their tracks.
    std::string valueText() const override {
        return "Tracks  " + std::to_string(countTracks(view_.trackLayout().session()));
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
        c.text({bounds().x + 12, bounds().y, bounds().w - 20, bounds().h}, valueText(),
               ui::Font::UiBold, c.theme().type("caption"), c.theme().color("ink-quiet"),
               ui::kLeft | ui::kMiddle);
    }

  private:
    ArrangeView& view_;
};

class Corner : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

// --- the playhead (on the overlay tree) ---------------------------------------------

class Playhead : public ui::Node {
  public:
    explicit Playhead(std::shared_ptr<ArrangeView*> alive)
        : Node("playhead"), alive_(std::move(alive)) {}
    // The overlay's children are arranged with their own bounds, so take the
    // parent's (the full window).
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        if (!*alive_) return;
        const ArrangeView& view = **alive_;
        const ui::Rect a = view.lanesRect();
        if (a.empty()) return;
        const float x = view.playheadX();
        if (x < a.x - 1 || x > a.right() + 1) return;
        const ui::Color col = c.theme().color("accent");
        c.line({x, a.y}, {x, a.bottom()}, col, 1.0f);
        c.fillRect({x - 3, a.y, 7, 7}, col);
    }

  private:
    std::shared_ptr<ArrangeView*> alive_;
};

// --- the view ----------------------------------------------------------------------

ArrangeView::ArrangeView(peaks::PeakService& peaks, Resolve resolve, TransportSource transport)
    : Node("arrange", ui::Role::Custom),
      peaks_(peaks),
      resolve_(std::move(resolve)),
      transportSource_(std::move(transport)) {
    alive_ = std::make_shared<ArrangeView*>(this);
    gestures_ = std::make_unique<ItemGestures>(*this);
    interactive = true;
    focusable = true;
    layout.dir = ui::Layout::Dir::Column;

    auto& header = emplace<Node>("arrange-header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(rulerRows_.height());
    headerRow_ = &header;
    header_ = &header.emplace<TcpHeader>(*this);
    header_->layout.width = ui::SizeRule::fixed(kTcpWidth);
    launcherHeader_ = &header.emplace<LauncherHeader>(*this);
    launcherHeader_->visible = false;
    ruler_ = &header.emplace<Ruler>(*this);
    ruler_->layout.width = ui::SizeRule::fraction();
    auto& corner = header.emplace<Corner>("arrange-corner");
    corner.layout.width = ui::SizeRule::fixed(ui::ScrollBar::kThickness);

    auto& body = emplace<Node>("arrange-body");
    body.layout.dir = ui::Layout::Dir::Row;
    // The panel's callbacks forward to the view's, so the app can install them
    // after the tree is built.
    TcpCallbacks forward;
    forward.selected = [this](const model::Id& id) {
        return tcpCallbacks_.selected && tcpCallbacks_.selected(id);
    };
    forward.select = [this](const model::Id& id, unsigned mods) {
        if (tcpCallbacks_.select) tcpCallbacks_.select(id, mods);
    };
    forward.commit = [this](std::string_view command, json::Value args, std::string gesture) {
        if (tcpCallbacks_.commit)
            tcpCallbacks_.commit(command, std::move(args), std::move(gesture));
    };
    forward.addStrip = [this](std::string_view type) {
        if (tcpCallbacks_.addStrip) tcpCallbacks_.addStrip(type);
    };
    forward.meterShown = [this](const std::string& track, ui::Meter* meter) {
        if (tcpCallbacks_.meterShown) tcpCallbacks_.meterShown(track, meter);
    };
    forward.meterHidden = [this](ui::Meter* meter) {
        if (tcpCallbacks_.meterHidden) tcpCallbacks_.meterHidden(meter);
    };
    // The input button's two: what to write on it, and the picker (both are the
    // app's, since it owns the engine's input list and the popup).
    forward.inputText = [this](const model::Id& id, const std::string& channels) {
        return tcpCallbacks_.inputText ? tcpCallbacks_.inputText(id, channels) : std::string{};
    };
    forward.pickInput = [this](const model::Id& id, const ui::Rect& anchor) {
        if (tcpCallbacks_.pickInput) tcpCallbacks_.pickInput(id, anchor);
    };
    forward.envelopes = [this](const model::Id& id, const ui::Rect& anchor) {
        if (tcpCallbacks_.envelopes) tcpCallbacks_.envelopes(id, anchor);
    };
    forward.fx = [this](const model::Id& id) {
        if (tcpCallbacks_.fx) tcpCallbacks_.fx(id);
    };
    forward.addFx = [this](const model::Id& id) {
        if (tcpCallbacks_.addFx) tcpCallbacks_.addFx(id);
    };
    forward.fxNeedsAttention = [this](const model::Id& id) {
        return tcpCallbacks_.fxNeedsAttention && tcpCallbacks_.fxNeedsAttention(id);
    };
    forward.mixer = [this] {
        if (tcpCallbacks_.mixer) tcpCallbacks_.mixer();
    };
    forward.menu = [this](const model::Id& id, const ui::Rect& anchor,
                          std::function<void()> rename) {
        if (tcpCallbacks_.menu) tcpCallbacks_.menu(id, anchor, std::move(rename));
    };
    forward.learn = [this](const model::Id& id, const std::string& param, const ui::Rect& anchor) {
        if (tcpCallbacks_.learn) tcpCallbacks_.learn(id, param, anchor);
    };
    forward.addSend = [this](const model::Id& from, const model::Id& to) -> Result<json::Value> {
        if (tcpCallbacks_.addSend) return tcpCallbacks_.addSend(from, to);
        return Error("gui.send", "send editing is unavailable", {}, "wire the TCP send callback");
    };
    forward.onError = [this](const Error& error) {
        if (tcpCallbacks_.onError) tcpCallbacks_.onError(error);
    };
    forward.routing = [this](const model::Id& track, const ui::Rect& anchor) {
        if (tcpCallbacks_.routing) tcpCallbacks_.routing(track, anchor);
    };
    forward.resizeTrack = [this](const model::Id& id, float height) {
        layout_.setTrackHeight(id, height);
        clampScroll();
        if (tcp_) tcp_->scrolled();
        damageViews();
    };
    forward.lanesRect = [this] {
        return tcpCallbacks_.lanesRect ? tcpCallbacks_.lanesRect() : ui::Rect{};
    };

    tcp_ = &body.emplace<TcpPanel>(layout_, view_, std::move(forward));
    tcp_->layout.width = ui::SizeRule::fixed(kTcpWidth);
    launcherPane_ = &body.emplace<LauncherPane>(*this, peaks_, resolve_);
    launcherPane_->visible = false;
    auto& lanesCol = body.emplace<Node>("lanes-column");
    lanesColumn_ = &lanesCol;
    lanesCol.layout.dir = ui::Layout::Dir::Column;
    lanesCol.layout.width = ui::SizeRule::fraction();
    lanes_ = &lanesCol.emplace<Lanes>(peaks_, resolve_, layout_, view_);
    lanes_->layout.height = ui::SizeRule::fraction();
    hbar_ = &lanesCol.emplace<ui::ScrollBar>("hscroll", ui::ScrollBar::Axis::Horizontal);
    hbar_->layout.height = ui::SizeRule::fixed(ui::ScrollBar::kThickness);
    hbar_->onScroll = [this](float v) {
        Viewport nv = view_;
        nv.startSeconds = static_cast<double>(v);
        setViewport(nv);
    };
    vbar_ = &body.emplace<ui::ScrollBar>("vscroll", ui::ScrollBar::Axis::Vertical);
    vbar_->layout.width = ui::SizeRule::fixed(ui::ScrollBar::kThickness);
    vbar_->onScroll = [this](float v) {
        Viewport nv = view_;
        nv.scrollY = v;
        setViewport(nv);
    };
    // Any row-geometry change (a new session, per-track heights, vertical zoom)
    // re-clamps the scroll and refreshes both bars.
    layout_.onChanged = [this] {
        clampScroll();
        syncScrollbars();
        if (tcp_) tcp_->scrolled();
        damageViews();
    };
}

ArrangeView::~ArrangeView() { *alive_ = nullptr; }

void ArrangeView::show(const model::Session& session, const std::vector<model::Change>* changes,
                       std::uint64_t generation) {
    const auto before = layout_.session();
    const bool replaced = generation != generation_;
    if (!replaced && before.version == session.version && layout_.rebuilds() != 0) return;
    peaks_.revalidate();  // a new version may name replaced media files
    if (replaced) {
        changes = nullptr;
        gestures_->clearClipboard();
        if (root()) root()->releaseWithin(this);
    }
    generation_ = generation;
    const bool rebuilt = layout_.show(session, changes, generation);
    syncChordRow();
    syncLauncher();
    if (rebuilt) {
        lanes_->rebuild();
        tcp_->setSession(session);
        header_->invalidate();
        clampScroll();
        syncScrollbars();
        damageViews();
        return;
    }
    auto tracks = changedObjects(before.tracks, session.tracks, changes);
    const auto changedItems = changedObjects(before.items, session.items, changes);
    auto items = changedItems;
    auto takes = changedObjects(before.takes, session.takes, changes);
    const auto envelopes = changedObjects(before.envelopes, session.envelopes, changes);
    const auto routes = changedObjects(before.routes, session.routes, changes);
    const auto midi = changedObjects(before.midi, session.midi, changes);
    const auto media = changedObjects(before.media, session.media, changes);
    const bool timing = !before.tempo.sameStorage(session.tempo) ||
                        !before.meters.sameStorage(session.meters) ||
                        before.project->timebase != session.project->timebase;
    std::set<model::Id> timebaseTracks, laneTracks, dirtyEnvelopes = envelopes;
    for (auto id : tracks) {
        const auto* old = before.tracks.find(id);
        const auto* now = session.tracks.find(id);
        if (old && now && old->timebase != now->timebase) timebaseTracks.insert(id);
        if (old && now && old->laneMute != now->laneMute) laneTracks.insert(id);  // ADR-0057
    }
    // FX patch/parameter edits do not affect the TCP's chain badge.
    for (auto id : changedObjects(before.fx, session.fx, changes)) {
        const auto* old = before.fx.find(id);
        const auto* now = session.fx.find(id);
        if (!old || !now || old->bypass != now->bypass || old->offline != now->offline) {
            if (auto track = model::trackOfFx(before, id)) tracks.insert(*track);
            if (auto track = model::trackOfFx(session, id)) tracks.insert(*track);
        }
    }
    tcp_->updateSession(session, tracks, envelopes, routes);
    for (auto id : changedObjects(before.autoItems, session.autoItems, changes)) {
        if (const auto* old = before.autoItems.find(id)) dirtyEnvelopes.insert(old->envelope);
        if (const auto* now = session.autoItems.find(id)) dirtyEnvelopes.insert(now->envelope);
    }
    // Track values are the fallback of mixer envelopes with no points.
    if (before.project->playrate.rate != session.project->playrate.rate)
        for (const auto& entry : session.envelopes)
            if (entry.value->target.object == model::kMaster &&
                entry.value->target.param == "playrate")
                dirtyEnvelopes.insert(entry.id);
    if (!tracks.empty())
        for (const auto& entry : session.envelopes)
            if (tracks.contains(entry.value->target.object)) dirtyEnvelopes.insert(entry.id);
    for (auto id : dirtyEnvelopes) {
        for (const auto* snapshot : {&before, &session})
            if (const auto* env = snapshot->envelopes.find(id))
                if (env->target.object.kind == model::Kind::Take) takes.insert(env->target.object);
        if (auto row = layout_.indexOfEnv(id))
            lanes_->invalidate(lanes_->rowRect(*row).intersect(lanes_->bounds()));
    }
    if (!media.empty() || !midi.empty())
        for (const auto* snapshot : {&before, &session})
            for (const auto& entry : snapshot->takes)
                if (entry.value->source &&
                    (media.contains(*entry.value->source) || midi.contains(*entry.value->source)))
                    takes.insert(entry.id);
    if (!takes.empty())
        for (const auto* snapshot : {&before, &session})
            for (const auto& entry : snapshot->items)
                if (std::any_of(entry.value->takes.begin(), entry.value->takes.end(),
                                [&](auto take) { return takes.contains(take); }))
                    items.insert(entry.id);
    if (timing) {
        lanes_->rebuild();
        ruler_->invalidate();
    } else {
        lanes_->updateIndex(before, items, timebaseTracks);
        for (const auto* set : {&timebaseTracks, &laneTracks})
            for (auto track : *set)
                if (auto row = layout_.indexOf(track))
                    lanes_->invalidate(lanes_->rowRect(*row).intersect(lanes_->bounds()));
        // Clear both the old and the new footprint (moves, deletion, undo).
        for (auto id : items)
            for (const auto* snapshot : {&before, &session}) {
                const auto* item = snapshot->items.find(id);
                if (!item) continue;
                const auto row = layout_.indexOf(item->track);
                if (!row) continue;
                const auto span = model::itemSpan(*snapshot, *item);
                const auto r = lanes_->rowRect(*row);
                lanes_->invalidate(ui::Rect{xAt(span.startSec), r.y,
                                            std::max(1.0f, xAt(span.endSec) - xAt(span.startSec)),
                                            r.h}
                                       .inset(-2)
                                       .intersect(lanes_->bounds()));
            }
    }
    if (before.project->mediaDir != session.project->mediaDir) lanes_->invalidate();
    const bool markers = !before.markers.sameStorage(session.markers) ||
                         !before.regions.sameStorage(session.regions) ||
                         !before.context.sameStorage(session.context);
    if (markers) ruler_->invalidate();
    bool extent = timing || markers || !timebaseTracks.empty();
    for (auto id : changedItems) {
        const auto* old = before.items.find(id);
        const auto* now = session.items.find(id);
        extent |= !old || !now ||
                  model::itemSpan(before, *old).endSec != model::itemSpan(session, *now).endSec;
    }
    if (extent) {
        clampScroll();
        syncScrollbars();
    }
}

bool ArrangeView::poll(bool force, std::chrono::steady_clock::time_point at) {
    const double now = std::chrono::duration<double, std::milli>(at.time_since_epoch()).count();
    // A drag held at an edge scrolls by the time since the last pass (capped,
    // so a stalled frame is not a jump; nothing on the scroll's first pass).
    bool scrolled = false;
    if (autoScrolling()) {
        const double seconds = edge_.clock.step(at);
        scrolled = autoScroll(seconds);
        if (scrolled || seconds == 0)
            if (ui::Root* r = root()) r->requestAnimationFrame();
    } else {
        edge_.clock.reset();
    }
    if (!transportSource_) return scrolled;
    if (!force && now - lastPollMs_ < 15.0) return scrolled;
    lastPollMs_ = now;
    const Transport t = transportSource_();
    const bool wasPlaying = transport_.playing;
    const bool rangesChanged =
        t.loopEnabled != transport_.loopEnabled || t.loopStart != transport_.loopStart ||
        t.loopEnd != transport_.loopEnd || t.punchEnabled != transport_.punchEnabled ||
        t.punchStart != transport_.punchStart || t.punchEnd != transport_.punchEnd;
    transport_ = t;
    if (rangesChanged && ruler_) ruler_->invalidate();
    if (t.seconds != playhead_) {
        setPlayhead(t.seconds);
        return true;
    }
    return scrolled || t.playing != wasPlaying;
}

void ArrangeView::setModulationOffsets(std::map<model::Id, double> offsets) {
    if (offsets == layout_.liveOffsets) return;
    layout_.liveOffsets = std::move(offsets);
    if (tcp_) tcp_->refreshModulation();
    invalidateModulatedLanes();
}

// The lanes of modulated targets: their live dot sits at the playhead.
void ArrangeView::invalidateModulatedLanes() {
    if (!lanes_ || layout_.session().modulations.empty()) return;
    for (std::size_t row = 0; row < layout_.rowCount(); ++row)
        if (const auto& target = layout_.rows()[row].target)
            if (liveOffset(layout_.session(), *target, layout_.liveOffsets) ||
                layout_.liveOffsets.empty())
                lanes_->invalidate(lanes_->rowRect(row).intersect(lanes_->bounds()));
}

void ArrangeView::setPlayhead(double seconds) {
    if (seconds == playhead_) return;
    playhead_ = seconds;
    if (lanes_) lanes_->livePlayhead = seconds;
    if (!layout_.liveOffsets.empty()) invalidateModulatedLanes();
    applyFollow();
    if (ui::Root* r = root()) r->requestAnimationFrame();
}

void ArrangeView::setLoopRange(double start, double end, bool on) {
    if (transport_.loopStart == start && transport_.loopEnd == end && transport_.loopEnabled == on)
        return;
    transport_.loopEnabled = on;
    transport_.loopStart = start;
    transport_.loopEnd = end;
    if (ruler_) ruler_->invalidate();
}

void ArrangeView::setPunchRange(double start, double end, bool on) {
    if (transport_.punchStart == start && transport_.punchEnd == end &&
        transport_.punchEnabled == on)
        return;
    transport_.punchEnabled = on;
    transport_.punchStart = start;
    transport_.punchEnd = end;
    if (ruler_) ruler_->invalidate();
}

void ArrangeView::setViewport(const Viewport& v) {
    const float scrolledFrom = view_.scrollY;
    const Viewport from = view_;
    view_ = v;
    view_.pixelsPerSecond = std::clamp(view_.pixelsPerSecond, 0.01, 200000.0);
    view_.startSeconds = std::max(0.0, view_.startSeconds);
    clampScroll();
    // A move in time other than a zoom step's lets go of the zoom anchor (zoomBy).
    if (view_.startSeconds != from.startSeconds || view_.pixelsPerSecond != from.pixelsPerSecond)
        zoomHold_.reset();
    if (tcp_ && view_.scrollY != scrolledFrom) tcp_->scrolled();  // the tracks follow the lanes
    syncScrollbars();
    damageViews();
}

// The rows plus room past the last one, so it can scroll clear of the track
// panel's foot (its "mixer" and "+ track" band).
float ArrangeView::scrollHeight() const {
    return layout_.contentHeight() + (tcp_ ? TcpPanel::kFootHeight : 0.0f);
}

void ArrangeView::syncScrollbars() {
    if (!hbar_ || !vbar_) return;
    hbar_->setRange(static_cast<float>(totalSeconds()), static_cast<float>(viewWidthSeconds()));
    hbar_->setValue(static_cast<float>(view_.startSeconds));
    const float shown = lanes_ ? lanes_->bounds().h : 0.0f;
    vbar_->setRange(scrollHeight(), shown);
    vbar_->setValue(view_.scrollY);
}

void ArrangeView::clampScroll() {
    const float visibleHeight = lanes_ ? lanes_->bounds().h : 0.0f;
    if (visibleHeight <= 0) return;  // not laid out yet: keep what was asked for
    const float maxScroll = std::max(0.0f, scrollHeight() - visibleHeight);
    view_.scrollY = std::clamp(view_.scrollY, 0.0f, maxScroll);
}

void ArrangeView::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    // Clamp before arranging: the rows the TCP binds must match the scroll the
    // frame will actually paint with (the visible height is known from `r`).
    const float visibleHeight =
        std::max(0.0f, r.h - rulerRows_.height() - ui::ScrollBar::kThickness);
    view_.scrollY = std::clamp(view_.scrollY, 0.0f, std::max(0.0f, scrollHeight() - visibleHeight));
    ui::Node::arrange(canvas, r);
    clampScroll();  // exact, now that the children have their bounds
    syncScrollbars();
}

double ArrangeView::viewWidthSeconds() const {
    const double pps = view_.pixelsPerSecond > 0 ? view_.pixelsPerSecond : 40;
    return static_cast<double>(lanes_ ? lanes_->bounds().w : 0.0f) / pps;
}

double ArrangeView::totalSeconds() const {
    double end = 0;
    const model::Session& s = layout_.session();
    for (const auto& e : s.items) end = std::max(end, model::itemSpan(s, *e.value).endSec);
    for (const auto& e : s.regions)
        end = std::max(end, atSeconds(*s.timeMap, e.value->end, e.value->timebase));
    for (const auto& e : s.markers)
        end = std::max(end, atSeconds(*s.timeMap, e.value->pos, e.value->timebase));
    if (transport_.loopEnabled) end = std::max(end, transport_.loopEnd);
    return end + 8.0;  // margin past the session
}

void ArrangeView::damageViews() {
    ui::Rect r;
    for (ui::Node* n :
         {static_cast<ui::Node*>(lanes_), static_cast<ui::Node*>(ruler_),
          static_cast<ui::Node*>(hbar_), static_cast<ui::Node*>(vbar_),
          static_cast<ui::Node*>(launcherPane_), static_cast<ui::Node*>(launcherHeader_)})
        if (n && n->visible) r = r.unite(n->bounds());
    if (!r.empty()) invalidate(r);
}

void ArrangeView::applyFollow() {
    if (!lanes_ || follow_ == Follow::Off) return;
    const ui::Rect a = lanes_->bounds();
    if (a.w <= 0) return;
    const float x = playheadX();
    const bool inView = x >= a.x && x <= a.right() - 4;
    Viewport v = view_;
    if (follow_ == Follow::Continuous && transport_.playing) {
        // The playhead walks to the middle, then the view scrolls under it;
        // one out of view (a seek, a loop back) is centred at once.
        const double half = static_cast<double>(a.w) / 2.0 / v.pixelsPerSecond;
        const double centred = std::max(0.0, playhead_ - half);
        if (inView && centred <= v.startSeconds) return;
        // On whole device pixels, so the view moves in whole-pixel steps:
        // ticks and text land on the same pixels from frame to frame, and each
        // waveform column keeps summarising the same stretch of audio rather
        // than a slightly shifted one (which made the waveforms wobble).
        const ui::Root* r = root();
        const double perSecond =
            v.pixelsPerSecond * static_cast<double>(r ? r->pixelRatio() : 1.0f);
        v.startSeconds = std::round(centred * perSecond) / perSecond;
    } else {  // page, and continuous while stopped: jump when the playhead leaves the view
        if (inView) return;
        v.startSeconds =
            std::max(0.0, playhead_ - static_cast<double>(a.w) * 0.25 / v.pixelsPerSecond);
    }
    setViewport(v);
}

bool ArrangeView::installOverlay(ui::Root& root) {
    if (overlayInstalled_) return true;
    overlayInstalled_ = true;
    overlay_ = root.addOverlay(std::make_unique<Playhead>(alive_));
    gestures_->installOverlay(root);
    return true;
}

ui::Node& ArrangeView::tcp() { return *tcp_; }
ui::Node& ArrangeView::ruler() { return *ruler_; }
void ArrangeView::setRulerCallbacks(RulerCallbacks callbacks) {
    ruler_->callbacks = std::move(callbacks);
}
void ArrangeView::setRulerRows(const RulerRows& rows) {
    if (rows == rulerRows_) return;
    rulerRows_ = rows;
    headerRow_->layout.height = ui::SizeRule::fixed(rulerRows_.height());
    relayout();  // the header, the lanes and the TCP below it move
    invalidate();
}
void ArrangeView::setChordRow(std::optional<bool> shown) {
    chordRowChoice_ = shown;
    syncChordRow();
}
void ArrangeView::syncChordRow() {
    RulerRows rows = rulerRows_;
    const bool shown = chordRowChoice_.value_or(!session().contextView->chords.empty());
    rows.chord = shown ? RulerRows::kChordHeight : 0;
    setRulerRows(rows);
}
bool ArrangeView::launcherShown() const {
    return launcherChoice_.value_or(!session().scenes.empty());
}
void ArrangeView::setLauncherShown(std::optional<bool> shown) {
    launcherChoice_ = shown;
    syncLauncher();
}
float ArrangeView::launcherWidth() const {
    return launcherPane_ && launcherPane_->visible ? launcherPane_->bounds().w : 0.0f;
}
float ArrangeView::launcherRoom() const {
    // The lanes keep at least this much before the pane takes the view.
    constexpr float kMinLanes = 120;
    return std::max(LauncherColumns::kMinWidth,
                    bounds().w - kTcpWidth - ui::ScrollBar::kThickness - kMinLanes);
}
void ArrangeView::setLauncherWidth(float width, bool full) {
    launcherWidth_ = width > 0 ? std::max(width, LauncherColumns::kMinWidth) : 0.0f;
    launcherFull_ = full;
    syncLauncher();
}
void ArrangeView::dragLauncherWidth(float width) {
    // Past the room the lanes need, the pane fills the view; back under it,
    // the lanes return at the dragged width.
    const float room = launcherRoom();
    if (width >= room)
        setLauncherWidth(launcherWidth_ > 0 ? launcherWidth_ : room, true);
    else
        setLauncherWidth(width, false);
}
void ArrangeView::syncLauncher() {
    if (!launcherPane_) return;
    const bool shown = launcherShown();
    const bool full = shown && launcherFull_;
    LauncherColumns cols = launcherColumns(session());
    const float fit = std::max(LauncherColumns::kMinWidth, cols.contentWidth());
    const float width = launcherWidth_ > 0 ? launcherWidth_ : fit;
    const ui::SizeRule rule = full ? ui::SizeRule::fraction() : ui::SizeRule::fixed(width);
    const bool changed = launcherPane_->visible != shown || lanesColumn_->visible == full ||
                         launcherPane_->layout.width.value != rule.value ||
                         launcherPane_->layout.width.kind != rule.kind;
    launcherPane_->visible = launcherHeader_->visible = shown;
    launcherPane_->layout.width = launcherHeader_->layout.width = rule;
    lanesColumn_->visible = ruler_->visible = !full;
    // The scene columns scroll no further than their content needs.
    const float span = cols.contentWidth() - LauncherColumns::kArr;
    launcherScroll_ =
        std::clamp(launcherScroll_, 0.0f, std::max(0.0f, span - (width - LauncherColumns::kArr)));
    if (changed) relayout();
    if (shown) {
        launcherPane_->invalidate();
        launcherHeader_->invalidate();
    }
}
void ArrangeView::selectClips(std::vector<model::Id> clips) {
    selectedCell_.reset();
    if (clips != selectedClips_) {
        selectedClips_ = std::move(clips);
        if (launcherCallbacks_.onSelection) launcherCallbacks_.onSelection(selectedClips_);
    }
    if (launcherPane_) launcherPane_->invalidate();
}
void ArrangeView::selectCell(std::optional<Cell> cell) {
    // Always told, cell or none: with a cell, a launcher cell was clicked last
    // (the app's Delete then takes clips only, and there are none).
    selectedClips_.clear();
    selectedCell_ = cell;
    if (launcherCallbacks_.onSelection) launcherCallbacks_.onSelection(selectedClips_);
    if (launcherPane_) launcherPane_->invalidate();
}
void ArrangeView::clearLauncherSelection() {
    if (selectedClips_.empty() && !selectedCell_) return;
    selectCell(std::nullopt);
}
Result<json::Value> ArrangeView::runLauncher(const std::string& command, json::Value args) {
    if (!launcherCallbacks_.run)
        return Error("gui.launcher", "the clip launcher has no command path", {},
                     "wire ArrangeView::setLauncherCallbacks");
    auto r = launcherCallbacks_.run(command, std::move(args));
    if (!r && launcherCallbacks_.onError) launcherCallbacks_.onError(r.error());
    return r;
}
void ArrangeView::setCaptureArmed(bool armed) {
    if (armed == captureArmed_) return;
    captureArmed_ = armed;
    if (launcherHeader_) launcherHeader_->invalidate();
}
void ArrangeView::setLaunchState(LaunchViews views) {
    if (views == launchViews_) return;
    std::set<model::Id> held;
    for (const auto& [track, v] : views)
        if (v.plays != LaunchView::Plays::Arrangement) held.insert(track);
    if (held != layout_.heldTracks) {
        layout_.heldTracks = std::move(held);
        damageViews();
    }
    launchViews_ = std::move(views);
    if (launcherPane_ && launcherPane_->visible) {
        launcherPane_->invalidate();
        launcherHeader_->invalidate();
    }
}
void ArrangeView::addMarker() { ruler_->addMarker(); }
void ArrangeView::addRegion() { ruler_->addRegion(); }
const RulerStats& ArrangeView::rulerStats() const { return ruler_->stats; }

ui::Rect ArrangeView::rowRect(std::size_t row) const {
    return lanes_ ? lanes_->rowRect(row) : ui::Rect{};
}

ui::Rect ArrangeView::tcpRowRect(std::size_t row) const {
    return tcp_ ? tcp_->rowRect(row) : ui::Rect{};
}

json::Value ArrangeView::saveLayout() const {
    const views::Viewport& vp = viewport();
    json::Value v = json::Value::object();
    v["start_seconds"] = vp.startSeconds;
    v["pixels_per_second"] = vp.pixelsPerSecond;
    v["scroll_y"] = static_cast<double>(vp.scrollY);
    v["row_height"] = static_cast<double>(trackLayout().globalHeight());
    v["follow"] = std::string(views::followName(follow()));
    if (chordRowChoice_) v["chords"] = *chordRowChoice_;  // View > Chords chose
    // The clip launcher (gui.md §4.15): what View > Clip launcher chose, its
    // dragged width and whether it fills the view.
    if (launcherChoice_ || launcherWidth_ > 0 || launcherFull_) {
        json::Value clips = json::Value::object();
        if (launcherChoice_) clips["shown"] = *launcherChoice_;
        if (launcherWidth_ > 0) clips["width"] = static_cast<double>(launcherWidth_);
        if (launcherFull_) clips["full"] = true;
        v["clips"] = std::move(clips);
    }
    json::Value heights = json::Value::object();
    for (const auto& [track, h] : trackLayout().heights())
        heights[track.str()] = static_cast<double>(h);
    v["track_heights"] = std::move(heights);
    json::Value expanded = json::Value::array();  // "Show all takes"
    for (model::Id item : trackLayout().expandedItems()) expanded.push_back(item.str());
    v["expanded_takes"] = std::move(expanded);
    return v;
}

void ArrangeView::loadLayout(const json::Value& v) {
    views::Viewport vp = viewport();
    auto num = [&](const char* k, auto& out) {
        if (const json::Value* x = v.find(k); x && x->isNumber())
            out = static_cast<std::remove_reference_t<decltype(out)>>(x->asDouble());
    };
    num("start_seconds", vp.startSeconds);
    num("pixels_per_second", vp.pixelsPerSecond);
    num("scroll_y", vp.scrollY);
    setViewport(vp);
    if (const json::Value* rh = v.find("row_height"); rh && rh->isNumber())
        trackLayout().setGlobalHeight(static_cast<float>(rh->asDouble()));
    trackLayout().clearTrackHeights();
    if (const json::Value* th = v.find("track_heights"); th && th->isObject())
        for (const auto& m : th->asObject()) {
            const auto id = model::Id::parse(m.key);
            if (id && m.value.isNumber())
                trackLayout().setTrackHeight(*id, static_cast<float>(m.value.asDouble()));
        }
    if (const json::Value* f = v.find("follow"); f && f->isString())
        if (const auto mode = views::followFrom(f->asString())) setFollow(*mode);
    {
        const json::Value* chords = v.find("chords");
        setChordRow(chords && chords->isBool() ? std::optional<bool>(chords->asBool())
                                               : std::nullopt);
    }
    {
        // The clip launcher's entry (gui.md §4.15); none: the project decides.
        const json::Value* clips = v.find("clips");
        const json::Value* shown = clips && clips->isObject() ? clips->find("shown") : nullptr;
        const json::Value* width = clips && clips->isObject() ? clips->find("width") : nullptr;
        const json::Value* full = clips && clips->isObject() ? clips->find("full") : nullptr;
        launcherChoice_ =
            shown && shown->isBool() ? std::optional<bool>(shown->asBool()) : std::nullopt;
        setLauncherWidth(width && width->isNumber() ? static_cast<float>(width->asDouble()) : 0.0f,
                         full && full->isBool() && full->asBool());
    }
    trackLayout().clearTakesExpanded();
    if (const json::Value* ex = v.find("expanded_takes"); ex && ex->isArray())
        for (const json::Value& item : ex->asArray())
            if (const auto id = item.isString() ? model::Id::parse(item.asString()) : std::nullopt)
                trackLayout().setTakesExpanded(*id, true);
}

ui::Cursor ArrangeView::cursorAt(ui::Point p) const { return gestures_->cursorAt(p); }

void ArrangeView::noteDrag(const ui::PointerEvent& e, bool ruler) {
    const bool dragging =
        ruler ? ruler_ && ruler_->dragging() : gestures_->state().drag != ItemGestures::Drag::None;
    if (!dragging || e.type != ui::PointerEvent::Type::Move) {
        // A wheel turn or another node's event leaves a drag's scroll alone.
        if (!dragging || e.type == ui::PointerEvent::Type::Up ||
            e.type == ui::PointerEvent::Type::Cancel)
            edge_ = {};
        return;
    }
    const ui::Rect lanes = lanesRect();
    edge_.vx = ui::edgeScrollSpeed(e.pos.x, lanes.x, lanes.right());
    edge_.vy = !ruler && gestures_->dragCrossesRows()
                   ? ui::edgeScrollSpeed(e.pos.y, lanes.y, lanes.bottom())
                   : 0;
    edge_.last = e;
    edge_.ruler = ruler;
    if (!autoScrolling()) return;
    if (ui::Root* r = root()) r->requestAnimationFrame();  // poll advances it
}

bool ArrangeView::autoScroll(double seconds) {
    if (!autoScrolling()) return false;
    const bool dragging = edge_.ruler ? ruler_ && ruler_->dragging()
                                      : gestures_->state().drag != ItemGestures::Drag::None;
    if (!dragging) {
        edge_ = {};
        return false;
    }
    const Viewport before = view_;
    Viewport next = view_;
    next.startSeconds = std::max(
        0.0, next.startSeconds + static_cast<double>(edge_.vx) * seconds / next.pixelsPerSecond);
    next.scrollY += edge_.vy * static_cast<float>(seconds);
    setViewport(next);  // clamps both
    const auto dx =
        static_cast<float>((view_.startSeconds - before.startSeconds) * view_.pixelsPerSecond);
    const float dy = view_.scrollY - before.scrollY;
    if (dx == 0 && dy == 0) return false;  // at the project's start, or the rows' end
    // The press point goes with the content and the pointer stays where it
    // is: the same event again is a move by what was scrolled.
    if (edge_.ruler) {
        ruler_->scrolled(dx);
        ruler_->handle(edge_.last);
    } else {
        gestures_->scrolled(dx, dy);
        gestures_->pointer(edge_.last);
    }
    return true;
}

bool ArrangeView::onPointer(const ui::PointerEvent& e) {
    // A press on the lanes clears the launcher's selection (gui.md §4.15).
    if (e.type == ui::PointerEvent::Type::Down && lanes_ && lanes_->bounds().contains(e.pos))
        clearLauncherSelection();
    if (e.type != ui::PointerEvent::Type::Wheel) {
        const bool handled = gestures_->pointer(e);
        noteDrag(e, false);
        return handled;
    }
    // The mouse map says what the wheel does: over the TCP its tcp.wheel
    // bindings (plain scrolls the tracks, Ctrl and Alt resize them), over the
    // lanes and the ruler its arrange.wheel ones (plain zooms at the project
    // cursor or the pointer, Shift scrolls in time, Ctrl scrolls the tracks,
    // Alt resizes every track). A sideways wheel scrolls in time where the
    // wheel would zoom.
    Viewport v = view_;
    const bool overTracks =
        (tcp_ && tcp_->bounds().contains(e.pos)) ||
        (launcherPane_ && launcherPane_->visible && launcherPane_->bounds().contains(e.pos));
    const std::string_view behaviour =
        gestures_->mouse.behaviour(overTracks ? "tcp.wheel" : "arrange.wheel", e.mods);
    if (behaviour == "resize_all") {
        std::optional<std::size_t> anchor;
        float fraction = 0;
        for (std::size_t i = 0; i < layout_.rowCount(); ++i) {
            const auto row = rowRect(i);
            if (row.y <= e.pos.y && e.pos.y < row.bottom()) {
                anchor = i;
                fraction = (e.pos.y - row.y) / row.h;
                break;
            }
        }
        // Keep fractional/trackpad deltas: one wheel notch is eight design units.
        layout_.adjustAllTrackHeights(e.wheelY * 8.0f);
        if (anchor)
            v.scrollY = layout_.yOf(*anchor) + fraction * layout_.heightOf(*anchor) -
                        (e.pos.y - lanesRect().y);
        setViewport(v);  // clamps at the timeline's top/end when the anchor cannot hold
        return true;
    }
    if (behaviour == "scroll") {  // vertical scroll
        v.scrollY -= e.wheelY * layout_.globalHeight();
    } else if (behaviour == "scroll_time" || e.wheelX != 0) {  // horizontal scroll
        const float dx = e.wheelX != 0 ? e.wheelX : e.wheelY;
        v.startSeconds =
            std::max(0.0, v.startSeconds - static_cast<double>(dx) * 60.0 / v.pixelsPerSecond);
    } else {  // horizontal zoom, held still at the anchor
        // The anchor goes with the follow mode (gui.md §4.3): the project
        // cursor (the playhead, the transport position) while the view follows
        // it, the mouse pointer when following is off. A cursor outside the
        // lanes' view has nothing to hold still: zoom at the pointer then, as
        // the cursor would otherwise slide out of reach.
        const ui::Rect lanes = lanesRect();
        const double factor = std::pow(1.25, static_cast<double>(e.wheelY));
        if (follow_ != Follow::Off) {
            const float cursorX = playheadX();
            if (cursorX >= lanes.x && cursorX <= lanes.right()) {
                zoomBy(factor, playhead_, cursorX - lanes.x, true);
                return true;
            }
        }
        // Over the TCP the pointer's x is not a time: hold the lanes' left edge.
        const float x = overTracks ? lanes.x : e.pos.x;
        zoomBy(factor, timeAt(x), x - lanes.x, false);
        return true;
    }
    setViewport(v);
    return true;
}

// Horizontal zoom by `factor`, holding `seconds` at `offset` design units
// into the lanes. The timeline's start is a wall: zooming out with the anchor
// near it pulls the anchor left on screen. While nothing but zooming has moved
// the view in time since and the anchor is the same (the same cursor time, or
// a pointer that has not moved), a step keeps the place the anchor first held
// rather than where the wall pushed it, so zooming back in brings it back to
// where it was (as REAPER does).
void ArrangeView::zoomBy(double factor, double seconds, float offset, bool cursor) {
    const float given = offset;
    if (zoomHold_ && zoomHold_->cursor == cursor &&
        (cursor ? zoomHold_->seconds == seconds : zoomHold_->given == given)) {
        seconds = zoomHold_->seconds;
        offset = zoomHold_->offset;
    }
    Viewport v = view_;
    v.pixelsPerSecond = std::clamp(v.pixelsPerSecond * factor, 0.01, 200000.0);
    v.startSeconds = std::max(0.0, seconds - static_cast<double>(offset) / v.pixelsPerSecond);
    setViewport(v);
    zoomHold_ = ZoomHold{seconds, offset, given, cursor};
}

void ArrangeView::zoomStep(bool in) {
    const double zoom = 1.25;
    zoomBy(in ? zoom : 1 / zoom, playhead_, playheadX() - lanesRect().x, true);
}

std::string_view followName(Follow f) {
    switch (f) {
        case Follow::Off:
            return "off";
        case Follow::Page:
            return "page";
        case Follow::Continuous:
            return "continuous";
    }
    return "page";
}

std::optional<Follow> followFrom(std::string_view name) {
    if (name == "off") return Follow::Off;
    if (name == "page") return Follow::Page;
    if (name == "continuous") return Follow::Continuous;
    return std::nullopt;
}

}  // namespace supple::views
