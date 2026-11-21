#include "supple/model/groups.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/tcp.h"

#include "supple/views/control_names.h"
#include "supple/views/envelope_lane.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/ui/widgets.h"

namespace supple::views {
namespace {

const char* monitorToken(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return "off";
        case model::Monitor::On:
            return "on";
        case model::Monitor::Auto:
            return "auto";
    }
    return "off";
}

model::Monitor nextMonitor(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return model::Monitor::On;
        case model::Monitor::On:
            return model::Monitor::Auto;
        case model::Monitor::Auto:
            return model::Monitor::Off;
    }
    return model::Monitor::Off;
}

json::Value trackArg(const model::Id& track) {
    json::Value v = json::Value::object();
    v["track"] = track.str();
    return v;
}

// One drag, one undo step: every move of a drag carries the same key, and the
// key carries a serial so two drags with nothing between them stay two steps
// (the bus merges consecutive commits that share an origin gesture).
std::string gestureFor(std::string_view what, const model::Id& track, std::uint64_t serial) {
    return std::string(what) + ":" + track.str() + ":" + std::to_string(serial);
}

}  // namespace

// Output gestures keep the source id in the panel, independent of pooled rows.
class TcpPanel::OutButton : public ui::Button {
  public:
    OutButton(TcpPanel& panel, std::string id, std::function<std::optional<model::Id>()> track)
        : Button(std::move(id), "out"), panel_(panel), track_(std::move(track)) {
        onClick = [this] {
            if (const auto from = track_(); from && panel_.cb_.routing)
                panel_.cb_.routing(*from, bounds());
        };
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (panel_.sendFrom_) return panel_.sendPointer(e, *panel_.sendFrom_, bounds());
        const auto from = track_();
        return from && panel_.sendPointer(e, *from, bounds());
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && panel_.sendFrom_) {
            panel_.cancelSend();
            if (root()) root()->capture(nullptr);
            return true;
        }
        return Button::onKey(e);
    }

  private:
    TcpPanel& panel_;
    std::function<std::optional<model::Id>()> track_;
};

class TcpPanel::SendGhost : public ui::Node {
  public:
    explicit SendGhost(std::shared_ptr<TcpPanel*> alive)
        : Node("tcp.send-ghost"), alive_(std::move(alive)) {}
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        if (!*alive_) return;
        const auto& panel = **alive_;
        if (!panel.sendFrom_ || !panel.sendMoved_) return;
        const auto colour = c.theme().color("focus");
        if (const auto row = panel.sendTargetAt(panel.sendPointer_)) {
            const ui::Rect tcp = panel.rowRect(*row).intersect(panel.bounds());
            c.fillRect(tcp, colour.withAlpha(0.2f));
            c.strokeRect(tcp.inset(0.5f), colour, 1);
            if (panel.cb_.lanesRect) {
                const ui::Rect lanes = panel.cb_.lanesRect();
                c.fillRect(ui::Rect{lanes.x, tcp.y, lanes.w, tcp.h}.intersect(lanes),
                           colour.withAlpha(0.1f));
            }
        }
        c.line({panel.sendAnchor_.x + panel.sendAnchor_.w / 2,
                panel.sendAnchor_.y + panel.sendAnchor_.h / 2},
               panel.sendPointer_, colour, 2);
    }

  private:
    std::shared_ptr<TcpPanel*> alive_;
};

// One strip's controls. Rows are pooled: a row is bound to a track and
// re-bound as the scroll moves, so its controls keep their identity.
class TcpPanel::Row : public ui::Node {
  public:
    Row(TcpPanel& panel, std::size_t slot);
    ~Row() override;

    void bind(const model::Id& track, int depth, const model::Track& tr,
              const model::Session& session);
    void refresh(const model::Session& session);  // values only
    // The fader's and pan bars' modulation (ADR-0096), with the layout's
    // live offsets.
    void showModulation(const model::Session& session);
    void unbind();

    bool bound() const { return track_.has_value(); }
    void startRename();  // the name becomes an edit field
    void cancelGestures();
    const model::Id& track() const { return *track_; }
    ui::Node* control(std::string_view what) const;
    // The control under `p` that MIDI learn can bind, as track `learn` names it.
    const char* learnableAt(ui::Point p) const;

    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override;
    void paint(ui::Canvas& canvas) override;
    bool onPointer(const ui::PointerEvent& e) override;
    ui::Cursor cursorAt(ui::Point p) const override;
    bool onKey(const ui::KeyEvent& e) override;

  private:
    TcpPanel& panel_;
    std::optional<model::Id> track_;
    std::optional<model::Id> faderTrack_, panTrack_, renameTrack_, resizeTrack_;
    int depth_ = 0;
    bool channel_ = true;
    bool master_ = false;  // a master's lane (ADR-0057): name, arm and lane mute only
    bool renaming_ = false;
    ui::Rect nameRect_{};
    bool frozen_ = false;  // a snowflake precedes the name
    ui::Label* name_ = nullptr;
    ui::TextField* rename_ = nullptr;
    ui::Toggle* mute_ = nullptr;
    ui::Toggle* laneMute_ = nullptr;  // a master's lane: silences its own items
    ui::Toggle* solo_ = nullptr;
    ui::Toggle* polarity_ = nullptr;  // polarity and mono (ADR-0022), on the fader row
    ui::Toggle* mono_ = nullptr;
    ui::ArmButton* arm_ = nullptr;
    ui::MonitorButton* monitor_ = nullptr;
    ui::Fader* fader_ = nullptr;
    ui::PanControl* pan_ = nullptr;
    ui::Meter* meter_ = nullptr;
    ui::Button* fx_ = nullptr;
    ui::Button* addFx_ = nullptr;  // "+" beside "fx": the plugin browser alone
    ui::Button* input_ = nullptr;
    ui::Button* env_ = nullptr;
    OutButton* out_ = nullptr;
    std::string inputText_;  // the full label; arrange() clips it to fit
    // Dragging the bottom edge (the track's height).
    bool resizing_ = false;
    float resizeY_ = 0, resizeFrom_ = 0;
    bool deferSelect_ = false;  // a plain press on a selected track: select it alone on release
};

