// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_labels.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace supple::gui::racks {
namespace {

using rack::PanelItem;
using Kind = PanelItem::Kind;

constexpr float kJackR = static_cast<float>(rack::kJackSize) / 2;
constexpr float kGap = 2;       // between a label and a control beside it
constexpr float kLabelGap = 8;  // between two labels
constexpr float kMaxHalf = 32;  // a label's reach either side of its control
constexpr float kEdge = 2;      // from the module's sides
constexpr float kLabelShort =
    8.75f;  // the shortest line a control beside it makes (fits kLabelMin)

bool isJack(Kind k) { return k == Kind::Input || k == Kind::Output; }
bool overlaps(float a0, float a1, float b0, float b1) { return a0 < b1 && b0 < a1; }

// A label's line: where its control is and where the line starts.
struct Anchor {
    float cx = 0, top = 0;
};
std::optional<Anchor> anchorOf(const PanelItem& item) {
    const float x = static_cast<float>(item.x), y = static_cast<float>(item.y);
    if (item.kind == Kind::Knob) return Anchor{x, y + static_cast<float>(item.w) / 2 + 1.5f};
    if (isJack(item.kind)) return Anchor{x, y + kJackR + 1};
    return std::nullopt;
}

std::string nameOf(const rack::ModuleType& type, const PanelItem& item) {
    if (item.index < 0) return {};
    const auto i = static_cast<std::size_t>(item.index);
    switch (item.kind) {
        case Kind::Knob:
            return i < type.params().size() ? type.params()[i].name : std::string();
        case Kind::Input:
            return i < type.inputs().size() ? type.input(item.index).name : std::string();
        case Kind::Output:
            return i < type.outputs().size() ? type.output(item.index).name : std::string();
        default:
            return {};
    }
}

// The best place to break `s` in two (at a space, else after a '/' or '-'):
// the one that makes the wider line narrowest.
std::optional<std::pair<std::string, std::string>> split(const std::string& s,
                                                         const MeasureText& measure, float size) {
    std::optional<std::pair<std::string, std::string>> best;
    float bestWidth = 0;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        std::string a, b;
        if (s[i] == ' ') {
            a = s.substr(0, i);
            b = s.substr(i + 1);
        } else if (s[i] == '/' || s[i] == '-') {
            a = s.substr(0, i + 1);
            b = s.substr(i + 1);
        } else {
            continue;
        }
        const float w = std::max(measure(a, size), measure(b, size));
        if (!best || w < bestWidth) {
            best = std::pair{std::move(a), std::move(b)};
            bestWidth = w;
        }
    }
    return best;
}

// `s` cut, at a character, to fit `width` with an ellipsis.
std::string elide(const std::string& s, const MeasureText& measure, float size, float width) {
    std::string cut = s;
    while (!cut.empty()) {
        std::size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80) --n;  // UTF-8
        cut.resize(n);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        const std::string shown = cut + "…";
        if (measure(shown, size) <= width) return shown;
    }
    return "…";
}

}  // namespace

ui::Rect footprint(const PanelItem& item) {
    const float x = static_cast<float>(item.x), y = static_cast<float>(item.y);
    if (item.kind == Kind::Knob) {
        const float r = static_cast<float>(item.w) / 2 + 3;  // its steps' dots
        return {x - r, y - r, 2 * r, 2 * r};
    }
    if (isJack(item.kind)) {
        const float r = kJackR + 1;
        return {x - r, y - r, 2 * r, 2 * r};
    }
    return {x, y, static_cast<float>(item.w), static_cast<float>(item.h)};
}

