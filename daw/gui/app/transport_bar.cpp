// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "transport_bar.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

#include "playrate_range.h"
#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/controls.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/ui/widgets.h"
#include "tap_tempo_button.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

template <class Base>
class RateMenuControl : public Base {
  public:
    explicit RateMenuControl(std::string id) : Base(std::move(id)) { this->interactive = true; }
    std::function<void()> onMenu;
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button == 3) {
            if (e.type == ui::PointerEvent::Type::Down && onMenu) onMenu();
            return true;
        }
        return Base::onPointer(e);
    }
};

std::string rateText(double rate) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", rate);
    std::string text(buf);
    while (text.size() > 2 && text.back() == '0' && text[text.size() - 2] != '.') text.pop_back();
    return text;
}

std::string timeText(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto totalMs = std::llround(seconds * 1000.0);
    const int minutes = static_cast<int>(totalMs / 60000);
    const int rest = static_cast<int>(totalMs % 60000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d.%03d", minutes, rest / 1000, rest % 1000);
    return buf;
}

std::string barsText(double beat, const time::TimeMap& tm) {
    const time::BarBeat bb = tm.barBeat(beat);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d.%d.%03d", bb.bar, bb.beat,
                  static_cast<int>(bb.fraction * 1000.0));
    return buf;
}

std::string bpmText(const time::TimeMap& tm) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.4g", tm.bpmAt(0));
    return buf;
}

std::string meterText(const time::TimeMap& tm) {
    const time::MeterChange m = tm.meterAt(1);
    return std::to_string(m.numerator) + "/" + std::to_string(m.denominator);
}

// Setting a label's text damages it (and relays out), so only write when the
// value actually changed: the idle loop must stay at zero work.
void setIfChanged(ui::Label* label, const std::string& text) {
    if (label && label->text() != text) label->setText(text);
}

void setIfChanged(ui::Button* button, const std::string& text) {
    if (button && button->valueText() != text) button->setText(text);
}

std::string countText(int bars) { return bars > 0 ? "count " + std::to_string(bars) : "count"; }

// An arc around `o` from a0 to a1 (clockwise) ending in an arrowhead.
void arcArrow(ui::Canvas& c, ui::Point o, float r, float a0, float a1, ui::Color ink) {
    c.strokeArc(o, r, a0, a1, ink, 1.5f);
    const ui::Point p{o.x + r * std::cos(a1), o.y + r * std::sin(a1)};
    const ui::Point along{-std::sin(a1), std::cos(a1)}, out{std::cos(a1), std::sin(a1)};
    const float len = r * 0.7f, half = r * 0.45f;
    c.fillPath({{p.x + along.x * len, p.y + along.y * len},
                {p.x + out.x * half, p.y + out.y * half},
                {p.x - out.x * half, p.y - out.y * half}},
               ink);
}

}  // namespace

// A transport button drawn as a symbol. Its text stays the state word ("play",
// "stop", "loop on", "count 2") for tests and the GUI query; only the symbol
// (and the count-in's bar count) is painted. A lit button (playing,
// recording, loop on…) is tinted with its colour token.
class TransportButton : public ui::Button {
  public:
    enum class Symbol { Start, Play, Stop, Record, CountingIn, Loop, Punch, Click, CountIn };
    TransportButton(std::string id, Symbol symbol, std::string text, std::function<void()> click)
        : ui::Button(std::move(id), std::move(text), std::move(click)), symbol_(symbol) {}