TcpPanel::Row::Row(TcpPanel& panel, std::size_t slot)
    : Node("tcp.row" + std::to_string(slot), ui::Role::Group), panel_(panel) {
    const std::string id = this->id();
    interactive = true;
    clipsChildren = true;
    name_ = &emplace<ui::Label>(id + ".name", "", ui::Font::UiBold, "small", "ink");
    name_->align = ui::kLeft | ui::kMiddle;
    rename_ = &emplace<ui::TextField>(id + ".rename", "");
    rename_->visible = false;
    rename_->onSubmit = [this](const std::string& text) {
        if (!renaming_) return;
        renaming_ = false;
        rename_->visible = false;
        name_->visible = true;
        name_->relayout();
        // Keys go back to the shortcuts (Space plays again).
        if (ui::Root* r = root(); r && r->focused() == rename_) r->focus(nullptr);
        const auto target = std::exchange(renameTrack_, std::nullopt);
        if (panel_.cb_.commit && target && target == track_) {
            json::Value args = trackArg(*target);
            args["name"] = text;
            panel_.cb_.commit("track.set", std::move(args), {});
        }
        invalidate();
    };
    // A click anywhere else ends the rename and keeps the edit.
    rename_->onBlur = [this](const std::string& text) {
        if (renaming_) rename_->onSubmit(text);
    };
    rename_->onCancel = [this] {
        renameTrack_.reset();
        renaming_ = false;
        rename_->visible = false;
        name_->visible = true;
        name_->relayout();
        if (ui::Root* r = root(); r && r->focused() == rename_) r->focus(nullptr);
        invalidate();
    };
    auto setTrack = [this](json::Value args, std::string gesture) {
        if (!panel_.cb_.commit || !track_.has_value()) return;
        panel_.cb_.commit("track.set", std::move(args), std::move(gesture));
    };
    mute_ = &emplace<ui::Toggle>(id + ".mute", "M", "mute", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["mute"] = on;
        setTrack(std::move(args), {});
    });
    laneMute_ = &emplace<ui::Toggle>(id + ".lanemute", "M", "mute", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["lane_mute"] = on;
        setTrack(std::move(args), {});
    });
    laneMute_->cell = true;
    solo_ = &emplace<ui::Toggle>(id + ".solo", "S", "solo", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["solo"] = on ? "on" : "off";
        setTrack(std::move(args), {});
    });
    polarity_ =
        &emplace<ui::Toggle>(id + ".polarity", "\u00d8", "polarity", [this, setTrack](bool on) {
            json::Value args = trackArg(*track_);
            args["phase_invert"] = on;
            setTrack(std::move(args), {});
        });
    mono_ = &emplace<ui::Toggle>(id + ".mono", "mono", "mono", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["mono"] = on;
        setTrack(std::move(args), {});
    });
    mute_->cell = solo_->cell = polarity_->cell = mono_->cell = true;
    arm_ = &emplace<ui::ArmButton>(id + ".arm", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["armed"] = on;
        setTrack(std::move(args), {});
    });
    monitor_ = &emplace<ui::MonitorButton>(id + ".monitor", [this, setTrack] {
        const model::Track* tr =
            track_.has_value() ? panel_.session_.tracks.find(*track_) : nullptr;
        json::Value args = trackArg(*track_);
        args["monitor"] = monitorToken(nextMonitor(tr ? tr->record.monitor : model::Monitor::Off));
        setTrack(std::move(args), {});
    });
    fader_ = &emplace<ui::Fader>(id + ".fader", ui::Fader::Orientation::Horizontal);
    fader_->onChange = [this, setTrack](double db, ui::Gesture g) {
        if (g == ui::Gesture::Begin) {
            ++panel_.gestureSerial_;
            faderTrack_ = track_;
            return;
        }
        if (g == ui::Gesture::End) {
            faderTrack_.reset();
            return;
        }
        const auto target = g == ui::Gesture::Move ? faderTrack_ : track_;
        if (!target || target != track_) return;
        json::Value args = trackArg(*target);
        args["volume_db"] = db;
        args["bypass_groups"] = bool(fader_->gestureModifiers() & ui::kAlt);
        setTrack(std::move(args), g == ui::Gesture::Move
                                      ? gestureFor("tcp.volume", *target, panel_.gestureSerial_)
                                      : std::string{});
    };
    pan_ = &emplace<ui::PanControl>(id + ".pan");
    // Split by the strip's pan mode (ADR-0066): dual's two inputs, or a
    // position and a width.
    auto panBar = [this, setTrack](int part) {
        return [this, setTrack, part](double value, ui::Gesture g) {
            if (g == ui::Gesture::Begin) {
                ++panel_.gestureSerial_;
                panTrack_ = track_;
                return;
            }
            if (g == ui::Gesture::End) {
                panTrack_.reset();
                return;
            }
            const auto target = g == ui::Gesture::Move ? panTrack_ : track_;
            if (!target || target != track_) return;
            const auto split = pan_->split();
            const char* field = split == ui::PanControl::Split::Dual
                                    ? (part == 0 ? "pan_left" : "pan_right")
                                : split == ui::PanControl::Split::Width && part == 1 ? "width"
                                                                                     : "pan";
            json::Value args = trackArg(*target);
            args[field] = value;
            args["bypass_groups"] = bool(pan_->gestureModifiers() & ui::kAlt);
            setTrack(std::move(args),
                     g == ui::Gesture::Move
                         ? gestureFor(std::string("tcp.") + field, *target, panel_.gestureSerial_)
                         : std::string{});
        };
    };
    pan_->onChange = panBar(0);
    pan_->onSecondChange = panBar(1);
    // A drag on a modulated bar edits its first enabled route's depth
    // (ADR-0096, modulation.set); a drag is one undo step.
    auto depth = [this](const char* param, double value, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++panel_.gestureSerial_;
        if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
        if (!track_ || !panel_.cb_.commit) return;
        const auto mod = controlModulation(panel_.layout_.session(), {*track_, param},
                                           panel_.layout_.unipolarSource);
        if (!mod) return;
        json::Value args = json::Value::object();
        args["modulation"] = mod->route.str();
        args["depth"] = value;
        panel_.cb_.commit("modulation.set", std::move(args),
                          g == ui::Gesture::Move
                              ? gestureFor("tcp.depth", mod->route, panel_.gestureSerial_)
                              : std::string{});
    };
    fader_->onDepth = [depth](double value, ui::Gesture g) { depth("volume", value, g); };
    pan_->onDepth = [this, depth](int part, double value, ui::Gesture g) {
        depth(part == 1 && pan_->split() == ui::PanControl::Split::Width ? "width" : "pan", value,
              g);
    };
    // The wheel scrolls the track list, never a strip's level or pan.
    fader_->wheel = false;
    pan_->wheel = false;
    meter_ = &emplace<ui::Meter>(id + ".meter", true);
    fx_ = &emplace<ui::Button>(id + ".fx", "fx", [this] {
        if (track_ && panel_.cb_.fx) panel_.cb_.fx(*track_);
    });
    addFx_ = &emplace<ui::Button>(id + ".addfx", "+", [this] {
        if (track_ && panel_.cb_.addFx) panel_.cb_.addFx(*track_);
    });
    input_ = &emplace<ui::Button>(id + ".input", "in \u2014", [this] {
        if (!track_ || !panel_.cb_.pickInput) return;
        panel_.cb_.pickInput(*track_, input_->bounds());
    });
    env_ = &emplace<ui::Button>(id + ".env", "env", [this] {
        if (!track_ || !panel_.cb_.envelopes) return;
        panel_.cb_.envelopes(*track_, env_->bounds());
    });
    out_ = &emplace<OutButton>(panel_, id + ".out", [this] { return track_; });
    // Secondary buttons are text alone until the pointer is on them; the
    // input stays a boxed field, since it shows a value.
    fx_->quiet = addFx_->quiet = true;  // "env" and "out" are boxed, as "out" is in the mixer
    // What the status line calls each while the pointer rests on it.
    mute_->hint = names::kMute;
    laneMute_->hint = names::kLaneMute;
    solo_->hint = names::kSolo;
    polarity_->hint = names::kPolarity;
    mono_->hint = names::kMono;
    fader_->hint = names::kVolume;
    pan_->hint = names::kPan;
    fx_->hint = names::kChain;
    addFx_->hint = names::kAddFx;
    input_->hint = names::kInput;
    env_->hint = names::kEnvelopes;
    out_->hint = names::kRouting;
}

TcpPanel::Row::~Row() = default;

void TcpPanel::Row::bind(const model::Id& track, int depth, const model::Track& tr,
                         const model::Session& session) {
    if (track_ != track) cancelGestures();
    track_ = track;
    depth_ = depth;
    channel_ = tr.type == model::StripType::Channel;
    master_ = tr.type == model::StripType::Master;
    renaming_ = false;
    rename_->visible = false;
    name_->visible = true;
    refresh(session);
    invalidate();
}

