// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mixer controls shared by the track control panel and the mixer (gui.md
// §4.2, §4.4): a dB fader, a pan control, a stereo meter with peak hold, and
// a toggle (mute, solo, arm, monitor). Drags report a gesture: Begin, Move…,
// End — callers commit each move with the same cmd::Origin::gesture so a drag
// is one undo step. Values set from the model (setValue) never call back.
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "supple/ui/node.h"

namespace supple::ui {

enum class Gesture { Begin, Move, End, Set };  // Set: one-shot (double-click reset, wheel, keys)

// dB <-> 0..1 position: a square-root taper over -60..+12 dB, -inf below.
// Unity sits near 70 % of the travel; the bottom 2 % is silence.
double faderPosition(double db);
double faderDb(double position);
constexpr double kFaderSilenceDb = -150.0;

class Fader : public Node {
  public:
    bool wheel = true;  // false: wheel events pass to the parent (a scrolled list)
    enum class Orientation { Vertical, Horizontal };
    Fader(std::string id, Orientation orientation = Orientation::Vertical);
    std::function<void(double db, Gesture)> onChange;
    void setValue(double db);  // from the model; no callback during a drag
    double value() const { return db_; }
    bool dragging() const { return dragging_; }
    void cancelGesture() { dragging_ = false; }
    unsigned gestureModifiers() const { return gestureMods_; }

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;
    bool coalescesDrag() const override { return true; }

  private:
    Rect track() const;
    void change(double db, Gesture g);
    Orientation orientation_;
    double db_ = 0, pressDb_ = 0;
    Point press_{};
    bool dragging_ = false;
    unsigned gestureMods_ = 0;
};

// A normalized slider: [0, 1] along its width, for values whose curve the
// caller owns (a plugin parameter through core-params, a wet/dry percent).
// Like the fader, a press never jumps the value: drags are relative to the
// press (Shift for fine), a double click resets to the caller's default and
// the keys/wheel step. Drags report a gesture, so a drag is one undo step.
class Slider : public Node {
  public:
    bool wheel = true;  // false: wheel events pass to the parent (a scrolled list)
    explicit Slider(std::string id);
    std::function<void(double value, Gesture)> onChange;
    void setValue(double value);  // from the model; no callback
    double value() const { return value_; }
    // What a double click (and the caller's "reset") returns to.
    void setDefault(double value) { defaultValue_ = std::clamp(value, 0.0, 1.0); }
    double defaultValue() const { return defaultValue_; }
    bool dragging() const { return dragging_; }
    static constexpr float kInset = 10;  // knob travel inset per side

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;
    bool coalescesDrag() const override { return true; }

  protected:
    virtual double dragDelta(Point from, Point to) const;

  private:
    Rect track() const;
    void change(double value, Gesture g);
    double value_ = 0, pressValue_ = 0, defaultValue_ = 0;
    Point press_{};
    bool dragging_ = false;
};

// A normalized round knob, with the slider's gestures/default/keys and a
// relative vertical drag (150 design units over its full travel; Shift fine).
class Knob : public Slider {
  public:
    explicit Knob(std::string id);
    void setValue(double value);  // model readback never takes over an active drag
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;

  protected:
    double dragDelta(Point from, Point to) const override;
};

// Pan -100 (left) .. +100 (right) as a horizontal bar from the centre. Split
// (ADR-0066), it is two half-width bars side by side: dual pan's left and
// right inputs, or a position and a width (-100 swapped .. 0 mono .. 100).
class PanControl : public Node {
  public:
    enum class Split { None, Dual, Width };
    bool wheel = true;  // false: wheel events pass to the parent (a scrolled list)
    explicit PanControl(std::string id);
    // The first bar (pan, or dual's left input) and the second (dual's right
    // input, or the width).
    std::function<void(double pan, Gesture)> onChange;
    std::function<void(double value, Gesture)> onSecondChange;
    void setValue(double pan);
    void setSecond(double value);
    void setSplit(Split split);
    double value() const { return pan_; }
    double second() const { return second_; }
    Split split() const { return split_; }
    int part() const { return part_; }  // the bar the last gesture moved: 0 or 1
    void cancelGesture() { dragging_ = false; }
    unsigned gestureModifiers() const { return gestureMods_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;
    bool coalescesDrag() const override { return true; }

  private:
    void change(int part, double value, Gesture g);
    double home(int part) const;  // a double-click's value
    Rect track(int part) const;
    double pan_ = 0, second_ = 100, pressValue_ = 0;
    Split split_ = Split::None;
    int part_ = 0;
    Point press_{};
    bool dragging_ = false;
    unsigned gestureMods_ = 0;
};

// Stereo peak/RMS meter, -60..+6 dBFS. Readings come from EngineApp::meters
// (what was heard since the last call); the meter decays and holds peaks
// itself and asks for animation frames while anything moves. A click resets
// the clip indicators.
class Meter : public Node {
  public:
    explicit Meter(std::string id, bool vertical = true);
    void push(const float peak[2], const float rms[2],
              std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void reset();
    bool clipped() const { return clip_[0] || clip_[1]; }
    float shownPeakDb(int channel) const { return peakDb_[channel]; }
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    void tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Includes a held peak waiting to decay; clip latches alone do not animate.
    bool moving() const;

  private:
    bool vertical_;
    float peakDb_[2] = {-150, -150}, rmsDb_[2] = {-150, -150}, holdDb_[2] = {-150, -150};
    std::chrono::steady_clock::time_point holdAt_[2]{}, last_{};
    bool clip_[2] = {false, false};
};

// A latching button (mute, solo, arm): text, an "on" colour token.
class Toggle : public Node {
  public:
    Toggle(std::string id, std::string text, std::string onColor,
           std::function<void(bool)> onToggle = {});
    std::function<void(bool)> onToggle;
    void setOn(bool on);
    bool on() const { return on_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return on_ ? "on" : "off"; }
    // A strip's mute and solo: a rounded pill whose letter and outline carry
    // the "on" colour, filled with it when on.
    bool pill = false;

  protected:
    bool pressed() const { return pressed_; }

  private:
    std::string text_, onColor_;
    bool on_ = false, pressed_ = false;
};

// Record arm: a red disc, faded when off and solid when armed.
class ArmButton : public Toggle {
  public:
    explicit ArmButton(std::string id, std::function<void(bool)> onToggle = {});
    void paint(Canvas& canvas) override;
};

// A frozen track's mark: a six-armed snowflake of `radius` around `centre`.
void drawSnowflake(Canvas& c, Point centre, float radius, Color ink);

// Input monitoring (off, on, auto): a small speaker, an outline when off,
// solid when on, green when auto. A click runs onClick (the owner cycles it).
class MonitorButton : public Node {
  public:
    enum class Mode { Off, On, Auto };
    explicit MonitorButton(std::string id, std::function<void()> onClick = {});
    std::function<void()> onClick;
    void setMode(Mode mode);
    Mode mode() const { return mode_; }
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override {
        return mode_ == Mode::Off ? "off" : mode_ == Mode::On ? "on" : "auto";
    }

  private:
    Mode mode_ = Mode::Off;
    bool pressed_ = false;
};

}  // namespace supple::ui
