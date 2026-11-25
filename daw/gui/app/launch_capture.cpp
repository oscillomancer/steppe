// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "launch_capture.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "supple/model/schemas.h"
#include "supple/views/launcher.h"
#include "supple/views/object_name.h"

namespace supple::gui {

namespace {
const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
}  // namespace

CaptureControl::CaptureControl(engine::EngineApp& app, cmd::Dispatcher& dispatcher)
    : app_(app), dispatcher_(dispatcher) {}

Result<json::Value> CaptureControl::run(const std::string& command, json::Value args) {
    return dispatcher_.execute({command, std::move(args), kOrigin});
}

namespace {
using CaptureView = engine::EngineApp::CaptureView;
using CaptureCommit = engine::EngineApp::CaptureCommit;
using CaptureEnd = engine::EngineApp::CaptureEnd;

// A take kept for the pane: uncommitted, with something to write, and not
// declined (an undo took its commit back).
bool pending(const std::optional<CaptureView>& v) {
    return v && v->capture.placed && v->capture.frames > 0 && !v->capture.launches.empty() &&
           v->commit == CaptureCommit::None;
}
}  // namespace

std::string CaptureControl::takeWords(const CaptureView& v) {
    const model::Session& s = app_.head();
    const int first = s.timeMap->barBeat(v.startBeat).bar;
    // The end is where the next bar would start: its last bar is the one
    // holding the frame before it.
    const int last = s.timeMap->barBeat(std::max(v.startBeat, v.endBeat - 1e-6)).bar;
    const std::size_t n = v.capture.launches.size();
    return (first == last ? "bar " + std::to_string(first)
                          : "bars " + std::to_string(first) + "\u2013" + std::to_string(last)) +
           ", " + std::to_string(n) + (n == 1 ? " launch" : " launches");
}

bool CaptureControl::kept() { return !keptLine().empty(); }

std::string CaptureControl::keptLine() {
    // Asked every frame (the header's cell); launchCapture() walks the
    // history (review round 168 S1), so it is asked again only when what a
    // kept take depends on can have changed.
    const KeptKey key{app_.projectGeneration(),
                      app_.head().version,
                      app_.engine() != nullptr,
                      app_.launchCapturing(),
                      lit(),
                      warned_,
                      refusal_};
    if (keptKey_ && *keptKey_ == key) return keptLine_;
    keptKey_ = key;
    keptLine_ = describeKept();
    return keptLine_;
}

std::string CaptureControl::describeKept() {
    if (lit()) return {};
    const auto v = app_.launchCapture();
    if (!pending(v)) return {};
    std::string line = "A take is kept, not committed: " + takeWords(*v) + ".";
    if (!refusal_.empty())
        line += " Its commit was refused: " + refusal_ + ".";
    else if (v->endedBy == CaptureEnd::EngineStop)
        line += " The engine stopped during it.";
    else if (v->endedBy == CaptureEnd::Device)
        line += " The audio device restarted during it.";
    line += warned_ ? " Click Capture again to drop it and arm." : " Click Capture to commit it.";
    return line;
}

std::string CaptureControl::lostLine() {
    const auto v = app_.launchCapture();
    if (!pending(v)) return {};
    std::string line = "The launcher take (" + takeWords(*v) + ") could not be committed";
    line += refusal_.empty() ? std::string(".") : ": " + refusal_ + ".";
    return line + " It will be lost; Save saves the project without it.";
}

std::string CaptureControl::takeKey() {
    const auto v = app_.launchCapture();
    if (!pending(v)) return {};
    return std::to_string(v->capture.startSample) + ":" + std::to_string(v->capture.frames) + ":" +
           std::to_string(v->capture.launches.size()) + ":" + std::to_string(v->capture.version);
}

Error CaptureControl::keep(Error refusal) {
    armed_ = capturing_ = false;
    refusal = views::objectNames(app_.head(), std::move(refusal));
    refusal_ = refusal.message;
    refusal.message += "; the performance is kept: fix that, then click Capture to commit it";
    return refusal;
}

void CaptureControl::settle() {
    if (auto st = commit(); !st) {
        Error refusal = keep(std::move(st).error());
        if (onError) onError(refusal);
    }
}

Status CaptureControl::finalize() {
    armed_ = capturing_ = ending_ = false;
    warned_ = false;
    if (auto st = commit(); !st) {
        Error refusal = std::move(st).error();
        (void)keep(refusal);
        return refusal;
    }
    return {};
}

Status CaptureControl::set(bool on) {
    if (on ? armed_ : !lit()) return {};
    if (on) {
        // A kept take is committed first and never armed over: a refusal
        // keeps it, and only the click after the one that said so drops it.
        // A declined take (its commit undone) is not committed again: the
        // new capture replaces it once it hears a frame.
        const auto view = app_.launchCapture();
        bool dropping = false;
        if (pending(view) && (view->kept || !app_.launchCapturing())) {
            if (auto st = commit(); !st) {
                Error refusal = views::objectNames(app_.head(), std::move(st).error());
                refusal_ = refusal.message;
                if (!warned_) {
                    warned_ = true;
                    refusal.message +=
                        "; the performance is still kept: click Capture again to drop "
                        "it and arm";
                    return refusal;
                }
                dropping = true;  // warned: dropped, and the arming replaces it
            }
        }
        const bool declined =
            view && view->commit == CaptureCommit::Undone && app_.captureUncommitted();
        json::Value args = json::Value::object();
        args["on"] = true;
        auto r = run("launch.capture", std::move(args));  // also cancels a pending end
        if (!r) return std::move(r).error();
        // Dropped by the user: the app keeps no copy of it for later (an
        // engine stop's warning, the unsaved-work question; ADR-0097).
        if (dropping) app_.dropKeptCapture();
        warned_ = false;
        refusal_.clear();
        replacing_ = declined && !dropping;
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
    if (engine && engine->launchCapturing()) {
        json::Value args = json::Value::object();
        args["on"] = false;
        auto r = run("launch.capture", std::move(args));
        if (!r) return std::move(r).error();
    }
    // What launch.commit has not written: the take just ended, or one the
    // app kept across an engine stop or a device restart (ADR-0097; review
    // round 168 F1). A take already committed is skipped, so none is written
    // twice, and so is one whose commit was undone: the user declined it.
    // Nothing ran, or every track followed the arrangement throughout: there
    // is nothing to write, and no empty undo step.
    const auto view = app_.launchCapture();
    if (!pending(view)) return {};
    // A take the engine ended is said, once, as committed (the warnings of
    // engine.stop and of the device events, in the pane's words).
    std::string said;
    if (view->kept && view->endedBy != CaptureEnd::None)
        said = std::string(view->endedBy == CaptureEnd::Device
                               ? "The audio device restarted during the capture"
                               : "The engine stopped during the capture") +
               ": its take (" + takeWords(*view) + ") is committed.";
    auto r = run("launch.commit", json::Value::object());
    if (!r) return std::move(r).error();
    refusal_.clear();
    warned_ = false;
    if (const auto notice =
            views::followNotice(app_.head(), "launch.commit", json::Value::object(), *r))
        said += (said.empty() ? "" : " ") + *notice;
    if (onNotice && !said.empty()) onNotice(said);
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
    if (replacing_) {
        // Armed over a declined take: the new capture replaced it once it
        // heard a frame (said once); a redo of its commit, or its project
        // going, ends the wait with nothing to say.
        const auto v = app_.launchCapture();
        if (v && !v->kept) {
            replacing_ = false;
            if (onNotice) onNotice("The earlier capture was replaced by this one.");
        } else if (!v || v->commit != CaptureCommit::Undone) {
            replacing_ = false;
        }
    }
    if (!refusal_.empty() || warned_)
        if (!pending(app_.launchCapture())) {  // committed, dropped or replaced
            refusal_.clear();
            warned_ = false;
        }
    if (ending_) {
        // Turned off while playing: committed once the engine has heard the
        // boundary, or as soon as a stop, a seek or the engine's own stop
        // ended it first.
        if (app_.engine() && app_.launchCapturing()) return;
        ending_ = false;
        settle();
        return;
    }
    if (!armed_) return;
    if (!app_.engine()) {
        // The engine stopped or the device is restarting: the app kept the
        // take, which is committed now; nothing arms by itself after it.
        armed_ = capturing_ = false;
        settle();
        return;
    }
    const bool now = app_.launchCapturing();
    if (capturing_ && !now) {
        // The engine ended a capture that ran: commit it. A take the app
        // kept (this engine is not the one it ran on, or none is armed any
        // more) leaves the toggle dark; otherwise the next one is armed. A
        // refusal keeps it and arms nothing, since arming would replace it.
        const auto v = app_.launchCapture();
        const bool restarted = !v || v->kept;
        armed_ = !restarted;
        if (auto st = commit(); !st) {
            Error refusal = keep(std::move(st).error());
            if (onError) onError(refusal);
            return;
        }
        if (restarted) {
            capturing_ = false;
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
