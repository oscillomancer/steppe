// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_device.h"

#include <algorithm>
#include <cmath>

#include "rack_scene.h"
#include "supple/ui/canvas.h"

namespace supple::gui {
namespace {

constexpr float kPad = 10, kHeader = 28, kCaption = 18, kCellW = 104, kCellH = 64, kKnob = 32,
                kGroupGap = 6;

// `s` cut to `width` with an ellipsis.
std::string fit(const ui::Canvas& c, const std::string& s, ui::Font font, float size, float width) {
    if (c.textWidth(s, font, size) <= width) return s;
    std::string cut = s;
    while (!cut.empty()) {
        std::size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80) --n;  // UTF-8
        cut.resize(n);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        if (c.textWidth(cut + "…", font, size) <= width) return cut + "…";
    }
    return "…";
}

}  // namespace

// A knob that also opens its menu on a right-click.
class RackDevice::MacroKnob : public ui::Knob {
  public:
    MacroKnob(std::string id, std::function<void(ui::Point)> menu)
        : ui::Knob(std::move(id)), menu_(std::move(menu)) {}
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3) {
            if (menu_) menu_(e.pos);
            return true;
        }
        return ui::Knob::onPointer(e);
    }

  private:
    std::function<void(ui::Point)> menu_;
};

RackDevice::RackDevice(engine::EngineApp& app, model::Id rack, Hooks hooks)
    : Node("fx.rack." + rack.str()), app_(app), rack_(rack), hooks_(std::move(hooks)) {
    layout.height = ui::SizeRule::content();
    const model::Session& s = app_.head();
    title_ = model::rackLabel(s, rack_);
    catalog_ = racks::catalog(app_);
    open_ = &emplace<ui::Button>("fx.rack.open." + rack.str(), "Open", [this] {
        if (hooks_.open) hooks_.open(rack_);
    });
    if (!catalog_) return;
    if (const rack::Catalog::Entry* e = catalog_->find(racks::kMacroModule)) macroType_ = e->type;
    for (const racks::MacroGroup& g : racks::macroGroups(s, *catalog_, rack_, hooks_.paramName)) {
        groups_.push_back({g.title, knobs_.size(), g.knobs.size()});
        macros_.push_back(g.module);
        for (const racks::MacroKnob& mk : g.knobs) {
            Knob k;
            k.module = g.module;
            k.param = mk.param;
            k.key = mk.key;
            k.group = g.title;
            k.name = mk.name;
            knobs_.push_back(std::move(k));
        }
    }
    telemetry_.resize(macros_.size());
    for (std::size_t i = 0; i < knobs_.size(); ++i) {
        Knob& k = knobs_[i];
        auto& knob = emplace<MacroKnob>("fx.rack.knob." + k.module.str() + "." + k.key,
                                        [this, i](ui::Point at) { openMenu(knobs_[i], at); });
        knob.wheel = false;  // the chain scrolls
        knob.onChange = [this, i](double position, ui::Gesture g) { turn(knobs_[i], position, g); };
        k.knob = &knob;
        if (const sd::ParamModel* p = paramOf(k)) knob.setDefault(rack::knobPosition(*p, p->def));
    }
    follow();
}

const sd::ParamModel* RackDevice::paramOf(const Knob& k) const {
    if (!macroType_ || k.param < 0 ||
        static_cast<std::size_t>(k.param) >= macroType_->params().size())
        return nullptr;
    return &macroType_->params()[static_cast<std::size_t>(k.param)];
}

void RackDevice::show(Knob& k, double physical) {
    const sd::ParamModel* p = paramOf(k);
    if (!p || !k.knob) return;
    k.physical = physical;
    k.knob->setValue(rack::knobPosition(*p, physical));
    k.value = paramText(*p, physical);
}

bool RackDevice::follow() {
    const model::Session& s = app_.head();
    bool moved = false;
    for (std::size_t g = 0; g < macros_.size(); ++g) {
        const rack::Instance* in = app_.moduleInstance(macros_[g]);
        const bool live = in && in->readTelemetry(telemetry_[g]);
        const bool known = in && telemetry_[g].serial > 0;
        const model::RackModule* m = s.modules.find(macros_[g]);
        for (Knob& k : knobs_) {
            if (k.module != macros_[g] || (k.knob && k.knob->dragging())) continue;
            const sd::ParamModel* p = paramOf(k);
            if (!p) continue;
            double v = p->def;
            if (known && static_cast<std::size_t>(k.param) < telemetry_[g].params.size())
                v = telemetry_[g].params[static_cast<std::size_t>(k.param)];
            else if (m)
                v = racks::paramValue(*p, m->params.isObject() ? m->params.find(p->key) : nullptr);
            if (v == k.physical && !k.value.empty()) continue;
            show(k, v);
            moved = true;
        }
        (void)live;
    }
    if (moved) invalidate();
    return moved;
}

void RackDevice::turn(Knob& k, double position, ui::Gesture g) {
    if (g == ui::Gesture::Begin) {
        ++gesture_;
        return;
    }
    if (g == ui::Gesture::End) return;
    const sd::ParamModel* p = paramOf(k);
    if (!p) return;
    const double physical = rack::knobValue(*p, position);
    k.physical = physical;
    k.value = paramText(*p, physical);
    invalidate();
    json::Value params = json::Value::object();
    params[p->key] = racks::paramJson(*p, physical);
    json::Value args = json::Value::object();
    args["module"] = k.module.str();
    args["params"] = std::move(params);
    if (hooks_.exec)
        hooks_.exec("module.set", std::move(args),
                    g == ui::Gesture::Move ? "rack.macro:" + k.module.str() + ":" + p->key + ":" +
                                                 std::to_string(gesture_)
                                           : std::string());
}