    // Only a change repaints: the idle loop must stay at zero work.
    void show(Symbol symbol, const std::string& text, std::string lit = {},
              std::string badge = {}) {
        if (symbol != symbol_ || lit != lit_ || badge != badge_) {
            symbol_ = symbol;
            lit_ = std::move(lit);
            badge_ = std::move(badge);
            invalidate();
        }
        if (valueText() != text) setText(text);
    }

    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRound(b, t.radius("s"),
                    pressed()   ? t.color("selection")
                    : hovered() ? t.color("raised")
                                : t.color("panel"));
        ui::Color ink = enabled ? t.color("ink") : t.color("ink-faint");
        ui::Color outline = t.color("rule");
        if (!lit_.empty()) {
            ink = t.color(lit_);
            c.fillRound(b, t.radius("s"), ink.withAlpha(0.2f));
            outline = ink.withAlpha(0.7f);
        }
        c.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : outline);
        // The symbol in an s×s square, left of centre when a count follows it.
        const float s = std::min(b.w, b.h) - 10;
        const float cx = std::round(b.x + b.w / 2 - (badge_.empty() ? 0 : 5));
        const float cy = std::round(b.y + b.h / 2);
        const float x = cx - s / 2, y = cy - s / 2;
        auto at = [&](float u, float v) { return ui::Point{x + s * u, y + s * v}; };
        const ui::Color red = t.color("armed");
        constexpr float pi = std::numbers::pi_v<float>;
        switch (symbol_) {
            case Symbol::Start:  // |◀
                c.fillRect({x + s * 0.1f, y + s * 0.1f, s * 0.16f, s * 0.8f}, ink);
                c.fillPath({at(0.92f, 0.1f), at(0.92f, 0.9f), at(0.3f, 0.5f)}, ink);
                break;
            case Symbol::Play:  // ▶
                c.fillPath({at(0.18f, 0.06f), at(0.94f, 0.5f), at(0.18f, 0.94f)}, ink);
                break;
            case Symbol::Stop:  // ■
                c.fillRound({x + s * 0.12f, y + s * 0.12f, s * 0.76f, s * 0.76f}, 1.0f, ink);
                break;
            case Symbol::Record:  // ●
                c.fillCircle({cx, cy}, s * 0.42f, red);
                break;
            case Symbol::CountingIn:  // ○, waiting for the count-in to end
                c.strokeCircle({cx, cy}, s * 0.36f, red, 1.8f);
                break;
            case Symbol::Loop:  // two arrows chasing round a circle
                arcArrow(c, {cx, cy}, s * 0.4f, pi + 0.3f, 2 * pi - 0.75f, ink);
                arcArrow(c, {cx, cy}, s * 0.4f, 0.3f, pi - 0.75f, ink);
                break;
            case Symbol::Punch:  // [●]: record inside a range
                c.strokePath({at(0.3f, 0.06f), at(0.06f, 0.06f), at(0.06f, 0.94f), at(0.3f, 0.94f)},
                             ink, 1.5f, false);
                c.strokePath({at(0.7f, 0.06f), at(0.94f, 0.06f), at(0.94f, 0.94f), at(0.7f, 0.94f)},
                             ink, 1.5f, false);
                c.fillCircle({cx, cy}, s * 0.2f, red);
                break;
            case Symbol::Click:  // a metronome
                c.strokePath(
                    {at(0.14f, 0.94f), at(0.86f, 0.94f), at(0.62f, 0.06f), at(0.38f, 0.06f)}, ink,
                    1.4f);
                c.line(at(0.5f, 0.72f), at(0.82f, 0.18f), ink, 1.5f);
                break;
            case Symbol::CountIn:  // →| : a run-up into the first bar
                c.line(at(0.04f, 0.5f), at(0.56f, 0.5f), ink, 1.5f);
                c.fillPath({at(0.46f, 0.24f), at(0.78f, 0.5f), at(0.46f, 0.76f)}, ink);
                c.fillRect({x + s * 0.82f, y + s * 0.1f, s * 0.16f, s * 0.8f}, ink);
                break;
        }
        if (!badge_.empty())
            c.text({cx + s / 2 + 2, b.y, b.x + b.w - (cx + s / 2 + 2), b.h}, badge_,
                   ui::Font::UiBold, t.type("small"), ink, ui::kLeft | ui::kMiddle);
    }

  private:
    Symbol symbol_;
    std::string lit_;  // a colour token; empty when not lit
    std::string badge_;
};

TransportBar::TransportBar(engine::EngineApp& app, AppActions& actions, cmd::Dispatcher& dispatcher,
                           ui::Root& root)
    : app_(app), actions_(actions), dispatcher_(dispatcher), root_(root) {}