std::vector<FaceLabel> faceLabels(const rack::ModuleType& type, const rack::PanelLayout& face,
                                  const MeasureText& measure) {
    struct Work {
        FaceLabel label;
        Anchor at;
        float want = 0;  // its width at full size
        float band = 0;  // its first line's height
        float left = 0, right = 0;
    };
    std::vector<Work> work;
    for (std::size_t k = 0; k < face.items.size(); ++k) {
        const auto at = anchorOf(face.items[k]);
        if (!at) continue;
        Work w;
        w.label.item = k;
        w.label.text = nameOf(type, face.items[k]);
        if (w.label.text.empty()) continue;
        w.at = *at;
        w.want = measure(w.label.text, kLabelSize);
        work.push_back(std::move(w));
    }
    std::vector<ui::Rect> feet;
    feet.reserve(face.items.size());
    for (const PanelItem& item : face.items) feet.push_back(footprint(item));
    const float width = static_cast<float>(face.width);

    // The first line: as tall as the room under the control, as wide as the
    // controls beside it and its neighbours' labels leave.
    // A control right under it shortens the line however close; one that
    // the label's width would reach shortens it down to kLabelShort, below
    // which the label narrows instead (a control beside the line).
    for (Work& w : work) {
        w.band = kLabelLine;
        const float x0 = w.at.cx - w.want / 2, x1 = w.at.cx + w.want / 2;
        for (std::size_t k = 0; k < feet.size(); ++k) {
            const ui::Rect& f = feet[k];
            if (k == w.label.item || f.y <= w.at.top) continue;
            if (f.x < w.at.cx && f.right() > w.at.cx)
                w.band = std::min(w.band, f.y - w.at.top);
            else if (overlaps(x0, x1, f.x, f.right()) && f.y - w.at.top >= kLabelShort)
                w.band = std::min(w.band, f.y - w.at.top);
        }
        w.left = std::max(kEdge, w.at.cx - kMaxHalf);
        w.right = std::min(width - kEdge, w.at.cx + kMaxHalf);
        for (std::size_t k = 0; k < feet.size(); ++k) {
            const ui::Rect& f = feet[k];
            if (k == w.label.item || !overlaps(w.at.top, w.at.top + w.band, f.y, f.bottom()))
                continue;
            if (f.right() <= w.at.cx) w.left = std::max(w.left, f.right() + kGap);
            if (f.x >= w.at.cx) w.right = std::min(w.right, f.x - kGap);
        }
    }
    // Two labels on one line split the room between them at one point, so
    // neither can reach the other: each gets what it needs (more when an
    // edge pushes it off its centre), and they share what is left or short.
    std::vector<float> left(work.size()), right(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) {
        left[i] = work[i].left;
        right[i] = work[i].right;
    }
    for (std::size_t i = 0; i < work.size(); ++i)
        for (std::size_t j = 0; j < work.size(); ++j) {
            const Work& l = work[i];
            const Work& r = work[j];
            if (r.at.cx - l.at.cx < 1 ||
                !overlaps(l.at.top, l.at.top + l.band, r.at.top, r.at.top + r.band))
                continue;
            const float d = r.at.cx - l.at.cx;
            const float lNeeds = std::max(l.want / 2, l.want - (l.at.cx - l.left));
            const float rNeeds = std::max(r.want / 2, r.want - (r.right - r.at.cx));
            const float spare = d - lNeeds - rNeeds - kLabelGap;
            const float at = spare >= 0 ? l.at.cx + lNeeds + kLabelGap / 2 + spare / 2
                                        : l.at.cx + d * lNeeds / std::max(lNeeds + rNeeds, 1.0f);
            right[i] = std::min(right[i], at - kLabelGap / 2);
            left[j] = std::max(left[j], at + kLabelGap / 2);
        }
    for (std::size_t i = 0; i < work.size(); ++i) {
        work[i].left = left[i];
        work[i].right = right[i];
    }

    // Fit: as is, smaller, on two lines, or cut.
    std::vector<ui::Rect> taken;  // the lines placed so far, for the second lines
    for (const Work& w : work) taken.push_back({w.left, w.at.top, w.right - w.left, w.band});
    std::vector<FaceLabel> out;
    // The largest size from `from` down to the smallest at which `text`
    // fits `room`, or 0. Widths are measured in whole pixels, so it steps.
    auto shrunk = [&](const std::string& text, float from, float room) {
        float z = std::min(from, from * room / std::max(measure(text, from), 1.0f));
        z = std::floor(z * 4) / 4;
        for (; z >= kLabelMin; z -= 0.25f)
            if (measure(text, z) <= room) return z;
        return 0.0f;
    };
    for (Work& w : work) {
        FaceLabel& l = w.label;
        const float room = std::max(0.0f, w.right - w.left);
        l.cell = {w.left, w.at.top, room, w.band};
        // A short line takes a smaller size.
        const float tallest = std::min(kLabelSize, w.band * 0.85f);
        auto place = [&](float textWidth, float height) {
            const float x =
                std::clamp(w.at.cx - textWidth / 2, w.left, std::max(w.left, w.right - textWidth));
            l.box = {x, w.at.top, std::min(textWidth, room), height};
        };
        if (w.want <= room && tallest >= kLabelSize) {
            l.lines = {l.text};
            l.size = kLabelSize;
            place(w.want, w.band);
        } else if (const float s = shrunk(l.text, tallest, room); s > 0) {
            l.lines = {l.text};
            l.size = s;
            place(measure(l.text, s), w.band);
        } else {
            std::optional<std::pair<std::string, std::string>> two;
            float size2 = 0;
            if (w.band >= kLabelLine)
                if (auto parts = split(l.text, measure, kLabelSize)) {
                    const float widest = std::max(measure(parts->first, kLabelSize),
                                                  measure(parts->second, kLabelSize));
                    size2 = std::min(kLabelSize, kLabelSize * room / std::max(widest, 1.0f));
                    const ui::Rect below{w.left, w.at.top + w.band, room, kLabelLine2};
                    bool free = size2 >= kLabelMin;
                    for (std::size_t k = 0; free && k < feet.size(); ++k)
                        if (k != l.item && !feet[k].intersect(below).empty()) free = false;
                    for (const ui::Rect& t : taken)
                        if (free && !t.intersect(below).empty()) free = false;
                    if (free) two = std::move(parts);
                }
            if (two) {
                l.lines = {two->first, two->second};
                l.size = size2;
                place(std::max(measure(two->first, size2), measure(two->second, size2)),
                      w.band + kLabelLine2);
                taken.push_back({w.left, w.at.top + w.band, room, kLabelLine2});
            } else {
                l.size = std::max(std::min(tallest, kLabelMin), 6.0f);
                l.lines = {elide(l.text, measure, l.size, room)};
                place(measure(l.lines[0], l.size), w.band);
            }
        }
        out.push_back(std::move(l));
    }
    return out;
}

}  // namespace supple::gui::racks
