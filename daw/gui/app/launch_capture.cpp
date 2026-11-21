// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "launch_capture.h"

#include <utility>

namespace supple::gui {

namespace {
const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
}  // namespace

CaptureControl::CaptureControl(engine::EngineApp& app, cmd::Dispatcher& dispatcher)
    : app_(app), dispatcher_(dispatcher) {}

Result<json::Value> CaptureControl::run(const std::string& command, json::Value args) {
    return dispatcher_.execute({command, std::move(args), kOrigin});
}

Error CaptureControl::keep(Error refusal) {
    armed_ = capturing_ = false;
    kept_ = Kept::Refused;
    keptGeneration_ = app_.projectGeneration();
    refusal.message += "; the performance is kept: fix that, then click Capture to commit it";
    return refusal;
}

Status CaptureControl::set(bool on) {
    if (on ? armed_ : !lit()) return {};
    if (on) {
        // A kept performance belongs to its project and its engine.
        if (kept_ != Kept::None && (!app_.engine() || app_.projectGeneration() != keptGeneration_))
            kept_ = Kept::None;
        if (kept_ != Kept::None) {
            if (auto st = commit(); st) {
                kept_ = Kept::None;
            } else if (kept_ == Kept::Refused) {
                kept_ = Kept::Warned;
                Error refusal = std::move(st).error();
                refusal.message +=
                    "; the performance is still kept: click Capture again to drop "
                    "it and arm";
                return refusal;
            } else {
                kept_ = Kept::None;  // warned: dropped, and the arming replaces it
            }
        }
        json::Value args = json::Value::object();
        args["on"] = true;
        auto r = run("launch.capture", std::move(args));
        if (!r) return std::move(r).error();
        armed_ = true;
        capturing_ = app_.launchCapturing();
        return {};
    }
    armed_ = false;
    capturing_ = false;
    if (auto st = commit(); !st) return keep(std::move(st).error());
    return {};
}

Status CaptureControl::commit() {
    engine::Engine* engine = app_.engine();
    if (!engine) return {};
    if (engine->launchCapturing()) {
        json::Value args = json::Value::object();
        args["on"] = false;
        auto r = run("launch.capture", std::move(args));
        if (!r) return std::move(r).error();
    }
    // Nothing ran, or every track followed the arrangement throughout: there
    // is nothing to write, and no empty undo step.
    const auto capture = engine->launchCapture();
    if (!capture || capture->frames <= 0 || capture->launches.empty()) return {};
    auto r = run("launch.commit", json::Value::object());
    if (!r) return std::move(r).error();
    return {};
}

void CaptureControl::service() {
    if (!armed_) return;
    if (!app_.engine()) {  // the engine stopped: the capture went with it
        armed_ = capturing_ = false;
        return;
    }
    const bool now = app_.launchCapturing();
    if (capturing_ && !now) {
        // The engine ended a capture that ran: commit it, and arm the next.
        // A refusal keeps it and arms nothing, since arming would replace it.
        if (auto st = commit(); !st) {
            Error refusal = keep(std::move(st).error());
            if (onError) onError(refusal);
            return;
        }
        json::Value args = json::Value::object();
        args["on"] = true;
        if (auto r = run("launch.capture", std::move(args)); !r) {
            armed_ = false;
            if (onError) onError(r.error());
        }
    }
    capturing_ = app_.launchCapturing();
}

}  // namespace supple::gui
