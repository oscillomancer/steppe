// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_face.h"

#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace supple::gui {
namespace {

namespace sd = script::detail;
namespace S = script;

constexpr std::int64_t kBudget = std::int64_t{1} << 22;
constexpr int kTextRoom = 1 << 14;

ui::Color mix(ui::Color a, ui::Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}

// The script palette (script.h Theme) in the app's theme: a display looks
// like the module it is on, in either theme.
ui::Color colourOf(int c, const ui::Theme& t) {
    if (c < S::Theme::ground) return ui::Color::hex(static_cast<std::uint32_t>(c) & 0xffffffu);
    switch (c) {
        case S::Theme::ground:
            return t.color("module");
        case S::Theme::surface:
            return t.color("module-title");
        case S::Theme::raised:
            return mix(t.color("module-title"), t.color("ink"), 0.08f);
        case S::Theme::recess:
            return t.color("module-well");
        case S::Theme::line:
            return t.color("rule");
        case S::Theme::lineLit:
            return mix(t.color("rule"), t.color("ink"), 0.25f);
        case S::Theme::textFaint:
            return t.color("ink-faint");
        case S::Theme::textMuted:
            return t.color("ink-quiet");
        case S::Theme::text:
            return t.color("ink");
        case S::Theme::accent:
            return t.color("accent");
        case S::Theme::accent2:
            return t.color("signal-pitch");
        case S::Theme::modulation:
            return t.color("signal-cv");
        case S::Theme::alert:
            return t.color("clip");
        default:
            return t.color("ink");
    }
}

NVGcolor nvg(ui::Color c, float alpha) { return nvgRGBAf(c.r, c.g, c.b, c.a * alpha); }

class NvgPainter final : public sd::Painter {
  public:
    NvgPainter(NVGcontext* vg, const ui::Theme& t, const gui::Fonts& f, double w, double h)
        : vg_(vg), t_(t), f_(f), w_(w), h_(h) {
        nvgFontFaceId(vg_, f_.ui);
        nvgFontSize(vg_, 11);
        nvgFillColor(vg_, nvg(t_.color("ink"), 1));
        nvgStrokeColor(vg_, nvg(t_.color("ink"), 1));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }
    double width() const override { return w_; }
    double height() const override { return h_; }
    void save() override {
        nvgSave(vg_);
        stack_.push_back({fill_, stroke_, alpha_, gradient_});
    }
    void restore() override {
        nvgRestore(vg_);
        if (stack_.empty()) return;
        std::tie(fill_, stroke_, alpha_, gradient_) = stack_.back();
        stack_.pop_back();
    }
    void translate(double x, double y) override { nvgTranslate(vg_, F(x), F(y)); }
    void rotate(double r) override { nvgRotate(vg_, F(r)); }
    void scale(double x, double y) override { nvgScale(vg_, F(x), F(y)); }
    void fillStyle(int c) override {
        fill_ = colourOf(c, t_);
        gradient_ = false;
        nvgFillColor(vg_, nvg(fill_, alpha_));
    }
    void strokeStyle(int c) override {
        stroke_ = colourOf(c, t_);
        nvgStrokeColor(vg_, nvg(stroke_, alpha_));
    }
    void globalAlpha(double a) override {
        alpha_ = static_cast<float>(std::clamp(std::isfinite(a) ? a : 1.0, 0.0, 1.0));
        if (!gradient_) nvgFillColor(vg_, nvg(fill_, alpha_));
        nvgStrokeColor(vg_, nvg(stroke_, alpha_));
    }
    void lineWidth(double w) override { nvgStrokeWidth(vg_, F(std::max(0.0, w))); }
    void lineCap(S::LineCap c) override {
        nvgLineCap(vg_, c == S::LineCap::Round    ? NVG_ROUND
                        : c == S::LineCap::Square ? NVG_SQUARE
                                                  : NVG_BUTT);
    }
    void lineJoin(S::LineJoin j) override {
        nvgLineJoin(vg_, j == S::LineJoin::Round   ? NVG_ROUND
                         : j == S::LineJoin::Bevel ? NVG_BEVEL
                                                   : NVG_MITER);
    }
    void linearGradient(double x0, double y0, double x1, double y1, int from, int to) override {
        gradient_ = true;
        nvgFillPaint(
            vg_, nvgLinearGradient(vg_, F(x0), F(y0), F(x1), F(y1), nvg(colourOf(from, t_), alpha_),
                                   nvg(colourOf(to, t_), alpha_)));
    }
    void radialGradient(double x, double y, double inner, double outer, int from, int to) override {
        gradient_ = true;
        nvgFillPaint(
            vg_, nvgRadialGradient(vg_, F(x), F(y), F(inner), F(outer),
                                   nvg(colourOf(from, t_), alpha_), nvg(colourOf(to, t_), alpha_)));
    }
    void fillRect(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, F(x), F(y), F(w), F(h));
        nvgFill(vg_);
    }
    void strokeRect(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, F(x), F(y), F(w), F(h));
        nvgStroke(vg_);
    }
    void clearRect(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, F(x), F(y), F(w), F(h));
        nvgFillColor(vg_, nvg(t_.color("module"), 1));
        nvgFill(vg_);
        if (!gradient_) nvgFillColor(vg_, nvg(fill_, alpha_));
    }
    void beginPath() override { nvgBeginPath(vg_); }
    void closePath() override { nvgClosePath(vg_); }
    void moveTo(double x, double y) override { nvgMoveTo(vg_, F(x), F(y)); }
    void lineTo(double x, double y) override { nvgLineTo(vg_, F(x), F(y)); }
    void bezierCurveTo(double a, double b, double c, double d, double x, double y) override {
        nvgBezierTo(vg_, F(a), F(b), F(c), F(d), F(x), F(y));
    }
    void quadraticCurveTo(double cx, double cy, double x, double y) override {
        nvgQuadTo(vg_, F(cx), F(cy), F(x), F(y));
    }
    void arc(double x, double y, double r, double a0, double a1) override {
        nvgArc(vg_, F(x), F(y), F(std::max(0.0, r)), F(a0), F(a1), NVG_CW);
    }
    void arcTo(double x1, double y1, double x2, double y2, double r) override {
        nvgArcTo(vg_, F(x1), F(y1), F(x2), F(y2), F(std::max(0.0, r)));
    }
    void rect(double x, double y, double w, double h) override {
        nvgRect(vg_, F(x), F(y), F(w), F(h));
    }
    void roundRect(double x, double y, double w, double h, double r) override {
        nvgRoundedRect(vg_, F(x), F(y), F(w), F(h), F(std::max(0.0, r)));
    }
    void ellipse(double x, double y, double rx, double ry) override {
        nvgEllipse(vg_, F(x), F(y), F(rx), F(ry));
    }
    void circle(double x, double y, double r) override {
        nvgCircle(vg_, F(x), F(y), F(std::max(0.0, r)));
    }
    void fill() override { nvgFill(vg_); }
    void stroke() override { nvgStroke(vg_); }
    void clip(double x, double y, double w, double h) override {
        nvgIntersectScissor(vg_, F(x), F(y), F(std::max(0.0, w)), F(std::max(0.0, h)));
    }
    void font(Face face, double size) override {
        // The app has no serif: names and headings take the bold face.
        nvgFontFaceId(vg_, face == Face::Mono                          ? f_.mono
                           : face == Face::Bold || face == Face::Serif ? f_.uiBold
                                                                       : f_.ui);
        nvgFontSize(vg_, F(std::clamp(size, 1.0, 200.0)));
    }
    void textAlign(S::TextAlign a) override {
        align_ = a == S::TextAlign::Center  ? NVG_ALIGN_CENTER
                 : a == S::TextAlign::Right ? NVG_ALIGN_RIGHT
                                            : NVG_ALIGN_LEFT;
        nvgTextAlign(vg_, align_ | baseline_);
    }
    void textBaseline(S::TextBaseline b) override {
        baseline_ = b == S::TextBaseline::Top      ? NVG_ALIGN_TOP
                    : b == S::TextBaseline::Middle ? NVG_ALIGN_MIDDLE
                    : b == S::TextBaseline::Bottom ? NVG_ALIGN_BOTTOM
                                                   : NVG_ALIGN_BASELINE;
        nvgTextAlign(vg_, align_ | baseline_);
    }
    void fillText(const char* text, double x, double y) override {
        if (text) nvgText(vg_, F(x), F(y), text, nullptr);
    }
    double measureText(const char* text) override {
        return text ? static_cast<double>(nvgTextBounds(vg_, 0, 0, text, nullptr, nullptr)) : 0.0;
    }
    void panel(double x, double y, double w, double h, const char* title) override {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, F(x), F(y), F(w), F(h), 3);
        nvgFillColor(vg_, nvg(t_.color("module-title"), alpha_));
        nvgFill(vg_);
        if (title && *title) {
            nvgSave(vg_);
            nvgFontFaceId(vg_, f_.uiBold);
            nvgFontSize(vg_, 9);
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg_, nvg(t_.color("ink-quiet"), alpha_));
            nvgText(vg_, F(x + 5), F(y + 4), title, nullptr);
            nvgRestore(vg_);
        }
        restoreFill();
    }
    void well(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, F(x), F(y), F(w), F(h), 2);
        nvgFillColor(vg_, nvg(t_.color("module-well"), alpha_));
        nvgFill(vg_);
        restoreFill();
    }
    void meter(double db, double x, double y, double w, double h) override {
        well(x, y, w, h);
        const double t = std::clamp(((std::isfinite(db) ? db : -72.0) + 72.0) / 78.0, 0.0, 1.0);
        const bool across = w > h;
        nvgBeginPath(vg_);
        if (across)
            nvgRect(vg_, F(x), F(y), F(w * t), F(h));
        else
            nvgRect(vg_, F(x), F(y + h * (1 - t)), F(w), F(h * t));
        nvgFillColor(vg_, nvg(db > 0 ? t_.color("clip") : t_.color("meter"), alpha_));
        nvgFill(vg_);
        restoreFill();
    }
    void frequencyGrid(double x, double y, double w, double h, double lo, double hi) override {
        if (!(lo > 0) || !(hi > lo)) return;
        nvgSave(vg_);
        nvgStrokeWidth(vg_, 1);
        for (double decade = std::pow(10.0, std::floor(std::log10(lo))); decade <= hi; decade *= 10)
            for (double m : {1.0, 2.0, 5.0}) {
                const double f = decade * m;
                if (f < lo || f > hi) continue;
                const double gx = x + w * std::log(f / lo) / std::log(hi / lo);
                nvgBeginPath(vg_);
                nvgMoveTo(vg_, F(gx), F(y));
                nvgLineTo(vg_, F(gx), F(y + h));
                nvgStrokeColor(vg_, nvg(m == 1.0 ? mix(t_.color("rule"), t_.color("ink"), 0.25f)
                                                 : t_.color("rule"),
                                        alpha_));
                nvgStroke(vg_);
            }
        nvgRestore(vg_);
    }
    void levelGrid(double x, double y, double w, double h, double lo, double hi,
                   double step) override {
        if (!(hi > lo) || !(step > 0) || (hi - lo) / step > 200) return;
        nvgSave(vg_);
        nvgStrokeWidth(vg_, 1);
        for (double db = lo; db <= hi + 1e-9; db += step) {
            const double gy = y + h * (1 - (db - lo) / (hi - lo));
            nvgBeginPath(vg_);
            nvgMoveTo(vg_, F(x), F(gy));
            nvgLineTo(vg_, F(x + w), F(gy));
            nvgStrokeColor(vg_,
                           nvg(std::fabs(db) < 1e-9 ? mix(t_.color("rule"), t_.color("ink"), 0.25f)
                                                    : t_.color("rule"),
                               alpha_));
            nvgStroke(vg_);
        }
        nvgRestore(vg_);
    }

  private:
    static float F(double v) { return std::isfinite(v) ? static_cast<float>(v) : 0.0f; }
    void restoreFill() {
        if (!gradient_) nvgFillColor(vg_, nvg(fill_, alpha_));
    }
    NVGcontext* vg_;
    const ui::Theme& t_;
    gui::Fonts f_;
    double w_, h_;
    ui::Color fill_ = {1, 1, 1, 1}, stroke_ = {1, 1, 1, 1};
    float alpha_ = 1;
    bool gradient_ = false;
    int align_ = NVG_ALIGN_LEFT, baseline_ = NVG_ALIGN_BASELINE;
    std::vector<std::tuple<ui::Color, ui::Color, float, bool>> stack_;
};

}  // namespace