void TcpPanel::Row::refresh(const model::Session& session) {
    const model::Track* tr = track_.has_value() ? session.tracks.find(*track_) : nullptr;
    if (!tr) return;
    channel_ = tr->type == model::StripType::Channel;
    master_ = tr->type == model::StripType::Master;
    if (const auto row = panel_.layout_.indexOf(*track_))
        depth_ = panel_.layout_.rows()[*row].depth;
    const std::string name =
        tr->name.empty() ? std::string(model::stripTypeName(tr->type)) : tr->name;
    name_->setText(name);
    if (frozen_ != tr->freeze.has_value()) {  // the snowflake takes room from the name
        frozen_ = tr->freeze.has_value();
        relayout();
    }
    if (!renaming_) rename_->setText(name);
    mute_->setOn(tr->mute);
    laneMute_->setOn(tr->laneMute);
    solo_->setOn(tr->solo != model::Solo::Off);
    polarity_->enabled = mono_->enabled = arm_->enabled = monitor_->enabled = !tr->freeze;
    polarity_->setOn(tr->phaseInvert);
    mono_->setOn(tr->mono);
    arm_->setOn(tr->record.armed);
    monitor_->setMode(tr->record.monitor == model::Monitor::Off  ? ui::MonitorButton::Mode::Off
                      : tr->record.monitor == model::Monitor::On ? ui::MonitorButton::Mode::On
                                                                 : ui::MonitorButton::Mode::Auto);
    fx_->setEmphasis(!tr->chain.empty() || !tr->inputChain.empty());
    fx_->setMark(chainModulated(session, *track_) ? std::string(kModMark) : std::string(),
                 "signal-cv");
    fx_->setTextColor(
        panel_.cb_.fxNeedsAttention && panel_.cb_.fxNeedsAttention(*track_) ? "meter-hot" : "");
    fader_->setValue(tr->volumeDb);
    // A master's pan is a balance; a channel's or buss's follows its mode.
    const bool master = tr->type == model::StripType::Master;
    const auto split = master || tr->panMode == model::PanMode::Pan ? ui::PanControl::Split::None
                       : tr->panMode == model::PanMode::Dual        ? ui::PanControl::Split::Dual
                                                                    : ui::PanControl::Split::Width;
    pan_->setSplit(split);
    pan_->setValue(split == ui::PanControl::Split::Dual ? tr->panLeft : tr->pan);
    pan_->setSecond(split == ui::PanControl::Split::Dual ? tr->panRight : tr->width);
    showModulation(session);
    // The record input: the view knows the channels, the app knows the device's
    // port names (engine.inputs), so the app writes the label.
    std::string channels;
    if (tr->record.input)
        if (const model::Route* r = session.routes.find(*tr->record.input); r && r->from.hwChannels)
            for (std::size_t i = 0; i < r->from.hwChannels->size(); ++i)
                channels += (i ? "/" : "") + std::to_string((*r->from.hwChannels)[i]);
    const std::string inputText =
        panel_.cb_.inputText ? panel_.cb_.inputText(*track_, channels)
                             : (channels.empty() ? std::string("in \u2014") : "in " + channels);
    if (input_->valueText() != inputText) input_->setText(inputText);
}

void TcpPanel::Row::cancelGestures() {
    faderTrack_.reset();
    panTrack_.reset();
    renameTrack_.reset();
    resizeTrack_.reset();
    fader_->cancelGesture();
    pan_->cancelGesture();
    resizing_ = renaming_ = false;
    rename_->visible = false;
    name_->visible = true;
    if (panel_.reorder_ && panel_.reorder_->track == track_) panel_.endReorder(false);
    if (auto* owner = root()) {
        if (owner->focused() == rename_) owner->focus(nullptr);
        owner->forget(this);
        owner->forget(fader_);
        owner->forget(pan_);
    }
}

const char* TcpPanel::Row::learnableAt(ui::Point p) const {
    const std::pair<const ui::Node*, const char*> controls[] = {
        {fader_, "volume"},       {pan_, "pan"},   {mute_, "mute"},
        {laneMute_, "lane_mute"}, {solo_, "solo"}, {polarity_, "phase_invert"},
        {mono_, "mono"},          {arm_, "armed"}, {monitor_, "monitor"}};
    for (const auto& [node, param] : controls)
        if (node->visible && node->bounds().contains(p)) return param;
    return nullptr;
}

void TcpPanel::Row::unbind() {
    cancelGestures();
    track_.reset();
    visible = false;
}

ui::Node* TcpPanel::Row::control(std::string_view what) const {
    if (what == "name") return name_;
    if (what == "env") return env_;
    if (what == "out") return out_;
    if (what == "rename") return rename_;
    if (what == "mute") return mute_;
    if (what == "lanemute") return laneMute_;
    if (what == "solo") return solo_;
    if (what == "polarity") return polarity_;
    if (what == "mono") return mono_;
    if (what == "arm") return arm_;
    if (what == "monitor") return monitor_;
    if (what == "fader") return fader_;
    if (what == "pan") return pan_;
    if (what == "meter") return meter_;
    if (what == "fx") return fx_;
    if (what == "addfx") return addFx_;
    if (what == "input") return input_;
    return nullptr;
}

void TcpPanel::Row::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    using G = TcpRowGeometry;
    setBounds(r);
    const float x = r.x, right = r.right();
    const float indent = 14.0f * static_cast<float>(depth_);
    // The meter stands at the row's right edge; the controls end a gap
    // before it.
    const float edge = right - G::kMeterWidth - 7;
    // The name line's four cells: arm, monitor, mute, solo. A cell is a
    // control's place: a buss has no arm or monitor and their cells stay
    // empty, so M and S are in one column down the whole panel.
    const float cells = edge - 4 * G::kCellWidth - 3 * G::kCellGap;
    auto cell = [&](int column) {
        return ui::Rect{cells + static_cast<float>(column) * (G::kCellWidth + G::kCellGap),
                        r.y + G::kStateTop, G::kCellWidth, G::kCellHeight};
    };
    const float mark = (master_ || !channel_ ? G::kMark : 0.0f) + (frozen_ ? G::kMark : 0.0f);
    nameRect_ = {x + G::kLeft + indent + mark, r.y + G::kNameTop,
                 std::max(20.0f, cells - 6 - (x + G::kLeft + indent + mark)), G::kNameHeight};
    name_->arrange(canvas, nameRect_);
    rename_->arrange(canvas, {nameRect_.x, nameRect_.y + 1, nameRect_.w, 17});
    laneMute_->visible = master_;
    mute_->visible = solo_->visible = !master_;
    arm_->visible = channel_ || master_;
    monitor_->visible = channel_;
    if (master_) {  // a master's lane (ADR-0057): its name, the lane's mute and arm
        for (ui::Node* n : std::initializer_list<ui::Node*>{input_, env_, out_, fader_, pan_,
                                                            polarity_, mono_, meter_, fx_, addFx_})
            n->visible = false;
        arm_->arrange(canvas, cell(0));
        laneMute_->arrange(canvas, cell(2));
        return;
    }
    meter_->visible = true;
    meter_->arrange(canvas,
                    {right - G::kMeterWidth - 2, r.y + 3, G::kMeterWidth, std::max(4.0f, r.h - 6)});
    if (arm_->visible) arm_->arrange(canvas, cell(0));
    if (monitor_->visible) monitor_->arrange(canvas, cell(1));
    mute_->arrange(canvas, cell(2));
    solo_->arrange(canvas, cell(3));

    // The mix line, in signal order: polarity and mono, the fader, the pan.
    const bool mix = r.h >= G::kMixHeight;
    fader_->visible = pan_->visible = polarity_->visible = mono_->visible = mix;
    if (mix) {
        const float y = r.y + G::kMixTop;
        polarity_->arrange(canvas, {x + G::kLeft, y, G::kCellWidth, G::kCellHeight});
        mono_->arrange(canvas, {x + G::kLeft + G::kCellWidth + G::kCellGap, y, 34, G::kCellHeight});
        const float faderX = x + G::kLeft + G::kCellWidth + G::kCellGap + 34 + 6;
        pan_->arrange(canvas, {edge - 42, y, 42, G::kCellHeight});
        fader_->arrange(canvas, {faderX, y, std::max(30.0f, edge - 42 - 8 - faderX), 14});
    }
    // The connections line, left to right as the signal goes: the input,
    // the plug-ins ("fx" and "+"), the automation ("env") and the output.
    const bool links = r.h >= G::kFullHeight;
    input_->visible = links && channel_;
    fx_->visible = addFx_->visible = env_->visible = out_->visible = links;
    if (links) {
        const float y = r.y + G::kLinkTop;
        const float outX = edge - 28, envX = outX - 30, addX = envX - 18, fxX = addX - 24;
        out_->arrange(canvas, {outX, y, 28, G::kCellHeight});
        env_->arrange(canvas, {envX, y, 28, G::kCellHeight});
        addFx_->arrange(canvas, {addX, y, 16, G::kCellHeight});
        fx_->arrange(canvas, {fxX, y, 22, G::kCellHeight});
        // The input names the device's ports; it takes the room up to "fx",
        // and the picker's menu shows them whole.
        if (input_->visible)
            input_->arrange(canvas, {x + G::kLeft, y, std::max(12.0f, fxX - 6 - (x + G::kLeft)),
                                     G::kCellHeight});
    }
}