std::unique_ptr<ui::Node> TransportBar::build() {
    auto bar = std::make_unique<ui::Node>("transport");
    bar->layout.dir = ui::Layout::Dir::Row;
    bar->layout.height = ui::SizeRule::fixed(44);
    bar->layout.padding = ui::Insets::xy(12, 0);
    bar->layout.gap = 6;
    using Symbol = TransportButton::Symbol;
    auto symbolButton = [&](const char* id, Symbol symbol, const char* text,
                            std::function<void()> click, float width = 30) {
        auto& b = bar->emplace<TransportButton>(id, symbol, text, std::move(click));
        b.layout.width = ui::SizeRule::fixed(width);
        b.layout.height = ui::SizeRule::fixed(24);
        return &b;
    };
    auto actionButton = [&](const char* id, Symbol symbol, const char* text, const char* action) {
        return symbolButton(id, symbol, text, [this, action] {
            auto r = actions_.run(action);
            if (!r && actions_.onError) actions_.onError(r.error().message);
        });
    };
    actionButton("transport.start", Symbol::Start, "start", "transport.seek:start");  // W, Home
    play_ = actionButton("transport.play", Symbol::Play, "play", "transport.toggle");
    record_ = actionButton("transport.record", Symbol::Record, "rec", "transport.record");
    loop_ = actionButton("transport.loop", Symbol::Loop, "loop", "transport.loop.toggle");
    punch_ = symbolButton("transport.punch", Symbol::Punch, "punch", [this] {
        // No action: the punch range is set elsewhere (P5.6); the button only
        // toggles it.
        json::Value args = json::Value::object();
        args["enabled"] = !app_.punch().enabled;
        auto r = dispatcher_.execute({"transport.punch", std::move(args), kOrigin});
        if (!r && actions_.onError) actions_.onError(r.error().message);
    });
    click_ = symbolButton("transport.metronome", Symbol::Click, "click", [this] {
        json::Value args = json::Value::object();
        args["enabled"] = !app_.metronome().enabled;
        auto r = dispatcher_.execute({"transport.metronome", std::move(args), kOrigin});
        if (!r && actions_.onError) actions_.onError(r.error().message);
        sync();
    });
    // Count-in before recording: none, 1, 2 or 4 bars (transport.metronome).
    count_ = symbolButton(
        "transport.count", Symbol::CountIn, "count",
        [this] {
            const int now = app_.metronome().countInBars;
            json::Value args = json::Value::object();
            args["count_in_bars"] = static_cast<std::int64_t>(now == 0  ? 1
                                                              : now < 2 ? 2
                                                              : now < 4 ? 4
                                                                        : 0);
            auto r = dispatcher_.execute({"transport.metronome", std::move(args), kOrigin});
            if (!r && actions_.onError) actions_.onError(r.error().message);
            sync();
        },
        40);
    bar->emplace<ui::Node>("transport.gap").layout.width = ui::SizeRule::fixed(10);
    position_ = &bar->emplace<ui::Label>("transport.position", "1.1.000", ui::Font::MonoBold,
                                         "title", "ink");
    time_ =
        &bar->emplace<ui::Label>("transport.time", "0:00.000", ui::Font::Mono, "mono", "ink-quiet");
    meter_ = &bar->emplace<ui::Button>("transport.meter", "4/4", [this] { openMeterForm(); });
    meter_->layout.width = ui::SizeRule::fixed(48);
    meter_->layout.height = ui::SizeRule::fixed(22);
    tempo_ = &bar->emplace<ui::Button>("transport.tempo", "120", [this] { openTempoForm(); });
    tempo_->layout.width = ui::SizeRule::fixed(58);
    tempo_->layout.height = ui::SizeRule::fixed(22);
    auto& rate = bar->emplace<RateMenuControl<ui::Knob>>("transport.rate");
    rate_ = &rate;
    rate.onMenu = [this] { openRateMenu(); };
    rate.onChange = [this](double position, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++rateGesture_;
        if (g == ui::Gesture::Begin) return;
        if (g == ui::Gesture::End) {
            sync();
            return;
        }
        const double value = positionRate(position, actions_.playrateRange);
        json::Value args = json::Value::object();
        args["rate"] = value;
        cmd::Origin origin = kOrigin;
        if (g == ui::Gesture::Move)
            origin.gesture = "transport.rate:" + std::to_string(rateGesture_);
        const auto r = dispatcher_.execute({"transport.playrate.set", std::move(args), origin});
        if (!r && actions_.onError) actions_.onError(r.error().message);
        if (r && rateValue_) setIfChanged(rateValue_, rateText(value));
        if (g == ui::Gesture::Set) sync();
    };
    auto& readout = bar->emplace<RateMenuControl<ui::Node>>("transport.rate.readout");
    rateReadout_ = &readout;
    readout.onMenu = [this] { openRateMenu(); };
    readout.layout.dir = ui::Layout::Dir::Column;
    readout.layout.width = ui::SizeRule::fixed(54);
    readout.layout.height = ui::SizeRule::fixed(28);
    readout.emplace<ui::Label>("transport.rate.label", "Rate:", ui::Font::Ui, "caption",
                               "ink-quiet");
    rateValue_ =
        &readout.emplace<ui::Label>("transport.rate.value", "1.0", ui::Font::Mono, "caption");
    bar->emplace<ui::Node>("transport.spacer");  // takes the rest
    engine_ =
        &bar->emplace<ui::Label>("transport.engine", "", ui::Font::Mono, "small", "ink-quiet");
    return bar;
}