// What draw() reaches through its record: room for format()'s text, and the
// knobs' names and values as the window says them.
class ModuleFace::Services final : public sd::EditorServices {
  public:
    Services(const rack::ModuleType& t, void* self) : type_(t), self_(self), text_(kTextRoom) {}
    void reset() { used_ = 0; }
    char* text(int bytes) override {
        if (bytes <= 0 || used_ + bytes > kTextRoom) return nullptr;
        char* at = text_.data() + used_;
        used_ += bytes;
        return at;
    }
    const char* paramText(int slot, double value) override {
        if (slot < 0 || static_cast<std::size_t>(slot) >= type_.params().size()) return "";
        const auto& p = type_.params()[static_cast<std::size_t>(slot)];
        // The script reads a percent as a fraction.
        const double physical = p.unit == sd::ParamModel::Unit::Percent ? value * 100 : value;
        const std::string s = gui::paramText(p, physical);
        char* out = text(static_cast<int>(s.size()) + 1);
        if (!out) return "";
        std::memcpy(out, s.c_str(), s.size() + 1);
        return out;
    }
    const char* paramName(int slot) override {
        if (slot < 0 || static_cast<std::size_t>(slot) >= type_.params().size()) return "";
        return type_.params()[static_cast<std::size_t>(slot)].name.c_str();
    }
    double paramNormalized(int slot, double value) override {
        if (slot < 0 || static_cast<std::size_t>(slot) >= type_.params().size()) return 0;
        const auto& p = type_.params()[static_cast<std::size_t>(slot)];
        return rack::knobPosition(p, p.unit == sd::ParamModel::Unit::Percent ? value * 100 : value);
    }
    // pointer()'s edits, as the plugin editor takes them: each knob's last,
    // in physical units, and set in the copy at once.
    void editParam(int slot, Edit kind, double value) override {
        if (!editing_ || slot < 0 || static_cast<std::size_t>(slot) >= type_.params().size())
            return;
        if (kind != Edit::Default && !std::isfinite(value)) return;
        const auto& p = type_.params()[static_cast<std::size_t>(slot)];
        const bool percent = p.unit == sd::ParamModel::Unit::Percent;
        double physical = kind == Edit::Value      ? (percent ? value * 100 : value)
                          : kind == Edit::Position ? rack::knobValue(p, std::clamp(value, 0.0, 1.0))
                                                   : p.def;
        physical = std::clamp(physical, p.min, p.max);
        if (p.kind != sd::ParamModel::Kind::Continuous) physical = std::round(physical);
        const auto& code = *type_.code();
        sd::ParamBinding::setValue(
            *reinterpret_cast<S::Param*>(static_cast<unsigned char*>(self_) +
                                         code.params()[static_cast<std::size_t>(slot)].offset),
            percent ? physical / 100 : physical, false);
        for (ModuleFace::Edit& e : edits_)
            if (e.param == slot) {
                e.value = physical;
                return;
            }
        edits_.push_back({slot, physical});
    }
    void playNote(int, double, bool) override {}  // a module has no keyboard
    void allowEdits(bool on) { editing_ = on; }
    std::vector<ModuleFace::Edit> takeEdits() { return std::exchange(edits_, {}); }