void TcpPanel::Row::paint(ui::Canvas& c) {
    if (!track_.has_value()) return;
    const ui::Theme& t = c.theme();
    const model::Track* tr = panel_.session_.tracks.find(*track_);
    if (!tr) return;
    const ui::Rect r = bounds();
    c.fillRect(r, t.color("panel"));
    // Selection (§4.2): the row is highlighted under its marks, and outlined
    // over them (below).
    const bool selected = panel_.cb_.selected && panel_.cb_.selected(*track_);
    if (selected) c.fillRect(r, t.color("selection").withAlpha(0.55f));
    c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.5f));
    // The track's colour (organizing): a stripe down the row's left edge.
    if (const auto colour = t.trackColor(tr->color)) c.fillRect({r.x, r.y, 4, r.h - 1}, *colour);
    // Parentage: a bar down the left of a child's row.
    if (depth_ > 0) {
        const float px = r.x + 8 + 14.0f * static_cast<float>(depth_ - 1);
        c.fillRect({px, r.y, 1, r.h}, t.color("rule"));
        c.fillRect({px, r.y + r.h / 2 - 3, 6, 1}, t.color("rule"));
    }
    // The type's mark before the name (a buss or a master; a channel, the
    // usual case, has none), then the track's groups as small colour tabs
    // under the name's start.
    const float lead = r.x + TcpRowGeometry::kLeft + 14.0f * static_cast<float>(depth_);
    if (tr->type != model::StripType::Channel)
        ui::drawTrackGlyph(
            c, {lead, r.y + TcpRowGeometry::kNameTop, 10, TcpRowGeometry::kNameHeight},
            tr->type == model::StripType::Buss ? ui::TrackGlyph::Buss : ui::TrackGlyph::Master,
            t.color("ink-quiet"));
    float tabX = lead;
    for (auto id : model::trackGroups(panel_.session_, *track_)) {
        const auto& group = *panel_.session_.groups.find(id);
        const auto color = t.trackColor(group.color).value_or(t.color("focus"));
        c.fillRect({tabX, r.y + TcpRowGeometry::kNameTop + TcpRowGeometry::kNameHeight - 1, 5, 2},
                   color.withAlpha(group.active ? 1.f : .3f));
        tabX += 7;
        if (tabX > lead + 21) break;
    }
    // A frozen channel: a snowflake before its name.
    if (frozen_)
        ui::drawSnowflake(c, {nameRect_.x - 7, nameRect_.y + nameRect_.h / 2}, 6.0f,
                          t.color("ink"));
    // The row being dragged to a new place.
    if (panel_.reorder_ && panel_.reorder_->moved && panel_.reorder_->track == track_)
        c.fillRect(r, t.color("focus").withAlpha(0.12f));
    if (selected) c.strokeRect({r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, t.color("focus"), 1.0f);
}

// The bottom edge sets the row's height.
ui::Cursor TcpPanel::Row::cursorAt(ui::Point p) const {
    const bool grip = track_.has_value() && panel_.cb_.resizeTrack &&
                      p.y >= bounds().bottom() - TcpRowGeometry::kResizeGrip;
    if (resizing_) return ui::Cursor::ResizeVertical;
    // A row in hand: the move cursor where a release would move it and over
    // its own place, "not allowed" elsewhere when it has nowhere to go (no
    // other row of its kind beside it).
    if (const auto& r = panel_.reorder_; r && r->moved && track_ && r->track == *track_)
        return r->moves.empty() && !r->home ? ui::Cursor::NotAllowed : ui::Cursor::Move;
    return grip ? ui::Cursor::ResizeVertical : ui::Cursor::Arrow;
}

