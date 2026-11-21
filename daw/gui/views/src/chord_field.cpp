// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/chord_field.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <memory>
#include <utility>

#include "supple/model/context.h"
#include "supple/time/timeline.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"

namespace supple::views {

std::string chordSymbolAt(const model::Session& s, double beat) {
    for (const auto& entry : s.contextView->chords)
        if (entry.beat == beat)
            if (const model::ContextPoint* p = s.context.find(entry.id)) return p->symbol;
    return {};
}

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

void openChordField(ui::Root& root, ChordFieldHost host, double beat, ui::Point at) {
    auto field = std::make_unique<ui::TextField>(
        "chord-field", host.symbolAt ? host.symbolAt(beat) : chordSymbolAt(host.session(), beat));
    field->layout.width = ui::SizeRule::fixed(140);
    field->selectAll();
    auto write = [run = host.run, beat](const std::string& text) {
        json::Value args = json::Value::object();
        args["at"] = beat;
        args["symbol"] = text;
        return run("chord.set", std::move(args));
    };
    field->onSubmit = [&root, write](const std::string& text) {
        if (write(text)) root.closePopup();
    };
    ui::TextField* self = field.get();
    field->onOtherKey = [&root, host, write, beat, at, self](const ui::KeyEvent& e) {
        if (e.key != SDLK_TAB || (e.mods & ui::kShift)) return false;
        const std::string text = self->text();
        if (!text.empty() && !write(text)) return true;
        const time::TimeMap& tm = *host.session().timeMap;
        const double next = tm.beatOfBar(tm.barBeat(beat).bar + 1);
        const ui::Point there{host.xAt(next), at.y};
        ChordFieldHost again = host;  // the field and this handler go with the popup
        root.closePopup();
        openChordField(root, std::move(again), next, there);
        return true;
    };
    root.openPopup(std::move(field), {at.x, at.y, 1, 1});
}

}  // namespace supple::views
