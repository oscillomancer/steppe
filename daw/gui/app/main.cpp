// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/base/paths.h>
#include <supple/model/groups.h>
#include "audio_edit.h"
// <slug>: the DAW with a window (docs/specs/gui.md). The UI thread is the
// model thread: it pumps the command bus's loop (socket requests, the service
// tick) between SDL events, and draws only when something changed.
//
//   <slug> [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG at exit]
//          [--bench] [--no-socket] [--instance NAME] [--socket PATH]
//          [--workdir DIR] [--plugin-path P]... [--generate N] [--script]
//          [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]
//          [--audio jack|portaudio|dummy|none] [--theme dark|light|FILE] [--palette warm|ink]
//          (without --audio, interactive runs start the device saved in Preferences)
//          [--action ID]...
#include <SDL3/SDL.h>
#if defined(_WIN32)
#include <SDL3/SDL_main.h>
#include <windows.h>
#endif
#include "supple/base/waitable.h"
#include "supple/gui/process.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "actions_list.h"
#include "app_actions.h"
#include "appearance.h"
#include "audio_device.h"
#include "chrome.h"
#include "file_actions.h"
#include "floating_panel.h"
#include "fx_window.h"
#include "generate.h"
#include "help_actions.h"
#include "history_panel.h"
#include "learn_dialog.h"
#include "meter_feed.h"
#include "midi_editor.h"
#include "mixer.h"
#include "mouse_dialog.h"
#include "plugin_windows.h"
#include "preferences.h"
#include "render_dialog.h"
#include "routing_matrix.h"
#include "screensets.h"
#include "shell.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/host/clap_host.h"
#include "supple/json/parse.h"
#include "supple/peaks/peaks.h"
#include "supple/product.h"
#include "supple/rpc/model_loop.h"
#include "supple/rpc/protocol.h"
#include "supple/ui/root.h"
#include "ui_persistence.h"

namespace fs = std::filesystem;
using namespace supple;