bool TcpPanel::Row::onPointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    if (e.type == T::Cancel) {
        resizing_ = false;
        resizeTrack_.reset();
        panel_.endReorder(false);
        panel_.cancelSend();
        invalidate();
        return true;
    }
    if (resizing_) {
        if ((e.type == T::Move || e.type == T::Up) && resizeTrack_ && resizeTrack_ == track_ &&
            panel_.cb_.resizeTrack)
            panel_.cb_.resizeTrack(*resizeTrack_,
                                   std::max(1.0f, resizeFrom_ + (e.pos.y - resizeY_)));
        if (e.type == T::Up) {
            resizing_ = false;
            resizeTrack_.reset();
            if (ui::Root* r = root()) r->capture(nullptr);
        }
        return true;
    }
    if (panel_.reorder_ && track_ && panel_.reorder_->track == *track_) {
        if (e.type == T::Move || e.type == T::Up) {
            if (e.type == T::Move) {
                panel_.moveReorder(e.pos);
                if (panel_.reorder_ && panel_.reorder_->moved)
                    if (ui::Root* r = root()) r->focus(this);  // Escape cancels
            } else {
                if (ui::Root* r = root(); r && r->focused() == this) r->focus(nullptr);
                const bool dragged = panel_.reorder_->moved;
                const model::Id track = *track_;
                panel_.endReorder(e.button == 1);
                if (std::exchange(deferSelect_, false) && !dragged && panel_.cb_.select)
                    panel_.cb_.select(track, 0);
            }
            return true;
        }
    }
    if (e.type != T::Down || !track_.has_value()) return false;
    // The bottom edge: drag to set this track's height, double-click to reset it.
    if (e.button == 1 && e.pos.y >= bounds().bottom() - TcpRowGeometry::kResizeGrip &&
        panel_.cb_.resizeTrack) {
        if (e.clicks >= 2) {
            panel_.cb_.resizeTrack(*track_, 0);
            return true;
        }
        resizing_ = true;
        resizeTrack_ = track_;
        resizeY_ = e.pos.y;
        resizeFrom_ = bounds().h;
        if (ui::Root* r = root()) r->capture(this);
        return true;
    }
    // A right-click on a fader, pan or button: its parameter menu.
    if (e.button == 3 && panel_.cb_.learn)
        if (const char* param = learnableAt(e.pos)) {
            panel_.cb_.learn(*track_, param, {e.pos.x, e.pos.y, 1, 1});
            return true;
        }
    if (e.clicks >= 2 && nameRect_.contains(e.pos)) {  // rename in place
        startRename();
        return true;
    }
    // A right-click on a selected track keeps the selection (its menu acts on
    // all of it), and so does a plain press on one (a drag moves all of it; a
    // release without one selects it alone); otherwise the press selects as usual.
    const bool selected = panel_.cb_.selected && panel_.cb_.selected(*track_);
    const bool keep = e.button == 3 && selected;
    deferSelect_ = e.button == 1 && selected && !(e.mods & (ui::kShift | ui::kCtrl));
    if (panel_.cb_.select && !keep)
        panel_.cb_.select(*track_, e.button == 3 ? 0 : deferSelect_ ? kSelectKeep : e.mods);
    if (e.button == 1) panel_.beginReorder(*track_, e.pos);  // a drag from here moves the track
    if (deferSelect_ && !panel_.reorder_) {  // nothing to drag (a master): select it now
        deferSelect_ = false;
        if (panel_.cb_.select) panel_.cb_.select(*track_, 0);
    }
    // A right-click: the track's menu, at the pointer.
    if (e.button == 3 && panel_.cb_.menu) {
        const model::Id track = *track_;
        TcpPanel* panel = &panel_;
        panel_.cb_.menu(track, {e.pos.x, e.pos.y, 1, 1}, [panel, track] { panel->rename(track); });
    }
    return true;
}

bool TcpPanel::Row::onKey(const ui::KeyEvent& e) {
    if (e.key != SDLK_ESCAPE || !panel_.reorder_ || !panel_.reorder_->moved) return false;
    panel_.endReorder(false);
    if (ui::Root* r = root()) {
        r->capture(nullptr);
        if (r->focused() == this) r->focus(nullptr);
    }
    return true;
}

void TcpPanel::Row::startRename() {
    if (!track_) return;
    renameTrack_ = track_;
    renaming_ = true;
    const auto* tr = panel_.session_.tracks.find(*track_);
    rename_->setText(
        tr ? (tr->name.empty() ? std::string(model::stripTypeName(tr->type)) : tr->name)
           : name_->text());
    rename_->selectAll();
    rename_->visible = true;
    name_->visible = false;
    rename_->relayout();
    name_->relayout();
    if (ui::Root* r = root()) r->focus(rename_);
    invalidate();
}

// --- the panel ---------------------------------------------------------------------

namespace {
// The rows' area: placed by the panel, which places each row itself.
class RowsHost : public ui::Node {
  public:
    RowsHost() : Node("tcp.rows") { clipsChildren = true; }
    void arrange(const ui::Canvas&, const ui::Rect& r) override { setBounds(r); }
};

// The panel's foot: an opaque band under the rows' area with "mixer" and
// "+ track" at its right.
class Foot : public ui::Node {
  public:
    Foot() : Node("tcp.foot") {}
    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override {
        setBounds(r);
        float x = r.right() - 6;
        for (auto it = children().rbegin(); it != children().rend(); ++it) {
            x -= 56;
            (*it)->arrange(canvas, {x, r.bottom() - 22, 56, 18});
            x -= 4;
        }
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 1}, c.theme().color("rule"));
    }
};
}  // namespace

TcpPanel::TcpPanel(const TrackLayout& rowLayout, const Viewport& view, TcpCallbacks callbacks)
    : Node("tcp", ui::Role::List), layout_(rowLayout), view_(view), cb_(std::move(callbacks)) {
    clipsChildren = true;
    alive_ = std::make_shared<TcpPanel*>(this);
    interactive = true;  // its empty space takes a double-click
    rowsHost_ = &emplace<RowsHost>();
    foot_ = &emplace<Foot>();  // after the rows: it paints over them
    mixer_ = &foot_->emplace<ui::Button>("tcp.mixer", "mixer", [this] {
        if (cb_.mixer) cb_.mixer();
    });
    add_ = &foot_->emplace<ui::Button>(
        "tcp.add", "+ track", [this] { openAddMenu(add_->bounds(), ui::Placement::Above); });
    add_->layout.width = ui::SizeRule::fixed(56);
    add_->layout.height = ui::SizeRule::fixed(18);
    mixer_->hint = {{}, "ui.mixer"};  // the action's own name and keys
    add_->hint = {"Add a track", {}};
}

// The three strip types, offered by the "+" and by a right-click on the panel's
// empty space (ADR-0019: exactly these).
void TcpPanel::openAddMenu(const ui::Rect& anchor, ui::Placement placement) {
    ui::Root* r = root();
    if (!r || !cb_.addStrip) return;
    std::vector<ui::MenuItem> items;
    for (const char* type : {"Channel", "Buss", "Master"}) {
        const std::string lower = [&] {
            std::string out(type);
            for (char& ch : out)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return out;
        }();
        items.push_back({type, {}, [this, lower] { cb_.addStrip(lower); }, true, false, false});
    }
    r->openPopup(ui::makeMenu("tcp.addmenu", std::move(items), [r] { r->closePopup(); }), anchor,
                 placement);
}

TcpPanel::~TcpPanel() {
    *alive_ = nullptr;
    // The app's meter feed must not keep pointers to nodes that are going away.
    if (cb_.meterHidden)
        for (Row* row : rows_)
            if (row->bound()) cb_.meterHidden(meterOf(row));
}

std::optional<std::size_t> TcpPanel::sendRowAt(ui::Point p) const {
    const ui::Rect lanes = cb_.lanesRect ? cb_.lanesRect() : ui::Rect{};
    if (!bounds().contains(p) && !lanes.contains(p)) return std::nullopt;
    for (std::size_t row = 0; row < layout_.rowCount(); ++row) {
        const ui::Rect r = rowRect(row);
        if (p.y >= r.y && p.y < r.bottom()) return row;
    }
    return std::nullopt;
}

// The row a send drag would feed: not its own, nor one it already sends to.
std::optional<std::size_t> TcpPanel::sendTargetAt(ui::Point p) const {
    const auto row = sendRowAt(p);
    if (!row || !sendFrom_) return std::nullopt;
    const model::Id target = layout_.rows()[*row].track;
    if (target == *sendFrom_ || model::sendsTo(session_, *sendFrom_, target)) return std::nullopt;
    return row;
}

void TcpPanel::cancelSend() {
    sendFrom_.reset();
    sendMoved_ = false;
    if (root()) root()->requestAnimationFrame();
}

