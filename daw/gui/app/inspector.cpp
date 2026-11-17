// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "inspector.h"

#include <set>
#include <utility>

#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"
#include "supple/views/lanes.h"

namespace supple::gui {
namespace {

// The bar beside the title, in the selected thing's colour.
class Swatch : public ui::Node {
  public:
    using Node::Node;
    std::optional<ui::Color> color;
    void paint(ui::Canvas& c) override {
        if (color) c.fillRound(bounds(), 2, *color);
    }
};

const char* stripWord(model::StripType type) {
    switch (type) {
        case model::StripType::Channel:
            return "Channel";
        case model::StripType::Buss:
            return "Buss";
        case model::StripType::Master:
            return "Master";
    }
    return "Track";
}

template <class T>
std::vector<std::pair<model::Id, const T*>> held(const model::IdMap<T>& table,
                                                 const std::vector<std::string>& ids) {
    std::vector<std::pair<model::Id, const T*>> out;
    for (const std::string& text : ids)
        if (const auto id = model::Id::parse(text))
            if (const T* value = table.find(*id)) out.emplace_back(*id, value);
    return out;
}

}  // namespace

InspectorSubject inspectorSubject(const model::Session& s, const UiState& selection,
                                  const ui::Theme& theme) {
    InspectorSubject out;
    const auto tracks = held(s.tracks, selection.selectedTracks);
    const auto items = held(s.items, selection.selectedItems);
    const bool showTracks = !tracks.empty() && (selection.tracksFocused || items.empty());
    if (showTracks && tracks.size() == 1) {
        const auto& [id, track] = tracks.front();
        out.kind = InspectorSubject::Kind::Track;
        out.title = model::trackLabel(s, id);
        out.detail = stripWord(track->type);
        out.color = theme.trackColor(track->color);
    } else if (showTracks) {
        out.kind = InspectorSubject::Kind::Tracks;
        out.title = std::to_string(tracks.size()) + " tracks";
        out.detail = "Selected together";
    } else if (items.size() == 1) {
        const auto& [id, item] = items.front();
        const model::Track* track = s.tracks.find(item->track);
        out.kind = InspectorSubject::Kind::Item;
        out.title = views::Lanes::itemLabel(s, *item);
        if (out.title.empty()) out.title = id.str();
        const model::Take* take = item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
        const char* what = !take || !take->source                   ? "Empty item"
                           : s.media.find(*take->source) != nullptr ? "Audio item"
                                                                    : "MIDI item";
        out.detail = std::string(what) + " on " + model::trackLabel(s, item->track);
        out.color = views::Lanes::itemColor(theme, *item, track,
                                            track ? track->type : model::StripType::Channel);
    } else if (!items.empty()) {
        out.kind = InspectorSubject::Kind::Items;
        out.title = std::to_string(items.size()) + " items";
        std::set<model::Id> on;
        for (const auto& [id, item] : items) on.insert(item->track);
        out.detail = on.size() == 1 ? "On " + model::trackLabel(s, *on.begin())
                                    : "On " + std::to_string(on.size()) + " tracks";
    } else {
        out.title = "Nothing selected";
        out.detail = "Click an item or a track to see it here.";
    }
    return out;
}

Inspector::Inspector(ui::Root& root, engine::EngineApp& app, const UiState& selection)
    : root_(root), app_(app), selection_(selection) {}

std::unique_ptr<ui::Node> Inspector::build() {
    auto panel = std::make_unique<ui::Node>("inspector");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    auto& header = panel->emplace<ui::Node>("inspector.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::content();
    header.layout.gap = 8;
    auto& swatch = header.emplace<Swatch>("inspector.swatch");
    swatch.layout.width = ui::SizeRule::fixed(6);
    swatch_ = &swatch;
    auto& text = header.emplace<ui::Node>("inspector.heading");
    text.layout.dir = ui::Layout::Dir::Column;
    text.layout.width = ui::SizeRule::fraction();
    text.layout.height = ui::SizeRule::content();
    text.layout.gap = 2;
    title_ = &text.emplace<ui::Label>("inspector.title", "", ui::Font::UiBold, "title");
    detail_ =
        &text.emplace<ui::TextBlock>("inspector.detail", "", ui::Font::Ui, "small", "ink-quiet");
    detail_->layout.height = ui::SizeRule::content();
    return panel;
}

void Inspector::setVisible(bool visible) {
    visible_ = visible;
    if (visible) {
        version_ = ~std::uint64_t{0};  // read afresh
        sync();
    }
}

void Inspector::sync() {
    if (!visible_ || !panel_) return;
    const model::Session& s = app_.head();
    if (s.version == version_ && app_.projectGeneration() == generation_ &&
        selection_.selectedTracks == tracks_ && selection_.selectedItems == items_ &&
        selection_.tracksFocused == tracksFocused_)
        return;
    version_ = s.version;
    generation_ = app_.projectGeneration();
    tracks_ = selection_.selectedTracks;
    items_ = selection_.selectedItems;
    tracksFocused_ = selection_.tracksFocused;
    const InspectorSubject subject = inspectorSubject(s, selection_, root_.theme());
    if (title_->text() != subject.title) title_->setText(subject.title);
    if (detail_->text() != subject.detail) detail_->setText(subject.detail);
    static_cast<Swatch*>(swatch_)->color = subject.color;
    swatch_->visible = subject.color.has_value();
    root_.relayout();
    panel_->invalidate();
}

}  // namespace supple::gui