namespace {

struct Options {
    fs::path project, workdir, socket, snapshot;
    std::vector<fs::path> pluginPath;
    std::string instance = "default";
    bool offscreen = false, bench = false, serve = true, script = false;
    int width = 1280, height = 800, frames = 0;  // frames 0 = run until closed
    int generate = 0;                            // tracks of a generated session
    double idleSeconds = 0;                      // measure idle CPU after the frames
    double viewStart = -1, viewPps = 0;          // initial lanes viewport
    std::string audio;  // jack | portaudio | dummy | none; native default: dummy
    // Override the saved appearance for this run (not saved): dark | light | a
    // supple.theme/1.0 file, and warm | ink.
    std::string theme, palette;
    // Run in order once the project and its UI state are loaded: an action id,
    // optionally followed by a space and a JSON object of its prompted inputs.
    std::vector<std::string> actions;
};

// MIDI learn's "enable only when selected": the effect's track is selected
// (its chain or input chain), or, for take FX, the take's item or its track.
bool fxSelected(const model::Session& s, const gui::UiState& ui, model::Id fx) {
    auto owner = model::trackOfFx(s, fx);
    if (!owner)
        for (const auto& t : s.tracks)
            if (std::find(t.value->inputChain.begin(), t.value->inputChain.end(), fx) !=
                t.value->inputChain.end())
                owner = t.id;
    if (owner) return gui::trackSelected(ui, owner->str());
    for (const auto& item : s.items)
        for (const model::Id& take : item.value->takes) {
            const model::Take* t = s.takes.find(take);
            if (!t || std::find(t->chain.begin(), t->chain.end(), fx) == t->chain.end()) continue;
            return gui::trackSelected(ui, item.value->track.str()) ||
                   std::find(ui.selectedItems.begin(), ui.selectedItems.end(), item.id.str()) !=
                       ui.selectedItems.end();
        }
    return false;
}

int usage() {
    std::fprintf(stderr,
                 "usage: %s [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG]\n"
                 "       [--bench] [--no-socket] [--instance NAME] [--socket PATH]\n"
                 "       [--workdir DIR] [--plugin-path P]... [--generate N] [--script]\n"
                 "       [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]\n"
                 "       [--audio jack|portaudio|dummy|none] [--theme dark|light|FILE] [--palette "
                 "warm|ink]\n"
                 "       [--action ID]...\n",
                 std::string(product::kSlug).c_str());
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--offscreen")
            o.offscreen = true;
        else if (a == "--bench")
            o.bench = true;
        else if (a == "--no-socket")
            o.serve = false;
        else if (a == "--script")
            o.script = true;
        else if (a == "--audio") {
            const char* v = value();
            if (!v || (std::string(v) != "jack" && std::string(v) != "portaudio" &&
                       std::string(v) != "dummy" && std::string(v) != "none"))
                return false;
            o.audio = v;
        } else if (a == "--action") {
            const char* v = value();
            if (!v) return false;
            o.actions.emplace_back(v);
        } else if (a == "--theme") {
            const char* v = value();
            if (!v) return false;
            o.theme = v;
        } else if (a == "--palette") {
            const char* v = value();
            if (!v || !ui::paletteFrom(v)) return false;
            o.palette = v;
        } else if (a == "--view") {
            const char* v = value();
            if (!v || std::sscanf(v, "%lf,%lf", &o.viewStart, &o.viewPps) != 2) return false;
        } else if (a == "--generate") {
            const char* v = value();
            if (!v) return false;
            o.generate = std::clamp(std::atoi(v), 1, 1024);
        } else if (a == "--idle") {
            const char* v = value();
            if (!v) return false;
            o.idleSeconds = std::max(0.0, std::atof(v));
        } else if (a == "--size") {
            const char* v = value();
            if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (a == "--frames") {
            const char* v = value();
            if (!v) return false;
            o.frames = std::max(1, std::atoi(v));
        } else if (a == "--snapshot") {
            const char* v = value();
            if (!v) return false;
            o.snapshot = v;
        } else if (a == "--instance") {
            const char* v = value();
            if (!v) return false;
            o.instance = v;
        } else if (a == "--socket") {
            const char* v = value();
            if (!v) return false;
            o.socket = v;
        } else if (a == "--workdir") {
            const char* v = value();
            if (!v) return false;
            o.workdir = v;
        } else if (a == "--plugin-path") {
            const char* v = value();
            if (!v) return false;
            o.pluginPath.emplace_back(v);
        } else if (!a.empty() && a[0] != '-' && o.project.empty())
            o.project = fs::absolute(a);
        else
            return false;
    }
    return o.width > 0 && o.height > 0;
}

// --theme / --palette over the saved appearance: dark or light, or a
// supple.theme/1.0 file of overrides ("base": "light" starts from light).
Result<ui::Theme> loadTheme(const std::string& spec, gui::Appearance& look) {
    if (spec == "dark" || spec == "light") look.dark = spec == "dark";
    if (spec.empty() || spec == "dark" || spec == "light") return look.theme();
    auto bytes = readFile(spec);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* base = doc->isObject() ? doc->find("base") : nullptr;
    look.dark = !(base && base->isString() && base->asString() == "light");
    return ui::Theme::fromJson(*doc, look.theme());
}

// The user's data folder: <XDG_DATA_HOME or ~/.local/share>/<slug>.
fs::path dataFolder() {
    if (const auto root = paths::data(); !root.empty()) return root / std::string(product::kSlug);
    return fs::temp_directory_path() / std::string(product::kSlug);
}

// An unsaved project's own folder, one per run (crashed runs' folders stay, with
// their recordings): <data>/<slug>/unsaved/<start time>-<pid>; under --workdir
// when one is given (tests), so nothing lands in the user's data folder.
fs::path scratchFolder(const Options& o) {
    const fs::path base = !o.workdir.empty() ? o.workdir : dataFolder();
    char stamp[32] = {};
    const std::time_t now = std::time(nullptr);
    const std::tm local = gui::localTime(now);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    return base / "unsaved" / (std::string(stamp) + "-" + std::to_string(gui::processId()));
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Preserve redirected handles from CTest/pipes. Only reopen streams with
    // no inherited handle when launched from a terminal without redirection.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        if (_fileno(stdout) < 0 && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR)
            (void)freopen_s(&stream, "CONOUT$", "w", stdout);
        if (_fileno(stderr) < 0 && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR)
            (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    Options o;
    if (!parse(argc, argv, o)) return usage();

    auto platform = gui::Platform::start(o.offscreen ? gui::Platform::Driver::Offscreen
                                                     : gui::Platform::Driver::Native);
    if (!platform) {
        std::fprintf(stderr, "%s\n", platform.error().toString().c_str());
        return 1;
    }
    auto window = gui::Window::open(**platform,
                                    {std::string(product::kName), o.width, o.height, o.offscreen});
    if (!window) {
        std::fprintf(stderr, "%s\n", window.error().toString().c_str());
        return 1;
    }
    fs::path fontDir = SUPPLE_GUI_FONTS_DIR;
    if (auto skill = host::skillDir(); skill && fs::is_directory(skill->parent_path() / "fonts"))
        fontDir = skill->parent_path() / "fonts";
    auto fonts = gui::loadFonts((*window)->vg(), fontDir);
    if (!fonts) {
        std::fprintf(stderr, "%s\n", fonts.error().toString().c_str());
        return 1;
    }

    engine::AppOptions options;
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
    // --plugin-path replaces the search path; otherwise the stock plugins built
    // beside this binary come before the standard locations.
    options.pluginPath = o.pluginPath;
    if (options.pluginPath.empty()) {
        if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
        for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    }
    // Without a project, the user's data folder, never the launch directory:
    // Finder starts apps in the read-only "/", and a launcher's choice (the
    // home or installation folder) would collect a stray renders/ folder.
    options.workdir = !o.workdir.empty()   ? o.workdir
                      : !o.project.empty() ? o.project.parent_path()
                                           : dataFolder();
    options.scratchDir = scratchFolder(o);
    // Renders go to the user's media folder (or the folder chosen in the render
    // dialog, a preference); under --workdir (tests) to <workdir>/renders.
    if (o.workdir.empty()) options.rendersDir = gui::mediaHome();
    // DSP scripts are the user's, shared by every project: <data>/<slug>/scripts;
    // under --workdir (tests) <workdir>/scripts.
    if (o.workdir.empty()) options.scriptDir = dataFolder() / "scripts";
    // Render presets and the render default likewise (ADR-0055).
    if (o.workdir.empty()) options.renderPresetDir = dataFolder() / "render-presets";
    auto app = engine::EngineApp::create(options);
    if (!app) {
        std::fprintf(stderr, "%s\n", app.error().toString().c_str());
        return 1;
    }
    // Leaving an unsaved project discards it: its recordings go with it.
    std::shared_ptr<void> discardUnsaved(nullptr, [&](void*) { (*app)->clearScratch(); });
    if (!o.project.empty() && fs::exists(o.project)) {
        auto opened = (*app)->open(o.project);
        if (!opened) {
            std::fprintf(stderr, "%s\n", opened.error().toString().c_str());
            return 1;
        }
    }
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    gui::UiState uiState;
    gui::registerGuiCommands(registry, uiState);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    if (o.generate > 0) {
        auto generated = gui::generateSession(dispatcher, (*app)->mediaDir(), o.generate);
        if (!generated) {
            std::fprintf(stderr, "%s\n", generated.error().toString().c_str());
            return 1;
        }
    }

    // The model loop runs inside the SDL loop: its work wakes SDL_WaitEvent.
    rpc::ModelLoop loop;
    loop.setWaker([p = platform->get()] { p->wake(); });
    std::unique_ptr<rpc::Server> server;
    auto publish = [&] {
        for (auto& [topic, payload] : (*app)->takeEvents())
            if (server) server->publish(topic, payload);
    };
    std::string status = "offscreen";
    if (o.serve) {
        server = std::make_unique<rpc::Server>(registry, dispatcher, [&](auto task) {
            loop.post([&, task = std::move(task)] {
                task();
                publish();
            });
        });
        const fs::path path = o.socket.empty() ? rpc::socketPath(o.instance) : o.socket;
        auto started = server->start(path);
        if (!started) {
            std::fprintf(stderr, "%s\n", started.error().toString().c_str());
            return 1;
        }
        status = "agents: " + path.string();
    }
    // The plugin bridge for isolated fx (and their editors, gap log #39).
    if (!host::locateBridgeExecutable(std::string(product::kSlug) + "-bridge"))
        std::fprintf(stderr, "no plugin bridge found: isolated fx will not start\n");
    peaks::PeakService peaks(peaks::defaultCacheDir(product::kSlug),
                             [&](std::function<void()> f) { loop.post(std::move(f)); });
    // One meter feed for both views (the TCP and the mixer), filled from what
    // the engine heard since the last poll, once per pass.
    gui::MeterFeed meterFeed([&] {
        std::vector<gui::StripReading> out;
        for (const engine::MeterReading& m : (*app)->meters()) {
            gui::StripReading r;
            r.track = m.track.str();
            for (int ch = 0; ch < 2; ++ch) {
                r.peak[ch] = m.peak[ch];
                r.rms[ch] = m.rms[ch];
            }
            out.push_back(r);
        }
        return out;
    });
    gui::Shell shell(**app, dispatcher, uiState, status, peaks, meterFeed);
    peaks.onReady = [&](const std::string&) { shell.arrange()->lanes().invalidate(); };
    const fs::path appearanceFile = gui::Appearance::defaultFile(product::kSlug);
    gui::Appearance look = gui::Appearance::load(appearanceFile);
    if (!o.palette.empty()) look.palette = *ui::paletteFrom(o.palette);
    auto theme = loadTheme(o.theme, look);
    if (!theme) {
        std::fprintf(stderr, "theme: %s\n", theme.error().toString().c_str());
        return 1;
    }
    ui::Root root(**window, *fonts, std::move(*theme));
    gui::FloatingPanel fxPanel(**platform, **window, root, SUPPLE_GUI_FONTS_DIR, "FX", 840, 560,
                               800, 360);
    gui::FloatingPanel routingPanel(**platform, **window, root, SUPPLE_GUI_FONTS_DIR, "Routing",
                                    860, 560, 800, 360);
    bool quit = false;
    std::vector<std::filesystem::path> dropped;  // files of a drop in progress
    ui::Point dropAt{};
    gui::AppActions appActions(registry, dispatcher, **app, uiState,
                               actions::Keymap::defaultFile(product::kSlug),
                               gui::ControlMap::defaultFile(product::kSlug));
    appActions.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    auto chromeLayout = gui::ChromeLayout::load(gui::ChromeLayout::defaultFile(product::kSlug));
    if (!chromeLayout)
        std::fprintf(stderr, "menus: %s (using the defaults)\n",
                     chromeLayout.error().toString().c_str());
    // Toolbar and menu edits from the GUI are saved there (not in offscreen runs).
    gui::Chrome chrome(root, appActions,
                       chromeLayout ? *chromeLayout : gui::ChromeLayout::defaults(),
                       o.offscreen ? fs::path{} : gui::ChromeLayout::defaultFile(product::kSlug));
    chrome.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    appActions.addUi("ui.actions", "View: actions list",
                     [&] { gui::openActionsList(root, appActions, &chrome); });
    // The look: switching repaints at once and is saved (not in offscreen runs).
    auto applyLook = [&] {
        root.setTheme(look.theme());
        fxPanel.setTheme(root.theme());
        routingPanel.setTheme(root.theme());
        if (o.offscreen) return;
        if (auto saved = look.save(appearanceFile); !saved)
            std::fprintf(stderr, "appearance: %s\n", saved.error().toString().c_str());
    };
    appActions.addUi("ui.theme.mode", "View: switch light / dark", [&] {
        look.dark = !look.dark;
        applyLook();
    });
    appActions.addUi("ui.theme.palette", "View: switch warm / ink palette", [&] {
        look.palette = look.palette == ui::Palette::Warm ? ui::Palette::Ink : ui::Palette::Warm;
        applyLook();
    });
    gui::PluginWindows pluginWindows(**platform, **app, [p = platform->get()] { p->wake(); });
    pluginWindows.setModelWake(loop.waitable());
    pluginWindows.onError = [&](const Error& error) {
        if (shell.onError) shell.onError(error.message);
    };
    pluginWindows.setOwner(window->get());
    gui::MidiEditor midiEditor(root, appActions, dispatcher, **app, uiState);
    gui::RoutingMatrix routing(routingPanel, appActions, dispatcher, **app);
    shell.onRouting = [&routing](const model::Id&, const ui::Rect& anchor) {
        routing.open(anchor);
    };
    gui::Mixer mixer(root, appActions, dispatcher, **app, meterFeed);
    mixer.onAddStrip = [&shell](const std::string& type) { shell.addStrip(type); };
    mixer.inputText = [&shell](const model::Id& track) { return shell.inputLabel(track); };
    mixer.pickInput = [&shell](const model::Id& track, const ui::Rect& anchor) {
        shell.openInputMenu(track, anchor);
    };
    mixer.envelopes = [&shell](const model::Id& id, const ui::Rect& anchor) {
        shell.openEnvelopeMenu(id, anchor);
    };
    gui::HistoryPanel history(root, appActions, dispatcher, **app);
    gui::FileActions fileActions(root, appActions, dispatcher, **app, uiState, (*window)->sdl(),
                                 [&](std::function<void()> f) { loop.post(std::move(f)); });
    fileActions.registerQuit([&] { quit = true; }, !o.offscreen && o.frames <= 0);
    gui::HelpActions helpActions(
        root, appActions, (*window)->sdl(),
        [&](std::function<void()> f) { loop.post(std::move(f)); }, gui::help::ownExecutable());
    appActions.addUi(
        "ui.fx.editor", "FX: open or close editor", "fx", [&](const json::Value& v) -> Status {
            const auto id = v.isString() ? model::Id::parse(v.asString()) : std::nullopt;
            if (!id) return Error("gui.editor", "an effect id like fx_3", "/fx");
            return pluginWindows.toggle(*id);
        });
    gui::FxWindow fxWindow(
        **app, dispatcher, root, fxPanel,
        gui::FxWindow::Hooks{
            [&](model::Id id) { return pluginWindows.toggle(id); },
            [](const std::string& message) { std::fprintf(stderr, "%s\n", message.c_str()); },
            [&](gui::LearnRequest request) {
                gui::openLearnDialog(fxPanel.root(), appActions, **app, std::move(request));
            }},
        gui::Favourites::defaultFile(product::kSlug));
    // MIDI learn (ADR-0027): the devices' messages reach the actions bound to
    // them, or a learn dialog that listens. "Enable only when" asks the
    // selection and the windows. Published MIDI batches wake app service,
    // including while the transport is stopped or a learn dialog listens.
    (*app)->onControl = [&](const std::vector<engine::EngineApp::ControlEvent>& events) {
        appActions.control(events);
    };
    (*app)->learnGate = [&](model::Id fx, const model::ParamLearn& learn) {
        if (learn.whenSelected && !fxSelected((*app)->head(), uiState, fx)) return false;
        if (learn.whenVisible && !fxWindow.shows(fx) && !pluginWindows.isOpen(fx)) return false;
        if (learn.whenFocused && !(fxWindow.shows(fx) && fxWindow.hasFocus()) &&
            !pluginWindows.hasFocus(fx))
            return false;
        return true;
    };
    std::shared_ptr<void> releaseControl(nullptr, [&](void*) {
        (*app)->onControl = nullptr;
        (*app)->learnGate = nullptr;
    });
    appActions.addUiFor("ui.fx.chain", "FX: chain…", "track", [&](const json::Value& v) -> Status {
        return fxWindow.openFor(uiState, v);
    });
    appActions.addUi("ui.fx.move_up", "FX: move the selected plugin up the chain",
                     [&] { fxWindow.moveSelected(-1); });
    appActions.addUi("ui.fx.move_down", "FX: move the selected plugin down the chain",
                     [&] { fxWindow.moveSelected(1); });
    // How many channels the first selected track has (one track: the count is its own).
    appActions.addUi("ui.track.channels", "Track: channels…", [&] {
        const auto id = uiState.selectedTracks.empty()
                            ? std::nullopt
                            : model::Id::parse(uiState.selectedTracks.front());
        Status st = id ? shell.openChannelsMenu(*id, {})
                       : Status(Error("gui.track", "nothing is selected", "/track",
                                      "select a track first"));
        if (!st && appActions.onError) appActions.onError("Track: channels: " + st.error().message);
    });
    // The colour menu on the first selected track colours every selected one.
    appActions.addUiFor("ui.track.color", "Track: color…", "track",
                        [&](const json::Value& v) -> Status {
                            std::string text = v.isString() ? v.asString() : std::string{};
                            if (text.empty() && !uiState.selectedTracks.empty())
                                text = uiState.selectedTracks.front();
                            const auto id = model::Id::parse(text);
                            if (!id)
                                return Error("gui.track", "no track given and nothing is selected",
                                             "/track", "select a track first");
                            return shell.openColorMenu(*id, {});
                        });
    appActions.addUiFor(
        "ui.fx.add", "FX: add plugin…", "track",
        [&](const json::Value& v) -> Status { return fxWindow.openToAdd(uiState, v); });
    gui::RenderDialog renderDialog(**app, dispatcher, root);
    appActions.addUi("ui.render", "File: render…", [&] { renderDialog.open(); });
    // File → Mix: the project's render settings, as a job; the label names the
    // file it will write (gui.md §4.8).
    appActions.addUi("ui.render.mix", "File: mix", [&] { renderDialog.mixJob(); });
    appActions.setLabel("ui.render.mix", [&] { return renderDialog.mixLabel(); });
    appActions.onPrompt = [&](const actions::Action& action) {
        gui::openActionPrompt(root, appActions, action);
    };
    auto unhandledKey = [&](const ui::KeyEvent& e, ui::Root& source) {
        const bool typing = source.focused() && source.focused()->role() == ui::Role::TextField;
        return appActions.key(e.key, e.mods, typing, gui::keySections(source));
    };
    root.onUnhandledKey = [&](const ui::KeyEvent& e) { return unhandledKey(e, root); };
    fxPanel.onUnhandledKey = routingPanel.onUnhandledKey = unhandledKey;
    {
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->add(chrome.build());
        auto& body = frame->emplace<ui::Node>("main");
        body.layout.dir = ui::Layout::Dir::Row;
        auto& center = body.emplace<ui::Node>("center");
        center.layout.dir = ui::Layout::Dir::Column;
        center.layout.width = ui::SizeRule::fraction();
        center.add(shell.build(root, appActions));
        center.add(midiEditor.build());
        center.add(mixer.build());
        body.add(history.build());
        root.setContent(std::move(frame));
    }
    // Arrange editing gestures (P5.6): one transaction per gesture; a later
    // step may use an earlier one's result ("$step0.track").
    // Mouse modifiers (gap log #37): the arrange view's map, from the user's file.
    // The MIDI editor takes its wheel bindings from it too.
    const fs::path mouseFile = views::MouseMap::defaultFile(product::kSlug);
    midiEditor.setMouseMap(&shell.arrange()->gestures().mouse);
    gui::MouseDialog mouseDialog(root, shell.arrange()->gestures().mouse,
                                 o.offscreen ? fs::path{} : mouseFile);
    mouseDialog.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    appActions.addUi("ui.mouse", "Edit: mouse modifiers…", [&] { mouseDialog.open(); });
    appActions.addUi("ui.mouse.zoom", "Edit: wheel zoom anchor…",
                     [&] { mouseDialog.open("wheel.zoom"); });
    // Preferences (Edit menu): the default monitoring of new channels, the
    // wheel zoom anchor, a way into the mouse modifiers.
    const fs::path prefsFile = gui::Preferences::defaultFile(product::kSlug);
    gui::Preferences prefs;
    if (!o.offscreen) {
        if (auto loaded = gui::Preferences::load(prefsFile))
            prefs = *loaded;
        else
            std::fprintf(stderr, "preferences: %s (using the defaults)\n",
                         loaded.error().toString().c_str());
    }
    gui::PreferencesDialog preferences(root, **app, mouseDialog, prefs,
                                       o.offscreen ? fs::path{} : prefsFile);
    preferences.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    appActions.addUi("ui.preferences", "Edit: preferences…", [&] { preferences.open(); });
    // The audio device: saved with the preferences, applied to the engine at once.
    gui::AudioDeviceDialog audioDevice(
        root, dispatcher, **app, prefs.audio,
        [&](const gui::AudioDeviceSettings& audio) { return preferences.setAudioDevice(audio); });
    audioDevice.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    preferences.onAudioDevice = [&] { audioDevice.open(); };
    preferences.audioSummary = [&] { return audioDevice.summary(); };
    appActions.addUi("ui.audio.device", "Edit: audio device…", [&] { audioDevice.open(); });
    renderDialog.onStatus = [&](const std::string& message) { shell.setStatus(message); };
    renderDialog.openFolderWhenDone = [&] { return preferences.preferences().renderOpenFolder; };
    renderDialog.setOpenFolderWhenDone = [&](bool on) {
        if (auto set = preferences.setRenderOpenFolder(on); !set)
            std::fprintf(stderr, "preferences: %s\n", set.error().toString().c_str());
    };
    renderDialog.onChooseCover = [&] {
        const fs::path folder = (*app)->projectPath().empty() ? fs::path() : (*app)->projectDir();
        if (folder.empty()) {
            shell.setStatus("Cover: save the project first; the cover lives in its folder");
            return;
        }
        fileActions.chooseImage(folder, [&, folder](const fs::path& image) {
            const fs::path relative = image.lexically_relative(folder);
            if (relative.empty() || *relative.begin() == "..") {
                shell.setStatus("Cover: choose an image inside the project folder");
                return;
            }
            renderDialog.setCover(relative.generic_string());
        });
    };
    renderDialog.onChooseFolder = [&] {
        fileActions.chooseFolder((*app)->rendersDir(), [&](const fs::path& dir) {
            if (auto set = preferences.setRenderDirectory(dir); !set)
                std::fprintf(stderr, "preferences: %s\n", set.error().toString().c_str());
            renderDialog.showFolder();
        });
    };
    gui::AudioEdit audioEdit(**app, dispatcher);
    audioEdit.onStatus = [&](const std::string& message) { shell.setStatus(message); };
    {
        views::ItemGestures& g = shell.arrange()->gestures();
        g.onAudioEdit = [&](const auto& items, const auto& operation, double grid) {
            audioEdit.run(items, operation, grid);
        };
        g.onConsolidate = [&](const auto& items) { audioEdit.consolidate(items); };
        g.pinVersion = [&app](std::uint64_t version) { return (*app)->versions().pin(version); };
        const cmd::Origin ui{cmd::Origin::Type::Ui, "gui", {}};
        if (auto mouse = views::MouseMap::load(mouseFile))
            g.mouse = std::move(*mouse);
        else
            std::fprintf(stderr, "mouse: %s (using the defaults)\n",
                         mouse.error().toString().c_str());
        g.run = [&dispatcher, &app, ui](
                    const std::string& label,
                    const std::vector<views::EditStep>& steps) -> Result<json::Value> {
            return dispatcher.transact(label, ui, [&]() -> Result<json::Value> {
                const auto before = (*app)->view();
                std::map<std::string, std::set<model::Id>> explicitItems;
                for (const auto& step : steps)
                    if (const auto* item = step.args.find("item"); item && item->isString())
                        if (auto id = model::Id::parse(item->asString()))
                            explicitItems[step.command].insert(*id);
                std::vector<json::Value> results;
                std::vector<bool> bypassed;
                for (views::EditStep step : steps) {
                    bool bypass = false;
                    if (const auto* item = step.args.find("item"); item && item->isString()) {
                        const auto text = item->asString();
                        if (auto id = model::Id::parse(text)) {
                            const auto peers = model::linkedItems(before, *id);
                            bypass = peers.size() > 1 &&
                                     std::all_of(peers.begin(), peers.end(), [&](auto peer) {
                                         return explicitItems[step.command].contains(peer);
                                     });
                        } else if (text.rfind("$step", 0) == 0) {
                            const auto at = static_cast<std::size_t>(std::stoul(text.substr(5)));
                            bypass = at < bypassed.size() && bypassed[at];
                        }
                    }
                    if (bypass &&
                        (step.command == "item.set" || step.command == "item.move" ||
                         step.command == "item.split" || step.command == "item.duplicate" ||
                         step.command == "item.remove"))
                        step.args["bypass_groups"] = true;
                    bypassed.push_back(bypass);
                    if (step.args.isObject())
                        for (auto& [key, value] : step.args.asObject()) {
                            if (!value.isString() || value.asString().rfind("$step", 0) != 0)
                                continue;
                            const std::string ref = value.asString().substr(5);
                            const auto dot = ref.find('.');
                            const std::size_t at =
                                static_cast<std::size_t>(std::stoul(ref.substr(0, dot)));
                            if (at < results.size())
                                if (const json::Value* v = results[at].find(ref.substr(dot + 1)))
                                    value = *v;
                        }
                    auto r = dispatcher.execute({step.command, step.args, ui});
                    if (!r) return r;
                    results.push_back(std::move(*r));
                }
                return results.empty() ? json::Value::object()
                                       : results.back();  // the last step's result
            });
        };
        g.onSelection = [&](const std::vector<model::Id>& items) {
            uiState.selectedItems.clear();
            for (model::Id id : items) uiState.selectedItems.push_back(id.str());
            if (!items.empty()) uiState.tracksFocused = false;  // Delete takes the items now
        };
        g.onEnvelopeFocus = [&](bool lane) {
            uiState.envelopeFocused = lane;
            if (lane) uiState.tracksFocused = false;
        };
        g.onOpenMidi = [&](model::Id item) {
            if (auto r = midiEditor.open(item.str()); !r)
                std::fprintf(stderr, "midi: %s\n", r.error().toString().c_str());
        };
        g.onTakeFx = [&](model::Id take) {
            if (auto r = fxWindow.openTake(take); !r)
                std::fprintf(stderr, "take fx: %s\n", r.error().toString().c_str());
        };
        g.onError = [](const Error& e) {
            std::fprintf(stderr, "edit refused: %s\n", e.toString().c_str());
        };
        g.keysFor = [&](std::string_view action) { return appActions.keysFor(action); };
        g.onSeek = [&dispatcher, ui](double seconds) {
            json::Value args = json::Value::object();
            args["seconds"] = seconds;
            (void)dispatcher.execute({"transport.seek", std::move(args), ui});
        };
    }
    shell.arrange()->setRulerCallbacks(
        {[&dispatcher](const std::string& command, json::Value args) {
             return dispatcher.execute(
                 {command, std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
         },
         [](const Error& e) {
             std::fprintf(stderr, "ruler edit refused: %s\n", e.toString().c_str());
         }});
    // A new, empty MIDI item on the selected channel (else the first), over the
    // loop range when it is on, else one bar at the playhead; opened.
    appActions.addUi("ui.midi.new", "Item: new MIDI item", [&] {
        views::ArrangeView& a = *shell.arrange();
        const model::Session& s = (*app)->head();
        std::optional<model::Id> track;
        for (const auto& t : uiState.selectedTracks)
            if (auto id = model::Id::parse(t);
                id && s.tracks.find(*id) && s.tracks.find(*id)->type == model::StripType::Channel) {
                track = id;
                break;
            }
        for (const auto& row : a.trackLayout().rows())
            if (!track && !row.env && row.type == model::StripType::Channel) track = row.track;
        if (!track) return;
        const bool looping = a.loopEnabled() && a.loopEnd() > a.loopStart();
        const double from = s.timeMap->beatAt(looping ? a.loopStart() : a.playhead());
        const std::optional<double> to =
            looping ? std::optional<double>(s.timeMap->beatAt(a.loopEnd())) : std::nullopt;
        if (auto made = a.gestures().newMidiItem(*track, from, to); !made)
            std::fprintf(stderr, "new MIDI item: %s\n", made.error().toString().c_str());
    });
    // The item clipboard: paste at the playhead, onto the selected channel.
    auto reportEdit = [](const char* what, const Status& st) {
        if (!st) std::fprintf(stderr, "%s: %s\n", what, st.error().toString().c_str());
    };
    // After a click on an automation lane they take its selected points; after
    // a marquee that took items too, Delete removes both and the clipboard
    // keys take the items.
    auto playheadBeat = [&] { return (*app)->head().timeMap->beatAt(shell.arrange()->playhead()); };
    auto pointKeys = [&] {
        return uiState.envelopeFocused && !shell.arrange()->gestures().mixedSelection();
    };
    appActions.addUi("ui.env.points.delete", "Envelope: delete the selected points", [&] {
        auto& g = shell.arrange()->gestures();
        reportEdit("delete points", g.mixedSelection() ? g.deleteAll() : g.deletePoints());
    });
    appActions.addUi("ui.env.points.copy", "Envelope: copy the selected points",
                     [&] { shell.arrange()->gestures().copyPoints(); });
    appActions.addUi("ui.env.points.cut", "Envelope: cut the selected points",
                     [&] { reportEdit("cut points", shell.arrange()->gestures().cutPoints()); });
    appActions.addUi("ui.env.points.paste", "Envelope: paste points at the playhead", [&] {
        reportEdit("paste points", shell.arrange()->gestures().pastePoints(playheadBeat()));
    });
    appActions.addUi("ui.item.copy", "Item: copy selected (or automation points)", [&] {
        if (pointKeys())
            shell.arrange()->gestures().copyPoints();
        else
            shell.arrange()->gestures().copySelection();
    });
    appActions.addUi("ui.item.cut", "Item: cut selected (or automation points)", [&] {
        auto& g = shell.arrange()->gestures();
        if (pointKeys())
            reportEdit("cut points", g.cutPoints());
        else
            reportEdit("cut", g.cutSelection());
    });
    appActions.addUi("ui.item.paste", "Item: paste at the playhead (or automation points)", [&] {
        if (pointKeys()) {
            reportEdit("paste points", shell.arrange()->gestures().pastePoints(playheadBeat()));
            return;
        }
        views::ArrangeView& a = *shell.arrange();
        std::optional<model::Id> track;
        if (!uiState.selectedTracks.empty())
            track = model::Id::parse(uiState.selectedTracks.front());
        reportEdit("paste",
                   a.gestures().paste((*app)->head().timeMap->beatAt(a.playhead()), track));
    });
    appActions.addUi("ui.item.duplicate", "Item: duplicate selected after itself", [&] {
        reportEdit("duplicate", shell.arrange()->gestures().duplicateSelection());
    });
    appActions.addUi("ui.item.merge", "Item: merge selected MIDI items (or one item's takes)",
                     [&] { reportEdit("merge", shell.arrange()->gestures().mergeSelection()); });
    appActions.addUi("ui.item.consolidate", "Item: consolidate selected items",
                     [&] { shell.arrange()->gestures().consolidateSelection(); });
    appActions.addUi("ui.item.heal", "Item: heal selected audio splits",
                     [&] { reportEdit("heal", shell.arrange()->gestures().healSelection()); });
    appActions.addUi("item.split:playhead", "Item: split selected at cursor",
                     [&] { reportEdit("split", shell.arrange()->gestures().splitSelection()); });
    appActions.addUi("ui.item.split-zero", "Item: split selected at prior zero crossing", [&] {
        reportEdit("split", shell.arrange()->gestures().splitSelection(true));
    });
    // Navigation: the previous/next marker or region start from the playhead.
    auto jump = [&](bool forward) {
        views::ArrangeView& a = *shell.arrange();
        const model::Session& s = (*app)->head();
        auto secondsOf = [&](double pos, model::Timebase tb) {
            return tb == model::Timebase::Time ? pos : s.timeMap->secondsAt(pos);
        };
        std::vector<double> stops;
        for (const auto& m : s.markers) stops.push_back(secondsOf(m.value->pos, m.value->timebase));
        for (const auto& r : s.regions)
            stops.push_back(secondsOf(r.value->start, r.value->timebase));
        const double now = a.playhead();
        std::optional<double> best;
        for (double t : stops)
            if (forward ? t > now + 1e-6 : t < now - 1e-6)
                if (!best || (forward ? t < *best : t > *best)) best = t;
        if (!best) return;
        json::Value args = json::Value::object();
        args["seconds"] = *best;
        if (!dispatcher.execute(
                {"transport.seek", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}}))
            a.setPlayhead(*best);  // the engine is stopped: move the shown playhead
    };
    appActions.addUi("ui.marker.prev", "Marker: go to the previous one", [jump] { jump(false); });
    appActions.addUi("ui.marker.next", "Marker: go to the next one", [jump] { jump(true); });
    appActions.addUi("ui.item.select_all", "Item: select all", [&] {
        auto& g = shell.arrange()->gestures();
        std::vector<model::Id> all;
        for (const auto& e : (*app)->head().items) all.push_back(e.id);
        g.setSelection(all);
        uiState.selectedItems.clear();
        for (model::Id id : all) uiState.selectedItems.push_back(id.str());
        uiState.tracksFocused = false;
    });
    appActions.addUi("ui.item.takes", "Item: show all takes (toggle)", [&] {
        views::TrackLayout& layout = shell.arrange()->trackLayout();
        for (const std::string& text : uiState.selectedItems)
            if (const auto id = model::Id::parse(text))
                layout.setTakesExpanded(*id, !layout.takesExpanded(*id));
    });
    appActions.addUi("ui.keys.reset", "Edit: reset key bindings to the defaults\u2026",
                     [&] { gui::openKeymapReset(root, appActions); });
    appActions.addUi("ui.arrange.zoom_in", "View: zoom in (about the playhead)",
                     [&] { shell.arrange()->zoomStep(true); });
    appActions.addUi("ui.arrange.zoom_out", "View: zoom out (about the playhead)",
                     [&] { shell.arrange()->zoomStep(false); });
    appActions.addUi("ui.marker.add", "Marker: add at playhead",
                     [&] { shell.arrange()->addMarker(); });
    appActions.addUi("ui.region.add", "Region: add over loop range",
                     [&] { shell.arrange()->addRegion(); });
    // The audio engine: the saved device for interactive use (Edit > Audio
    // device), nothing offscreen; --audio picks a backend with its defaults.
    if (!o.audio.empty() && o.audio != "none") {
        json::Value args = json::Value::object();
        args["backend"] = o.audio;
        if (auto started = dispatcher.execute(
                {"engine.start", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
            !started)
            std::fprintf(stderr, "audio: %s (choose a device in Edit > Audio device)\n",
                         started.error().toString().c_str());
    } else if (o.audio.empty() && !o.offscreen && o.frames == 0) {
        if (auto started = audioDevice.start(true); !started)
            std::fprintf(stderr, "audio: %s (choose a device in Edit > Audio device)\n",
                         started.error().toString().c_str());
        // Launched from Finder or a desktop entry, stderr goes nowhere: a
        // refused device (or the fallback to the default) opens the dialog,
        // which says why and offers the others.
        if (!audioDevice.lastError().empty()) audioDevice.open();
    }

    // Per-project UI state (gui.md §1.2).
    gui::UiPersistence uiFile([&loop] { loop.post([] {}); });
    uiFile.add(
        "selection",
        [&] {
            json::Value v = json::Value::object(), tracks = json::Value::array(),
                        items = json::Value::array();
            for (const auto& t : uiState.selectedTracks) tracks.push_back(t);
            for (const auto& i : uiState.selectedItems) items.push_back(i);
            v["tracks"] = std::move(tracks);
            v["items"] = std::move(items);
            return v;
        },
        [&](const json::Value& v) {
            auto ids = [](const json::Value* a) {
                std::vector<std::string> out;
                if (a && a->isArray())
                    for (const auto& x : a->asArray())
                        if (x.isString()) out.push_back(x.asString());
                return out;
            };
            uiState.selectedTracks = ids(v.find("tracks"));
            uiState.selectedItems = ids(v.find("items"));
        });
    auto panelsSave = [&] {
        json::Value v = json::Value::object();
        v["mixer"] = mixer.visible();
        v["history"] = history.visible();
        return v;
    };
    auto panelsLoad = [&](const json::Value& v) {
        if (const json::Value* m = v.find("mixer"); m && m->isBool()) mixer.setVisible(m->asBool());
        if (const json::Value* h = v.find("history"); h && h->isBool())
            history.setVisible(h->asBool());
    };
    uiFile.add("panels", panelsSave, panelsLoad);
    uiFile.add(
        "transport", [&] { return shell.saveTransportLayout(); },
        [&](const json::Value& v) { shell.loadTransportLayout(v); });
    uiFile.add(
        "midi", [&] { return midiEditor.saveLayout(); },
        [&](const json::Value& v) { midiEditor.loadLayout(v); });
    uiFile.add(
        "mixer", [&] { return mixer.saveLayout(); },
        [&](const json::Value& v) { mixer.loadLayout(v); });
    uiFile.add(
        "plugin_windows", [&] { return pluginWindows.saveLayout(); },
        [&](const json::Value& v) { pluginWindows.loadLayout(v); });
    uiFile.add(
        "windows",
        [&] {
            auto v = json::Value::object();
            v["fx"] = fxPanel.saveLayout();
            v["routing"] = routingPanel.saveLayout();
            return v;
        },
        [&](const json::Value& v) {
            fxPanel.loadLayout(v.find("fx") ? *v.find("fx") : json::Value());
            routingPanel.loadLayout(v.find("routing") ? *v.find("routing") : json::Value());
        });
    uiFile.add(
        "window",
        [&] {
            json::Value v = json::Value::object();
            v["width"] = static_cast<int>((*window)->width());
            v["height"] = static_cast<int>((*window)->height());
            return v;
        },
        [&](const json::Value& v) {
            const json::Value* w = v.find("width");
            const json::Value* h = v.find("height");
            if (!o.offscreen && w && h && w->isNumber() && h->isNumber())
                (*window)->setSize(static_cast<int>(w->asInt()), static_cast<int>(h->asInt()));
        });
    // The arrange view's state (zoom, scroll, row heights, follow).
    auto arrangeSave = [&] { return shell.arrange()->saveLayout(); };
    auto arrangeLoad = [&](const json::Value& v) { shell.arrange()->loadLayout(v); };
    uiFile.add("arrange", arrangeSave, arrangeLoad);
    // Named screensets (gap log #38): panels + the arrange view, by name.
    gui::Screensets screensets(
        [&] {
            json::Value v = json::Value::object();
            v["panels"] = panelsSave();
            v["arrange"] = arrangeSave();
            return v;
        },
        [&](const json::Value& v) {
            if (const json::Value* p = v.find("panels")) panelsLoad(*p);
            if (const json::Value* a = v.find("arrange")) {
                // A layout's row heights replace the current ones entirely.
                shell.arrange()->trackLayout().clearTrackHeights();
                arrangeLoad(*a);
            }
            root.damageAll();
        });
    uiFile.add(
        "screensets", [&] { return screensets.toJson(); },
        [&](const json::Value& v) { screensets.fromJson(v); });
    auto screensetAction = [&](auto op) {
        return [&, op](const json::Value& v) -> Status {
            const std::string name = v.isString() ? v.asString() : std::string{};
            return (screensets.*op)(name);
        };
    };
    appActions.addUi("ui.screenset.save", "View: save screenset…", "name",
                     screensetAction(&gui::Screensets::save));
    appActions.addUi("ui.screenset.load", "View: recall screenset…", "name",
                     [&](const json::Value& v) -> Status {
                         return screensets.load(v.isString() ? v.asString() : std::string{});
                     });
    appActions.addUi("ui.screenset.delete", "View: delete screenset…", "name",
                     screensetAction(&gui::Screensets::remove));
    for (int slot = 1; slot <= 5; ++slot) {
        const std::string n = std::to_string(slot);
        appActions.addUi("ui.screenset.save:" + n, "View: save screenset " + n,
                         [&, n] { (void)screensets.save(n); });
        appActions.addUi("ui.screenset.load:" + n, "View: recall screenset " + n, [&, n] {
            if (auto r = screensets.load(n); !r && appActions.onError)
                appActions.onError(r.error().message);
        });
    }
    uiFile.tick((*app)->projectPath(), (*app)->projectGeneration());
    shell.installOverlay(root);  // the playhead paints over the whole window
    if (o.viewStart >= 0) {
        views::Viewport v;
        v.startSeconds = o.viewStart;
        v.pixelsPerSecond = o.viewPps;
        shell.arrange()->setViewport(v);
    }
    int drawn = 0, longTasks = 0;
    double coldStartMs = -1, longestTaskMs = 0;
    std::vector<double> frameCpu;
    // Counted runs (--frames) first wait, uncounted, until every visible
    // pyramid is ready; --script then moves the viewport every frame.
    bool warm = o.frames == 0;
    bool inputChanged = true;
    auto preserveUi = [&](const std::function<void()>& work) {
        (void)dispatcher.guarded(
            [&](const cmd::Invocation& invocation) -> Status {
                uiFile.arm();
                inputChanged = true;
                const auto name = invocation.command.substr(0, invocation.command.find('@'));
                if (name == "project.new" || name == "project.open") uiFile.prepareProjectSwitch();
                return {};
            },
            [&]() -> Result<json::Value> {
                work();
                return json::Value();
            });
    };
    const double warmUntil = gui::wallMs() + 60000;
    int step = 0;
    // The main-thread budget (docs/benchmarks/gui.md): each synchronous task
    // (the bus pump, the panels' sync pass, one input event) should finish in
    // 2 ms; frames are measured on their own.
    auto timed = [&](auto&& work) {
        const double t0 = gui::wallMs();
        work();
        if (const double ms = gui::wallMs() - t0; ms > 2.0) {
            ++longTasks;
            longestTaskMs = std::max(longestTaskMs, ms);
        }
    };
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> meterFrame;
    bool serviceFailed = false;
    auto service = [&] {
        bool event = std::exchange(inputChanged, false);
        int next = -1;
        timed([&] {
            event = pluginWindows.dispatchFds() || event;
            // A deadline may beat the FD worker to a newly readable relay.
            // Remember that notification before tick drains it, so a stopped
            // status/error change cannot disappear into an animation pass.
            WaitItem relay{(*app)->serviceWaitable(), kWaitRead};
            if (waitFor(std::span(&relay, 1), 0) > 0 && relay.ready) event = true;
            preserveUi([&] {
                next = loop.pump();
                (*app)->tick();
                publish();
            });
            event = std::exchange(inputChanged, false) || event;
            uiFile.tick((*app)->projectPath(), (*app)->projectGeneration());
        });
        timed([&] {
            shell.sync(event);
            midiEditor.sync();
            routing.sync();
            mixer.sync();
            history.sync();
            const auto now = Clock::now();
            const auto activity = (*app)->meterActivitySequence();
            // Observe before reading; arm/recheck after the visible meters rest.
            if (event || !meterFrame || now >= *meterFrame) {
                meterFeed.poll(now);
                if (meterFeed.moving())
                    meterFrame = now + std::chrono::milliseconds(33);
                else {
                    meterFrame.reset();
                    if (meterFeed.visible()) (*app)->armMeterActivity(activity);
                }
            }
            fxWindow.tick(event);
            renderDialog.tick();
            audioEdit.poll();
            if (preferences.isOpen()) preferences.refresh();
            if (audioDevice.isOpen()) audioDevice.refresh();
            if (shell.arrange()->poll(event)) root.requestAnimationFrame();
        });
        return next;
    };
    auto waitUntil = [&](int next) {
        const int plugin = pluginWindows.service();
        // A bridge-owned editor window moved: no input event told the UI file.
        if (pluginWindows.takeLayoutChanged()) uiFile.arm();
        const auto now = Clock::now();
        auto include = [&](std::optional<Clock::time_point> deadline) {
            if (!deadline) return;
            const int ms = rpc::detail::waitMilliseconds(*deadline - now);
            next = next < 0 ? ms : std::min(next, ms);
        };
        include((*app)->nextServiceDeadline());
        include(uiFile.nextDeadline());
        include(midiEditor.nextDeadline());
        include(renderDialog.nextDeadline());
        include(shell.nextDeadline());
        include(meterFrame);
        include(root.frameDeadline());  // a frame held back by the frame cap
        include(fxPanel.frameDeadline());
        include(routingPanel.frameDeadline());
        // The playhead has a frame deadline only during playback/recording.
        if (shell.arrange()->playing()) include(now + std::chrono::milliseconds(16));
        if (auto error = pluginWindows.fdError()) {
            std::fprintf(stderr, "%s\n", error->toString().c_str());
            quit = true;  // no replacement safety poll after FD service fails
            serviceFailed = true;
        }
        if (plugin >= 0) next = next < 0 ? plugin : std::min(next, plugin);
        return next;
    };
    while (!quit && !loop.stopRequested()) {
        int next = service();
        // --action: after the first tick has read the project's UI state.
        for (const auto& spec : std::exchange(o.actions, {})) {
            uiFile.arm();
            inputChanged = true;
            const auto space = spec.find(' ');
            const std::string id = spec.substr(0, space);
            const actions::Action* action = appActions.registry().find(id);
            Result<json::Value> inputs = json::Value::object();
            if (space != std::string::npos) inputs = json::parse(spec.substr(space + 1));
            Result<json::Value> r = json::Value();
            preserveUi([&] {
                r = !action   ? Result<json::Value>(Error("action.unknown", "no action " + id))
                    : !inputs ? Result<json::Value>(inputs.error())
                              : appActions.run(*action, *inputs);
            });
            if (!r) {
                std::fprintf(stderr, "action %s: %s\n", id.c_str(), r.error().toString().c_str());
                return 1;
            }
        }
        if (inputChanged) next = service();
        if (o.script && warm) {
            views::ArrangeView& arrange = *shell.arrange();
            views::Viewport v = arrange.viewport();
            const double k = step++;
            v.pixelsPerSecond = 40.0 * std::pow(2.0, 3.0 * std::sin(k / 40.0));
            v.startSeconds = 2.0 + 20.0 * (1 + std::sin(k / 70.0));
            v.scrollY =
                static_cast<float>((1 + std::sin(k / 25.0)) / 2) *
                std::max(0.0f, arrange.trackLayout().contentHeight() - arrange.lanesRect().h);
            arrange.setViewport(v);
        }
        fxPanel.frame();
        routingPanel.frame();
        if (o.frames > 0) root.requestAnimationFrame();  // benchmark runs draw every pass
        if (o.frames > 0 ? root.wantsFrame() : root.frameReady(Clock::now())) {
            const double cpu0 = gui::threadCpuMs();
            root.frame();
            const double cpu = gui::threadCpuMs() - cpu0;
            if (drawn == 0 && coldStartMs < 0) coldStartMs = gui::millisecondsSinceProcessStart();
            if (!warm) {
                warm = shell.arrange()->lanes().stats().pending == 0;
                if (!warm && gui::wallMs() > warmUntil) {
                    std::fprintf(stderr, "peaks did not become ready within 60 s\n");
                    return 1;
                }
            } else {
                frameCpu.push_back(cpu);
                if (o.frames > 0 && ++drawn >= o.frames) break;
                if (o.frames == 0) ++drawn;
            }
        }
        SDL_Event e;
        const int wait = waitUntil(next);
        if (quit || loop.stopRequested()) break;
        const bool got = o.frames > 0 ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, wait);
        if (got) inputChanged = pluginWindows.dispatchFds() || inputChanged;
        for (bool have = got; have; have = SDL_PollEvent(&e)) {
            if (e.type != (*platform)->wakeEvent()) {
                uiFile.arm();
                inputChanged = true;
            }
            bool panelEvent = false;
            timed([&] {
                preserveUi([&] {
                    fxWindow.syncProject();
                    panelEvent = fxPanel.handle(e) || routingPanel.handle(e);
                });
            });
            if (panelEvent) continue;
            if (pluginWindows.handle(e)) continue;  // an editor window's event
            // Bridged Cocoa editors follow the DAW window back to the front (ADR-0041).
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
                SDL_GetWindowFromEvent(&e) == (*window)->sdl())
                pluginWindows.appActivated();
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                             SDL_GetWindowFromEvent(&e) == (*window)->sdl())) {
                (void)appActions.run("ui.quit");
            } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                dropped.emplace_back(e.drop.data);
                dropAt = {e.drop.x / (*window)->inputScale(), e.drop.y / (*window)->inputScale()};
            } else if (e.type == SDL_EVENT_DROP_COMPLETE) {
                if (auto r = shell.arrange()->gestures().drop(dropped, dropAt); !r)
                    std::fprintf(stderr, "drop: %s\n", r.error().toString().c_str());
                dropped.clear();
            } else
                timed([&] {
                    preserveUi([&] { root.handle(e); });
                });  // the wake event is ignored: it only interrupts the wait
        }
    }
    double idleCpuPercent = -1;
    if (o.idleSeconds > 0) {  // no input, no model changes: the loop should sleep
        const double c0 = gui::processCpuMs(), w0 = gui::wallMs();
        while (!quit && !loop.stopRequested() && gui::wallMs() - w0 < o.idleSeconds * 1000) {
            const int next = service();
            fxPanel.frame();
            routingPanel.frame();
            if (root.frameReady(Clock::now())) root.frame();
            SDL_Event e;
            const double left = o.idleSeconds * 1000 - (gui::wallMs() - w0);
            const int deadline = waitUntil(next);
            const int wait =
                deadline < 0 ? static_cast<int>(left) : std::min(deadline, static_cast<int>(left));
            if (SDL_WaitEventTimeout(&e, std::max(1, wait)))
                while (SDL_PollEvent(&e)) {
                }
        }
        idleCpuPercent = 100.0 * (gui::processCpuMs() - c0) / (gui::wallMs() - w0);
    }
    pluginWindows.suspendFdWatches();
    if (server) server->stop();
    (void)uiFile.flush((*app)->projectPath());
    if (!o.snapshot.empty()) {  // the final state, for tests and bug reports
        shell.sync();
        midiEditor.sync();
        routing.sync();
        mixer.sync();
        history.sync();
        meterFeed.poll();
        root.damageAll();
        root.frame(false);
        for (const auto& [name, panel] : {std::pair{"fx", &fxPanel}, {"routing", &routingPanel}}) {
            const fs::path path =
                o.snapshot.parent_path() /
                (o.snapshot.stem().string() + "." + name + o.snapshot.extension().string());
            if (auto written = panel->snapshot(path); !written) {
                std::fprintf(stderr, "%s\n", written.error().toString().c_str());
                return 1;
            }
        }
        (*window)->forceCurrent();
        auto written = gui::writePng(o.snapshot, root.readBack());
        if (!written) {
            std::fprintf(stderr, "%s\n", written.error().toString().c_str());
            return 1;
        }
    }
    if (o.bench) {
        const auto& ls = shell.arrange()->lanes().stats();
        std::printf(
            "{\"schema\": \"supple.gui-bench/1.1\", \"driver\": \"%s\", \"cold_start_ms\": %.1f, "
            "\"frames\": %d, \"frame_cpu_ms\": {\"p50\": %.3f, \"p95\": %.3f, \"max\": %.3f}, "
            "\"ui_tasks_over_2ms\": %d, \"longest_ui_task_ms\": %.2f, \"idle_cpu_percent\": %.2f, "
            "\"tracks\": %d, \"last_frame\": {\"rows\": %zu, \"items\": %zu, \"columns\": %zu, "
            "\"notes\": %zu}, \"peaks\": {\"built\": %d, \"loaded\": %d}, \"gl\": \"%s\"}\n",
            SDL_GetCurrentVideoDriver(), coldStartMs, drawn, percentile(frameCpu, 0.5),
            percentile(frameCpu, 0.95),
            frameCpu.empty() ? 0.0 : *std::max_element(frameCpu.begin(), frameCpu.end()), longTasks,
            longestTaskMs, idleCpuPercent, o.generate, ls.rows, ls.items, ls.columns, ls.notes,
            peaks.stats().built, peaks.stats().loaded, (*window)->renderer().c_str());
    }
    return serviceFailed ? 1 : 0;
}