bool TcpPanel::sendPointer(const ui::PointerEvent& e, const model::Id& from,
                           const ui::Rect& anchor) {
    if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
        sendFrom_ = from;
        sendAnchor_ = anchor;
        sendPress_ = sendPointer_ = e.pos;
        sendMoved_ = false;
        if (root() && !sendOverlay_) {
            sendOverlayHandle_ = root()->addOverlay(std::make_unique<SendGhost>(alive_));
            sendOverlay_ = true;
        }
        return true;
    }
    if (!sendFrom_) return false;
    if (e.type != ui::PointerEvent::Type::Move && e.type != ui::PointerEvent::Type::Up)
        return false;
    sendPointer_ = e.pos;
    sendMoved_ = sendMoved_ || std::hypot(e.pos.x - sendPress_.x, e.pos.y - sendPress_.y) >= 4;
    if (root()) root()->requestAnimationFrame();
    if (e.type == ui::PointerEvent::Type::Up && e.button == 1) {
        const model::Id source = *sendFrom_;
        const bool moved = sendMoved_;
        const ui::Rect button = sendAnchor_;
        const auto row = sendTargetAt(e.pos);
        cancelSend();
        if (!moved) {
            if (cb_.routing) cb_.routing(source, button);
        } else if (row && cb_.addSend) {
            const model::Id target = layout_.rows()[*row].track;
            auto result = cb_.addSend(source, target);
            if (!result && cb_.onError) cb_.onError(result.error());
        }
    }
    return true;
}

// A line between rows where the dragged track would land.
class TcpPanel::ReorderGhost : public ui::Node {
  public:
    explicit ReorderGhost(std::shared_ptr<TcpPanel*> alive)
        : Node("tcp.reorder-ghost"), alive_(std::move(alive)) {}
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        if (!*alive_) return;
        const auto& panel = **alive_;
        if (!panel.reorder_ || !panel.reorder_->moved || panel.reorder_->moves.empty()) return;
        const ui::Rect tcp = panel.bounds();
        const float y = panel.reorder_->markerY;
        if (y < tcp.y || y > tcp.bottom()) return;
        const auto colour = c.theme().color("focus");
        c.fillRect({tcp.x, y - 1, tcp.w, 3}, colour);
        c.fillRect({tcp.x, y - 4, 4, 9}, colour);
    }

  private:
    std::shared_ptr<TcpPanel*> alive_;
};

void TcpPanel::beginReorder(const model::Id& track, ui::Point press) {
    const model::Track* t = session_.tracks.find(track);
    if (!t || t->type == model::StripType::Master) return;
    reorder_ = Reorder{track, press, false, {}, 0};
}

void TcpPanel::moveReorder(ui::Point p) {
    if (!reorder_) return;
    if (!reorder_->moved) {
        if (std::hypot(p.x - reorder_->press.x, p.y - reorder_->press.y) < 4) return;
        reorder_->moved = true;
        if (root())
            reorderOverlayHandle_ = root()->addOverlay(std::make_unique<ReorderGhost>(alive_));
    }
    updateReorder(p);
    invalidate();
    if (root()) root()->requestAnimationFrame();
}

// The drop slot under the pointer: in front of the first same-type sibling whose
// row's middle is below it, else after the last one (and its descendants and
// automation lanes). Other types and other parents are never targets.
void TcpPanel::updateReorder(ui::Point p) {
    Reorder& r = *reorder_;
    r.moves.clear();
    const auto own = layout_.indexOf(r.track);
    r.home = own && rowRect(*own).contains({rowRect(*own).x, p.y});  // still over its own row
    auto selected = [this](model::Id id) { return cb_.selected && cb_.selected(id); };
    const std::vector<model::Id> group = reorderGroup(session_, r.track, selected);
    const model::Track* t = session_.tracks.find(r.track);
    const model::Track* parent = t && t->parent ? session_.tracks.find(*t->parent) : nullptr;
    if (!parent) return;
    std::optional<model::Id> before;
    std::optional<float> end;
    float markerY = 0;
    const auto& rows = layout_.rows();
    for (model::Id id : displayChildren(session_, *parent)) {
        const model::Track* s = session_.tracks.find(id);
        const auto row = layout_.indexOf(id);
        if (!s || s->type != t->type || !row ||
            std::find(group.begin(), group.end(), id) != group.end())
            continue;
        const ui::Rect rect = rowRect(*row);
        if (p.y < rect.y + rect.h / 2) {
            before = id;
            markerY = rect.y;
            break;
        }
        std::size_t last = *row;
        while (last + 1 < rows.size() &&
               (rows[last + 1].lane() || rows[last + 1].depth > rows[*row].depth))
            ++last;
        end = rowRect(last).bottom();
    }
    if (!before) {
        if (!end) return;  // nothing of this type to reorder among
        markerY = *end;
    }
    r.moves = reorderMoves(session_, r.track, selected, before);
    r.home = r.home || r.moves.empty();  // or over the slot it is already in
    r.markerY = markerY;
}

void TcpPanel::endReorder(bool commit) {
    if (!reorder_) return;
    const Reorder r = *reorder_;
    reorder_.reset();
    reorderOverlayHandle_.reset();
    invalidate();
    if (commit && r.moved && cb_.commit) {
        // Several tracks: one gesture, so the moves are one undo step.
        const std::string gesture =
            r.moves.size() > 1 ? gestureFor("tcp.reorder", r.track, ++gestureSerial_) : "";
        for (const TrackMove& m : r.moves) {
            json::Value args = trackArg(m.track);
            args["index"] = static_cast<std::int64_t>(m.index);
            cb_.commit("track.move", std::move(args), gesture);
        }
    }
}

ui::Rect TcpPanel::rowRect(std::size_t row) const {
    return {bounds().x, bounds().y + layout_.yOf(row) - view_.scrollY, bounds().w,
            layout_.heightOf(row)};
}

ui::Meter* TcpPanel::meterOf(Row* row) const {
    return row ? static_cast<ui::Meter*>(row->control("meter")) : nullptr;
}

// ----------------------------------------------------------------- EnvRow