  private:
    const rack::ModuleType& type_;
    void* self_;
    std::vector<char> text_;
    int used_ = 0;
    bool editing_ = false;
    std::vector<ModuleFace::Edit> edits_;
};

std::unique_ptr<ModuleFace> ModuleFace::make(std::shared_ptr<const rack::ModuleType> type) {
    if (!type || !type->code() || (!type->code()->draw() && !type->code()->pointer()))
        return nullptr;
    std::unique_ptr<ModuleFace> f(new ModuleFace());
    const sd::ModuleCode& code = *type->code();
    f->type_ = std::move(type);
    f->self_ = code.create();
    f->record_ = std::make_unique<sd::Instance>();
    f->services_ = std::make_unique<Services>(*f->type_, f->self_);
    f->record_->editor = f->services_.get();
    auto* base = static_cast<unsigned char*>(f->self_);
    for (std::size_t i = 0; i < code.params().size(); ++i)
        sd::ParamBinding::setSlot(*reinterpret_cast<S::Param*>(base + code.params()[i].offset),
                                  static_cast<int>(i));
    sd::Instance::bind(*reinterpret_cast<S::Plugin*>(f->self_), f->record_.get());
    return f;
}

ModuleFace::~ModuleFace() {
    if (self_ && type_ && type_->code()) type_->code()->destroy(self_);
}

