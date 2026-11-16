// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// dsp.preview, a plugin's editor as a picture (ADR-0032 §8), shared by the
// engine and the sandbox: a fresh instance, set by a patch, plays a test
// signal (or notes) while its editor is watched, so meters and the
// script's own displays show what played; then the editor takes the
// pointer actions given (clicks, drags, turns of the wheel), is drawn
// without a window (com.suppleaudio.editor-picture/1) and saved as a PNG for
// an agent to look at, with what the actions edited as a merge patch.
// Renders as dsp.test does (TestTraits<Host>).
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "artifact_name.h"
#include "dsp_test.h"
#include "png.h"
#include "supple/json/merge.h"

namespace supple::dsp_commands {

namespace preview {

// What the pointer does before the picture, in the editor's design units.
enum class Action { Move, Press, Release, Click, DoubleClick, Drag, Wheel };
struct PointerStep {
    Action action = Action::Click;
    double x = 0.0, y = 0.0;
    std::optional<double> toX, toY;  // a drag's end
    double steps = 1.0;              // the wheel's notches, positive up
    bool shift = false, ctrl = false, alt = false;
};
inline constexpr std::size_t kMaxSteps = 64;

struct PreviewArgs {
    std::optional<std::string> plugin, script, audioFile;
    std::optional<json::Value> patch;
    std::optional<test::Signal> signal;
    double levelDb = -12.0;
    double frequencyHz = 1000.0;
    double seconds = 1.0;
    double scale = 1.0;
    std::optional<std::vector<PointerStep>> pointer;
};
struct PreviewResult {
    std::string plugin;
    std::string path;
    std::int64_t width = 0, height = 0;
    json::Value edits = json::Value::object();
    std::vector<std::string> problems;
    std::vector<std::string> log;
};

// The steps as a mouse would make them: a click is a move there, a press
// and a release; a drag, a press, moves in eight steps, and a release.
inline std::vector<clap_supple_pointer_input_t> pointerInput(
    const std::vector<PointerStep>& steps) {
    std::vector<clap_supple_pointer_input_t> out;
    for (const PointerStep& s : steps) {
        const std::uint32_t keys = (s.shift ? std::uint32_t{CLAP_SUPPLE_POINTER_SHIFT} : 0u) |
                                   (s.ctrl ? std::uint32_t{CLAP_SUPPLE_POINTER_CTRL} : 0u) |
                                   (s.alt ? std::uint32_t{CLAP_SUPPLE_POINTER_ALT} : 0u);
        auto add = [&](std::uint32_t kind, double x, double y, std::uint32_t more = 0,
                       double wheel = 0.0) { out.push_back({kind, keys | more, x, y, wheel}); };
        add(CLAP_SUPPLE_POINTER_MOVE, s.x, s.y);
        switch (s.action) {
            case Action::Move:
                break;
            case Action::Press:
                add(CLAP_SUPPLE_POINTER_PRESS, s.x, s.y);
                break;
            case Action::Release:
                add(CLAP_SUPPLE_POINTER_RELEASE, s.x, s.y);
                break;
            case Action::Click:
            case Action::DoubleClick:
                add(CLAP_SUPPLE_POINTER_PRESS, s.x, s.y);
                add(CLAP_SUPPLE_POINTER_RELEASE, s.x, s.y);
                if (s.action == Action::DoubleClick) {
                    add(CLAP_SUPPLE_POINTER_PRESS, s.x, s.y, CLAP_SUPPLE_POINTER_DOUBLE_CLICK);
                    add(CLAP_SUPPLE_POINTER_RELEASE, s.x, s.y);
                }
                break;
            case Action::Drag: {
                const double toX = s.toX.value_or(s.x), toY = s.toY.value_or(s.y);
                add(CLAP_SUPPLE_POINTER_PRESS, s.x, s.y);
                for (int k = 1; k <= 8; ++k)
                    add(CLAP_SUPPLE_POINTER_MOVE, s.x + (toX - s.x) * k / 8.0,
                        s.y + (toY - s.y) * k / 8.0);
                add(CLAP_SUPPLE_POINTER_RELEASE, toX, toY);
                break;
            }
            case Action::Wheel:
                add(CLAP_SUPPLE_POINTER_WHEEL, s.x, s.y, 0, s.steps);
                break;
        }
    }
    return out;
}

}  // namespace preview

template <class Host>
void registerDspPreview(cmd::Registry& registry, cmd::Exposure exposure = cmd::Exposure::McpTool) {
    using namespace test;
    using namespace preview;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<PreviewArgs, PreviewResult> c;
    c.name = "dsp.preview";
    c.kind = Kind::Action;
    c.exposure = exposure;
    c.summary = "Draw a plugin's editor to a PNG, after a test signal plays through it.";
    c.description =
        "Give plugin (an id) or script (a name in the library). A fresh instance, set by patch (a "
        "merge patch) when given, plays signal -- an effect: sine (the default, at frequency_hz), "
        "sweep, impulse, noise or silence, peaking at level_db, for seconds, or file (the "
        "default with audio_file), the file played whole as dsp.test plays it; an instrument: "
        "notes (the default), note or silence, for seconds -- while its editor watches, so "
        "meters and the script's draw() show what played. Then the editor is drawn without a "
        "window, scale "
        "times its design size, and saved as a PNG (path; width and height in pixels): read the "
        "file to see it. A script with neither editor() nor draw() shows the automatic editor. "
        "pointer, if given, acts on the editor first, in its design units (the picture's pixels "
        "at scale 1): move (a hover; a drag after a press), press, release, click, double_click, "
        "drag (from x, y to to_x, to_y) and wheel (steps notches, up positive), with shift, ctrl "
        "or alt held; edits is a merge patch of what they changed, as a user's edits would. "
        "problems says what went wrong in the editor's code (a loop stopped by its budget, an "
        "index out of range), with the line. Headless pictures use EGL on Linux, Metal on "
        "macOS and WGL on Windows; an unavailable renderer returns dsp.no_picture.";
    c.tags = {"dsp", "editor"};
    const auto place = [] {
        return schema::number()
            .range(-1000, 5000)
            .doc(
                "design units: the picture's pixels at "
                "scale 1");
    };
    auto step =
        schema::object<PointerStep>("dsp_pointer_step")
            .field("action", &PointerStep::action,
                   schema::enumeration<Action>({{Action::Move, "move"},
                                                {Action::Press, "press"},
                                                {Action::Release, "release"},
                                                {Action::Click, "click"},
                                                {Action::DoubleClick, "double_click"},
                                                {Action::Drag, "drag"},
                                                {Action::Wheel, "wheel"}}))
            .field("x", &PointerStep::x, place())
            .field("y", &PointerStep::y, place())
            .field("to_x", &PointerStep::toX, place().doc("drag: where it ends"))
            .field("to_y", &PointerStep::toY, place().doc("drag: where it ends"))
            .field("steps", &PointerStep::steps,
                   schema::number().range(-20, 20).doc("wheel: notches turned, up positive"))
            .defaultValue(1.0)
            .field("shift", &PointerStep::shift, schema::boolean())
            .defaultValue(false)
            .field("ctrl", &PointerStep::ctrl, schema::boolean())
            .defaultValue(false)
            .field("alt", &PointerStep::alt, schema::boolean())
            .defaultValue(false)
            .build();
    c.args = schema::object<PreviewArgs>("dsp_preview_args")
                 .field("plugin", &PreviewArgs::plugin, schema::string().minLength(1))
                 .field("script", &PreviewArgs::script, schema::string().minLength(1))
                 .field("patch", &PreviewArgs::patch, schema::any())
                 .field("signal", &PreviewArgs::signal,
                        schema::enumeration<Signal>({{Signal::Sine, "sine"},
                                                     {Signal::Sweep, "sweep"},
                                                     {Signal::Impulse, "impulse"},
                                                     {Signal::Noise, "noise"},
                                                     {Signal::Silence, "silence"},
                                                     {Signal::Notes, "notes"},
                                                     {Signal::Note, "note"},
                                                     {Signal::File, "file"}}))
                 .field("audio_file", &PreviewArgs::audioFile,
                        schema::string().minLength(1).doc(
                            "the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file "
                            "signal plays through an effect"))
                 .field("level_db", &PreviewArgs::levelDb,
                        schema::number().range(-60, 0).unit(units::Unit::dB))
                 .defaultValue(-12.0)
                 .field("frequency_hz", &PreviewArgs::frequencyHz,
                        schema::number().range(10, 20000).unit(units::Unit::Hz))
                 .defaultValue(1000.0)
                 .field("seconds", &PreviewArgs::seconds, schema::number().range(0, 10))
                 .defaultValue(1.0)
                 .field("scale", &PreviewArgs::scale, schema::number().range(0.5, 3))
                 .defaultValue(1.0)
                 .field("pointer", &PreviewArgs::pointer, schema::arrayOf(step))
                 .build();
    c.result = schema::object<PreviewResult>("dsp_preview_result")
                   .field("plugin", &PreviewResult::plugin, schema::string())
                   .field("path", &PreviewResult::path, schema::string().doc("the PNG"))
                   .field("width", &PreviewResult::width, schema::integer().doc("pixels"))
                   .field("height", &PreviewResult::height, schema::integer().doc("pixels"))
                   .field("edits", &PreviewResult::edits,
                          schema::any().doc("what pointer changed, as a merge patch"))
                   .field("problems", &PreviewResult::problems, schema::arrayOf(schema::string()))
                   .field("log", &PreviewResult::log, schema::arrayOf(schema::string()))
                   .build();
    c.errors = {{"dsp.args", "neither or both of plugin and script", "give exactly one"},
                {"dsp.id_taken", "another script in the library has the script's id",
                 "give each script its own Info.id"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.signal", "a signal the plugin cannot play",
                 "an effect plays audio signals, an instrument notes"},
                {"dsp.file",
                 "the file signal without audio_file, or a file that is not absolute, is over 10 "
                 "minutes or holds no audio",
                 "give audio_file a WAV, FLAC or MP3, or another signal"},
                {"dsp.pointer", "a drag without its end, or too many steps",
                 "give a drag to_x and to_y; at most 64 steps"},
                {"dsp.no_picture", "the plugin cannot draw its editor without a window",
                 "scripts can, through the script player; other plugins need the editor-picture "
                 "extension"}};
    {
        json::Value args = json::Value::object();
        args["script"] = "gain";
        json::Value drag = json::Value::object();
        drag["script"] = "gain";
        drag["pointer"] = detail::j(R"([{"action":"drag","x":86,"y":132,"to_x":86,"to_y":92}])");
        c.examples = {{"See a script's editor", std::move(args),
                       detail::j(R"({"plugin":"com.example.gain","path":"<any>","width":"<any>",
                                     "height":"<any>","edits":{},"problems":[],"log":"<any>"})"),
                       "dsp_player_editor"},
                      {"Drag a knob, then see the editor", std::move(drag),
                       detail::j(R"({"plugin":"com.example.gain","path":"<any>","width":"<any>",
                                     "height":"<any>","edits":{"params":{"gain":"<any>"}},"problems":[],
                                     "log":"<any>"})"),
                       "dsp_player_editor"}};
    }
    c.run = [](const PreviewArgs& a, Context& ctx) -> Result<PreviewResult> {
        Host& s = ctx.service<Host>();
        using Traits = TestTraits<Host>;
        if (a.plugin.has_value() == a.script.has_value())
            return Error("dsp.args", "give exactly one of plugin and script", "",
                         "plugin is an id from plugin.list; script is a name from dsp.list");
        const std::vector<PointerStep> steps = a.pointer.value_or(std::vector<PointerStep>{});
        if (steps.size() > kMaxSteps)
            return Error("dsp.pointer",
                         std::to_string(steps.size()) + " pointer steps; at most " +
                             std::to_string(kMaxSteps),
                         "/pointer", "split the actions over more than one call");
        for (std::size_t i = 0; i < steps.size(); ++i)
            if (steps[i].action == Action::Drag && !(steps[i].toX && steps[i].toY))
                return Error("dsp.pointer", "a drag needs where it ends",
                             "/pointer/" + std::to_string(i),
                             "give to_x and to_y: the drag goes from (x, y) to there");
        std::string id;
        if (a.plugin) {
            id = *a.plugin;
        } else {
            auto text = detail::readScript(s, *a.script, "/script");
            if (!text) return std::move(text).error();
            auto compiled = detail::compile(*text);
            if (!compiled.plugin)
                return Error("dsp.compile", "the script does not compile", "/script",
                             "dsp.check {name: \"" + *a.script + "\"} gives its diagnostics");
            id = compiled.plugin->id;
            SUPPLE_TRY(detail::uniqueId(s, id, *a.script, "/script"));
        }
        auto info = s.findPlugin(id);
        if (!info && a.script) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
            if (!info)
                return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                             "the script player must be on the plugin path (--plugin-path)");
        }
        if (!info) return std::move(info).error().at("/plugin");
        auto created = host::Instance::create(*info, Traits::identity(s));
        if (!created) return std::move(created).error();
        host::Instance& instance = **created;
        if (!instance.drawsEditor())
            return Error("dsp.no_picture", info->name + " cannot draw its editor without a window",
                         a.plugin ? "/plugin" : "/script",
                         "scripts can, through the script player; other plugins need "
                         "com.suppleaudio.editor-picture/1");
        const bool instrument = instance.audioInputChannels() == 0;
        const Signal sig = a.signal.value_or(instrument    ? Signal::Notes
                                             : a.audioFile ? Signal::File
                                                           : Signal::Sine);
        if (sig != Signal::Silence && forInstruments(sig) != instrument)
            return Error("dsp.signal",
                         info->name +
                             (instrument ? " is an instrument: it plays notes, not "
                                         : " takes audio, not ") +
                             signalName(sig),
                         "/signal",
                         instrument ? "use notes, note or silence"
                                    : "use sine, sweep, impulse, noise, silence or file");
        if (sig == Signal::File && !a.audioFile)
            return Error("dsp.file", "the file signal plays audio_file, which is not given",
                         "/audio_file", "give audio_file a WAV, FLAC or MP3 to play");
        if (a.patch) {
            if (!instance.info().semantic)
                return Error("dsp.not_semantic", info->name + " takes no patch", "/patch",
                             "set it by plugin.load and param.set instead");
            auto applied = instance.setPatch(json::writeCompact(*a.patch), CLAP_SUPPLE_SET_MERGE);
            if (!applied) return patchError(std::move(applied).error());
        }

        PreviewResult out;
        out.plugin = info->id;
        SUPPLE_TRY(instance.watchEditor(true));
        if (a.seconds > 0.0 || sig == Signal::File) {
            const sandbox::RenderSettings settings = Traits::settings(s);
            audio::AudioData input;
            if (sig == Signal::File) {
                auto loaded = fileSignal(*a.audioFile, settings.sampleRate, out.log);
                if (!loaded) return std::move(loaded).error();
                input = std::move(*loaded);
            } else if (!instrument) {
                input = stimulus(sig, settings.sampleRate, a.seconds, a.levelDb, a.frequencyHz);
            }
            sandbox::RenderRequest request;
            if (instrument) {
                request.events = noteEvents(sig, settings.sampleRate, a.seconds);
                request.lengthSamples =
                    static_cast<std::int64_t>(std::llround(a.seconds * settings.sampleRate));
            } else {
                request.input = &input;
            }
            host::Instance* chain[] = {&instance};
            auto r = sandbox::render(chain, settings, request);
            if (!r) return std::move(r).error();
            out.log.insert(out.log.end(), r->log.begin(), r->log.end());
        }
        // What the pointer edits: the patch before and after.
        auto patchNow = [&]() -> json::Value {
            if (steps.empty() || !instance.info().semantic) return json::Value::object();
            auto text = instance.getPatch(false);
            if (!text) return json::Value::object();
            auto parsed = json::parse(*text);
            return parsed ? std::move(*parsed) : json::Value::object();
        };
        const json::Value before = patchNow();
        const std::vector<clap_supple_pointer_input_t> input = pointerInput(steps);
        auto picture = instance.drawEditor(a.scale, input);
        (void)instance.watchEditor(false);
        if (!picture)
            return Error(
                "dsp.no_picture", picture.error().message, a.plugin ? "/plugin" : "/script",
#if defined(__APPLE__)
                "drawing without a window needs an editor build and an available Metal device"
#elif defined(_WIN32)
                "drawing without a window needs an editor build and an OpenGL driver with "
                "framebuffer objects; Windows' GDI Generic driver is insufficient"
#else
                "drawing without a window needs an editor build and EGL (Mesa's libEGL), "
                "with or without a GPU"
#endif
            );
        if (!steps.empty())
            if (auto edits = json::createMergePatch(before, patchNow()))
                out.edits = std::move(*edits);
        for (auto& line : instance.takeLog()) {
            if (line.find("loop budget") != std::string::npos ||
                line.find("outside an array") != std::string::npos)
                out.problems.push_back(line);
            out.log.push_back(info->name + ": " + line);
        }
        const std::filesystem::path file =
            Traits::outputDir(s) / ("dsp-preview-" + artifactId(info->id) + ".png");
        SUPPLE_TRY(png::write(file, picture->width, picture->height, picture->rgba));
        out.path = file.string();
        out.width = picture->width;
        out.height = picture->height;
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