// An automation lane's header, indented under its strip: the parameter
// first, then what owns it (elided before the parameter is), and the cells in
// fixed columns: "mod" (its modulation's source; empty without a route), the
// automation mode (click cycles read → touch → latch → write → trim), whether
// the envelope is heard, and hiding the lane (the envelope keeps playing). A
// lane of a modulated parameter without an envelope has its title, "mod" and
// "hide". From 32 units tall the title has a line of its own, the cells under
// it; a lower lane puts both on one line.
class TcpPanel::EnvRow : public ui::Node {
  public:
    EnvRow(TcpPanel& panel, std::size_t slot)
        : Node("tcp.env" + std::to_string(slot), ui::Role::Group), panel_(panel) {
        const std::string id = this->id();
        clipsChildren = true;
        title_ = &emplace<ui::Label>(id + ".title", "", ui::Font::Ui, "small", "ink-quiet");
        mod_ = &emplace<ui::Button>(id + ".mod", "mod", [this] {
            if (target_ && panel_.cb_.modulationSource)
                panel_.cb_.modulationSource(*target_, mod_->bounds());
        });
        mod_->setTextColor("signal-cv");
        mode_ = &emplace<ui::Button>(id + ".mode", "read", [this] {
            const model::Envelope* e = envelope();
            if (!e) return;
            static constexpr const char* kCycle[] = {"read", "touch", "latch", "write", "trim"};
            const std::string now = modeName(e->mode);
            std::size_t at = 0;
            while (at < 5 && now != kCycle[at]) ++at;
            set("mode", json::Value(std::string(kCycle[(at + 1) % 5])));
        });
        active_ = &emplace<ui::Toggle>(id + ".active", "on", "meter",
                                       [this](bool on) { set("active", json::Value(on)); });
        hide_ = &emplace<ui::Button>(id + ".hide", "hide", [this] { hideLane(); });
        mod_->hint = {"Show the modulation source", {}};
        mode_->hint = {"Automation mode", {}};
        active_->hint = {"Envelope on or off", {}};
        hide_->hint = {"Hide the lane", {}};
    }
    void bind(const model::EnvTarget& target, std::optional<model::Id> env, int depth,
              const model::Session& session) {
        target_ = target;
        env_ = env;
        depth_ = depth;
        refresh(session);
    }
    void refresh(const model::Session& session) {
        if (!target_) return;
        const model::Envelope* e = env_ ? session.envelopes.find(*env_) : nullptr;
        model::Envelope probe;
        probe.target = *target_;
        const model::Envelope& shown = e ? *e : probe;
        const auto scale = panel_.layout_.scaleOf(shown);
        const LaneTitle title = laneTitle(shown, session, scale ? &*scale : nullptr);
        const bool mod = !routesOn(session, *target_).empty(), envelope = e != nullptr;
        const bool changed = title.headerText() != title_parts_.headerText() ||
                             mod != mod_->visible || envelope != mode_->visible;
        title_parts_ = title;
        mod_->visible = mod;
        mode_->visible = active_->visible = envelope;
        if (e) {
            mode_->setText(modeName(e->mode));
            active_->setOn(e->active);
        }
        if (changed) {  // the title is fitted, and the cells placed, in arrange
            panel_.rearrange_ = true;
            invalidate();
        }
    }
    void unbind() {
        target_.reset();
        env_.reset();
        visible = false;
    }
    bool bound() const { return target_.has_value(); }
    const std::optional<model::Id>& env() const { return env_; }
    const model::EnvTarget& target() const { return *target_; }
    ui::Node* control(std::string_view what) const {
        if (what == "title") return title_;
        if (what == "mod") return mod_;
        if (what == "mode") return mode_;
        if (what == "active") return active_;
        if (what == "hide") return hide_;
        return nullptr;
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override {
        setBounds(r);
        const float x = r.x + 26 + 14.0f * static_cast<float>(depth_ + 1);
        const float right = r.right() - 6;
        // The cells' fixed columns: mod, mode, on, hide.
        const float cells = kModW + 2 + kModeW + 2 + kActiveW + 2 + kHideW;
        const bool twoLines = r.h >= 32;
        const float titleY = twoLines ? r.y + std::max(1.0f, (r.h - 32) / 2 + 1)
                                      : r.y + std::max(2.0f, (r.h - 16) / 2);
        const float cellY = twoLines ? titleY + 16 : titleY + 1;
        const float titleW = twoLines ? right - x : std::max(24.0f, right - x - cells - 4);
        const float cx = twoLines ? x : right - cells;
        title_->setText(fitTitle(canvas, titleW));
        title_->arrange(canvas, {x, titleY, titleW, 16});
        mod_->arrange(canvas, {cx, cellY, kModW, 14});
        mode_->arrange(canvas, {cx + kModW + 2, cellY, kModeW, 14});
        active_->arrange(canvas, {cx + kModW + kModeW + 4, cellY, kActiveW, 14});
        hide_->arrange(canvas, {cx + kModW + kModeW + kActiveW + 6, cellY, kHideW, 14});
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRect(bounds(), t.color("ground").withAlpha(0.6f));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1},
                   t.color("rule").withAlpha(0.4f));
    }

  private:
    static constexpr float kModW = 32, kModeW = 44, kActiveW = 22, kHideW = 34;
    // The header's words in `width`: the parameter whole when it fits, what
    // owns it elided first, then the parameter.
    std::string fitTitle(const ui::Canvas& c, float width) const {
        const float size = c.theme().type("small");
        const std::string full = title_parts_.headerText();
        if (c.textWidth(full, ui::Font::Ui, size) <= width) return full;
        std::string rest = title_parts_.owner;
        if (!title_parts_.note.empty()) rest += (rest.empty() ? "" : " ") + title_parts_.note;
        const std::string head = title_parts_.param + " · ";
        const float room = width - c.textWidth(head, ui::Font::Ui, size);
        const std::string fit = c.elide(rest, ui::Font::Ui, size, room);
        if (!fit.empty() && room > c.textWidth("M…", ui::Font::Ui, size)) return head + fit;
        return c.elide(title_parts_.param, ui::Font::Ui, size, width);
    }
    static std::string modeName(model::EnvMode m) {
        switch (m) {
            case model::EnvMode::Trim:
                return "trim";
            case model::EnvMode::Read:
                return "read";
            case model::EnvMode::Touch:
                return "touch";
            case model::EnvMode::Latch:
                return "latch";
            case model::EnvMode::LatchPreview:
                return "preview";
            case model::EnvMode::Write:
                return "write";
        }
        return "read";
    }
    const model::Envelope* envelope() const {
        return env_ ? panel_.session_.envelopes.find(*env_) : nullptr;
    }
    void set(const char* key, json::Value value) {
        if (!env_ || !panel_.cb_.commit) return;
        json::Value args = json::Value::object();
        args["env"] = env_->str();
        args[key] = std::move(value);
        panel_.cb_.commit("env.set", std::move(args), {});
    }
    // Hide: the envelope's `visible` and each route's `lane` off, one undo
    // step; the envelope and the routes keep playing.
    void hideLane() {
        if (!target_ || !panel_.cb_.commit) return;
        const model::Session& s = panel_.session_;
        std::vector<std::pair<std::string, json::Value>> edits;
        if (const model::Envelope* e = envelope(); e && e->visible) {
            json::Value args = json::Value::object();
            args["env"] = env_->str();
            args["visible"] = false;
            edits.emplace_back("env.set", std::move(args));
        }
        for (model::Id route : routesOn(s, *target_))
            if (s.modulations.find(route)->lane) {
                json::Value args = json::Value::object();
                args["modulation"] = route.str();
                args["lane"] = false;
                edits.emplace_back("modulation.set", std::move(args));
            }
        const std::string gesture =
            edits.size() > 1 ? "tcp.lane.hide:" + std::to_string(++panel_.gestureSerial_) : "";
        for (auto& [command, args] : edits) panel_.cb_.commit(command, std::move(args), gesture);
    }
    TcpPanel& panel_;
    std::optional<model::EnvTarget> target_;
    std::optional<model::Id> env_;
    int depth_ = 0;
    LaneTitle title_parts_;
    ui::Label* title_ = nullptr;
    ui::Button* mod_ = nullptr;
    ui::Button* mode_ = nullptr;
    ui::Toggle* active_ = nullptr;
    ui::Button* hide_ = nullptr;
};

void TcpPanel::setSession(const model::Session& session) {
    session_ = session;
    if (reorder_ && !session_.tracks.contains(reorder_->track)) endReorder(false);
    rearrange_ = true;
    bindRows();
    for (Row* row : rows_)
        if (row->visible && row->bound()) row->refresh(session_);
    for (EnvRow* row : envRows_)
        if (row->visible && row->bound()) row->refresh(session_);
    invalidate();
}

void TcpPanel::updateSession(const model::Session& session, const std::set<model::Id>& tracks,
                             const std::set<model::Id>& envelopes,
                             const std::set<model::Id>& routes, bool modulations) {
    session_ = session;
    for (auto* row : rows_) {
        if (!row->visible || !row->bound()) continue;
        const auto* track = session_.tracks.find(row->track());
        if (tracks.contains(row->track()) ||
            (track && track->record.input && routes.contains(*track->record.input))) {
            row->refresh(session_);
            row->invalidate();  // includes the colour stripe and type/selection badge
        }
    }
    // Every shown lane header: its title names effects, sends and groups that
    // may have been renamed, and its "mod" follows the routes. A refresh that
    // changes nothing redraws nothing.
    (void)envelopes;
    (void)modulations;
    for (auto* row : envRows_)
        if (row->visible && row->bound()) row->refresh(session_);
}

