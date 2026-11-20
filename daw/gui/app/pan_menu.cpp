// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "pan_menu.h"

namespace supple::gui {

ui::PanControl::Split panSplit(const model::Track& track) {
    if (track.type == model::StripType::Master) return ui::PanControl::Split::None;
    switch (track.panMode) {
        case model::PanMode::Dual:
            return ui::PanControl::Split::Dual;
        case model::PanMode::Width:
            return ui::PanControl::Split::Width;
        case model::PanMode::Pan:
        default:
            return ui::PanControl::Split::None;
    }
}

void showPan(ui::PanControl& control, const model::Track& track) {
    const auto split = panSplit(track);
    control.setSplit(split);
    if (split == ui::PanControl::Split::Dual) {
        control.setValue(track.panLeft);
        control.setSecond(track.panRight);
    } else {
        control.setValue(track.pan);
        control.setSecond(track.width);
    }
}

const char* panField(const model::Track& track, int part) {
    const auto split = panSplit(track);
    if (split == ui::PanControl::Split::Dual) return part == 0 ? "pan_left" : "pan_right";
    if (split == ui::PanControl::Split::Width && part == 1) return "width";
    return "pan";
}

}  // namespace supple::gui
