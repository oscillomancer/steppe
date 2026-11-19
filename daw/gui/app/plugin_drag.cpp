// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plugin_drag.h"

#include <utility>

#include "supple/ui/canvas.h"
#include "supple/ui/theme.h"

namespace supple::gui {

namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

// The feedback: the target outlined, and the plugin's name on a chip beside
// the pointer. The chip is quiet while there is nowhere to drop.
class Ghost : public ui::Node {
  public:
    explicit Ghost(std::shared_ptr<const PluginDrag::View> view)
        : Node("plugin.drag"), view_(std::move(view)) {}
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        const PluginDrag::View& d = *view_;
        if (!d.active) return;
        const ui::Theme& t = c.theme();
        const ui::Color accent = t.color("focus");
        const std::optional<ui::Rect> mark = d.target ? d.target->rect : d.place;
        if (mark) {
            c.fillRect(*mark, accent.withAlpha(0.18f));
            c.strokeRect(mark->inset(1), accent, 2);
        }
        const float size = t.type("small");
        const float w = c.textWidth(d.label, ui::Font::UiBold, size) + 16;
        const ui::Rect chip{d.pos.x + 12, d.pos.y + 10, w, 22};
        c.fillRound(chip, t.radius("s"), t.color("raised"));
        c.strokeRound(chip, t.radius("s"), mark ? accent : t.color("rule"));
        c.text(chip, d.label, ui::Font::UiBold, size, mark ? t.color("ink") : t.color("ink-quiet"),
               ui::kCenter | ui::kMiddle);
    }
    std::string valueText() const override { return view_->label; }

  private:
    std::shared_ptr<const PluginDrag::View> view_;
};

}  // namespace

PluginDrag::PluginDrag(ui::Root& root, cmd::Dispatcher& dispatcher)
    : root_(root), dispatcher_(dispatcher), view_(std::make_shared<View>()) {}

void PluginDrag::addTargets(Finder finder) { finders_.push_back(std::move(finder)); }

void PluginDrag::end() {
    *view_ = View{};
    root_.setDragCursor(std::nullopt);
    ghost_.reset();
    root_.damageAll();
}

// The pointer while something is carried: the move cursor where it would
// land and over the place it came from, "not allowed" where nothing takes it.
void PluginDrag::showCursor(bool lands, ui::Point at) {
    const bool atHome = home && home().contains(at);
    root_.setDragCursor(lands || atHome ? ui::Cursor::Move : ui::Cursor::NotAllowed);
}

void PluginDrag::drag(const std::string& plugin, const std::string& name, ui::DragPhase phase,
                      ui::Point at) {
    drag(name.empty() ? plugin : name, phase, at, [this, plugin](const model::Id& track) {
        json::Value args = json::Value::object();
        args["track"] = track.str();
        args["plugin"] = plugin;
        const auto added = dispatcher_.execute({"fx.add", std::move(args), kUiOrigin});
        if (onDrop) onDrop(track, added ? Status{} : Status{added.error()});
    });
    if (view_->active) view_->plugin = plugin;
}

void PluginDrag::drag(const std::string& label, ui::DragPhase phase, ui::Point at, Drop drop) {
    View& d = *view_;
    if (phase == ui::DragPhase::Cancel) return end();
    if (phase == ui::DragPhase::Begin) {
        end();
        d.active = true;
        d.label = label;
        ghost_ = root_.addOverlay(std::make_unique<Ghost>(view_));
    }
    if (!d.active) return;
    d.pos = at;
    d.target.reset();
    for (const Finder& find : finders_)
        if ((d.target = find(at))) break;
    if (phase != ui::DragPhase::End) {
        showCursor(d.target.has_value(), at);
        root_.requestAnimationFrame();
        return;
    }
    const std::optional<TrackTarget> target = d.target;
    end();
    if (target && drop) drop(target->track);  // released over nothing: nothing happens
}

void PluginDrag::dragTo(const std::string& label, ui::DragPhase phase, ui::Point at,
                        const Place& where, const std::function<void(ui::Point)>& drop) {
    View& d = *view_;
    if (phase == ui::DragPhase::Cancel) return end();
    if (phase == ui::DragPhase::Begin) {
        end();
        d.active = true;
        d.label = label;
        ghost_ = root_.addOverlay(std::make_unique<Ghost>(view_));
    }
    if (!d.active) return;
    d.pos = at;
    d.place = where ? where(at) : std::nullopt;
    if (phase != ui::DragPhase::End) {
        showCursor(d.place.has_value(), at);
        root_.requestAnimationFrame();
        return;
    }
    const bool lands = d.place.has_value();
    end();
    if (lands && drop) drop(at);
}

}  // namespace supple::gui
