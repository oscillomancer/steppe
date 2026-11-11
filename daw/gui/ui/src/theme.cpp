// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/theme.h"

#include <charconv>
#include <cmath>
#include <limits>

#include "supple/json/pointer.h"

namespace supple::ui {

std::optional<Color> parseColor(std::string_view text) {
    if (text.size() != 7 || text.front() != '#') return std::nullopt;
    std::uint32_t rgb = 0;
    const auto end = text.data() + text.size();
    auto result = std::from_chars(text.data() + 1, end, rgb, 16);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return Color::hex(rgb);
}

std::string_view paletteName(Palette palette) { return palette == Palette::Warm ? "warm" : "ink"; }

std::optional<Palette> paletteFrom(std::string_view name) {
    if (name == "warm") return Palette::Warm;
    if (name == "ink") return Palette::Ink;
    return std::nullopt;
}

Theme Theme::dark(Palette palette) {
    Theme t;
    t.name_ = palette == Palette::Warm ? "dark-warm" : "dark";
    // Warm graphite ground, ink-toned text, one ink accent; strip types as
    // bindings (gui.md §7). State colours are reserved for state.
    for (auto [k, v] : {std::pair{"ground", 0x1c1a17u},
                        {"panel", 0x24211du},
                        {"raised", 0x2e2a25u},
                        {"rule", 0x3a352fu},
                        {"ink", 0xebe6dau},
                        {"ink-quiet", 0x9d968bu},
                        {"ink-faint", 0x6a645bu},
                        {"accent", 0x7fa3cfu},
                        {"selection", 0x34465du},
                        {"focus", 0x9dbde6u},
                        {"channel", 0x7fa3cfu},
                        {"buss", 0xc9a14au},
                        {"master", 0xc0634fu},
                        {"clip", 0xe0503cu},
                        {"armed", 0xd6453au},
                        {"agent", 0x9b84d6u},
                        {"meter", 0x8fb27au},
                        {"meter-hot", 0xd8b04au},
                        {"mute", 0xd9773fu},
                        {"solo", 0xe0c341u},
                        {"polarity", 0x6db3c2u},
                        {"mono", 0xb48ad0u},
                        {"midi-send", 0x58b9a9u}})
        t.colors_[k] = Color::hex(v);
    // Track colours: muted enough for the graphite ground.
    for (auto [k, v] : {std::pair{"track-red", 0xc96a5eu},
                        {"track-orange", 0xd08a4au},
                        {"track-yellow", 0xc9b04fu},
                        {"track-green", 0x7fa66au},
                        {"track-teal", 0x5fa39bu},
                        {"track-blue", 0x6f95c4u},
                        {"track-violet", 0x9a84c9u},
                        {"track-grey", 0x8f8a82u}})
        t.colors_[k] = Color::hex(v);
    // Modular racks (ADR-0062): a jack's and cable's colour by signal kind,
    // the hole of a jack, and a module's face. Distinct in hue and close in
    // weight, so no kind shouts; none is a state colour.
    for (auto [k, v] : {std::pair{"signal-audio", 0xd9a75au},
                        {"signal-pitch", 0x6f9fd6u},
                        {"signal-gate", 0xd56f5fu},
                        {"signal-cv", 0xa68bd9u},
                        {"signal-unipolar", 0x86b56fu},
                        {"signal-phase", 0x5db2aau},
                        {"jack-hole", 0x0d0c0au},
                        {"module", 0x2b2823u},
                        {"module-title", 0x35312bu},
                        {"module-well", 0x181613u}})
        t.colors_[k] = Color::hex(v);
    // Warm: an orange ink for the accent, focus and selection; strip types keep
    // their colours (channel blue, buss amber, master red).
    if (palette == Palette::Warm)
        for (auto [k, v] :
             {std::pair{"accent", 0xe08a3cu}, {"focus", 0xf0a35cu}, {"selection", 0x4a3525u}})
            t.colors_[k] = Color::hex(v);
    for (auto [k, v] : {std::pair{"caption", 11.0f},
                        {"small", 12.0f},
                        {"body", 14.0f},
                        {"title", 16.0f},
                        {"display", 20.0f},
                        {"mono", 13.0f}})
        t.type_[k] = v;
    for (auto [k, v] :
         {std::pair{"xs", 4.0f}, {"s", 8.0f}, {"m", 12.0f}, {"l", 16.0f}, {"xl", 24.0f}})
        t.space_[k] = v;
    for (auto [k, v] : {std::pair{"s", 3.0f}, {"m", 6.0f}, {"l", 10.0f}}) t.radius_[k] = v;
    return t;
}

Theme Theme::light(Palette palette) {
    Theme t = dark(Palette::Ink);
    t.name_ = palette == Palette::Warm ? "light-warm" : "light";
    // Off-white paper, ink black; the same accents slightly deepened.
    for (auto [k, v] : {std::pair{"ground", 0xf3efe6u},
                        {"panel", 0xebe6dbu},
                        {"raised", 0xfaf7f0u},
                        {"rule", 0xd3ccbeu},
                        {"ink", 0x1f1c18u},
                        {"ink-quiet", 0x6b655cu},
                        {"ink-faint", 0x9c958au},
                        {"accent", 0x3f6ea8u},
                        {"selection", 0xcdd9e8u},
                        {"focus", 0x3f6ea8u},
                        {"channel", 0x3f6ea8u},
                        {"buss", 0xa57d24u},
                        {"master", 0xa4442fu},
                        {"mute", 0xc0612au},
                        {"solo", 0xa8890fu},
                        {"polarity", 0x2c7c8cu},
                        {"mono", 0x8456a6u}})
        t.colors_[k] = Color::hex(v);
    for (auto [k, v] : {std::pair{"track-red", 0xb4493cu},
                        {"track-orange", 0xb8692au},
                        {"track-yellow", 0xa88a1eu},
                        {"track-green", 0x5a8a45u},
                        {"track-teal", 0x3d8279u},
                        {"track-blue", 0x456fa3u},
                        {"track-violet", 0x7461a8u},
                        {"track-grey", 0x77716au}})
        t.colors_[k] = Color::hex(v);
    for (auto [k, v] : {std::pair{"signal-audio", 0xa8741eu},
                        {"signal-pitch", 0x3a69a5u},
                        {"signal-gate", 0xb2483au},
                        {"signal-cv", 0x7458a9u},
                        {"signal-unipolar", 0x4c8a3bu},
                        {"signal-phase", 0x2a7f78u},
                        {"jack-hole", 0x26221du},
                        {"module", 0xfffdf9u},
                        {"module-title", 0xebe5d9u},
                        {"module-well", 0xe2dccfu}})
        t.colors_[k] = Color::hex(v);
    if (palette == Palette::Warm)
        for (auto [k, v] :
             {std::pair{"accent", 0xb4601eu}, {"focus", 0xb4601eu}, {"selection", 0xf0dcc8u}})
            t.colors_[k] = Color::hex(v);
    return t;
}

Result<Theme> Theme::fromJson(const json::Value& document, const Theme& baseTheme) {
    if (!document.isObject() || !document.find("schema") || !document.find("schema")->isString() ||
        document.find("schema")->asString() != "supple.theme/1.0")
        return Error("theme.schema", "expected a supple.theme/1.0 document", "/schema",
                     R"({"schema": "supple.theme/1.0", "colors": {"accent": "#7fa3cf"}})");
    Theme t = baseTheme;
    if (const auto* n = document.find("name"); n && n->isString()) t.name_ = n->asString();
    if (const auto* colors = document.find("colors")) {
        if (!colors->isObject())
            return Error("theme.colors", "colors must be an object", "/colors");
        for (const auto& m : colors->asObject()) {
            const std::string path = "/colors/" + json::pointerEscape(m.key);
            auto colour = m.value.isString() ? parseColor(m.value.asString()) : std::nullopt;
            if (!colour) return Error("theme.color", "a colour is \"#rrggbb\"", path);
            t.colors_[m.key] = *colour;
        }
    }
    for (auto [section, target] :
         {std::pair{"type", &t.type_}, {"space", &t.space_}, {"radius", &t.radius_}}) {
        const auto* block = document.find(section);
        if (!block) continue;
        if (!block->isObject())
            return Error("theme.section", std::string(section) + " must be an object",
                         std::string("/") + section);
        for (const auto& m : block->asObject()) {
            if (!m.value.isNumber() || !std::isfinite(m.value.asDouble()) ||
                m.value.asDouble() < 0 ||
                m.value.asDouble() > static_cast<double>(std::numeric_limits<float>::max()) ||
                (m.value.asDouble() > 0 &&
                 m.value.asDouble() <
                     static_cast<double>(std::numeric_limits<float>::denorm_min())))
                return Error("theme.value", "a size is a finite non-negative float",
                             std::string("/") + section + "/" + json::pointerEscape(m.key));
            (*target)[m.key] = static_cast<float>(m.value.asDouble());
        }
    }
    return t;
}

std::optional<Color> Theme::trackColor(std::string_view name) const {
    if (name.starts_with('#')) return parseColor(name);
    if (name.empty()) return std::nullopt;
    auto it = colors_.find("track-" + std::string(name));
    if (it == colors_.end()) return std::nullopt;
    return it->second;
}

Color Theme::color(std::string_view token) const {
    auto it = colors_.find(token);
    return it != colors_.end() ? it->second : Color::hex(0xff00ff);
}

float Theme::type(std::string_view token) const {
    auto it = type_.find(token);
    return it != type_.end() ? it->second : 14.0f;
}

float Theme::space(std::string_view token) const {
    auto it = space_.find(token);
    return it != space_.end() ? it->second : 8.0f;
}

float Theme::radius(std::string_view token) const {
    auto it = radius_.find(token);
    return it != radius_.end() ? it->second : 3.0f;
}

}  // namespace supple::ui