void RackDevice::openMenu(const Knob& k, ui::Point at) {
    const sd::ParamModel* p = paramOf(k);
    if (!p || !hooks_.menu) return;
    const model::Id module = k.module;
    const int param = k.param;
    auto exec = [this](const std::string& command, json::Value args) {
        if (hooks_.exec) hooks_.exec(command, std::move(args), {});
    };
    racks::KnobMenuHooks mh;
    mh.exec = exec;
    mh.set = [this, module, param](double physical) {
        for (Knob& x : knobs_)
            if (x.module == module && x.param == param)
                if (const sd::ParamModel* q = paramOf(x))
                    turn(x, rack::knobPosition(*q, physical), ui::Gesture::Set);
    };
    mh.learn = [this, module, title = k.group, p, exec] {
        if (hooks_.learn) hooks_.learn(racks::knobLearn(app_.head(), module, title, *p, exec));
    };
    hooks_.menu(racks::knobMenu(app_.head(), module, *p, k.physical, std::move(mh)), at);
}

int RackDevice::columns(float width) const {
    return std::max(1, static_cast<int>((width - 2 * kPad) / kCellW));
}

ui::Size RackDevice::measure(const ui::Canvas&, ui::Size available) {
    float h = kPad + kHeader;
    const int cols = columns(available.w);
    for (const Group& g : groups_) {
        const auto rows = (static_cast<int>(g.count) + cols - 1) / cols;
        h += kGroupGap + kCaption + static_cast<float>(rows) * kCellH;
    }
    return {available.w, h + kPad};
}

void RackDevice::arrange(const ui::Canvas& c, const ui::Rect& b) {
    setBounds(b);
    const ui::Size button = open_->measure(c, {120, 24});
    const ui::Rect openRect{b.right() - kPad - button.w, b.y + kPad + (kHeader - 24) / 2, button.w,
                            24};
    open_->arrange(c, openRect);
    cells_.assign(knobs_.size(), {});
    captions_.assign(groups_.size(), 0);
    const int cols = columns(b.w);
    float y = b.y + kPad + kHeader;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const Group& g = groups_[gi];
        y += kGroupGap;
        captions_[gi] = y;
        y += kCaption;
        for (std::size_t i = 0; i < g.count; ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const ui::Rect cell{b.x + kPad + static_cast<float>(col) * kCellW,
                                y + static_cast<float>(row) * kCellH, kCellW, kCellH};
            cells_[g.first + i] = cell;
            if (MacroKnob* k = knobs_[g.first + i].knob)
                k->arrange(c, {cell.x + (cell.w - kKnob) / 2, cell.y + 2, kKnob, kKnob});
        }
        y += static_cast<float>((static_cast<int>(g.count) + cols - 1) / cols) * kCellH;
    }
}

void RackDevice::paint(ui::Canvas& c) {
    const ui::Theme& t = c.theme();
    const ui::Rect b = bounds();
    c.fillRound(b, t.radius("s"), t.color("module"));
    c.strokeRound(b.inset(0.5f), t.radius("s"), t.color("rule"), 1);
    const ui::Rect head{b.x + kPad, b.y + kPad, b.w - 3 * kPad - open_->bounds().w, kHeader};
    const float nameW = c.textWidth(title_, ui::Font::UiBold, 13);
    c.text(head, fit(c, title_, ui::Font::UiBold, 13, head.w - 50), ui::Font::UiBold, 13,
           t.color("ink"), ui::kLeft | ui::kMiddle);
    c.text({head.x + std::min(nameW, head.w - 50) + 8, head.y, 50, head.h}, "rack", ui::Font::Ui,
           11, t.color("ink-faint"), ui::kLeft | ui::kMiddle);
    for (std::size_t gi = 0; gi < groups_.size() && gi < captions_.size(); ++gi) {
        std::string caption = groups_[gi].title;
        for (char& ch : caption)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        c.text({b.x + kPad, captions_[gi], b.w - 2 * kPad, kCaption},
               fit(c, caption, ui::Font::UiBold, 9.5f, b.w - 2 * kPad), ui::Font::UiBold, 9.5f,
               t.color("ink-faint"), ui::kLeft | ui::kMiddle);
    }
    for (std::size_t i = 0; i < knobs_.size() && i < cells_.size(); ++i) {
        const ui::Rect cell = cells_[i];
        const Knob& k = knobs_[i];
        const float w = cell.w - 8;
        c.text({cell.x + 4, cell.y + kKnob + 5, w, 14}, fit(c, k.name, ui::Font::Ui, 11, w),
               ui::Font::Ui, 11, t.color("ink"), ui::kCenter | ui::kMiddle);
        c.text({cell.x + 4, cell.y + kKnob + 19, w, 13}, fit(c, k.value, ui::Font::Mono, 10, w),
               ui::Font::Mono, 10, t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
    }
}

std::string RackDevice::valueText() const {
    std::string out = title_;
    for (const Knob& k : knobs_) out += " | " + k.name + " " + k.value;
    return out;
}

std::vector<RackDevice::Shown> RackDevice::shown() const {
    std::vector<Shown> out;
    for (const Knob& k : knobs_) out.push_back({k.group, k.name, k.value, k.module, k.key});
    return out;
}

std::optional<ui::Point> RackDevice::knobAt(const std::string& name) const {
    for (const Knob& k : knobs_)
        if (k.name == name && k.knob) {
            const ui::Rect r = k.knob->bounds();
            return ui::Point{r.x + r.w / 2, r.y + r.h / 2};
        }
    return std::nullopt;
}

}  // namespace supple::gui