void ModuleFace::update(const RackScene::Module& m) {
    const sd::ModuleCode& code = *type_->code();
    auto* base = static_cast<unsigned char*>(self_);
    const auto& params = code.params();
    for (std::size_t i = 0; i < params.size() && i < m.values.size(); ++i) {
        // Knobs as the module hears them: with their cables when it runs.
        const double physical =
            m.live && i < m.telemetry.params.size() ? m.telemetry.params[i] : m.values[i];
        const double value =
            params[i].unit == sd::ParamModel::Unit::Percent ? physical / 100 : physical;
        sd::ParamBinding::setValue(*reinterpret_cast<S::Param*>(base + params[i].offset), value,
                                   false);
    }
    if (!m.live || m.telemetry.serial == shown_) return;
    shown_ = m.telemetry.serial;
    std::size_t at = 0;
    for (const sd::SharedModel& s : code.shared()) {
        const auto bytes = static_cast<std::size_t>(s.bytes);
        if (at + bytes > m.telemetry.shared.size()) break;
        std::memcpy(base + s.offset, m.telemetry.shared.data() + at, bytes);
        at += bytes;
    }
}

void ModuleFace::draw(NVGcontext* vg, const ui::Theme& theme, const gui::Fonts& fonts, double width,
                      double height) {
    if (stopped_ || !type_->code()->draw()) return;
    NvgPainter painter(vg, theme, fonts, width, height);
    S::Canvas canvas;
    sd::Layout::bind(canvas, &painter);
    services_->reset();
    nvgSave(vg);
    {
        sd::CurrentScope scope(record_.get());
        record_->budget = kBudget;
        const std::uint32_t before = record_->status.budgetExceeded.load(std::memory_order_relaxed);
        type_->code()->draw()(record_.get(), self_, &canvas);
        stopped_ = record_->status.budgetExceeded.load(std::memory_order_relaxed) != before;
    }
    nvgRestore(vg);
}

bool ModuleFace::hearsPointer() const { return type_->code()->pointer() != nullptr; }

std::vector<ModuleFace::Edit> ModuleFace::pointer(const Pointer& p) {
    const auto hook = type_->code()->pointer();
    if (!hook || stopped_) return {};
    S::Pointer event;
    sd::Layout::set(event, p.action, p.x, p.y, p.dx, p.dy, p.wheel, p.doubleClick, p.shift, p.ctrl,
                    p.alt);
    services_->reset();
    services_->allowEdits(p.action != S::PointerAction::Move &&
                          p.action != S::PointerAction::Leave);
    {
        sd::CurrentScope scope(record_.get());
        record_->budget = kBudget;
        const std::uint32_t before = record_->status.budgetExceeded.load(std::memory_order_relaxed);
        hook(record_.get(), self_, &event);
        stopped_ = record_->status.budgetExceeded.load(std::memory_order_relaxed) != before;
    }
    services_->allowEdits(false);
    return services_->takeEdits();
}

}  // namespace supple::gui
