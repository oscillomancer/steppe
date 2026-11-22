// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "launch_capture.h"

#include <utility>

#include "supple/model/schemas.h"
#include "supple/views/launcher.h"

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
        auto r = run("launch.capture", std::move(args));  // also cancels a pending end
        if (!r) return std::move(r).error();
        armed_ = true;
        ending_ = false;
        capturing_ = app_.launchCapturing();
        return {};
    }
    // Playing, the capture ends on the project's launch quantize boundary,
    // as a scene launch made now would land, and is committed once heard there
    // (service). Off again while it is ending ends it now (commit), also
    // when the end was another caller's.
    engine::Engine* engine = app_.engine();
    const auto transport = app_.transport("get");
    if (!ending() && engine && engine->launchCapturing() && transport && transport->playing) {
        json::Value args = json::Value::object();
        args["on"] = false;
        args["quantize"] =
            model::launchQuantizeSchema().encode(app_.head().project->launchQuantize);
        auto r = run("launch.capture", std::move(args));
        if (!r) return std::move(r).error();
        armed_ = capturing_ = false;
        if (engine->launchCapturing()) {
            ending_ = true;
            return {};
        }
    }
    armed_ = false;
    capturing_ = false;
    ending_ = false;
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
    if (onNotice)
        if (const auto notice =
                views::followNotice(app_.head(), "launch.commit", json::Value::object(), *r))
            onNotice(*notice);
    // Stopped (a commit at stop, or the toggle turned off while stopped):
    // every track returns to the arrangement where playing starts, so play
    // plays the take just committed, not the clips again. While playing (a
    // seek or a loop change ended it, or the toggle went off mid-take) the
    // tracks keep what they play. A refusal here is not the commit's.
    if (auto transport = app_.transport("get"); transport && !transport->playing)
        if (auto back = run("launch.arrangement", json::Value::object()); !back && onError)
            onError(back.error());
    return {};
}

void CaptureControl::service() {
    if (ending_) {
        // Turned off while playing: committed once the engine has heard the
        // boundary, or as soon as a stop or a seek ended it first.
        if (app_.engine() && app_.launchCapturing()) return;
        ending_ = false;
        if (!app_.engine()) return;
        if (auto st = commit(); !st) {
            Error refusal = keep(std::move(st).error());
            if (onError) onError(refusal);
        }
        return;
    }
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