// Modulation (ADR-0096): a bar beside the fader and each pan bar that a
// route moves, with its live value; a dual pan's bars take none.
void TcpPanel::Row::showModulation(const model::Session& session) {
    if (!track_) return;
    const auto split = pan_->split();
    auto modulation = [&](const char* param) -> std::optional<ui::ModulationView> {
        if (!stripModulatable(session, *track_, param)) return std::nullopt;
        const auto mod = controlModulation(session, {*track_, param}, panel_.layout_.unipolarSource,
                                           &panel_.layout_.liveOffsets);
        return mod ? std::optional(mod->view) : std::nullopt;
    };
    fader_->setModulation(modulation("volume"));
    pan_->setModulation(0, split == ui::PanControl::Split::Dual ? std::nullopt : modulation("pan"));
    pan_->setModulation(1,
                        split == ui::PanControl::Split::Width ? modulation("width") : std::nullopt);
}

void TcpPanel::refreshModulation() {
    if (session_.modulations.empty() && layout_.liveOffsets.empty()) return;
    for (Row* row : rows_)
        if (row->bound()) row->showModulation(session_);
}

void TcpPanel::refresh() {
    for (Row* row : rows_)
        if (row->bound()) row->refresh(session_);
    for (EnvRow* row : envRows_)
        if (row->bound()) row->refresh(session_);
}

void TcpPanel::bindRows() {
    // The rows the current scroll shows; one pooled row each.
    std::vector<std::size_t> shownRows, shownLanes;
    for (std::size_t i = 0; i < layout_.rowCount(); ++i) {
        const ui::Rect r = rowRect(i);
        if (r.bottom() <= bounds().y) continue;
        if (r.y >= bounds().bottom() - kFootHeight) break;  // under the foot
        // Every lane has a header, with its envelope or without one (a
        // modulated parameter).
        const TrackRow& spec = layout_.rows()[i];
        if (!spec.lane())
            shownRows.push_back(i);
        else
            shownLanes.push_back(i);
    }
    while (envRows_.size() < shownLanes.size())
        envRows_.push_back(&rowsHost_->emplace<EnvRow>(*this, envRows_.size()));
    for (std::size_t i = 0; i < envRows_.size(); ++i) {
        if (i >= shownLanes.size()) {
            envRows_[i]->unbind();
            continue;
        }
        const TrackRow& spec = layout_.rows()[shownLanes[i]];
        envRows_[i]->visible = true;
        envRows_[i]->bind(*spec.target, spec.env, spec.depth, session_);
    }
    while (rows_.size() < shownRows.size())
        rows_.push_back(&rowsHost_->emplace<Row>(*this, rows_.size()));
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Row* row = rows_[i];
        if (i >= shownRows.size()) {  // scrolled away: release the meter, keep the node
            if (row->bound() && cb_.meterHidden) cb_.meterHidden(meterOf(row));
            row->unbind();
            continue;
        }
        const TrackRow& spec = layout_.rows()[shownRows[i]];
        const model::Track* tr = session_.tracks.find(spec.track);
        if (!tr) continue;
        const bool rebind = !row->bound() || row->track() != spec.track;
        if (rebind && row->bound() && cb_.meterHidden) cb_.meterHidden(meterOf(row));
        row->visible = true;
        if (rebind) {
            row->bind(spec.track, spec.depth, *tr, session_);
            if (cb_.meterShown) cb_.meterShown(spec.track.str(), meterOf(row));
        }
    }
}

void TcpPanel::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    setBounds(r);
    rearrange_ = false;
    const float foot = std::min(kFootHeight, r.h);
    rowsHost_->arrange(canvas, {r.x, r.y, r.w, r.h - foot});
    foot_->arrange(canvas, {r.x, r.bottom() - foot, r.w, foot});
    bindRows();
    for (Row* row : rows_) {
        if (!row->visible || !row->bound()) continue;
        const auto it = std::find_if(
            layout_.rows().begin(), layout_.rows().end(),
            [&](const TrackRow& spec) { return spec.track == row->track() && !spec.lane(); });
        if (it == layout_.rows().end()) continue;
        row->arrange(canvas, rowRect(static_cast<std::size_t>(it - layout_.rows().begin())));
    }
    for (EnvRow* row : envRows_) {
        if (!row->visible || !row->bound()) continue;
        if (const auto at = layout_.indexOfTarget(row->target()))
            row->arrange(canvas, rowRect(*at));
    }
}

void TcpPanel::scrolled() {
    rearrange_ = true;
    invalidate();
}

void TcpPanel::paint(ui::Canvas& c) {
    // Children paint after this, so re-binding and re-placing the rows here
    // shows them at the new scroll in this same frame.
    if (rearrange_) arrange(c, bounds());
    c.fillRect(bounds(), c.theme().color("panel"));
}

// A double-click on the panel's empty space (no row, not the foot's buttons)
// adds a channel, as "+ track" > Channel does; a right-click there opens the
// "+ track" menu at the pointer.
bool TcpPanel::onPointer(const ui::PointerEvent& e) {
    const bool doubleClick = e.button == 1 && e.clicks >= 2;
    if (e.type != ui::PointerEvent::Type::Down || !(doubleClick || e.button == 3) || !cb_.addStrip)
        return false;
    for (const auto& row : rowsHost_->children())
        if (row->visible && row->bounds().intersect(rowsHost_->bounds()).contains(e.pos))
            return false;
    for (const ui::Button* b : {add_, mixer_})
        if (b->bounds().contains(e.pos)) return false;
    if (e.button == 3)
        openAddMenu({e.pos.x, e.pos.y, 1, 1}, ui::Placement::Below);
    else
        cb_.addStrip("channel");
    return true;
}

ui::Node* TcpPanel::hitTest(ui::Point p) {
    for (ui::Button* b : {add_, mixer_})
        if (b && b->visible && b->enabled && b->bounds().contains(p)) return b;
    return ui::Node::hitTest(p);
}

ui::Node* TcpPanel::rowNode(const model::Id& track) const {
    for (Row* row : const_cast<TcpPanel*>(this)->rows_)
        if (row->visible && row->bound() && row->track() == track) return row;
    return nullptr;
}

ui::Node* TcpPanel::envControl(const model::Id& env, std::string_view what) const {
    for (EnvRow* row : const_cast<TcpPanel*>(this)->envRows_)
        if (row->visible && row->bound() && row->env() == env) return row->control(what);
    return nullptr;
}

ui::Node* TcpPanel::laneControl(const model::EnvTarget& target, std::string_view what) const {
    for (EnvRow* row : const_cast<TcpPanel*>(this)->envRows_)
        if (row->visible && row->bound() && row->target() == target) return row->control(what);
    return nullptr;
}

bool TcpPanel::rename(const model::Id& track) {
    for (Row* row : rows_)
        if (row->visible && row->bound() && row->track() == track) {
            row->startRename();
            return true;
        }
    return false;
}

ui::Node* TcpPanel::controlNode(const model::Id& track, std::string_view what) const {
    for (Row* row : const_cast<TcpPanel*>(this)->rows_)
        if (row->visible && row->bound() && row->track() == track) return row->control(what);
    return nullptr;
}

}  // namespace supple::views