void TransportBar::sync(bool event, std::chrono::steady_clock::time_point now) {
    const model::Session& s = app_.head();
    engine::TransportInfo info;
    if (auto r = app_.transport("get")) info = *r;
    if (position_) setIfChanged(position_, barsText(info.beat, *s.timeMap));
    if (time_) setIfChanged(time_, timeText(info.seconds));
    if (tempo_) setIfChanged(tempo_, bpmText(*s.timeMap));
    if (meter_) setIfChanged(meter_, meterText(*s.timeMap));
    using Symbol = TransportButton::Symbol;
    if (play_)
        play_->show(info.playing ? Symbol::Stop : Symbol::Play, info.playing ? "stop" : "play",
                    info.playing ? "meter" : "");
    if (record_)
        record_->show(info.countingIn ? Symbol::CountingIn : Symbol::Record,
                      info.countingIn ? "count" : "rec",
                      info.recording || info.countingIn ? "armed" : "");
    if (count_) {
        const int bars = app_.metronome().countInBars;
        count_->show(Symbol::CountIn, countText(bars), bars > 0 ? "accent" : "",
                     bars > 0 ? std::to_string(bars) : "");
    }
    if (loop_)
        loop_->show(Symbol::Loop, info.loop.enabled ? "loop on" : "loop",
                    info.loop.enabled ? "accent" : "");
    if (punch_)
        punch_->show(Symbol::Punch, info.punch.enabled ? "punch on" : "punch",
                     info.punch.enabled ? "accent" : "");
    if (click_)
        click_->show(Symbol::Click, app_.metronome().enabled ? "click on" : "click",
                     app_.metronome().enabled ? "accent" : "");
    if (rate_ && !rate_->dragging()) {
        // Automation follows consumption telemetry, not the latest authored
        // curve or renderer lookahead. The accessor executes no logged command.
        const auto state = app_.playrateState();
        const double shown = state.envelope ? state.effectiveRate : state.requestedRate;
        rate_->setValue(ratePosition(shown, actions_.playrateRange));
        if (rateValue_) setIfChanged(rateValue_, rateText(shown));
    }
    const bool moving = info.playing || info.recording;
    if (engine_ && (!lastStatus_ || moving != moving_ || (!moving && event) ||
                    (moving && now - *lastStatus_ >= std::chrono::milliseconds(250)))) {
        lastStatus_ = now;
        const engine::EngineStatus st = app_.engineStatus();
        if (!st.running) {
            setIfChanged(engine_, "engine stopped");
        } else {
            // The device's period (the JACK quantum) is the buffer the user
            // set; the engine's block only splits it. A backend that reports
            // no period runs in blocks.
            char buf[96];
            const std::int64_t buffer = st.period > 0 ? st.period : st.block;
            std::snprintf(buf, sizeof buf, "%.1fk/%lld  xr %lld  err %lld  mis %lld",
                          st.sampleRate / 1000.0, static_cast<long long>(buffer),
                          static_cast<long long>(st.xruns),
                          static_cast<long long>(st.processErrors),
                          static_cast<long long>(st.anticipationMisses));
            setIfChanged(engine_, buf);
        }
    }
    moving_ = moving;
}

std::optional<std::chrono::steady_clock::time_point> TransportBar::nextDeadline() const {
    if (!moving_ || !lastStatus_) return {};
    return *lastStatus_ + std::chrono::milliseconds(250);
}

json::Value TransportBar::saveLayout() const {
    json::Value value = json::Value::object();
    value["playrate_range"] = kRateRanges[actions_.playrateRange].name;
    return value;
}

void TransportBar::loadLayout(const json::Value& value) {
    if (rate_ && rate_->dragging()) root_.releaseWithin(rate_);
    actions_.playrateRange = 0;
    if (const auto* name = value.find("playrate_range"); name && name->isString())
        for (std::size_t i = 0; i < kRateRanges.size(); ++i)
            if (name->asString() == kRateRanges[i].name) actions_.playrateRange = i;
    sync();
}

void TransportBar::openRateMenu() {
    std::vector<ui::MenuItem> items;
    auto action = [&](const char* label, const char* id) {
        items.push_back({label, actions_.keysFor(id), [this, id] {
                             actions_.invoke(id);
                             sync();
                         }});
    };
    action("Set to 1.0", "transport.playrate.reset");
    action("Increase by one semitone (~6%)", "transport.playrate.semitone-up");
    action("Decrease by one semitone (~6%)", "transport.playrate.semitone-down");
    action("Increase by ten cents (~0.6%)", "transport.playrate.ten-cents-up");
    action("Decrease by ten cents (~0.6%)", "transport.playrate.ten-cents-down");
    action("Apply playrate to current BPM", "transport.playrate.apply-to-tempo");
    items.push_back({"", "", {}, true, true});
    action("Preserve pitch in audio items when changing master playrate",
           "transport.playrate.preserve-pitch");
    items.back().checked = app_.head().project->playrate.preservePitch;
    items.push_back({"", "", {}, true, true});
    for (std::size_t i = 0; i < kRateRanges.size(); ++i) {
        ui::MenuItem item;
        item.label = std::string("Playrate range: ") + kRateRanges[i].name;
        if (i == 0) item.label += " (default)";
        item.checked = i == actions_.playrateRange;
        item.run = [this, i] {
            actions_.playrateRange = i;
            sync();
        };
        items.push_back(std::move(item));
    }
    if (actions_.registry().find("ui.master.envelopes")) {
        items.push_back({"", "", {}, true, true});
        action("Master envelopes...", "ui.master.envelopes");
    }
    root_.openPopup(
        ui::makeMenu("transport.rate.menu", std::move(items), [this] { root_.closePopup(); }),
        rateAnchor(), ui::Placement::Below);
}

ui::Rect TransportBar::rateAnchor() const {
    return rateReadout_ ? rateReadout_->bounds() : ui::Rect{};
}

void TransportBar::openTempoForm() {
    auto form = std::make_unique<ui::Node>("tempo.form");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    form->layout.width = ui::SizeRule::fixed(240);
    form->emplace<ui::Label>("tempo.title", "Tempo (BPM)", ui::Font::UiBold, "title");
    auto& field = form->emplace<ui::TextField>("tempo.field", bpmText(*app_.head().timeMap));
    field.layout.height = ui::SizeRule::fixed(24);
    auto apply = [this, &field] {
        const double bpm = std::atof(field.text().c_str());
        if (!(bpm >= 1.0 && bpm <= 1000.0)) {
            if (actions_.onError) actions_.onError("tempo: 1..1000 BPM");
            return;
        }
        // Keep the map; the session tempo is the first point.
        json::Value points = json::Value::array();
        bool first = true;
        for (const time::TempoPoint& p : app_.head().timeMap->tempo()) {
            json::Value point = json::Value::object();
            point["beat"] = p.beat;
            point["bpm"] = first ? bpm : p.bpm;
            point["ramp"] = p.shape == time::TempoShape::Ramp;
            points.push_back(std::move(point));
            first = false;
        }
        json::Value args = json::Value::object();
        args["points"] = std::move(points);
        auto r = dispatcher_.execute({"tempo.set", std::move(args), kOrigin});
        if (!r && actions_.onError)
            actions_.onError(r.error().message);
        else
            root_.closePopup();
    };
    field.onSubmit = [apply](const std::string&) { apply(); };
    auto& row = form->emplace<ui::Node>("tempo.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(26);
    row.layout.gap = 6;
    row.emplace<TapTempoButton>("tempo.tap", field, root_);
    row.emplace<ui::Button>("tempo.set", "Set", apply);
    row.emplace<ui::Button>("tempo.cancel", "Cancel", [this] { root_.closePopup(); });
    auto& content = root_.openPopup(std::move(form), {}, ui::Placement::Center);
    if (ui::Node* f = content.find("tempo.field")) root_.focus(f);
}

void TransportBar::openMeterForm() {
    auto form = std::make_unique<ui::Node>("meter.form");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    form->layout.width = ui::SizeRule::fixed(240);
    form->emplace<ui::Label>("meter.title", "Meter from bar 1 (N/D)", ui::Font::UiBold, "title");
    auto& field = form->emplace<ui::TextField>("meter.field", meterText(*app_.head().timeMap));
    field.layout.height = ui::SizeRule::fixed(24);
    auto apply = [this, &field] {
        json::Value args = json::Value::object();
        args["bar"] = 1;
        args["meter"] = field.text();
        auto r = dispatcher_.execute({"timesig.set", std::move(args), kOrigin});
        if (!r && actions_.onError)
            actions_.onError(r.error().message);
        else
            root_.closePopup();
    };
    field.onSubmit = [apply](const std::string&) { apply(); };
    auto& row = form->emplace<ui::Node>("meter.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(26);
    row.layout.gap = 6;
    row.emplace<ui::Button>("meter.set", "Set", apply);
    row.emplace<ui::Button>("meter.cancel", "Cancel", [this] { root_.closePopup(); });
    auto& content = root_.openPopup(std::move(form), {}, ui::Placement::Center);
    if (ui::Node* f = content.find("meter.field")) root_.focus(f);
}

}  // namespace supple::gui
