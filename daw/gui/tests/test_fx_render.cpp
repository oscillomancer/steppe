// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The FX chain window, the plugin browser and the parameter panel, and the
// render dialog: driven by keys and clicks through the bus (gui.md §10).
#include <SDL3/SDL_keycode.h>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>
#include "generated_media.h"
#include "product_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include "test_support.h"

#include "app_actions.h"
#include "fx_window.h"
#include "generate.h"
#include "learn_dialog.h"
#include "plugin_browser.h"
#include "plugin_drag.h"
#include "render_dialog.h"
#include "side_browser.h"
#include "side_panel.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/host/clap_host.h"
#include "supple/json/write.h"
#include "supple/ui/forms.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

using namespace supple;

namespace {

const cmd::Origin kTestOrigin{cmd::Origin::Type::Test, "fx-render-test", {}};

struct Fixture {
    std::filesystem::path dir, configDir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState ui;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<gui::FloatingPanel> panel;
    std::unique_ptr<gui::FxWindow> fx;
    std::unique_ptr<gui::RenderDialog> render;
    bool ok = false;

    // `fixtures`: the host test plug-ins too (com.suppleaudio.test.*).
    explicit Fixture(bool fixtures = false) {
        dir = std::filesystem::path(__FILE__)
                  .parent_path()
                  .parent_path()
                  .parent_path()
                  .parent_path() /
              "build" / ("supple-fx-render-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        configDir = dir / "config";
        std::filesystem::create_directories(configDir);
        testConfigHome(configDir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"fx-render-test", 1100, 740, true});
#if !defined(__linux__)
        REQUIRE_MESSAGE(w, (w ? "" : w.error().message));
#endif
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
#if !defined(__linux__)
        REQUIRE_MESSAGE(f, (f ? "" : f.error().message));
#endif
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.presetDir = dir / "presets";
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
        if (fixtures) options.pluginPath.push_back(FIXTURE_CLAP_PATH);
        options.identity = host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"};
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        gui::registerGuiCommands(registry, ui);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        panel = std::make_unique<gui::FloatingPanel>(
            *platform, *window, *root, SUPPLE_GUI_FONTS_DIR, "FX", 840, 560, 800, 360);
        panel->onUnhandledKey = [this](const ui::KeyEvent& e, ui::Root& source) {
            const bool typing = source.focused() && source.focused()->role() == ui::Role::TextField;
            return actions->key(e.key, e.mods, typing, gui::keySections(source));
        };
        fx = std::make_unique<gui::FxWindow>(*app, *dispatcher, *root, *panel,
                                             gui::FxWindow::Hooks{},
                                             gui::Favourites::defaultFile(gui::test::slug()));
        render = std::make_unique<gui::RenderDialog>(*app, *dispatcher, *root);
        actions->addUi("ui.fx.chain", "FX: chain…", "track",
                       [this](const json::Value& v) -> Status { return fx->openFor(ui, v); });
        actions->addUi("ui.render", "File: render…", [this] { render->open(); });
        actions->addUi("ui.fx.move_up", "FX: move up", [this] { fx->moveSelected(-1); });
        actions->addUi("ui.fx.move_down", "FX: move down", [this] { fx->moveSelected(1); });
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            const bool typing = root->focused() && root->focused()->role() == ui::Role::TextField;
            return actions->key(e.key, e.mods, typing);
        };
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Row;
        frame->emplace<ui::Node>("body");
        root->setContent(std::move(frame));
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        // Destroy the panels before the root they point into.
        render.reset();
        fx.reset();
        panel.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }

    Result<json::Value> run(const char* command, json::Value args) {
        return dispatcher->execute({command, std::move(args), kTestOrigin});
    }
    json::Value args(std::initializer_list<std::pair<const char*, json::Value>> fields) {
        json::Value o = json::Value::object();
        for (const auto& [k, v] : fields) o[k] = v;
        return o;
    }
    model::Id addTrack(const char* name) {
        auto r = run("track.add", args({{"type", "channel"}, {"name", name}}));
        REQUIRE(r);
        auto id = model::Id::parse((*r)["track"].asString());
        REQUIRE(id);
        return *id;
    }
    // The first channel strip (generateSession makes tr_1, tr_2, …).
    model::Id firstChannel() {
        for (const auto& e : app->head().tracks)
            if (e.value->type == model::StripType::Channel) return e.id;
        FAIL("no channel track");
        return {};
    }
    std::size_t trackCount() { return app->head().tracks.size(); }
    // The media path of the first item on the track named `name`.
    std::string sourcePathOf(const std::string& name) {
        const model::Session& s = app->head();
        for (const auto& t : s.tracks)
            if (t.value->name == name)
                for (const auto& i : s.items)
                    if (i.value->track == t.id && i.value->activeTake) {
                        const model::Take* take = s.takes.find(*i.value->activeTake);
                        if (!take || !take->source) continue;
                        if (const model::MediaSource* media = s.media.find(*take->source))
                            return media->path;
                    }
        return {};
    }
    ui::Root& uiRoot() { return root->popupOpen() || !fx->isOpen() ? *root : panel->root(); }
    void frame() {
        root->damageAll();
        root->frame(false);
        if (panel->isOpen()) {
            panel->root().damageAll();
            panel->frame(false);
        }
    }
    // Clicks need a layout pass: a popup has no bounds until the next frame.
    bool click(const std::string& id) {
        frame();
        const bool hit = uiRoot().click(id);
        frame();
        return hit;
    }
    void type(std::string_view text) {
        uiRoot().text(text);
        frame();
    }
    // A press and release of `button` on the first row of a list.
    void pressFirstRow(const std::string& id, int button) {
        frame();
        auto* node = uiRoot().find(id);
        REQUIRE(node);
        const ui::Point at{node->bounds().x + 10, node->bounds().y + 8};
        uiRoot().pointer({ui::PointerEvent::Type::Down, at, button});
        uiRoot().pointer({ui::PointerEvent::Type::Up, at, button});
        frame();
    }
    void key(const char* chord) {
        const auto c = actions::parseChord(chord);
        REQUIRE(c);
        uiRoot().key(c->key, c->mods);
        frame();
    }
    std::size_t historyCount() {
        // The full linear undo depth (history.list caps at 20 entries).
        return app->versions().history(1000).size();
    }
    std::string fxStatus() {
        auto* label = dynamic_cast<ui::Label*>(uiRoot().find("fx.status"));
        return label ? label->text() : std::string();
    }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        frame();
        (void)gui::writePng(std::filesystem::path(out) / name, uiRoot().readBack());
    }
    void golden(const char* name) {
        const gui::Image image = uiRoot().readBack();
#if !defined(__linux__)
        REQUIRE(image.width > 0);
        REQUIRE(image.height > 0);
        REQUIRE(image.rgba.size() == static_cast<std::size_t>(image.width * image.height * 4));
        bool varied = false;
        for (std::size_t i = 4; i < image.rgba.size(); i += 4)
            if (image.rgba[i] != image.rgba[0] || image.rgba[i + 1] != image.rgba[1] ||
                image.rgba[i + 2] != image.rgba[2]) {
                varied = true;
                break;
            }
        CHECK(varied);
        return;
#endif

        const auto path = std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / name;
        if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(path)) {
            REQUIRE(gui::writePng(path, image));
            MESSAGE("golden written: " << path.string());
            return;
        }
        int w = 0, h = 0, n = 0;
        unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
        REQUIRE(data);
        REQUIRE(w == image.width);
        REQUIRE(h == image.height);
        std::size_t off = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(int(image.rgba[i + c]) - int(data[i + c])));
            off += d > 48;
        }
        stbi_image_free(data);
        CHECK(off < image.rgba.size() / 4 / 200);
    }
    // The stock plugins build with ALL: a partial build has none, and the FX
    // tests skip rather than fail.
    bool stockPlugins() {
        auto r = run("plugin.list", json::Value::object());
        return r && !(*r)["plugins"].asArray().empty();
    }
    // Finds a stock plugin id by display name (the scan ordering varies).
    std::string pluginId(const char* name) {
        auto r = run("plugin.list", json::Value::object());
        REQUIRE(r);
        for (const json::Value& p : (*r)["plugins"].asArray()) {
            const json::Value* n = p.find("name");
            if (n && n->isString() && n->asString() == name) return p.find("id")->asString();
        }
        FAIL("no plugin named " << name);
        return {};
    }
};

}  // namespace

TEST_CASE("FX and render hidden subpanels release focus; popup close releases it too") {
    Fixture f;
    if (!f.ok) return;
    const auto track = f.app->head().project->masters.front();
    REQUIRE(f.fx->open(track));
    REQUIRE(f.click("fx.chain.save"));
    auto* name = dynamic_cast<ui::TextField*>(f.uiRoot().find("fx.chain.name"));
    REQUIRE(name);
    REQUIRE(f.uiRoot().focused() == name);
    REQUIRE(f.fx->open(track));
    CHECK(f.uiRoot().focused() == nullptr);
    const auto before = name->text();
    f.uiRoot().text("hidden");
    CHECK(name->text() == before);
    f.uiRoot().focus(f.uiRoot().find("fx.close"));
    f.fx->close();
    CHECK(f.uiRoot().focused() == nullptr);
    f.render->open();
    f.render->setSource(gui::RenderDialog::Source::Regions);
    f.frame();
    // A field of the regions section (handles) has focus; switching the source
    // removes the section, and focus goes with it.
    auto* handles = dynamic_cast<ui::TextField*>(f.uiRoot().find("render.handles.text"));
    REQUIRE(handles);
    f.uiRoot().focus(handles);
    f.render->setSource(gui::RenderDialog::Source::Master);
    CHECK(f.uiRoot().focused() == nullptr);
    CHECK_FALSE(f.uiRoot().find("render.handles.text"));
    f.uiRoot().text("hidden");
    CHECK(f.render->settings().find("handles_s")->asDouble() == 0);
    f.uiRoot().focus(f.uiRoot().find("render.pattern"));
    f.render->close();
    CHECK(f.uiRoot().focused() == nullptr);
}

TEST_CASE("the render dialog says renders leave out the clips playing now") {
    Fixture f;
    if (!f.ok) return;
    const auto run = [&](const char* name, std::string_view args) {
        auto r = f.dispatcher->execute({name, json::parse(args).value(), kTestOrigin});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
    };
    run("track.add", R"({"name": "Bass"})");
    run("scene.add", R"({"name": "A"})");
    run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:q r:*"})");
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    struct StopEngine {
        engine::EngineApp& app;
        ~StopEngine() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                               {.anticipative = false}));
    f.render->open();
    f.frame();
    CHECK_FALSE(f.uiRoot().find("render.clips.note"));  // every track on the arrangement
    f.render->close();
    run("launch.clip", R"({"clip": "cl_1"})");
    backend.run(4);
    f.app->tick();
    f.render->open();
    f.frame();
    CHECK(f.uiRoot().find("render.clips.note"));
    f.snapshot("render-clips-note.png");
    f.render->close();
}

TEST_CASE("FX drags survive model refresh and separate drags have separate undo steps") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) return;
    const auto track = f.addTrack("Drag");
    auto added =
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Hickory")}}));
    REQUIRE(added);
    REQUIRE(f.fx->open(track));
    REQUIRE(f.fx->chain().size() == 1);
    const auto effect = f.fx->chain().front();
    auto drag = [&](const std::string& id, bool right) {
        f.frame();
        auto* slider = f.uiRoot().find(id);
        REQUIRE(slider);
        const auto b = slider->bounds();
        const float y = b.y + b.h / 2;
        const float from = right ? b.x + 12 : b.right() - 12;
        const float to = right ? b.right() - 12 : b.x + 12;
        const auto history = f.historyCount();
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, {from, y}, 1});
        for (int i = 1; i <= 5; ++i) {
            const float x = from + (to - from) * i / 5;
            f.uiRoot().pointer({ui::PointerEvent::Type::Move, {x, y}, 1});
            f.fx->tick();
            f.uiRoot().frame(false);
            CHECK(f.uiRoot().find(id) == slider);
        }
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, {to, y}, 1});
        f.fx->tick();
        f.frame();
        CHECK(f.historyCount() == history + 1);
    };
    drag("fx.wet." + effect.str(), false);
    CHECK(f.app->head().fx.find(effect)->wet < 10);
    drag("fx.wet." + effect.str(), true);
    CHECK(f.app->head().fx.find(effect)->wet > 90);
    f.fx->select(effect);
    f.fx->showParamsTab();
    drag("fx.param.gain", true);
    drag("fx.param.gain", false);
    REQUIRE(f.run("track.remove", f.args({{"track", track.str()}})));
    f.fx->tick();
    CHECK_FALSE(f.uiRoot().find("fx.param.gain"));
}

TEST_CASE("plugin browser refresh after close does not retain destroyed labels") {
    Fixture f;
    if (!f.ok) return;
    f.fx->browser().open([](const std::string&) {});
    f.frame();
    f.fx->browser().close();
    f.fx->browser().refresh();
    f.fx->browser().tick();
    CHECK_FALSE(f.fx->browser().isOpen());
}

TEST_CASE("render numeric fields reject non-finite values and overflowing integers") {
    Fixture f;
    if (!f.ok) return;
    f.render->open();
    f.render->setField("render.end", "4");
    for (const auto& [field, value] : {std::pair{"render.start", "nan"},
                                       {"render.tail", "inf"},
                                       {"render.sample_rate", "1e100"},
                                       {"render.channels", "nan"},
                                       {"render.bit_depth", "17"}}) {
        f.render->setField(field, value);
        f.render->addToQueue();
        CHECK(f.app->renderQueue().empty());
        CHECK(f.render->statusText().find(field) != std::string::npos);
        f.render->refreshPreview();
        CHECK(f.render->preview().find(field) != std::string::npos);
        f.render->run();
        CHECK(f.render->job().empty());
        f.render->setField(field, "");
    }
}

TEST_CASE("the FX chain: browser, bypass/offline, reorder, remove, parameters, favourites") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id track = f.addTrack("Guitar");
    const std::string hickory = f.pluginId("Hickory");
    const std::string yarrow = f.pluginId("Yarrow");

    REQUIRE(f.fx->open(track));
    REQUIRE(f.fx->isOpen());
    CHECK(f.fx->chain().empty());
    // Add a stock plugin from the browser.
    f.click("fx.add");
    REQUIRE(f.fx->browser().isOpen());
    auto* search = dynamic_cast<ui::TextField*>(f.uiRoot().find("browser.search"));
    REQUIRE(search);
    f.type("Hickory");
    f.key("Return");
    CHECK_FALSE(f.fx->browser().isOpen());
    REQUIRE(f.fx->chain().size() == 1);
    const model::Id fx1 = f.fx->chain()[0];
    CHECK(f.app->head().fx.find(fx1)->plugin.name == "Hickory");
    CHECK(f.app->head().fx.find(fx1)->plugin.id == hickory);

    // Add a second one and reorder it by dragging (fx.move).
    f.click("fx.add");
    f.type("Yarrow");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 2);
    const model::Id fx2 = f.fx->chain()[1];
    CHECK(f.app->head().fx.find(fx2)->plugin.id == yarrow);
    const auto secondSelected = f.uiRoot().readBack();
    f.fx->select(fx1);
    f.frame();
    CHECK(f.uiRoot().readBack().rgba != secondSelected.rgba);
    // Drag the second row above the first (one fx.move), then back below it.
    auto dragRow = [&](const model::Id& fx, const model::Id& over, bool above) {
        const ui::Rect from = f.uiRoot().find("fx.row." + fx.str())->bounds();
        const ui::Rect to = f.uiRoot().find("fx.row." + over.str())->bounds();
        const ui::Point start{from.x + 40, from.y + from.h / 2};
        const ui::Point end{to.x + 40, above ? to.y + 2 : to.bottom() - 2};
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, start, 1});
        for (int i = 1; i <= 8; ++i)
            f.uiRoot().pointer({ui::PointerEvent::Type::Move,
                                {start.x, start.y + (end.y - start.y) * static_cast<float>(i) / 8},
                                1});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, end, 1});
        f.frame();
    };
    CHECK_FALSE(f.uiRoot().find("fx.up." + fx2.str()));  // no arrow buttons: drag instead
    dragRow(fx2, fx1, true);
    REQUIRE(f.fx->chain().size() == 2);
    CHECK(f.fx->chain()[0] == fx2);
    CHECK(f.fx->chain()[1] == fx1);
    dragRow(fx2, fx1, false);
    CHECK(f.fx->chain()[0] == fx1);

    // Reorder by keyboard too: Alt+Down / Alt+Up (the keymap's fx section)
    // move the selected effect.
    f.fx->select(fx1);
    f.uiRoot().focus(f.uiRoot().find("fx.row." + fx1.str()));
    f.key("Alt+Down");
    CHECK(f.fx->chain()[0] == fx2);
    f.key("Alt+Up");
    CHECK(f.fx->chain()[0] == fx1);

    // Bypass and offline are distinct fields (fx.set).
    f.click("fx.bypass." + fx1.str());
    CHECK(f.app->head().fx.find(fx1)->bypass);
    CHECK_FALSE(f.app->head().fx.find(fx1)->offline);
    f.click("fx.offline." + fx1.str());
    CHECK(f.app->head().fx.find(fx1)->bypass);
    CHECK(f.app->head().fx.find(fx1)->offline);
    CHECK(f.app->head().fx.find(fx2)->bypass == false);

    // A wet/dry drag is one undo step too (the cap sits at 100 %: drag left).
    ui::Node* wet = f.uiRoot().find("fx.wet." + fx1.str());
    REQUIRE(wet);
    const std::size_t wetBefore = f.historyCount();
    const float wetY = wet->bounds().y + wet->bounds().h / 2;
    const float wetX = wet->bounds().x + wet->bounds().w - 12;
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, {wetX, wetY}, 1});
    for (int i = 1; i <= 20; ++i)
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, {wetX - i * 6.0f, wetY}, 1});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, {wetX - 140.0f, wetY}, 1});
    CHECK(f.historyCount() == wetBefore + 1);
    CHECK(f.app->head().fx.find(fx1)->wet < 100.0);

    // The semantic parameter panel: a value set with units.
    f.click("fx.params." + fx1.str());
    auto* gainText = dynamic_cast<ui::TextField*>(f.uiRoot().find("fx.param.text.gain"));
    REQUIRE(gainText);
    f.uiRoot().focus(gainText);
    f.key("Ctrl+A");  // the field holds the formatted value: replace it
    f.type("-6 dB");
    f.key("Return");
    auto gain = f.run("fx.param.get", f.args({{"fx", fx1.str()}, {"param", "gain"}}));
    REQUIRE(gain);
    CHECK((*gain)["value"].asDouble() == doctest::Approx(-6.0));

    // A 20-move slider drag is one undo step (Origin::gesture).
    ui::Node* slider = f.uiRoot().find("fx.param.gain");
    REQUIRE(slider);
    const std::size_t before = f.historyCount();
    const float y = slider->bounds().y + slider->bounds().h / 2;
    const float x0 = slider->bounds().x + 12;
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, {x0, y}, 1});
    for (int i = 1; i <= 20; ++i)
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, {x0 + i * 8.0f, y}, 1});
    f.uiRoot().pointer(
        {ui::PointerEvent::Type::Up, {slider->bounds().x + slider->bounds().w - 12, y}, 1});
    CHECK(f.historyCount() == before + 1);
    auto moved = f.run("fx.param.get", f.args({{"fx", fx1.str()}, {"param", "gain"}}));
    REQUIRE(moved);
    CHECK((*moved)["value"].asDouble() > -6.0);

    // An out-of-range value is rejected by the plugin and the reason shown.
    f.uiRoot().focus(gainText);
    f.key("Ctrl+A");
    f.type("9999 dB");
    f.key("Return");
    CHECK_FALSE(f.fxStatus().empty());

    // Favourites persist to the user config.
    f.click("fx.add");
    f.type("Hickory");
    f.click("browser.fav");
    const auto favFile = gui::Favourites::defaultFile(gui::test::slug());
    CHECK(std::filesystem::exists(favFile));
    auto loaded = gui::Favourites::load(favFile);
    REQUIRE(loaded);
    CHECK(loaded->has(hickory));
    CHECK(f.fx->browser().favourites().has(hickory));
    f.key("Escape");  // close the browser
    CHECK_FALSE(f.fx->browser().isOpen());

    // The input chain: add one from here (fx.add {chain:"input"}), and its
    // rows use the same controls (ifx. ids).
    f.click("fx.tab.chain");
    f.click("fx.add.input");
    REQUIRE(f.fx->browser().isOpen());
    f.type("Hickory");
    f.key("Return");
    CHECK(f.fx->inputChain().size() == 1);
    CHECK(f.fx->chain().size() == 2);  // the main chain is untouched
    const model::Id inputFx = f.fx->inputChain()[0];
    CHECK(f.app->head().fx.find(inputFx)->plugin.name == "Hickory");
    CHECK(f.uiRoot().find("ifx.row." + inputFx.str()) != nullptr);
    f.click("ifx.remove." + inputFx.str());
    CHECK(f.fx->inputChain().empty());
    f.fx->close();
    gui::UiState selection;
    selection.selectedTracks = {track.str()};
    // The strips' "+": the browser alone (no chain window), with its own Close.
    REQUIRE(f.fx->openToAdd(selection, json::Value::object()));
    f.frame();
    CHECK_FALSE(f.fx->isOpen());
    REQUIRE(f.fx->browser().isOpen());
    CHECK(f.uiRoot().find("browser.title")->valueText() == "Add plugin to Guitar");
    f.click("browser.close");
    CHECK_FALSE(f.fx->browser().isOpen());
    REQUIRE(f.fx->openToAdd(selection, json::Value::object()));
    f.frame();
    f.type("Hickory");
    f.key("Return");
    CHECK_FALSE(f.fx->browser().isOpen());
    CHECK(f.fx->inputChain().empty());
    REQUIRE(f.fx->chain().size() == 3);
    REQUIRE(f.fx->open(track));
    f.frame();
    f.click("fx.remove." + f.fx->chain().back().str());

    // Remove (from the chain tab: the rows live there).
    f.click("fx.tab.chain");
    f.click("fx.remove." + fx1.str());
    REQUIRE(f.fx->chain().size() == 1);
    CHECK(f.fx->chain()[0] == fx2);
}

TEST_CASE(
    "the FX chain: Ctrl and Shift+click select several for bypass, offline and remove; a double "
    "click opens one editor") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    std::vector<model::Id> opened;
    f.fx = std::make_unique<gui::FxWindow>(*f.app, *f.dispatcher, *f.root, *f.panel,
                                           gui::FxWindow::Hooks{[&](model::Id id) -> Status {
                                               opened.push_back(id);
                                               return {};
                                           }},
                                           gui::Favourites::defaultFile(gui::test::slug()));
    const model::Id track = f.addTrack("Keys");
    std::vector<model::Id> fx;
    for (const char* name : {"Hickory", "Yarrow", "Hickory"}) {
        auto made = f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId(name)}}));
        REQUIRE(made);
        fx.push_back(*model::Id::parse((*made)["fx"].asString()));
    }
    REQUIRE(f.fx->open(track));
    f.frame();
    auto press = [&](const model::Id& id, unsigned mods, int clicks = 1) {
        f.frame();
        const ui::Rect r = f.uiRoot().find("fx.row." + id.str())->bounds();
        const ui::Point at{r.x + 6, r.y + 8};
        ui::PointerEvent down{ui::PointerEvent::Type::Down, at, 1, clicks, mods};
        ui::PointerEvent up{ui::PointerEvent::Type::Up, at, 1, clicks, mods};
        f.uiRoot().pointer(down);
        f.uiRoot().pointer(up);
        f.frame();
    };
    auto inst = [&](const model::Id& id) { return f.app->head().fx.find(id); };
    press(fx[0], 0);
    CHECK(f.fx->selection() == std::vector<model::Id>{fx[0]});
    press(fx[2], ui::kCtrl);
    CHECK(f.fx->selection() == std::vector<model::Id>{fx[0], fx[2]});
    // Bypass on a selected row bypasses both, in one undo step.
    std::size_t before = f.historyCount();
    REQUIRE(f.click("fx.bypass." + fx[2].str()));
    CHECK(inst(fx[0])->bypass);
    CHECK_FALSE(inst(fx[1])->bypass);
    CHECK(inst(fx[2])->bypass);
    CHECK(f.historyCount() == before + 1);
    // On a row outside the selection it acts on that row alone.
    REQUIRE(f.click("fx.bypass." + fx[1].str()));
    CHECK(inst(fx[1])->bypass);
    CHECK(inst(fx[0])->bypass);
    // Shift extends from the last row clicked (the third) to the first.
    press(fx[0], ui::kShift);
    CHECK(f.fx->selection() == fx);
    REQUIRE(f.click("fx.offline." + fx[1].str()));
    for (const model::Id& id : fx) CHECK(inst(id)->offline);
    // Ctrl takes one out again; Remove goes for the other two.
    press(fx[1], ui::kCtrl);
    CHECK(f.fx->selection() == std::vector<model::Id>{fx[0], fx[2]});
    before = f.historyCount();
    REQUIRE(f.click("fx.remove." + fx[0].str()));
    CHECK(f.fx->chain() == std::vector<model::Id>{fx[1]});
    CHECK(f.historyCount() == before + 1);
    CHECK(f.fx->selection() == std::vector<model::Id>{fx[1]});
    CHECK(opened.empty());
    // A double click opens that effect's editor, and only that one.
    auto more = f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Yarrow")}}));
    REQUIRE(more);
    const model::Id fx4 = *model::Id::parse((*more)["fx"].asString());
    f.fx->refresh();
    press(fx[1], 0);
    press(fx4, ui::kCtrl);
    REQUIRE(f.fx->selection().size() == 2);
    press(fx4, 0, 2);
    CHECK(opened == std::vector<model::Id>{fx4});
    CHECK(f.fx->selection() == std::vector<model::Id>{fx4});
}

TEST_CASE("FX selection drops toggled and moved effects") {
    Fixture f;
    if (!f.ok) return;
    const auto track = f.addTrack("Source"), other = f.addTrack("Destination");
    auto transaction = f.app->beginTransaction(kTestOrigin, "Offline FX fixture");
    REQUIRE(transaction);
    model::FxInstance effect;
    effect.plugin.id = "com.suppleaudio.test.unavailable";
    effect.offline = true;
    const auto first = f.app->txn().create(effect), second = f.app->txn().create(effect);
    f.app->txn().modify<model::Track>(track, [&](auto& t) { t.chain = {first, second}; });
    REQUIRE(f.app->commitTransaction(*transaction));
    REQUIRE(f.fx->open(track));

    f.fx->press(second, 0);
    f.fx->press(first, ui::kCtrl);
    REQUIRE(f.fx->selection() == std::vector<model::Id>{first, second});
    f.fx->press(first, ui::kCtrl);
    CHECK(f.fx->selection() == std::vector<model::Id>{second});
    CHECK(f.fx->selected() == second);

    f.fx->press(first, ui::kCtrl);
    REQUIRE(f.run("fx.move", f.args({{"fx", first.str()}, {"track", other.str()}, {"index", 0}})));
    f.fx->tick();
    CHECK(f.fx->selected() == second);
    CHECK(f.fx->selection() == std::vector<model::Id>{second});
    f.fx->press(second, ui::kCtrl);
    CHECK(f.fx->selection().empty());
    CHECK(f.fx->selected().n == 0);

    // A queued row event can precede the next refresh after agent deletion.
    REQUIRE(f.run("fx.move", f.args({{"fx", first.str()}, {"track", track.str()}, {"index", 0}})));
    f.fx->refresh();
    f.fx->press(second, 0);
    f.fx->press(first, ui::kCtrl);
    REQUIRE(f.run("fx.remove", f.args({{"fx", first.str()}})));
    REQUIRE(f.run("fx.remove", f.args({{"fx", second.str()}})));
    f.fx->press(first, ui::kCtrl);
    CHECK(f.fx->selection().empty());
    CHECK(f.fx->selected().n == 0);
}

TEST_CASE("the FX window's MIDI button learns a controller for a parameter (fx.learn.set)") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    f.fx = std::make_unique<gui::FxWindow>(*f.app, *f.dispatcher, *f.root, *f.panel,
                                           gui::FxWindow::Hooks{{},
                                                                {},
                                                                [&](gui::LearnRequest request) {
                                                                    gui::openLearnDialog(
                                                                        f.panel->root(), *f.actions,
                                                                        *f.app, std::move(request));
                                                                }},
                                           gui::Favourites::defaultFile(gui::test::slug()));
    const model::Id track = f.addTrack("Keys");
    auto made =
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Hickory")}}));
    REQUIRE(made);
    const model::Id fx = *model::Id::parse((*made)["fx"].asString());
    // A writable parameter of the plug-in.
    auto described = f.run("fx.describe", f.args({{"fx", fx.str()}, {"full", true}}));
    REQUIRE(described);
    std::string key;
    for (const auto& m : (*described)["params"].asObject()) {
        const json::Value* flags = m.value.find("flags");
        const std::string text = flags ? json::writeCompact(*flags) : "";
        if (text.find("readonly") == std::string::npos &&
            text.find("hidden") == std::string::npos) {
            key = m.key;
            break;
        }
    }
    REQUIRE_FALSE(key.empty());
    REQUIRE(f.fx->open(track));
    f.fx->select(fx);
    f.fx->showParamsTab();
    f.frame();
    REQUIRE(f.uiRoot().find("fx.param.learn." + key));
    CHECK(f.uiRoot().find("fx.param.learn." + key)->valueText() == "MIDI");
    REQUIRE(f.click("fx.param.learn." + key));
    REQUIRE(f.uiRoot().find("learn"));
    CHECK(f.uiRoot().find("learn.soft"));  // a parameter's options
    CHECK(f.app->learning());
    engine::EngineApp::ControlEvent e;
    e.midi.bytes[0] = 0xb1;
    e.midi.bytes[1] = 21;
    e.midi.bytes[2] = 64;
    e.device = "nanoKONTROL2";
    f.actions->control({e});
    f.frame();
    CHECK(f.uiRoot().find("learn.control.value")->valueText() == "CC 21 · ch 2 · nanoKONTROL2");
    f.snapshot("fx-midi-learn.png");
    REQUIRE(f.click("learn.mode.set"));
    REQUIRE(f.uiRoot().find("learn.mode.menu"));
    f.uiRoot().closePopup();
    REQUIRE(f.click("learn.soft.set"));
    CHECK(f.uiRoot().find("learn.soft.set")->valueText() == "On");
    REQUIRE(f.click("learn.ok"));
    CHECK_FALSE(f.app->learning());
    const model::FxInstance& bound = *f.app->head().fx.find(fx);
    REQUIRE(bound.learn.size() == 1);
    CHECK(bound.learn[0].param == key);
    CHECK(bound.learn[0].control == model::MidiControl{model::ControlKind::Cc, 2, 21, ""});
    CHECK(bound.learn[0].softTakeover);
    f.fx->tick();
    f.frame();
    CHECK(f.uiRoot().find("fx.param.learn." + key)->valueText() == "CC 21");
    f.snapshot("fx-midi-learn-params.png");
    // Remove from the same button.
    REQUIRE(f.click("fx.param.learn." + key));
    REQUIRE(f.click("learn.remove"));
    CHECK(f.app->head().fx.find(fx)->learn.empty());
}

TEST_CASE(
    "the FX window's parameter menu: MIDI learn, Modulate (greyed unless modulatable), lane") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id track = f.addTrack("Keys");
    // A stock plug-in that marks a parameter modulatable (fx.describe), and
    // one writable parameter of it that is not.
    model::Id fx;
    std::string modulatable, fixed, modulatableName;
    auto listed = f.run("plugin.list", json::Value::object());
    REQUIRE(listed);
    for (const char* name : {"Marram", "Coppice"}) {
        std::string plugin;  // a partial build may not have it
        for (const json::Value& p : (*listed)["plugins"].asArray())
            if (const json::Value* n = p.find("name"); n && n->isString() && n->asString() == name)
                plugin = p.find("id")->asString();
        if (plugin.empty()) continue;
        auto made = f.run("fx.add", f.args({{"track", track.str()}, {"plugin", plugin}}));
        REQUIRE(made);
        const model::Id id = *model::Id::parse((*made)["fx"].asString());
        auto described = f.run("fx.describe", f.args({{"fx", id.str()}, {"full", true}}));
        REQUIRE(described);
        modulatable.clear();
        fixed.clear();
        for (const auto& m : (*described)["params"].asObject()) {
            const json::Value* flags = m.value.find("flags");
            const std::string text = flags ? json::writeCompact(*flags) : "";
            if (text.find("readonly") != std::string::npos ||
                text.find("hidden") != std::string::npos)
                continue;
            const json::Value* mod = m.value.find("modulatable");
            const json::Value* viaEnvelope = m.value.find("modulatable_by_envelope");
            if (viaEnvelope && viaEnvelope->isBool() && viaEnvelope->asBool()) continue;
            if (mod && mod->isBool() && mod->asBool()) {
                if (modulatable.empty()) {
                    modulatable = m.key;
                    const json::Value* n = m.value.find("name");
                    modulatableName = n && n->isString() ? n->asString() : m.key;
                }
            } else if (fixed.empty()) {
                fixed = m.key;
            }
        }
        if (!modulatable.empty() && !fixed.empty()) {
            fx = id;
            break;
        }
    }
    if (fx == model::Id{}) {
        MESSAGE("skipped: neither Marram nor Coppice is built (a partial build)");
        return;
    }
    REQUIRE(!modulatable.empty());
    REQUIRE(f.fx->open(track));
    f.fx->select(fx);
    f.fx->showParamsTab();
    f.frame();
    const auto menuFor = [&](const std::string& key) -> ui::Node* {
        ui::Node* name = f.uiRoot().find("fx.param.name." + key);
        REQUIRE(name);
        // A long list: scroll the row into view first.
        for (ui::Node* n = name->parent(); n; n = n->parent())
            if (auto* view = dynamic_cast<ui::ScrollView*>(n)) {
                view->scrollTo(view->scroll() + name->bounds().y - view->bounds().y - 20);
                f.frame();
                break;
            }
        const ui::Point p{name->bounds().x + 4, name->bounds().y + name->bounds().h / 2};
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 3});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 3});
        f.frame();
        return f.uiRoot().find("fx.param.menu");
    };
    const auto row = [](ui::Node* menu, const std::string& prefix) {
        const auto items = ui::menuItems(*menu);
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const auto& i) { return i.label.rfind(prefix, 0) == 0; });
        REQUIRE_MESSAGE(it != items.end(), prefix);
        return *it;
    };
    ui::Node* menu = menuFor(fixed);
    REQUIRE(menu);
    CHECK_FALSE(row(menu, "Modulate: ").enabled);
    CHECK(row(menu, "Show lane: ").enabled);
    f.uiRoot().closePopup();
    menu = menuFor(modulatable);
    REQUIRE(menu);
    auto modulate = row(menu, "Modulate: ");
    CHECK(modulate.label == "Modulate: " + modulatableName + "…");
    CHECK(modulate.enabled);
    f.uiRoot().closePopup();
    modulate.run();
    ui::Node* sources = f.uiRoot().find("param.modulate.menu");
    REQUIRE(sources);
    auto lfo = row(sources, "LFO");
    f.uiRoot().closePopup();
    const std::size_t before = f.historyCount();
    lfo.run();
    CHECK(f.historyCount() == before + 1);
    const model::Session s = f.app->head();
    REQUIRE(s.modulations.size() == 1);
    CHECK(s.modulations.begin()->value->target == model::EnvTarget{fx, modulatable});
    // The route holds the lane: the menu offers to hide it, not to add one.
    f.fx->tick();
    f.frame();
    menu = menuFor(modulatable);
    REQUIRE(menu);
    CHECK(row(menu, "Hide lane: ").enabled);
    f.uiRoot().closePopup();
    // Its slider shows the route on its travel (an LFO at a quarter of the
    // range: ±0.25), and a drag on the bar under the track sets the depth in
    // one undo step while the panel holds still.
    auto* slider = dynamic_cast<ui::Slider*>(f.uiRoot().find("fx.param." + modulatable));
    REQUIRE(slider);
    REQUIRE(slider->modulation());
    CHECK(slider->modulation()->low == doctest::Approx(-0.25));
    CHECK(slider->modulation()->high == doctest::Approx(0.25));
    const model::Id route = s.modulations.begin()->id;
    const ui::Rect b = slider->bounds();
    const bool left = slider->value() > 0.5;  // away from the cap
    const ui::Point p{left ? b.x + ui::Slider::kInset + 6 : b.right() - ui::Slider::kInset - 6,
                      b.y + b.h / 2 + 4.5f};
    const std::size_t steps = f.historyCount();
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 1});
    for (int i = 1; i <= 10; ++i) {
        f.uiRoot().pointer(
            {ui::PointerEvent::Type::Move, {p.x + (left ? 1 : -1) * i * 4.0f, p.y}, 0});
        f.fx->tick();
        f.frame();
    }
    CHECK(f.uiRoot().find("fx.param." + modulatable) == slider);  // not rebuilt mid-drag
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, {p.x + (left ? 40.0f : -40.0f), p.y}, 1});
    f.fx->tick();
    f.frame();
    CHECK(f.historyCount() == steps + 1);
    CHECK(f.app->head().modulations.find(route)->depth != doctest::Approx(0.25));
}

TEST_CASE("a plain CLAP plug-in's modulatable parameter is editable in the FX window") {
    // ADR-0096 M3: a plug-in without semantic keys lists its CLAP parameters;
    // a modulatable one gets a slider over its declared range (fx.param.set by
    // id, a drag one undo step) and the menu's Modulate.
    Fixture f(true);
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    const model::Id track = f.addTrack("Raw");
    auto made = f.run(
        "fx.add", f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.raw-gain"}}));
    REQUIRE(made);
    const model::Id fx = *model::Id::parse((*made)["fx"].asString());
    auto described = f.run("fx.describe", f.args({{"fx", fx.str()}, {"full", true}}));
    REQUIRE(described);
    REQUIRE((*described)["params"].isArray());
    std::string key;
    for (const json::Value& p : (*described)["params"].asArray())
        if (const json::Value* m = p.find("modulatable"); m && m->isBool() && m->asBool())
            key = std::to_string(static_cast<std::uint32_t>(p.find("id")->asDouble()));
    REQUIRE_MESSAGE(!key.empty(), "the raw gain marks its gain modulatable");
    REQUIRE(f.fx->open(track));
    f.fx->select(fx);
    f.fx->showParamsTab();
    f.frame();
    auto* slider = dynamic_cast<ui::Slider*>(f.uiRoot().find("fx.param." + key));
    REQUIRE(slider);
    // A drag sets the value by id, one undo step.
    const std::size_t before = f.historyCount();
    const ui::Rect b = slider->bounds();
    const float y = b.y + b.h / 2 - 3;  // the track, above the modulation bar's place
    const float x0 = b.x + ui::Slider::kInset +
                     static_cast<float>(slider->value()) * (b.w - 2 * ui::Slider::kInset);
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, {x0, y}, 1});
    for (int i = 1; i <= 10; ++i)
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, {x0 - i * 5.0f, y}, 0});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, {x0 - 50.0f, y}, 1});
    f.fx->tick();
    f.frame();
    CHECK(f.historyCount() == before + 1);
    const auto& params = f.app->head().fx.find(fx)->params;
    const auto saved = params.find(static_cast<std::uint32_t>(std::stoul(key)));
    REQUIRE(saved != params.end());
    // Its menu has Modulate (no lane: a plain parameter has none), which
    // routes the parameter by its CLAP id.
    ui::Node* name = f.uiRoot().find("fx.plain.name.0");
    REQUIRE(name);
    const ui::Point p{name->bounds().x + 4, name->bounds().y + name->bounds().h / 2};
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 3});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 3});
    f.frame();
    ui::Node* menu = f.uiRoot().find("fx.param.menu");
    REQUIRE(menu);
    std::vector<std::string> labels;
    std::function<void()> modulate;
    for (const auto& item : ui::menuItems(*menu)) {
        labels.push_back(item.label);
        if (item.label.rfind("Modulate: ", 0) == 0) {
            CHECK(item.enabled);
            modulate = item.run;
        }
    }
    CHECK(std::none_of(labels.begin(), labels.end(),
                       [](const std::string& l) { return l.find("lane") != std::string::npos; }));
    REQUIRE(modulate);
    f.uiRoot().closePopup();
    modulate();
    ui::Node* sources = f.uiRoot().find("param.modulate.menu");
    REQUIRE(sources);
    std::function<void()> lfo;
    for (const auto& item : ui::menuItems(*sources))
        if (item.label == "LFO") lfo = item.run;
    REQUIRE(lfo);
    f.uiRoot().closePopup();
    lfo();
    REQUIRE(f.app->head().modulations.size() == 1);
    CHECK(f.app->head().modulations.begin()->value->target == model::EnvTarget{fx, key});
    f.fx->tick();
    f.frame();
    slider = dynamic_cast<ui::Slider*>(f.uiRoot().find("fx.param." + key));
    REQUIRE(slider);
    REQUIRE(slider->modulation());
    CHECK(slider->modulation()->high == doctest::Approx(0.25));
}

TEST_CASE("a parameter reached through its envelope takes Modulate; an idle route says why") {
    // ADR-0096 M4: a plug-in parameter that is automatable but not
    // modulatable (the fixed gain's, as any VST3 one) is marked
    // modulatable_by_envelope. Its row is editable and its Modulate enabled;
    // modulate adds the one-point envelope that carries the route, and with
    // that envelope off the route is idle and the menu names the reason.
    Fixture f(true);
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    const model::Id track = f.addTrack("Fixed");
    auto made = f.run(
        "fx.add", f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.fixed-gain"}}));
    REQUIRE(made);
    const model::Id fx = *model::Id::parse((*made)["fx"].asString());
    auto described = f.run("fx.describe", f.args({{"fx", fx.str()}, {"full", true}}));
    REQUIRE(described);
    REQUIRE((*described)["params"].isArray());
    std::string key;
    for (const json::Value& p : (*described)["params"].asArray())
        if (const json::Value* m = p.find("modulatable_by_envelope");
            m && m->isBool() && m->asBool())
            key = std::to_string(static_cast<std::uint32_t>(p.find("id")->asDouble()));
    REQUIRE_MESSAGE(!key.empty(), "the fixed gain's gain is reached through its envelope");
    REQUIRE(f.fx->open(track));
    f.fx->select(fx);
    f.fx->showParamsTab();
    f.frame();
    REQUIRE(dynamic_cast<ui::Slider*>(f.uiRoot().find("fx.param." + key)));
    auto openMenu = [&]() -> ui::Node* {
        ui::Node* name = f.uiRoot().find("fx.plain.name.0");
        REQUIRE(name);
        const ui::Point p{name->bounds().x + 4, name->bounds().y + name->bounds().h / 2};
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 3});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 3});
        f.frame();
        return f.uiRoot().find("fx.param.menu");
    };
    auto labels = [](ui::Node* menu) {
        std::vector<std::string> out;
        for (const auto& item : ui::menuItems(*menu)) out.push_back(item.label);
        return out;
    };
    ui::Node* menu = openMenu();
    REQUIRE(menu);
    std::function<void()> modulate;
    for (const auto& item : ui::menuItems(*menu))
        if (item.label.rfind("Modulate: ", 0) == 0) {
            CHECK(item.enabled);
            modulate = item.run;
        }
    REQUIRE(modulate);
    f.uiRoot().closePopup();
    modulate();
    ui::Node* sources = f.uiRoot().find("param.modulate.menu");
    REQUIRE(sources);
    std::function<void()> lfo;
    for (const auto& item : ui::menuItems(*sources))
        if (item.label == "LFO") lfo = item.run;
    REQUIRE(lfo);
    f.uiRoot().closePopup();
    lfo();
    const model::Session s = f.app->head();
    REQUIRE(s.modulations.size() == 1);
    CHECK(s.modulations.begin()->value->target == model::EnvTarget{fx, key});
    std::optional<model::Id> env;
    for (const auto& e : s.envelopes)
        if (e.value->target == model::EnvTarget{fx, key}) env = e.id;
    REQUIRE_MESSAGE(env, "modulate added the envelope that carries the route");
    f.fx->tick();
    f.frame();
    // It plays: no idle row.
    menu = openMenu();
    REQUIRE(menu);
    for (const std::string& l : labels(menu)) CHECK(l.find("idle") == std::string::npos);
    f.uiRoot().closePopup();
    // With its envelope off the route is idle, and the menu says why.
    REQUIRE(f.run("env.set", f.args({{"env", env->str()}, {"active", false}})));
    f.fx->tick();
    f.frame();
    menu = openMenu();
    REQUIRE(menu);
    bool said = false;
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Modulation idle: the envelope that carries it is off") {
            CHECK_FALSE(item.enabled);
            said = true;
        }
    std::string rows;
    for (const std::string& l : labels(menu)) rows += l + " | ";
    CHECK_MESSAGE(said, "menu rows: " << rows);
    f.uiRoot().closePopup();
}

TEST_CASE("a knob bound to the last touched parameter follows the panel's last edit") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id track = f.addTrack("Keys");
    auto made =
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Hickory")}}));
    REQUIRE(made);
    const model::Id fx = *model::Id::parse((*made)["fx"].asString());
    for (const auto& [number, mode] : {std::pair{16, model::ControlMode::Relative2},
                                       std::pair{17, model::ControlMode::Absolute}}) {
        gui::ActionControl binding;
        binding.action = "control.fx.last";
        binding.control = {model::ControlKind::Cc, 1, number, ""};
        binding.mode = mode;
        f.actions->controls().bind(binding);
    }
    auto cc = [&](std::uint8_t number, std::uint8_t value) {
        engine::EngineApp::ControlEvent e;
        e.midi.bytes[0] = 0xb0;
        e.midi.bytes[1] = number;
        e.midi.bytes[2] = value;
        f.actions->control({e});
    };
    // Nothing touched yet: the action says so.
    std::string error;
    f.actions->onError = [&](const std::string& message) { error = message; };
    cc(16, 65);
    CHECK(error.find("no plug-in parameter was touched") != std::string::npos);
    // An edit in the parameter panel (a typed value) makes it the target.
    REQUIRE(f.fx->open(track));
    f.fx->select(fx);
    f.fx->showParamsTab();
    f.frame();
    auto* field = dynamic_cast<ui::TextField*>(f.uiRoot().find("fx.param.text.gain"));
    REQUIRE(field);
    f.uiRoot().focus(field);
    field->setText("-6");
    REQUIRE(field->onSubmit);
    field->onSubmit("-6");
    f.fx->tick();
    f.frame();
    CHECK(f.uiRoot().find("fx.touched")->valueText() == "Last touched: Hickory · Gain");
    f.snapshot("fx-last-touched.png");
    auto gain = [&] {
        const model::FxInstance& inst = *f.app->head().fx.find(fx);
        return inst.patch->find("params")->find("gain")->asDouble();
    };
    CHECK(gain() == doctest::Approx(-6.0));
    // An encoder nudges it up.
    cc(16, 65);
    const double nudged = gain();
    CHECK(nudged > -6.0);
    // A knob far from the value waits (soft takeover), then follows once it
    // passes the value.
    cc(17, 127);
    CHECK(gain() == doctest::Approx(nudged));
    cc(17, 0);  // crossed it on the way down
    CHECK(gain() < nudged);
    cc(17, 127);  // and now it holds it: straight back up
    CHECK(gain() > nudged);
}

TEST_CASE("golden image: the FX chain") {
    Fixture f;
    if (!f.ok) return;
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 2));
    const model::Id track = f.firstChannel();

    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Hickory");
    f.key("Return");
    f.click("fx.add");
    f.type("Yarrow");
    f.key("Return");
    f.click("fx.offline." + f.fx->chain()[0].str());
    f.frame();
    f.golden("fx-chain-dark.png");
}

TEST_CASE("the plugin browser names the plugin's file and marks a shadowed copy") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    REQUIRE(f.fx->browser().isOpen());
    f.type("Hickory");
    // The detail line names the file the plugin would be loaded from.
    CHECK(f.fx->browser().detail().find("Hickory.clap") != std::string::npos);
    CHECK(f.fx->browser().detail().find("shadowed") == std::string::npos);
    CHECK(f.fx->browser().rowText(0).vendor == "stock");
    // A stock plugin's name does not say what it is; the row does.
    CHECK(f.fx->browser().rowText(0).name == "Hickory (utility)");
    f.key("Escape");

    // A copy of the same plugin earlier on the search path shadows the built
    // one; the browser says which copy wins and which is ignored.
    const std::filesystem::path dup = f.dir / "shadow";
    std::filesystem::create_directories(dup);
    std::filesystem::copy(std::filesystem::path(SUPPLE_CLAP_DIR) / "Hickory.clap",
                          dup / "Hickory.clap",
                          std::filesystem::copy_options::overwrite_existing |
                              std::filesystem::copy_options::recursive);
    engine::AppOptions options;
    options.workdir = f.dir / "shadow-work";
    options.pluginPath = {dup, std::filesystem::path(SUPPLE_CLAP_DIR)};
    options.identity = host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"};
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    (*app)->attachCommands(bus);
    gui::PluginBrowser browser(**app, bus, *f.root,
                               f.configDir / gui::test::slug() / "favourites.json");
    browser.open([](const std::string&) {});
    auto* search = dynamic_cast<ui::TextField*>(f.uiRoot().find("browser.search"));
    REQUIRE(search);
    f.uiRoot().focus(search);
    f.type("Hickory");
    auto* list = dynamic_cast<ui::List*>(f.uiRoot().find("browser.results"));
    REQUIRE(list);
    bool sawShadowed = false, sawWinner = false;
    std::optional<std::size_t> shadowedRow;
    for (std::size_t i = 0; i < browser.rowCount(); ++i) {
        const auto row = browser.rowText(i);
        if (row.id != "com.suppleaudio.hickory") continue;
        if (row.vendor == "shadowed") {
            sawShadowed = true;
            shadowedRow = i;
        } else {
            sawWinner = true;
        }
    }
    CHECK(sawWinner);
    CHECK(sawShadowed);
    REQUIRE(shadowedRow);
    list->select(*shadowedRow, true);  // as a click or the arrow keys would
    // The shadowed copy is the built one: its own file, and why it is ignored.
    CHECK(browser.detail().find("Hickory.clap") != std::string::npos);
    CHECK(browser.detail().find("shadowed") != std::string::npos);
    // The copy the loader would use is the one earlier on the search path.
    list->select(*shadowedRow == 0 ? 1 : 0, true);
    CHECK(browser.detail().find(std::filesystem::canonical(dup).string()) != std::string::npos);
    browser.close();
}

TEST_CASE("the plugin browser with no track given adds to every selected track") {
    Fixture f;
    if (!f.ok) return;
    const model::Id one = f.addTrack("One"), two = f.addTrack("Two");
    gui::UiState selection;
    selection.selectedTracks = {one.str(), two.str()};
    const auto before = f.historyCount();
    REQUIRE(f.fx->openToAdd(selection, json::Value()));
    f.frame();
    CHECK(f.uiRoot().find("browser.title")->valueText() == "Add plugin to 2 tracks");
    f.type("Hickory");
    f.key("Return");
    CHECK(f.app->head().tracks.find(one)->chain.size() == 1);
    CHECK(f.app->head().tracks.find(two)->chain.size() == 1);
    CHECK(f.historyCount() == before + 1);
    // A strip's own "+" names its track: only that one.
    REQUIRE(f.fx->openToAdd(selection, json::Value(two.str())));
    f.frame();
    f.type("Hickory");
    f.key("Return");
    CHECK(f.app->head().tracks.find(one)->chain.size() == 1);
    CHECK(f.app->head().tracks.find(two)->chain.size() == 2);
}

TEST_CASE("the side panel's browser adds to the selected tracks, and stays") {
    Fixture f;
    if (!f.ok) return;
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id one = f.addTrack("One"), two = f.addTrack("Two");
    gui::SidePanel side(*f.root);
    int shown = 0;
    struct Dragged {
        std::string plugin, name;
        ui::DragPhase phase;
    };
    std::vector<Dragged> dragged;
    auto hear = [&](const std::string& plugin, const std::string& name, ui::DragPhase phase,
                    ui::Point) { dragged.push_back({plugin, name, phase}); };
    side.addTab("browser", "Browser", f.fx->buildSideBrowser(f.ui, hear), [&](bool on) {
        if (on) {
            ++shown;
            f.fx->sideBrowser().reload();
        }
    });
    f.root->find("window")->add(side.build());
    side.show("browser");
    f.root->frame(false);
    CHECK(shown == 1);
    auto* search = dynamic_cast<ui::TextField*>(f.root->find("side.browser.plugins.search"));
    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.plugins.results"));
    auto* status = f.root->find("side.browser.plugins.status");
    REQUIRE(search);
    REQUIRE(list);
    REQUIRE(status);
    CHECK(search->visibleInTree());
    // A panel has its own tab and Close: the browser brings no title.
    CHECK_FALSE(f.root->find("side.browser.plugins.title"));
    CHECK_FALSE(f.root->find("side.browser.plugins.close"));
    CHECK(f.fx->sideBrowser().rowCount() > 0);
    // A row says which format it is when it is not CLAP.
    {
        std::size_t vst3 = 0, marked = 0;
        auto listed = f.run("plugin.list", json::Value::object());
        REQUIRE(listed);
        for (const json::Value& p : (*listed)["plugins"].asArray())
            if (const json::Value* format = p.find("format")) vst3 += format->asString() == "vst3";
        for (std::size_t i = 0; i < f.fx->sideBrowser().rowCount(); ++i)
            marked += f.fx->sideBrowser().rowText(i).format == "VST3";
        CHECK(marked == vst3);
    }
    // It fits the panel: nothing is wider than it.
    const ui::Rect panel = f.root->find("side")->bounds();
    for (const char* id : {"side.browser.plugins.search", "side.browser.plugins.results",
                           "side.browser.plugins.footer", "side.browser.plugins.add"}) {
        const ui::Rect r = f.root->find(id)->bounds();
        CHECK_MESSAGE(r.x >= panel.x, id);
        CHECK_MESSAGE(r.right() <= panel.right(), id);
    }

    // Add waits for a chosen row: here it goes to every selected track, so a
    // stray click must not put the list's first plug-in on all of them.
    {
        ui::Node* add = f.root->find("side.browser.plugins.add");
        CHECK_FALSE(list->selected());
        CHECK_FALSE(add->enabled);
        f.ui.selectedTracks = {one.str(), two.str()};
        CHECK_FALSE(f.root->click("side.browser.plugins.add"));
        f.root->focus(search);
        f.root->key(SDLK_RETURN, 0);  // Enter in an empty search: nothing either
        f.root->frame(false);
        CHECK(f.app->head().tracks.find(one)->chain.empty());
        CHECK(f.app->head().tracks.find(two)->chain.empty());
        // The arrow keys choose a row from the search, and Add is on.
        f.root->key(SDLK_DOWN, 0);
        f.root->frame(false);
        CHECK(list->selected());
        CHECK(add->enabled);
        f.ui.selectedTracks.clear();
    }

    auto typed = [&](const std::string& text) {
        f.root->focus(search);
        search->setText("");
        f.root->text(text);
        f.root->frame(false);
    };
    // Nothing selected: nothing is added, and the status line says why.
    auto before = f.historyCount();
    typed("Hickory");
    REQUIRE(f.fx->sideBrowser().rowCount() >= 1);
    CHECK(f.fx->sideBrowser().rowText(0).name.find("Hickory") != std::string::npos);
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    CHECK(status->valueText() == "Select a track first");
    CHECK(f.historyCount() == before);

    // One track: Enter adds there; the browser and its search stay.
    f.ui.selectedTracks = {one.str()};
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    CHECK(f.app->head().tracks.find(one)->chain.size() == 1);
    CHECK(f.app->head().tracks.find(two)->chain.empty());
    CHECK(status->valueText() == "Added to One");
    CHECK(search->visibleInTree());
    CHECK(search->text() == "Hickory");
    CHECK(f.historyCount() == before + 1);

    // Two tracks: the Add button puts it on both, as one undo step.
    f.ui.selectedTracks = {one.str(), two.str()};
    REQUIRE(f.root->click("side.browser.plugins.add"));
    f.root->frame(false);
    CHECK(f.app->head().tracks.find(one)->chain.size() == 2);
    CHECK(f.app->head().tracks.find(two)->chain.size() == 1);
    CHECK(status->valueText() == "Added to 2 tracks");
    CHECK(f.historyCount() == before + 2);
    // A double click on a row adds it too.
    const ui::Rect rows = list->bounds();
    const ui::Point first{rows.x + 20, rows.y + list->rowHeight / 2};
    f.root->pointer({ui::PointerEvent::Type::Down, first, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, first, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Down, first, 1, 2});
    f.root->pointer({ui::PointerEvent::Type::Up, first, 1, 2});
    f.root->frame(false);
    CHECK(f.app->head().tracks.find(one)->chain.size() == 3);
    CHECK(f.app->head().tracks.find(two)->chain.size() == 2);
    CHECK(f.historyCount() == before + 3);

    // A row dragged out is told to the owner with the plugin's id and name;
    // the browser itself adds nothing. The plugin is the one the drag began
    // on, even when the rows change under it.
    before = f.historyCount();
    const std::string plugin = f.fx->sideBrowser().shown().front();
    f.root->pointer({ui::PointerEvent::Type::Down, first, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {first.x - 40, first.y}, 1, 1});
    REQUIRE(dragged.size() == 1);
    CHECK(dragged[0].phase == ui::DragPhase::Begin);
    CHECK(dragged[0].plugin == plugin);
    CHECK(dragged[0].name.find("Hickory") != std::string::npos);
    typed("no such plugin");
    CHECK(f.fx->sideBrowser().rowCount() == 0);
    f.root->pointer({ui::PointerEvent::Type::Move, {first.x - 300, first.y}, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {first.x - 300, first.y}, 1, 1});
    REQUIRE(dragged.size() == 3);
    CHECK(dragged[2].phase == ui::DragPhase::End);
    CHECK(dragged[2].plugin == plugin);
    CHECK(f.historyCount() == before);
    typed("Hickory");

    // The strip's popup browser opens beside it; each keeps its own search.
    REQUIRE(f.fx->openToAdd(f.ui, json::Value(two.str())));
    f.root->frame(false);
    REQUIRE(f.root->find("browser.search"));
    CHECK(f.root->find("browser.title")->valueText() == "Add plugin to Two");
    f.root->text("Hickory");
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    CHECK_FALSE(f.root->find("browser.search"));  // the popup closed
    CHECK(f.app->head().tracks.find(two)->chain.size() == 3);
    CHECK(f.app->head().tracks.find(one)->chain.size() == 3);
    CHECK(search->text() == "Hickory");

    // A favourite set in one is first in the other when it comes into view.
    typed("");
    const std::string last = f.fx->sideBrowser().shown().back();
    list->select(f.fx->sideBrowser().rowCount() - 1, true);
    REQUIRE(f.root->click("side.browser.plugins.fav"));
    f.root->frame(false);
    CHECK(f.fx->sideBrowser().shown().front() == last);
    side.hide();
    f.root->frame(false);
    CHECK_FALSE(search->visibleInTree());
    side.show("browser");
    f.root->frame(false);
    CHECK(shown == 2);
    CHECK(f.fx->sideBrowser().shown().front() == last);
}

TEST_CASE("the side panel's browser has kinds: saved FX chains go on tracks by Add or by a drag") {
    Fixture f;
    if (!f.ok) return;
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id source = f.addTrack("Source"), one = f.addTrack("One"), two = f.addTrack("Two");
    auto chain = [&](const model::Id& track) {
        return f.app->head().tracks.find(track)->chain.size();
    };
    gui::PluginDrag drag(*f.root, *f.dispatcher);
    // The test's drop target: anything left of x = 100 is track Two.
    drag.addTargets([&](ui::Point p) -> std::optional<gui::TrackTarget> {
        if (p.x >= 100) return std::nullopt;
        return gui::TrackTarget{two, {0, 0, 100, 400}};
    });
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    f.root->frame(false);
    auto inView = [&](const char* id) {
        const ui::Node* n = f.root->find(id);
        return n && n->visibleInTree();
    };
    auto text = [&](const char* id) { return f.root->find(id)->valueText(); };

    // Plug-ins first; a click on the row's second quarter shows Chains.
    CHECK(browser.kind() == "plugins");
    CHECK(text("side.browser.kinds") == "Plug-ins");
    CHECK(inView("side.browser.plugins.search"));
    CHECK_FALSE(inView("side.browser.chains.search"));
    const ui::Rect kinds = f.root->find("side.browser.kinds")->bounds();
    auto pick = [&](float part) {
        const ui::Point at{kinds.x + kinds.w * part, kinds.y + kinds.h / 2};
        f.root->pointer({ui::PointerEvent::Type::Down, at, 1});
        f.root->pointer({ui::PointerEvent::Type::Up, at, 1});
        f.root->frame(false);
    };
    f.root->focus(f.root->find("side.browser.plugins.search"));
    pick(0.375f);
    CHECK(browser.kind() == "chains");
    CHECK(text("side.browser.kinds") == "Chains");
    CHECK(inView("side.browser.chains.search"));
    CHECK_FALSE(inView("side.browser.plugins.search"));
    // The page that left gave up the keyboard.
    CHECK(f.root->focused() != f.root->find("side.browser.plugins.search"));
    // Nothing saved: the page says where chains come from.
    CHECK(browser.chainCount() == 0);
    REQUIRE(inView("side.browser.chains.empty"));
    CHECK(text("side.browser.chains.empty").find("Save chain") != std::string::npos);

    // Two chains saved from a track: listed by name with their size, when
    // the page next comes into view.
    REQUIRE(f.run("fx.add", f.args({{"track", source.str()}, {"plugin", f.pluginId("Hickory")}})));
    REQUIRE(f.run("fx.chain.save", f.args({{"track", source.str()}, {"name", "Clean"}})));
    REQUIRE(f.run("fx.add", f.args({{"track", source.str()}, {"plugin", f.pluginId("Hickory")}})));
    REQUIRE(f.run("fx.chain.save", f.args({{"track", source.str()}, {"name", "Vocal bus"}})));
    CHECK(browser.chainCount() == 0);  // not read while nothing asked
    pick(0.125f);
    pick(0.375f);
    REQUIRE(browser.chainCount() == 2);
    CHECK_FALSE(inView("side.browser.chains.empty"));
    CHECK(browser.chainRow(0).name == "Clean");
    CHECK(browser.chainRow(0).detail == "1 effect");
    CHECK(browser.chainRow(1).name == "Vocal bus");
    CHECK(browser.chainRow(1).detail == "2 effects");
    // Add waits for a chosen row: nothing loads onto the selected tracks
    // from a click on it, or from Enter in an empty search.
    {
        ui::Node* add = f.root->find("side.browser.chains.add");
        CHECK_FALSE(add->enabled);
        f.ui.selectedTracks = {one.str(), two.str()};
        const auto steps = f.historyCount();
        CHECK_FALSE(f.root->click("side.browser.chains.add"));
        browser.focusSearch();
        f.root->key(SDLK_RETURN, 0);
        f.root->frame(false);
        CHECK(f.historyCount() == steps);
        f.root->key(SDLK_DOWN, 0);  // the arrow keys choose a row
        f.root->frame(false);
        CHECK(add->enabled);
        f.ui.selectedTracks.clear();
    }

    // The search narrows by name; nothing found says so.
    browser.focusSearch();
    CHECK(f.root->focused() == f.root->find("side.browser.chains.search"));
    f.root->text("voc");
    f.root->frame(false);
    REQUIRE(browser.chainCount() == 1);
    CHECK(browser.chainRow(0).name == "Vocal bus");
    // Enter with nothing selected: nothing loads.
    auto before = f.historyCount();
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    CHECK(text("side.browser.chains.status") == "Select a track first");
    CHECK(f.historyCount() == before);
    // One track: Enter appends the chain there.
    f.ui.selectedTracks = {one.str()};
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    CHECK(chain(one) == 2);
    CHECK(text("side.browser.chains.status") == "Loaded onto One");
    CHECK(f.historyCount() == before + 1);
    // Two tracks: Add loads it onto both as one undo step.
    f.ui.selectedTracks = {one.str(), two.str()};
    REQUIRE(f.root->click("side.browser.chains.add"));
    f.root->frame(false);
    CHECK(chain(one) == 4);
    CHECK(chain(two) == 2);
    CHECK(text("side.browser.chains.status") == "Loaded onto 2 tracks");
    CHECK(f.historyCount() == before + 2);
    REQUIRE(f.run("history.undo", json::Value::object()));
    CHECK(chain(one) == 2);
    CHECK(chain(two) == 0);

    // A row dragged onto a track loads the chain on that track only,
    // whatever is selected; released elsewhere it does nothing.
    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.chains.results"));
    REQUIRE(list);
    const ui::Point row{list->bounds().x + 20, list->bounds().y + list->rowHeight / 2};
    before = f.historyCount();
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {row.x - 30, row.y}, 1});
    CHECK(drag.view().active);
    CHECK(drag.view().label == "Vocal bus");
    CHECK_FALSE(drag.view().target);
    // Still over the Browser it came from, the pointer is the move cursor:
    // its own place refuses nothing.
    f.root->pointer({ui::PointerEvent::Type::Move, {row.x + 8, row.y}, 1});
    CHECK_FALSE(drag.view().target);
    CHECK(f.root->cursor() == ui::Cursor::Move);
    f.root->pointer({ui::PointerEvent::Type::Move, {50, 50}, 1});
    REQUIRE(drag.view().target);
    CHECK(drag.view().target->track == two);
    f.root->pointer({ui::PointerEvent::Type::Up, {50, 50}, 1});
    f.root->frame(false);
    CHECK_FALSE(drag.view().active);
    CHECK(chain(two) == 2);
    CHECK(chain(one) == 2);
    CHECK(text("side.browser.chains.status") == "Loaded onto Two");
    CHECK(f.historyCount() == before + 1);
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {row.x - 30, row.y}, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {row.x - 30, row.y}, 1});
    f.root->frame(false);
    CHECK(f.historyCount() == before + 1);
    f.snapshot("side-browser-chains.png");

    // A search that finds nothing, then the kinds from the keyboard.
    browser.focusSearch();
    f.root->text("zzz");
    f.root->frame(false);
    CHECK(browser.chainCount() == 0);
    CHECK(text("side.browser.chains.empty") == "No chain matches.");
    f.root->focus(f.root->find("side.browser.kinds"));
    f.root->key(SDLK_LEFT, 0);
    f.root->frame(false);
    CHECK(browser.kind() == "plugins");
    f.root->key(SDLK_LEFT, 0);  // already the first
    CHECK(browser.kind() == "plugins");
    f.root->key(SDLK_RIGHT, 0);
    CHECK(browser.kind() == "chains");
    browser.show("no such kind");
    CHECK(browser.kind() == "chains");
    // A plug-in dragged from the Plug-ins page still lands through fx.add.
    browser.show("plugins");
    f.root->frame(false);
    auto* plugins = dynamic_cast<ui::List*>(f.root->find("side.browser.plugins.results"));
    REQUIRE(plugins);
    const ui::Point first{plugins->bounds().x + 20, plugins->bounds().y + plugins->rowHeight / 2};
    const auto twoBefore = chain(two);
    f.root->pointer({ui::PointerEvent::Type::Down, first, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {first.x - 30, first.y}, 1});
    CHECK(drag.view().plugin == f.fx->sideBrowser().shown().front());
    f.root->pointer({ui::PointerEvent::Type::Up, {50, 50}, 1});
    f.root->frame(false);
    CHECK(chain(two) == twoBefore + 1);
    CHECK(text("side.browser.plugins.status") == "Added to Two");
}

TEST_CASE("the side panel's browser lists a folder's files; a file is added or dragged") {
    Fixture f;
    if (!f.ok) return;
    // A folder with two subfolders, three files that import, one that does
    // not and a hidden one.
    const std::filesystem::path media = f.dir / "media";
    std::filesystem::create_directories(media / "Loops");
    std::filesystem::create_directories(media / "zeta");
    auto touch = [](const std::filesystem::path& file) {
        std::FILE* out = std::fopen(file.string().c_str(), "wb");
        REQUIRE(out);
        std::fclose(out);
    };
    for (const char* name : {"b.wav", "A.flac", "song.MID", "notes.txt", ".hidden.wav"})
        touch(media / name);
    touch(media / "Loops" / "x.mp3");

    gui::PluginDrag drag(*f.root, *f.dispatcher);
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    std::vector<std::filesystem::path> added;
    std::vector<std::pair<std::filesystem::path, ui::Point>> dropped;
    bool refuse = false;
    browser.files.add = [&](const std::filesystem::path& file) -> Status {
        if (refuse) return Error("test.refused", "that file cannot be read");
        added.push_back(file);
        return {};
    };
    // The test's arrangement: anything left of x = 100.
    browser.files.where = [](ui::Point p) -> std::optional<ui::Rect> {
        if (p.x >= 100) return std::nullopt;
        return ui::Rect{0, 0, 100, 400};
    };
    browser.files.drop = [&](const std::filesystem::path& file, ui::Point at) -> Status {
        dropped.emplace_back(file, at);
        return {};
    };
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    REQUIRE(browser.openFolder(media));  // before the page is first shown: it stays there
    browser.show("files");
    f.root->frame(false);
    auto text = [&](const char* id) { return f.root->find(id)->valueText(); };
    auto inView = [&](const char* id) {
        const ui::Node* n = f.root->find(id);
        return n && n->visibleInTree();
    };
    CHECK(text("side.browser.kinds") == "Files");
    REQUIRE(inView("side.browser.files.results"));
    CHECK(browser.folder() == media);
    CHECK(text("side.browser.files.folder") == "media");
    CHECK(text("side.browser.files.path") == media.string());

    // Folders first, then what imports, by name whatever the case.
    auto names = [&] {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < browser.fileCount(); ++i) {
            const auto row = browser.fileRow(i);
            out.push_back(row.name + "|" + row.detail);
        }
        return out;
    };
    CHECK(names() == std::vector<std::string>{"Loops|folder", "zeta|folder", "A.flac|FLAC",
                                              "b.wav|WAV", "song.MID|MIDI"});
    CHECK(gui::SideBrowser::importable("take.WAV"));
    CHECK(gui::SideBrowser::importable("a/b/c.midi"));
    CHECK_FALSE(gui::SideBrowser::importable("notes.txt"));
    CHECK_FALSE(gui::SideBrowser::importable("wav"));

    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.files.results"));
    REQUIRE(list);
    auto rowPoint = [&](std::size_t row) {
        return ui::Point{list->bounds().x + 20,
                         list->bounds().y + (static_cast<float>(row) + 0.5f) * list->rowHeight};
    };
    auto doubleClick = [&](std::size_t row) {
        const ui::Point at = rowPoint(row);
        f.root->pointer({ui::PointerEvent::Type::Down, at, 1, 1});
        f.root->pointer({ui::PointerEvent::Type::Up, at, 1, 1});
        f.root->pointer({ui::PointerEvent::Type::Down, at, 1, 2});
        f.root->pointer({ui::PointerEvent::Type::Up, at, 1, 2});
        f.root->frame(false);
    };
    // Add waits for a chosen row; one click chooses it.
    {
        ui::Node* add = f.root->find("side.browser.files.add");
        CHECK_FALSE(list->selected());
        CHECK_FALSE(add->enabled);
        CHECK_FALSE(f.root->click("side.browser.files.add"));
        CHECK(added.empty());
        f.root->pointer({ui::PointerEvent::Type::Down, rowPoint(3), 1, 1});
        f.root->pointer({ui::PointerEvent::Type::Up, rowPoint(3), 1, 1});
        f.root->frame(false);
        CHECK(add->enabled);
        REQUIRE(f.root->click("side.browser.files.add"));
        f.root->frame(false);
        REQUIRE(added.size() == 1);
        CHECK(added[0] == media / "b.wav");
        added.clear();
    }
    // A double click on a folder goes into it; Up comes back.
    doubleClick(0);
    CHECK(browser.folder() == media / "Loops");
    CHECK(text("side.browser.files.folder") == "Loops");
    CHECK(names() == std::vector<std::string>{"x.mp3|MP3"});
    CHECK(added.empty());
    REQUIRE(f.root->click("side.browser.files.up"));
    f.root->frame(false);
    CHECK(browser.folder() == media);
    CHECK(browser.fileCount() == 5);
    // An empty folder says so.
    doubleClick(1);
    CHECK(browser.folder() == media / "zeta");
    CHECK(browser.fileCount() == 0);
    REQUIRE(inView("side.browser.files.empty"));
    CHECK(text("side.browser.files.empty") == "No folders and no audio or MIDI files here.");
    REQUIRE(f.root->click("side.browser.files.up"));
    f.root->frame(false);
    CHECK_FALSE(inView("side.browser.files.empty"));

    // The search narrows this folder; Enter adds the file found.
    browser.focusSearch();
    CHECK(f.root->focused() == f.root->find("side.browser.files.search"));
    f.root->text("SONG");
    f.root->frame(false);
    CHECK(names() == std::vector<std::string>{"song.MID|MIDI"});
    f.root->key(SDLK_RETURN, 0);
    f.root->frame(false);
    REQUIRE(added.size() == 1);
    CHECK(added[0] == media / "song.MID");
    CHECK(text("side.browser.files.status") == "Added song.MID");
    // What the import refuses is said, and nothing is claimed.
    refuse = true;
    REQUIRE(f.root->click("side.browser.files.add"));
    f.root->frame(false);
    CHECK(added.size() == 1);
    CHECK(text("side.browser.files.status") == "that file cannot be read");
    refuse = false;
    browser.focusSearch();
    f.root->text("zzz");
    f.root->frame(false);
    CHECK(browser.fileCount() == 0);
    CHECK(text("side.browser.files.empty") == "Nothing here matches.");
    // Going into a folder starts with an empty search.
    REQUIRE(browser.openFolder(media));
    f.root->frame(false);
    CHECK(text("side.browser.files.search").empty());
    CHECK(browser.fileCount() == 5);

    // A file drags to where the app says it may land; released there it is
    // dropped at that point, elsewhere nothing happens.
    const ui::Point wav = rowPoint(3);
    f.root->pointer({ui::PointerEvent::Type::Down, wav, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {wav.x - 30, wav.y}, 1});
    CHECK(drag.view().active);
    CHECK(drag.view().label == "b.wav");
    CHECK_FALSE(drag.view().place);
    f.root->pointer({ui::PointerEvent::Type::Move, {60, 70}, 1});
    REQUIRE(drag.view().place);
    CHECK(drag.view().place->w == 100);
    CHECK_FALSE(drag.view().target);
    f.root->pointer({ui::PointerEvent::Type::Up, {61, 71}, 1});
    f.root->frame(false);
    CHECK_FALSE(drag.view().active);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].first == media / "b.wav");
    CHECK(dropped[0].second.x == 61);
    CHECK(dropped[0].second.y == 71);
    CHECK(text("side.browser.files.status") == "Added b.wav");
    f.root->pointer({ui::PointerEvent::Type::Down, wav, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {wav.x - 30, wav.y}, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {wav.x - 30, wav.y}, 1});
    CHECK(dropped.size() == 1);
    // A folder does not drag.
    const ui::Point folder = rowPoint(0);
    f.root->pointer({ui::PointerEvent::Type::Down, folder, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {60, 70}, 1});
    CHECK_FALSE(drag.view().active);
    f.root->pointer({ui::PointerEvent::Type::Up, {60, 70}, 1});
    CHECK(dropped.size() == 1);
    CHECK(browser.folder() == media);
    f.snapshot("side-browser-files.png");

    // Up from a root lists the roots where there are several (the drives
    // on Windows); one of them opens as a folder does.
    CHECK(f.root->find("side.browser.files.up")->enabled);
    const std::filesystem::path top = media.root_path();
    REQUIRE(browser.openFolder(top));
    f.root->frame(false);
    CHECK(text("side.browser.files.folder") == top.string());  // a root has no name
#if !defined(_WIN32)
    CHECK_FALSE(f.root->find("side.browser.files.up")->enabled);  // one root: nowhere to go
#endif
    browser.files.roots = [&] {
        return std::vector<std::filesystem::path>{media, media / "Loops"};
    };
    REQUIRE(browser.openFolder(top));
    f.root->frame(false);
    CHECK(f.root->find("side.browser.files.up")->enabled);
    REQUIRE(f.root->click("side.browser.files.up"));
    f.root->frame(false);
    CHECK(browser.folder().empty());
    CHECK(text("side.browser.files.folder") == "Computer");
    CHECK_FALSE(f.root->find("side.browser.files.up")->enabled);
    CHECK(names() == std::vector<std::string>{media.string() + "|folder",
                                              (media / "Loops").string() + "|folder"});
    doubleClick(1);
    CHECK(browser.folder() == media / "Loops");
    CHECK(names() == std::vector<std::string>{"x.mp3|MP3"});
    REQUIRE(browser.openFolder(media));
    f.root->frame(false);

    // The path under the list: whole when it fits; a long one loses its
    // start, at a separator, and keeps its last folders.
    const std::filesystem::path deep =
        media / "a-folder-with-a-rather-long-name" / "and-another-one-inside-it";
    std::filesystem::create_directories(deep);
    REQUIRE(browser.openFolder(deep));
    f.root->frame(false);
    CHECK(text("side.browser.files.path") == deep.string());  // what it stands for
    const std::string shownPath = browser.shownPath();
    const std::string ellipsis = "\xE2\x80\xA6";
    REQUIRE(shownPath.size() > ellipsis.size() + 1);
    CHECK(shownPath.rfind(ellipsis, 0) == 0);
    CHECK((shownPath[ellipsis.size()] == '/' || shownPath[ellipsis.size()] == '\\'));
    CHECK(shownPath.size() < deep.string().size());
    const std::string last = "and-another-one-inside-it";
    REQUIRE(shownPath.size() >= last.size());
    CHECK(shownPath.compare(shownPath.size() - last.size(), last.size(), last) == 0);
    CHECK(deep.string().find(shownPath.substr(ellipsis.size())) != std::string::npos);
    browser.files.roots = [] { return std::vector<std::filesystem::path>{"/a", "/b"}; };
    browser.openRoots();
    f.root->frame(false);
    CHECK(browser.shownPath().empty());
    browser.files.roots = [&] {
        return std::vector<std::filesystem::path>{media, media / "Loops"};
    };
    std::filesystem::remove_all(media / "a-folder-with-a-rather-long-name");
    REQUIRE(browser.openFolder(media));
    f.root->frame(false);

    // A folder that cannot be opened: said, and the listing stays.
    CHECK_FALSE(browser.openFolder(media / "gone"));
    CHECK(text("side.browser.files.status").find("Cannot open gone") != std::string::npos);
    CHECK(browser.folder() == media);
    CHECK(browser.fileCount() == 5);
    // A folder removed while the tab was closed: the page falls back to
    // somewhere that exists when it comes into view again.
    REQUIRE(browser.openFolder(media / "zeta"));
    side.hide();
    std::filesystem::remove_all(media / "zeta");
    side.show("browser");
    f.root->frame(false);
    CHECK(browser.folder() != media / "zeta");
    std::error_code ec;
    CHECK(std::filesystem::is_directory(browser.folder(), ec));
}

// gui.md §4.14, Media: the audio the project already holds, put on a track
// again without a second copy or a second source.
TEST_CASE("the side panel's browser lists the project's media; one is added or dragged") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* command, const json::Value& args) {
        auto r = f.run(command, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    // Three files outside the project: twelve seconds, 65 seconds, a third.
    const std::filesystem::path sounds = f.dir / "sounds";
    std::filesystem::create_directories(sounds);
    auto tone = [&](const char* name, int seconds, float level) {
        audio::AudioData audio;
        audio.sampleRate = 8000;
        audio.channels.assign(1,
                              std::vector<float>(static_cast<std::size_t>(8000 * seconds), level));
        REQUIRE(audio::writeWav(sounds / name, audio));
        return sounds / name;
    };
    const auto kick = tone("kick.wav", 12, 0.25f);
    const auto bass = tone("Bass.wav", 65, 0.5f);
    const auto pad = tone("pad.wav", 1, 0.125f);
    const std::string track =
        exec("track.add", json::parse(R"({"name":"Keys"})").value())["track"].asString();
    auto import = [&](const std::filesystem::path& file, double position) {
        json::Value add = json::Value::object();
        add["track"] = track;
        add["audio_file"] = file.string();
        add["position"] = position;
        return exec("item.add", add);
    };

    gui::PluginDrag drag(*f.root, *f.dispatcher);
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    std::vector<std::filesystem::path> added;
    std::vector<std::pair<std::filesystem::path, ui::Point>> dropped;
    bool refuse = false;
    // As the app's hook: the file goes on a track through item.add.
    browser.files.add = [&](const std::filesystem::path& file) -> Status {
        if (refuse) return Error("test.refused", "that file cannot be read");
        added.push_back(file);
        (void)import(file, 64.0 * static_cast<double>(added.size()));
        return {};
    };
    browser.files.where = [](ui::Point p) -> std::optional<ui::Rect> {
        if (p.x >= 100) return std::nullopt;
        return ui::Rect{0, 0, 100, 400};
    };
    browser.files.drop = [&](const std::filesystem::path& file, ui::Point at) -> Status {
        dropped.emplace_back(file, at);
        return {};
    };
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    browser.show("media");
    f.root->frame(false);
    auto text = [&](const char* id) { return f.root->find(id)->valueText(); };
    auto names = [&] {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < browser.mediaCount(); ++i) {
            const auto row = browser.mediaRow(i);
            out.push_back(row.name + "|" + row.detail);
        }
        return out;
    };
    auto pass = [&] {  // the loop's
        browser.sync();
        f.root->frame(false);
    };
    CHECK(text("side.browser.kinds") == "Media");
    const ui::Node* results = f.root->find("side.browser.media.results");
    REQUIRE(results);
    REQUIRE(results->visibleInTree());
    CHECK_FALSE(f.root->find("side.browser.files.results")->visibleInTree());

    // A project with no audio: no rows, and Add waits.
    ui::Node* add = f.root->find("side.browser.media.add");
    REQUIRE(add);
    CHECK(browser.mediaCount() == 0);
    CHECK_FALSE(add->enabled);
    f.snapshot("side-browser-media-empty.png");

    // The project takes in audio: the page in view lists it at the loop's
    // next pass, by name whatever the case, with its format and length.
    (void)import(kick, 0);
    (void)import(bass, 32);
    CHECK(browser.mediaCount() == 0);
    pass();
    CHECK(names() ==
          std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05", "kick.wav|WAV \u00b7 0:12"});
    REQUIRE(f.app->head().media.size() == 2);

    // Add waits for a chosen row; one click chooses it, and the chosen row
    // stays chosen when the project changes elsewhere.
    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.media.results"));
    REQUIRE(list);
    auto rowPoint = [&](std::size_t row) {
        return ui::Point{list->bounds().x + 20,
                         list->bounds().y + (static_cast<float>(row) + 0.5f) * list->rowHeight};
    };
    CHECK_FALSE(list->selected());
    CHECK_FALSE(f.root->click("side.browser.media.add"));
    CHECK(added.empty());
    f.root->pointer({ui::PointerEvent::Type::Down, rowPoint(1), 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, rowPoint(1), 1, 1});
    f.root->frame(false);
    CHECK(add->enabled);
    (void)exec("track.add", json::parse(R"({"name":"Other"})").value());
    pass();
    REQUIRE(list->selected());
    CHECK(*list->selected() == 1);
    // A source added that sorts before it: the same source stays chosen.
    (void)import(pad, 48);
    pass();
    CHECK(names() == std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05",
                                              "kick.wav|WAV \u00b7 0:12",
                                              "pad.wav|WAV \u00b7 0:01"});
    REQUIRE(list->selected());
    CHECK(browser.mediaRow(*list->selected()).name == "kick.wav");

    // Add puts the chosen media on a track: the file the source names, so
    // the project gains an item and no second source.
    const std::size_t items = f.app->head().items.size();
    REQUIRE(f.root->click("side.browser.media.add"));
    pass();
    REQUIRE(added.size() == 1);
    CHECK(added[0] == kick);
    CHECK(text("side.browser.media.status") == "Added kick.wav");
    CHECK(f.app->head().items.size() == items + 1);
    CHECK(f.app->head().media.size() == 3);
    f.snapshot("side-browser-media.png");
    // What the import refuses is said, and nothing is claimed.
    refuse = true;
    REQUIRE(f.root->click("side.browser.media.add"));
    f.root->frame(false);
    CHECK(added.size() == 1);
    CHECK(text("side.browser.media.status") == "that file cannot be read");
    refuse = false;

    // The search narrows the list; Enter adds the one found.
    browser.focusSearch();
    CHECK(f.root->focused() == f.root->find("side.browser.media.search"));
    f.root->text("BASS");
    f.root->frame(false);
    CHECK(names() == std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05"});
    f.root->key(SDLK_RETURN, 0);
    pass();
    REQUIRE(added.size() == 2);
    CHECK(added[1] == bass);
    CHECK(f.app->head().media.size() == 3);
    for (int i = 0; i < 4; ++i) f.root->key(SDLK_BACKSPACE, 0);
    f.root->frame(false);
    CHECK(browser.mediaCount() == 3);

    // It drags into the arrangement as a file does.
    const ui::Point row = rowPoint(2);
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {row.x - 30, row.y}, 1});
    CHECK(drag.view().active);
    CHECK(drag.view().label == "pad.wav");
    CHECK_FALSE(drag.view().place);
    f.root->pointer({ui::PointerEvent::Type::Move, {60, 70}, 1});
    REQUIRE(drag.view().place);
    f.root->pointer({ui::PointerEvent::Type::Up, {61, 71}, 1});
    f.root->frame(false);
    CHECK_FALSE(drag.view().active);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].first == pad);
    CHECK(dropped[0].second.x == 61);
    CHECK(text("side.browser.media.status") == "Added pad.wav");

    // The files are looked for when the page comes into view and when the
    // project's sources change, not at every edit: a file that has gone is
    // said to be missing when the page comes into view again; the others
    // are as they were.
    std::filesystem::remove(pad);
    (void)exec("track.add", json::parse(R"({"name":"Elsewhere"})").value());
    pass();
    CHECK(names() == std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05",
                                              "kick.wav|WAV \u00b7 0:12",
                                              "pad.wav|WAV \u00b7 0:01"});
    side.hide();
    side.show("browser");
    f.root->frame(false);
    CHECK(names() == std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05",
                                              "kick.wav|WAV \u00b7 0:12", "pad.wav|missing"});
    // It comes back: still "missing" after an edit elsewhere, found again
    // when the project takes in another source, and at an undo of that.
    (void)tone("pad.wav", 1, 0.125f);
    (void)exec("track.add", json::parse(R"({"name":"Elsewhere"})").value());
    pass();
    CHECK(browser.mediaRow(2).detail == "missing");
    (void)import(tone("hat.wav", 3, 0.25f), 80);
    pass();
    CHECK(names() == std::vector<std::string>{"Bass.wav|WAV \u00b7 1:05", "hat.wav|WAV \u00b7 0:03",
                                              "kick.wav|WAV \u00b7 0:12",
                                              "pad.wav|WAV \u00b7 0:01"});
    (void)exec("history.undo", json::Value::object());
    pass();
    CHECK(browser.mediaCount() == 3);
    // The other kinds are a click away, and the page comes back as it was.
    browser.show("files");
    f.root->frame(false);
    CHECK_FALSE(results->visibleInTree());
    // On the kinds row Right goes on from Files to Media, the last.
    f.root->focus(f.root->find("side.browser.kinds"));
    f.root->key(SDLK_RIGHT, 0);
    f.root->frame(false);
    CHECK(browser.kind() == "media");
    f.root->key(SDLK_RIGHT, 0);
    CHECK(browser.kind() == "media");
    CHECK(results->visibleInTree());
    CHECK(browser.mediaCount() == 3);
}

// A saved project that copies what it imports names its media relative to
// its media folder: the page finds the file there.
TEST_CASE("the side panel's browser finds a project's own copy of its media") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* command, const json::Value& args) {
        auto r = f.run(command, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    f.app->setCopyImportedMedia(true);
    json::Value save = json::Value::object();
    save["path"] = (f.dir / "song" / "song.json").string();
    (void)exec("project.save", save);
    REQUIRE_FALSE(f.app->projectDir().empty());
    audio::AudioData audio;
    audio.sampleRate = 8000;
    audio.channels.assign(1, std::vector<float>(8000 * 3, 0.25f));
    std::filesystem::create_directories(f.dir / "sounds");
    const auto outside = f.dir / "sounds" / "clap.wav";
    REQUIRE(audio::writeWav(outside, audio));
    const std::string track =
        exec("track.add", json::parse(R"({"name":"Keys"})").value())["track"].asString();
    auto import = [&](const std::filesystem::path& file, double position) {
        json::Value add = json::Value::object();
        add["track"] = track;
        add["audio_file"] = file.string();
        add["position"] = position;
        (void)exec("item.add", add);
    };
    import(outside, 0);
    REQUIRE(f.app->head().media.size() == 1);
    const std::string stored = f.app->head().media.begin()->value->path;
    REQUIRE(std::filesystem::path(stored).is_relative());  // the project's own copy

    gui::PluginDrag drag(*f.root, *f.dispatcher);
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    std::vector<std::filesystem::path> added;
    browser.files.add = [&](const std::filesystem::path& file) -> Status {
        added.push_back(file);
        import(file, 32);
        return {};
    };
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    browser.show("media");
    f.root->frame(false);
    REQUIRE(browser.mediaCount() == 1);
    CHECK(browser.mediaRow(0).name == "clap.wav");
    CHECK(browser.mediaRow(0).detail == "WAV \u00b7 0:03");  // found, not "missing"
    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.media.results"));
    REQUIRE(list);
    const ui::Point row{list->bounds().x + 20, list->bounds().y + 0.5f * list->rowHeight};
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, row, 1, 1});
    f.root->frame(false);
    REQUIRE(f.root->click("side.browser.media.add"));
    REQUIRE(added.size() == 1);
    CHECK(added[0] == f.app->mediaDir() / stored);
    CHECK(added[0] != outside);
    // The copy's bytes are the source's: an item more, and still one source.
    CHECK(f.app->head().media.size() == 1);
    CHECK(f.app->head().items.size() == 2);

    // Saved somewhere else, the project has another media folder: the same
    // sources are found there from the next pass after an edit on.
    const std::filesystem::path was = f.app->mediaDir();
    save["path"] = (f.dir / "elsewhere" / "song.json").string();
    (void)exec("project.save", save);
    REQUIRE(f.app->mediaDir() != was);
    (void)exec("track.add", json::parse(R"({"name":"Other"})").value());
    browser.sync();
    f.root->frame(false);
    REQUIRE(browser.mediaCount() == 1);
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, row, 1, 1});
    f.root->frame(false);
    REQUIRE(f.root->click("side.browser.media.add"));
    REQUIRE(added.size() == 2);
    CHECK(added[1] == f.app->mediaDir() / stored);
    CHECK(browser.mediaRow(0).detail == "WAV \u00b7 0:03");  // the save took its copy along
}

// Two projects made by the same steps have the same head version: the page
// tells the projects apart, not only the versions.
TEST_CASE("the side panel's browser lists the opened project's media, not the last one's") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* command, const json::Value& args) {
        auto r = f.run(command, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    const std::filesystem::path sounds = f.dir / "sounds";
    std::filesystem::create_directories(sounds);
    auto tone = [&](const char* name, int seconds) {
        audio::AudioData audio;
        audio.sampleRate = 8000;
        audio.channels.assign(1,
                              std::vector<float>(static_cast<std::size_t>(8000 * seconds), 0.25f));
        REQUIRE(audio::writeWav(sounds / name, audio));
        return sounds / name;
    };
    // A project: one track, one audio file on it, saved.
    auto project = [&](const std::filesystem::path& audio, const char* file) {
        json::Value add = json::Value::object();
        add["track"] =
            exec("track.add", json::parse(R"({"name":"Keys"})").value())["track"].asString();
        add["audio_file"] = audio.string();
        add["position"] = 0.0;
        (void)exec("item.add", add);
        REQUIRE(f.app->save(f.dir / file));
    };
    project(tone("kick.wav", 2), "one.json");
    REQUIRE(f.app->newProject({}));
    project(tone("snare.wav", 5), "two.json");
    REQUIRE(f.app->open(f.dir / "one.json"));

    gui::PluginDrag drag(*f.root, *f.dispatcher);
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    browser.show("media");
    f.root->frame(false);
    auto names = [&] {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < browser.mediaCount(); ++i) {
            const auto row = browser.mediaRow(i);
            out.push_back(row.name + "|" + row.detail);
        }
        return out;
    };
    auto pass = [&] {  // the loop's
        browser.sync();
        f.root->frame(false);
    };
    pass();
    CHECK(names() == std::vector<std::string>{"kick.wav|WAV \u00b7 0:02"});
    // Its row is chosen.
    auto* list = dynamic_cast<ui::List*>(f.root->find("side.browser.media.results"));
    REQUIRE(list);
    const ui::Node* add = f.root->find("side.browser.media.add");
    REQUIRE(add);
    const ui::Point row{list->bounds().x + 20, list->bounds().y + 0.5f * list->rowHeight};
    f.root->pointer({ui::PointerEvent::Type::Down, row, 1, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, row, 1, 1});
    f.root->frame(false);
    REQUIRE(list->selected());
    CHECK(add->enabled);
    const std::string chosenId = f.app->head().media.begin()->id.str();

    // The other project, opened with the page in view, at the same version.
    const auto version = f.app->head().version;
    REQUIRE(f.app->open(f.dir / "two.json"));
    REQUIRE(f.app->head().version == version);
    pass();
    CHECK(names() == std::vector<std::string>{"snare.wav|WAV \u00b7 0:05"});
    // Its source has the id the first project's had, and is another file:
    // what was chosen there is not chosen here.
    REQUIRE(f.app->head().media.begin()->id.str() == chosenId);
    CHECK_FALSE(list->selected());
    CHECK_FALSE(add->enabled);
    // And an empty one.
    REQUIRE(f.app->newProject({}));
    pass();
    CHECK(browser.mediaCount() == 0);
}

// Play, beside Add on the Files and Media pages: the chosen audio file is
// heard before it is added (audition.play, ADR-0094), and what ends it.
TEST_CASE("the side panel's browser plays the chosen audio file until something ends it") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* command, const json::Value& args) {
        auto r = f.run(command, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    // A folder with a folder, two short audio files and a MIDI file.
    const std::filesystem::path sounds = f.dir / "sounds";
    std::filesystem::create_directories(sounds / "Loops");
    auto tone = [&](const char* name, float level) {
        audio::AudioData audio;
        audio.sampleRate = 8000;
        audio.channels.assign(1, std::vector<float>(400, level));  // 50 ms
        REQUIRE(audio::writeWav(sounds / name, audio));
        return sounds / name;
    };
    const auto kick = tone("kick.wav", 0.25f);
    const auto snare = tone("snare.wav", 0.5f);
    std::ofstream(sounds / "tune.mid") << "MThd";

    gui::PluginDrag drag(*f.root, *f.dispatcher);
    gui::SideBrowser browser(*f.root, *f.app, *f.dispatcher, f.ui, *f.fx, drag);
    std::vector<std::filesystem::path> added;
    browser.files.add = [&](const std::filesystem::path& file) -> Status {
        added.push_back(file);
        return {};
    };
    gui::SidePanel side(*f.root);
    side.addTab("browser", "Browser", browser.build(), [&](bool on) { browser.setVisible(on); });
    f.root->find("window")->add(side.build());
    side.show("browser");
    browser.show("files");
    REQUIRE(browser.openFolder(sounds));
    f.root->frame(false);
    // Folders first, then the files by name: Loops, kick.wav, snare.wav, tune.mid.
    REQUIRE(browser.fileCount() == 4);
    REQUIRE(browser.fileRow(1).name == "kick.wav");
    REQUIRE(browser.fileRow(3).name == "tune.mid");
    auto text = [&](const char* id) { return f.root->find(id)->valueText(); };
    auto choose = [&](const char* results, std::size_t row) {
        auto* list = dynamic_cast<ui::List*>(f.root->find(results));
        REQUIRE(list);
        const ui::Point at{list->bounds().x + 20,
                           list->bounds().y + (static_cast<float>(row) + 0.5f) * list->rowHeight};
        f.root->pointer({ui::PointerEvent::Type::Down, at, 1, 1});
        f.root->pointer({ui::PointerEvent::Type::Up, at, 1, 1});
        f.root->frame(false);
    };
    auto pass = [&] {  // the loop's
        f.app->tick();
        browser.sync();
        f.root->frame(false);
    };
    const char* const files = "side.browser.files.results";
    const char* const play = "side.browser.files.play";
    const char* const status = "side.browser.files.status";
    const ui::Node* button = f.root->find(play);
    REQUIRE(button);

    // Play waits for an audio file: no row, a folder and a MIDI file leave
    // it off, though Add is on for the last two.
    CHECK(text(play) == "Play");
    CHECK_FALSE(button->enabled);
    choose(files, 0);
    CHECK_FALSE(button->enabled);
    choose(files, 3);
    CHECK_FALSE(button->enabled);
    CHECK(f.root->find("side.browser.files.add")->enabled);
    CHECK_FALSE(f.root->click(play));
    choose(files, 1);
    CHECK(button->enabled);

    // With no engine running the press says so, and nothing is claimed.
    REQUIRE(f.root->click(play));
    CHECK(text(status) == "the engine is not running");
    CHECK(text(play) == "Play");
    CHECK(browser.auditioned().empty());

    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    struct StopEngine {
        engine::EngineApp& app;
        ~StopEngine() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                               {.anticipative = false}));
    // The file is read off this thread: waited for by its state, not the clock.
    auto read = [&] {
        while (f.app->auditionStatus().decoding) {
            f.app->tick();
            std::this_thread::yield();
        }
        pass();
    };
    auto press = [&](const char* id) {
        REQUIRE(f.root->click(id));
        f.root->frame(false);
    };

    // A press runs audition.play with the chosen file: the button reads Stop
    // and the line says what plays, first that it is being read.
    press(play);
    CHECK(browser.auditioned() == kick);
    CHECK(text(play) == "Stop");
    CHECK(text(status) == "Reading kick.wav…");
    CHECK(f.app->auditionStatus().playing);
    CHECK(std::filesystem::path(f.app->auditionStatus().path) == kick);
    read();
    CHECK(text(status) == "Playing kick.wav");
    CHECK(text(play) == "Stop");
    f.snapshot("side-browser-files-playing.png");
    // The file's end gives the button back: 50 ms is ten cycles of 256 at 48 kHz.
    for (int cycle = 0; f.app->auditionStatus().playing; ++cycle) {
        REQUIRE(cycle < 64);
        backend.run(1);
        f.app->tick();
    }
    CHECK(text(play) == "Stop");  // until the loop's next pass
    pass();
    CHECK(text(play) == "Play");
    CHECK(text(status) == "");
    CHECK(browser.auditioned().empty());
    CHECK(button->enabled);
    f.snapshot("side-browser-files-play.png");

    // A second press stops it.
    press(play);
    read();
    CHECK(f.app->auditionStatus().playing);
    press(play);
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(text(play) == "Play");
    CHECK(text(status) == "");

    // Another row chosen stops it; the row it was played from does not.
    press(play);
    choose(files, 1);
    CHECK(f.app->auditionStatus().playing);
    CHECK(text(play) == "Stop");
    choose(files, 2);
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(text(play) == "Play");
    CHECK(button->enabled);  // the row now chosen can be played
    // A MIDI row chosen stops it too, and is not offered: a press there
    // reaches no command, with the engine running as without it.
    press(play);
    choose(files, 3);
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(text(play) == "Play");
    CHECK_FALSE(button->enabled);
    CHECK_FALSE(f.root->click(play));
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(browser.auditioned().empty());
    CHECK(text(status) == "");
    choose(files, 2);
    CHECK(button->enabled);
    // Add stops it: the file is in the project, no longer only heard.
    press(play);
    CHECK(browser.auditioned() == snare);
    press("side.browser.files.add");
    CHECK_FALSE(f.app->auditionStatus().playing);
    REQUIRE(added.size() == 1);
    CHECK(added[0] == snare);
    CHECK(text(status) == "Added snare.wav");
    CHECK(text(play) == "Play");
    // A search that chooses another file stops it.
    press(play);
    browser.focusSearch();
    f.root->text("kick");
    f.root->frame(false);
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(text(play) == "Play");
    // The same file found by the search goes on playing.
    press(play);
    CHECK(browser.auditioned() == kick);
    browser.focusSearch();  // the press took the keyboard
    f.root->key(SDLK_BACKSPACE, 0);
    f.root->frame(false);
    CHECK(browser.fileCount() == 1);  // "kic" still finds it
    CHECK(f.app->auditionStatus().playing);
    for (int i = 0; i < 3; ++i) f.root->key(SDLK_BACKSPACE, 0);  // no search: no row chosen
    f.root->frame(false);
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK_FALSE(button->enabled);

    // Another kind shown stops it, and so does the Browser going out of view.
    choose(files, 1);
    press(play);
    browser.show("media");
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(text(play) == "Play");
    browser.show("files");
    f.root->frame(false);
    choose(files, 1);
    press(play);
    side.hide();
    CHECK_FALSE(f.app->auditionStatus().playing);
    CHECK(browser.auditioned().empty());
    side.show("browser");
    f.root->frame(false);
    CHECK(text(play) == "Play");

    // What something else plays in its place is not this page's: the button
    // reads Play again at the next pass, and the other file is left playing.
    choose(files, 1);
    press(play);
    json::Value other = json::Value::object();
    other["path"] = snare.string();
    (void)exec("audition.play", other);
    pass();
    CHECK(text(play) == "Play");
    CHECK(browser.auditioned().empty());
    choose(files, 2);
    choose(files, 1);
    CHECK(f.app->auditionStatus().playing);
    CHECK(std::filesystem::path(f.app->auditionStatus().path) == snare);
    (void)exec("audition.stop", json::Value::object());
    // The same when a row is chosen before the loop's next pass: the page
    // gives up its own, and does not stop the other.
    press(play);
    CHECK(browser.auditioned() == kick);
    (void)exec("audition.play", other);
    choose(files, 2);
    CHECK(text(play) == "Play");
    CHECK(f.app->auditionStatus().playing);
    CHECK(std::filesystem::path(f.app->auditionStatus().path) == snare);
    (void)exec("audition.stop", json::Value::object());
    choose(files, 1);

    // A file the engine could not read after all (its audition.error event)
    // is said, when it is the one played from here.
    press(play);
    json::Value failed = json::Value::object();
    failed["path"] = snare.string();
    failed["code"] = "audio.format";
    failed["message"] = "snare.wav: not audio";
    browser.auditionFailed(failed);
    CHECK(text(play) == "Stop");
    failed["path"] = kick.string();
    failed["message"] = "kick.wav: its data cannot be read";
    browser.auditionFailed(failed);
    CHECK(text(play) == "Play");
    CHECK(text(status) == "kick.wav: its data cannot be read");
    (void)exec("audition.stop", json::Value::object());

    // The Media page has the same button for the project's own audio.
    const std::string track =
        exec("track.add", json::parse(R"({"name":"Keys"})").value())["track"].asString();
    auto import = [&](const std::filesystem::path& file, double position) {
        json::Value add = json::Value::object();
        add["track"] = track;
        add["audio_file"] = file.string();
        add["position"] = position;
        (void)exec("item.add", add);
    };
    import(kick, 0);
    import(snare, 16);
    browser.show("media");
    pass();
    const char* const media = "side.browser.media.results";
    const char* const mediaPlay = "side.browser.media.play";
    REQUIRE(browser.mediaCount() == 2);
    CHECK_FALSE(f.root->find(mediaPlay)->enabled);
    choose(media, 1);
    CHECK(f.root->find(mediaPlay)->enabled);
    press(mediaPlay);
    CHECK(browser.auditioned() == snare);
    CHECK(text(mediaPlay) == "Stop");
    read();
    CHECK(text("side.browser.media.status") == "Playing snare.wav");
    f.snapshot("side-browser-media-playing.png");
    // The project changing elsewhere, and a source listed before the chosen
    // one, leave it playing: the same source is still chosen.
    (void)exec("track.add", json::parse(R"({"name":"Other"})").value());
    pass();
    CHECK(text(mediaPlay) == "Stop");
    import(tone("alpha.wav", 0.125f), 32);
    pass();
    REQUIRE(browser.mediaCount() == 3);
    CHECK(browser.mediaRow(2).name == "snare.wav");
    CHECK(text(mediaPlay) == "Stop");
    CHECK(f.app->auditionStatus().playing);
    // Another project opened ends it in the engine; the button follows.
    (void)exec("project.new", json::Value::object());
    CHECK_FALSE(f.app->auditionStatus().playing);
    pass();
    CHECK(text(mediaPlay) == "Play");
    CHECK(browser.auditioned().empty());

    // A file longer than the cap: the line says how much of it is heard.
    CHECK(gui::SideBrowser::playingText("long.mp3", 600, true, true) ==
          "Reading the first 10:00 of long.mp3…");
    CHECK(gui::SideBrowser::playingText("long.mp3", 600, true, false) ==
          "Playing the first 10:00 of long.mp3");
}

TEST_CASE("the plugin browser finds a stock plugin by what it is") {
    Fixture f;
    if (!f.ok) return;
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.snapshot("plugin-browser.png");
    f.type("reverb");
    const auto& shown = f.fx->browser().shown();
    const auto ghyll = std::find(shown.begin(), shown.end(), "com.suppleaudio.ghyll");
    REQUIRE(ghyll != shown.end());
    CHECK(f.fx->browser().rowText(static_cast<std::size_t>(ghyll - shown.begin())).name ==
          "Ghyll (reverb)");
    f.key("Escape");
}

TEST_CASE("the FX chain badges each effect's state, and a failed one offers a restart") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Hickory");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 1);
    const model::Id fx = f.fx->chain()[0];
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process (default)");
    f.click("fx.offline." + fx.str());
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process (default) · offline");
    f.click("fx.offline." + fx.str());  // back on
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process (default)");
    // The isolation modes and a failed instance compose the same badge.
    model::FxInstance inst;
    inst.isolation = model::Isolation::Dedicated;
    CHECK(gui::FxWindow::badgeText(inst, model::Isolation::Dedicated, false) == "isolated");
    inst.isolation = model::Isolation::SharedBridge;
    inst.offline = true;
    CHECK(gui::FxWindow::badgeText(inst, model::Isolation::SharedBridge, true) ==
          "shared bridge · offline · failed");
    // On inherit: where the preference put it (ADR-0081), marked as the default.
    inst.isolation = model::Isolation::Inherit;
    inst.offline = false;
    CHECK(gui::FxWindow::badgeText(inst, model::Isolation::SharedBridge, false) ==
          "shared bridge (default)");
    CHECK(gui::FxWindow::badgeText(inst, model::Isolation::InProcess, false) ==
          "in-process (default)");
    // A healthy engine reports no failure: no Restart button, and tick() keeps
    // the set empty.
    f.fx->tick();
    CHECK(f.fx->failed().empty());
    CHECK(f.uiRoot().find("fx.restart." + fx.str()) == nullptr);
}

TEST_CASE("a disabled plug-in is marked in the FX chain, with Enable on its row") {
    Fixture f;
    if (!f.ok) return;
    std::vector<model::Id> enabled;
    gui::FxWindow::Hooks hooks;
    hooks.enableFx = [&](model::Id fx) { enabled.push_back(fx); };
    f.fx =
        std::make_unique<gui::FxWindow>(*f.app, *f.dispatcher, *f.root, *f.panel, std::move(hooks),
                                        gui::Favourites::defaultFile(gui::test::slug()));
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Hickory");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 1);
    const std::string id = f.fx->chain()[0].str();
    CHECK(f.uiRoot().find("fx.enable." + id) == nullptr);
    CHECK(f.uiRoot().find("fx.badge." + id)->valueText() == "in-process (default)");
    REQUIRE(
        f.run("plugin.override", f.args({{"plugin", f.pluginId("Hickory")}, {"mode", "disable"}})));
    f.fx->tick();  // the window follows the override with no change to the project
    f.frame();
    REQUIRE(f.uiRoot().find("fx.enable." + id));
    CHECK(
        f.uiRoot().find("fx.badge." + id)->valueText().find("disabled after an unexpected close") ==
        0);
    CHECK(f.uiRoot().find("fx.badge." + id)->valueText().find("audio passes through") !=
          std::string::npos);
    f.snapshot("fx-disabled.png");
    // It is not loaded, so there is no editor to open: the button is off, and a double click on
    // the row says why in the status line.
    CHECK_FALSE(f.uiRoot().find("fx.editor." + id)->enabled);
    // Everything that acts on a plug-in that is not loaded is off; Enable and Remove are not.
    for (const char* button : {"bypass", "offline", "params", "presets"})
        CHECK_FALSE(f.uiRoot().find(std::string("fx.") + button + "." + id)->enabled);
    CHECK(f.uiRoot().find("fx.enable." + id)->enabled);
    CHECK(f.uiRoot().find("fx.remove." + id)->enabled);
    CHECK(f.uiRoot().find("fx.badge." + id)->valueText().find("disabled") == 0);
    {
        const ui::Rect r = f.uiRoot().find("fx.row." + id)->bounds();
        const ui::Point at{r.x + 6, r.y + 8};
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, at, 1, 2, 0});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, at, 1, 2, 0});
        f.frame();
        CHECK(f.uiRoot().find("fx.status")->valueText().find("not loaded") != std::string::npos);
    }
    REQUIRE(f.click("fx.enable." + id));
    REQUIRE(enabled.size() == 1);
    CHECK(enabled[0].str() == id);
    // Loaded again: the marker goes with no other change.
    REQUIRE(
        f.run("plugin.override", f.args({{"plugin", f.pluginId("Hickory")}, {"mode", "normal"}})));
    f.fx->tick();
    f.frame();
    CHECK(f.uiRoot().find("fx.enable." + id) == nullptr);
    CHECK(f.uiRoot().find("fx.badge." + id)->valueText() == "in-process (default)");
}

TEST_CASE("a failed effect links to the report its bridge's death left") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> shown;
    gui::FxWindow::Hooks hooks;
    hooks.showCrashReport = [&](const std::string& incident) { shown.push_back(incident); };
    f.fx =
        std::make_unique<gui::FxWindow>(*f.app, *f.dispatcher, *f.root, *f.panel, std::move(hooks),
                                        gui::Favourites::defaultFile(gui::test::slug()));
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Hickory");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 1);
    const std::string id = f.fx->chain()[0].str();
    auto statusWith = [&](const char* incident) {
        engine::EngineStatus status;
        status.failedFx = {id};
        auto failure = json::Value::object();
        failure["fx"] = id;
        failure["reason"] = "the bridge ended";
        if (*incident) failure["incident"] = incident;
        status.failedErrors.asArray().push_back(std::move(failure));
        return status;
    };
    // A failure that left no report offers a restart and no link.
    f.fx->applyFailures(statusWith(""));
    f.frame();
    CHECK(f.uiRoot().find("fx.restart." + id));
    CHECK(f.uiRoot().find("fx.report." + id) == nullptr);
    CHECK(f.fx->incidentOf(id).empty());
    // One that did gets "Report", which opens the viewer on that incident.
    f.fx->applyFailures(statusWith("0123456789abcdef0123456789abcdef"));
    f.frame();
    REQUIRE(f.uiRoot().find("fx.report." + id));
    CHECK(f.uiRoot().find("fx.report." + id)->valueText() == "Show report");
    // Next to Restart on the first line; the mode stays quiet and only "failed" is red.
    CHECK(f.uiRoot().find("fx.report." + id)->parent() ==
          f.uiRoot().find("fx.restart." + id)->parent());
    CHECK(f.uiRoot().find("fx.badge." + id)->valueText() == "in-process (default) \u00b7 failed");
    const auto* badge = dynamic_cast<ui::RichText*>(f.uiRoot().find("fx.badge." + id));
    REQUIRE(badge);
    REQUIRE(badge->runs().size() == 2);
    CHECK(badge->runs()[0].colorToken == "ink-quiet");
    CHECK(badge->runs()[1].colorToken == "meter-clip");
    f.snapshot("fx-failed-report.png");
    CHECK(f.fx->incidentOf(id) == "0123456789abcdef0123456789abcdef");
    REQUIRE(f.click("fx.report." + id));
    REQUIRE(shown.size() == 1);
    CHECK(shown[0] == "0123456789abcdef0123456789abcdef");
    f.fx->applyFailures(engine::EngineStatus{});  // restarted: both go
    f.frame();
    CHECK(f.uiRoot().find("fx.restart." + id) == nullptr);
    CHECK(f.uiRoot().find("fx.report." + id) == nullptr);
}

TEST_CASE(
    "the FX chain's pins choose a plugin's channels on a strip with aux channels (ADR-0020)") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Hickory");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 1);
    const model::Id fx = f.fx->chain()[0];
    // A stereo strip: no pins button.
    CHECK(f.uiRoot().find("fx.pins." + fx.str()) == nullptr);
    REQUIRE(f.run("track.set", f.args({{"track", track.str()}, {"channels", 4}})));
    f.fx->refresh();
    f.frame();
    REQUIRE(f.uiRoot().find("fx.pins." + fx.str()));
    CHECK(f.uiRoot().find("fx.pins." + fx.str())->valueText() == "pins 1/2");
    // Rows: In 1/2, In 3/4, rule, Out 1/2, Out 3/4, rule, Key (routes), Key 1/2, Key 3/4.
    auto pick = [&](int row) {
        REQUIRE(f.click("fx.pins." + fx.str()));
        ui::Node* menu = f.uiRoot().find("fx.pins.menu");
        REQUIRE(menu);
        const ui::Point p{menu->bounds().x + 30, menu->bounds().y + row * 26.0f + 13};
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, p});
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 1});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 1});
        f.frame();
    };
    pick(1);
    CHECK(f.app->head().fx.find(fx)->pins.in == std::vector<int>{3, 4});
    CHECK(f.uiRoot().find("fx.pins." + fx.str())->valueText() == "3/4 → 1/2");
    pick(0);  // back to 1/2: stored as the default
    CHECK(f.app->head().fx.find(fx)->pins.empty());
    CHECK(gui::FxWindow::pinsText({{3, 4}, {1, 2}, {3, 4}}) == "3/4 → 1/2 · key 3/4");
}

TEST_CASE("the FX chain's MIDI out chooses replace or merge for a plug-in that sends notes") {
    Fixture f(true);
    if (!f.ok) return;
    const model::Id track = f.addTrack("Arp");
    REQUIRE(f.run("fx.add",
                  f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.note-echo"}})));
    REQUIRE(
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.sine"}})));
    REQUIRE(f.fx->open(track));
    f.frame();
    const auto& chain = f.app->head().tracks.find(track)->chain;
    REQUIRE(chain.size() == 2);
    const model::Id echo = chain[0], sine = chain[1];
    REQUIRE(f.app->head().fx.find(echo)->plugin.id == "com.suppleaudio.test.note-echo");
    // Only the plug-in that sends notes has the control.
    CHECK(f.uiRoot().find("fx.midiout." + sine.str()) == nullptr);
    REQUIRE(f.uiRoot().find("fx.midiout." + echo.str()));
    CHECK(f.uiRoot().find("fx.midiout." + echo.str())->valueText() == "MIDI out: replace");
    auto pick = [&](int row) {
        REQUIRE(f.click("fx.midiout." + echo.str()));
        ui::Node* menu = f.uiRoot().find("fx.midiout.menu");
        REQUIRE(menu);
        std::vector<std::string> checked;
        for (const auto& item : ui::menuItems(*menu))
            if (item.checked) checked.push_back(item.label);
        CHECK(checked.size() == 1);
        f.snapshot("fx-midi-out.png");
        const ui::Point p{menu->bounds().x + 30, menu->bounds().y + row * 26.0f + 13};
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, p});
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 1});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 1});
        f.frame();
    };
    pick(1);
    CHECK(f.app->head().fx.find(echo)->midiOutput == model::MidiOutputMode::Merge);
    CHECK(f.uiRoot().find("fx.midiout." + echo.str())->valueText() == "MIDI out: merge");
    pick(0);
    CHECK(f.app->head().fx.find(echo)->midiOutput == model::MidiOutputMode::Replace);
    // The chain's MIDI mode (the track's chain_midi), beside the Chain heading.
    REQUIRE(f.uiRoot().find("fx.chain.midi"));
    CHECK(f.uiRoot().find("fx.chain.midi")->valueText() == "MIDI: through the chain");
    auto chainMode = [&](int row) {
        REQUIRE(f.click("fx.chain.midi"));
        ui::Node* menu = f.uiRoot().find("fx.chainmidi.menu");
        REQUIRE(menu);
        f.snapshot("fx-chain-midi.png");
        const ui::Point p{menu->bounds().x + 30, menu->bounds().y + row * 26.0f + 13};
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, p});
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, p, 1});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, p, 1});
        f.frame();
    };
    chainMode(1);
    CHECK(f.app->head().tracks.find(track)->chainMidi == model::ChainMidi::Instrument);
    CHECK(f.uiRoot().find("fx.chain.midi")->valueText() == "MIDI: instrument only");
    chainMode(0);
    CHECK(f.app->head().tracks.find(track)->chainMidi == model::ChainMidi::Bus);
    // A single plug-in, or plug-ins that neither send notes nor two of
    // which take them: nothing to choose.
    REQUIRE(f.run("fx.remove", f.args({{"fx", echo.str()}})));
    f.fx->refresh();
    f.frame();
    CHECK(f.uiRoot().find("fx.chain.midi") == nullptr);
    REQUIRE(
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.gain"}})));
    f.fx->refresh();
    f.frame();
    CHECK(f.uiRoot().find("fx.chain.midi") == nullptr);
    // A second instrument after the first: both hear notes on the bus.
    REQUIRE(
        f.run("fx.add", f.args({{"track", track.str()}, {"plugin", "com.suppleaudio.test.sine"}})));
    f.fx->refresh();
    f.frame();
    CHECK(f.uiRoot().find("fx.chain.midi") != nullptr);
}

TEST_CASE("the render dialog: the preview matches render.dry-run and a job lists its file") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));

    f.render->open();
    f.frame();
    // The controls show what the engine would use (render.settings).
    const auto shown = [&](const char* id) {
        auto* node = f.uiRoot().find(id);
        REQUIRE(node);
        return node->valueText();
    };
    CHECK(shown("render.range") == "Entire project");
    CHECK(shown("render.format") == "WAV");
    CHECK(shown("render.bit_depth") == "32-bit float");
    CHECK(shown("render.channels") == "Stereo");
    CHECK(shown("render.tail.text") == "0");
    CHECK(shown("render.normalize") == "Off");
    auto* rate = dynamic_cast<ui::TextField*>(f.uiRoot().find("render.sample_rate.text"));
    REQUIRE(rate);
    CHECK(rate->placeholder() == "Project (48000)");
    f.render->setField("render.end", "8");  // a short render: fast and load-insensitive
    f.frame();
    CHECK(shown("render.range") == "Custom");
    f.render->setField("render.pattern", "preview-{counter:02}");
    f.render->refreshPreview();

    // The preview matches render.dry-run for the same arguments.
    auto dry = f.run(
        "render.dry-run",
        f.args({{"operation", "mix"}, {"pattern", "preview-{counter:02}"}, {"end_beat", 8.0}}));
    REQUIRE(dry);
    std::string expected;
    for (const json::Value& file : (*dry)["files"].asArray()) {
        const std::string path = file.find("path")->asString();
        expected +=
            (expected.empty() ? "" : "\n") + std::filesystem::path(path).filename().string();
    }
    REQUIRE_FALSE(expected.empty());
    CHECK(f.render->preview().find(expected.substr(0, expected.find('\n'))) == 0);

    // Editing the pattern updates the preview.
    const std::string before = f.render->preview();
    f.render->setField("render.pattern", "other-{counter:02}");
    f.render->refreshPreview();
    CHECK(f.render->preview() != before);
    CHECK(f.render->preview().find("other-01.wav") != std::string::npos);

    // Stems preview over two tracks names each track's file.
    f.render->setSource(gui::RenderDialog::Source::Stems);
    f.render->setField("render.pattern", "stem-{track_number:02}");
    f.render->refreshPreview();
    CHECK(f.render->preview().find("stem-01.wav") != std::string::npos);

    // A render job completes and lists its file.
    f.render->setSource(gui::RenderDialog::Source::Master);
    f.render->setField("render.pattern", "job-{counter:02}");
    f.render->refreshPreview();
    f.render->run();
    REQUIRE_FALSE(f.render->job().empty());
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.render->resultFiles().size() == 1);
    CHECK(std::filesystem::exists(f.render->resultFiles()[0]));
    CHECK(f.render->statusText().find("1 file") != std::string::npos);
    // The result list carries the analysis summary when the render has one.
    CHECK(f.uiRoot().find("render.results")->valueText().find("LUFS") != std::string::npos);

    // The render wrote the dialog's settings to the project first.
    CHECK(f.app->head().project->render.find("pattern"));
    // add_to_project: the file lands as an item on a new track (one undo step).
    const std::size_t tracksBefore = f.trackCount();
    const std::size_t historyBefore = f.historyCount();
    CHECK_FALSE(f.render->addToProject());
    f.render->setAddToProject(true);  // the checkbox (below the fold of the scroll view)
    f.frame();
    CHECK(f.render->addToProject());
    CHECK(f.uiRoot().find("render.add")->valueText() == "on");
    f.render->setField("render.pattern", "added-{counter:02}");
    f.render->refreshPreview();
    f.render->run();
    REQUIRE_FALSE(f.render->job().empty());
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    f.dispatcher->flushJobEvents();  // the follow-up runs on the model thread
    REQUIRE(f.render->resultFiles().size() == 1);
    CHECK(f.trackCount() == tracksBefore + 1);
    CHECK(f.sourcePathOf("added-01") == f.render->resultFiles()[0]);
    // Two steps: "Render settings" (the changed pattern and add_to_project)
    // before the job, and the import after it.
    CHECK(f.historyCount() == historyBefore + 2);
    CHECK(f.app->head().project->render.find("add_to_project")->asBool());
}

TEST_CASE("the render dialog: FLAC, a second file, and the render queue") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));
    f.render->open();
    f.render->setField("render.end", "4");
    f.render->setField("render.pattern", "q-{counter:02}");
    f.render->setField("render.format", "flac");
    f.render->setField("render.bit_depth", "24");
    f.render->setField("render.secondary", "wav 16");
    f.render->refreshPreview();
    CHECK(f.render->preview().find("q-01.flac") != std::string::npos);  // the extension follows
    // Queue it, change the pattern, queue again: two pinned renders.
    f.click("render.queue.add");
    CHECK(f.app->renderQueue().size() == 1);
    CHECK(f.uiRoot().find("render.queue")->valueText() == "Queue (1)");
    f.render->setField("render.pattern", "r-{counter:02}");
    f.render->setField("render.secondary", "");
    f.click("render.queue.add");
    REQUIRE(f.app->renderQueue().size() == 2);
    // The queue panel lists both; Render all runs them as one job.
    f.click("render.queue");
    f.frame();
    REQUIRE(f.uiRoot().find("render.queue.list"));
    CHECK(f.uiRoot().find("render.queue.list")->valueText().find("q-01.flac") != std::string::npos);
    f.click("render.queue.run");
    REQUIRE_FALSE(f.render->job().empty());
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    f.dispatcher->flushJobEvents();  // finished renders leave the queue on the model thread
    REQUIRE(f.render->resultFiles().size() == 2);
    CHECK(f.render->resultFiles()[0].ends_with("q-01.flac"));
    CHECK(f.render->resultFiles()[1].ends_with("r-01.flac"));
    CHECK(std::filesystem::exists(f.app->rendersDir() / "q-01.wav"));  // its second file
    CHECK_FALSE(std::filesystem::exists(f.app->rendersDir() / "r-01.wav"));
    CHECK(f.app->renderQueue().empty());
    f.render->tick();
    CHECK(f.uiRoot().find("render.queue")->valueText() == "Queue");
}

TEST_CASE("FX chain preset buttons save, append and Shift-replace through the bus") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) {
        MESSAGE("skipped: no GUI or stock plugins");
        return;
    }
    const auto source = f.addTrack("Source");
    const auto target = f.addTrack("Target");
    REQUIRE(f.run("fx.add", f.args({{"track", source.str()},
                                    {"plugin", f.pluginId("Hickory")},
                                    {"patch", json::parse(R"({"params":{"gain":-6}})").value()}})));
    REQUIRE(f.fx->open(source));
    f.frame();
    const auto beforeSave = f.historyCount();
    REQUIRE(f.click("fx.chain.save"));
    REQUIRE(f.uiRoot().focused()->id() == "fx.chain.name");
    f.type("Guitar");
    f.key("Return");
    CHECK(f.historyCount() == beforeSave);
    CHECK(f.fxStatus() == "Saved Guitar");
    auto listed = f.run("fx.chain.list", json::Value::object());
    REQUIRE(listed);
    CHECK((*listed)["chains"].size() == 1);
    REQUIRE(f.fx->open(target));
    const auto beforeLoad = f.historyCount();
    REQUIRE(f.click("fx.chain.load"));
    REQUIRE(f.uiRoot().find("fx.chain.menu"));
    f.key("Home");
    f.key("Return");
    CHECK(f.historyCount() == beforeLoad + 1);
    REQUIRE(f.fx->chain().size() == 1);
    const auto loaded = f.fx->chain()[0];
    CHECK(f.app->head().fx.find(loaded)->plugin.id == f.pluginId("Hickory"));
    auto patch = f.run("fx.patch.get", f.args({{"fx", loaded.str()}}));
    auto original = f.run("fx.patch.get", f.args({{"fx", "fx_1"}}));
    REQUIRE(patch);
    REQUIRE(original);
    CHECK(*patch == *original);
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 2);
    const auto beforeReplace = f.historyCount();
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Shift+Return");
    CHECK(f.historyCount() == beforeReplace + 1);
    CHECK(f.fx->chain().size() == 1);
    REQUIRE(f.run("history.undo", json::Value::object()));
    f.fx->refresh();
    CHECK(f.fx->chain().size() == 2);
    // The same controls can target the input chain without changing main FX.
    REQUIRE(f.click("fx.chain.target"));
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Return");
    CHECK(f.app->head().tracks.find(target)->inputChain.size() == 1);
    CHECK(f.app->head().tracks.find(target)->chain.size() == 2);
    REQUIRE(f.click("fx.chain.save"));
    f.type("Input");
    f.key("Return");
    CHECK(f.fxStatus() == "Saved Input");
}

TEST_CASE("FX chain preset buttons toggle their popups; a right click deletes a preset") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) {
        MESSAGE("skipped: no GUI or stock plugins");
        return;
    }
    const auto track = f.addTrack("Source");
    REQUIRE(f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Hickory")}})));
    REQUIRE(f.fx->open(track));
    for (const char* name : {"Keep", "Drop"}) {
        REQUIRE(f.run("fx.chain.save", f.args({{"track", track.str()}, {"name", name}})));
    }
    // Save chain… shows its name field; pressed again, hides it.
    REQUIRE(f.click("fx.chain.save"));
    CHECK(f.uiRoot().find("fx.chain.prompt")->visible);
    REQUIRE(f.click("fx.chain.save"));
    CHECK_FALSE(f.uiRoot().find("fx.chain.prompt")->visible);
    // Load chain… opens one menu; pressed again, closes it (never a second).
    REQUIRE(f.click("fx.chain.load"));
    REQUIRE(f.uiRoot().find("fx.chain.menu"));
    REQUIRE(f.click("fx.chain.load"));
    CHECK_FALSE(f.uiRoot().find("fx.chain.menu"));
    CHECK(f.uiRoot().find("fx"));  // the FX window itself stays
    REQUIRE(f.click("fx.chain.load"));
    REQUIRE(f.click("fx.chain.load"));
    REQUIRE(f.click("fx.chain.load"));
    CHECK(f.uiRoot().find("fx.chain.menu"));
    // Sorted by name: Drop first. A right click offers to delete it.
    const auto before = f.historyCount();
    f.pressFirstRow("fx.chain.menu", 3);
    REQUIRE(f.click("fx.chain.menu.context"));  // its one item, Delete
    CHECK_FALSE(std::filesystem::exists(f.dir / "chains" / "Drop.json"));
    CHECK(std::filesystem::exists(f.dir / "chains" / "Keep.json"));
    CHECK(f.historyCount() == before);
    CHECK(f.fxStatus() == "Deleted Drop");
    auto* menu = dynamic_cast<ui::List*>(f.uiRoot().find("fx.chain.menu"));
    REQUIRE(menu);
    CHECK(menu->count() == 1);
    // Deleting the last one closes the menu.
    f.pressFirstRow("fx.chain.menu", 3);
    REQUIRE(f.click("fx.chain.menu.context"));
    CHECK_FALSE(f.uiRoot().find("fx.chain.menu"));
    CHECK_FALSE(std::filesystem::exists(f.dir / "chains" / "Keep.json"));
}

TEST_CASE("the render dialog shows the renders folder and follows a new choice") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no GUI");
        return;
    }
    int asked = 0;
    f.render->onChooseFolder = [&] {
        ++asked;
        f.app->setRendersDir(f.dir / "elsewhere");
        f.render->showFolder();
    };
    f.render->open();
    f.frame();
    CHECK(f.render->folderText().ends_with("/renders"));
    REQUIRE(f.click("render.folder.choose"));
    CHECK(asked == 1);
    CHECK(f.render->folderText().ends_with("/elsewhere"));
    auto dry = f.run("render.dry-run", f.args({{"operation", "mix"}, {"end_beat", 4.0}}));
    REQUIRE(dry);
    REQUIRE((*dry)["files"].size() == 1);
    CHECK(std::filesystem::path((*dry)["files"].asArray()[0].find("path")->asString())
              .parent_path() == f.dir / "elsewhere");
}

TEST_CASE("the FX window shows a take's chain (take FX): add, reorder, no input chain") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) {
        MESSAGE("skipped: no GUI or stock plugins");
        return;
    }
    const auto track = f.addTrack("Synth");
    auto item = f.run(
        "item.add",
        f.args(
            {{"track", track.str()}, {"position", 0.0}, {"length", 4.0}, {"notation", "1: C4:w"}}));
    REQUIRE(item);
    const auto take = *model::Id::parse((*item)["take"].asString());
    REQUIRE(f.run("fx.add", f.args({{"take", take.str()}, {"plugin", f.pluginId("Hickory")}})));
    REQUIRE(f.run("fx.add", f.args({{"take", take.str()}, {"plugin", f.pluginId("Yarrow")}})));
    REQUIRE(f.fx->openTake(take));
    f.frame();
    REQUIRE(f.fx->take());
    CHECK(f.uiRoot().find("fx.title")->valueText().rfind("Take FX: Synth", 0) == 0);
    CHECK(f.fx->chain().size() == 2);
    CHECK(f.fx->inputChain().empty());
    CHECK(f.uiRoot().find("fx.add.input") == nullptr);  // a take has no input chain
    // Preset buttons are for track chains.
    f.click("fx.chain.save");
    CHECK(f.fxStatus() == "Chain presets are for track chains");
    // Opening a track's chain leaves take mode.
    REQUIRE(f.fx->open(track));
    CHECK_FALSE(f.fx->take());
    CHECK(f.fx->chain().empty());
}

TEST_CASE("FX latency badge follows an effect edit and undo without reopening") {
    Fixture f;
    REQUIRE(f.ok);
    if (!f.stockPlugins()) return;
    const auto track = f.addTrack("Latency");
    REQUIRE(f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Sundew")}})));
    REQUIRE(f.fx->open(track));
    f.frame();
    const auto effect = f.fx->chain().front();
    auto text = [&] {
        auto* label = f.uiRoot().find("fx.latency." + effect.str());
        REQUIRE(label);
        return label->valueText();
    };
    CHECK(text() == "0 spl");
    json::Value values = json::Value::object();
    values["lookahead"] = "ms2";
    REQUIRE(f.run("fx.param.set", f.args({{"fx", effect.str()}, {"values", values}})));
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    struct StopEngine {
        engine::EngineApp& app;
        ~StopEngine() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                               {.anticipative = false}));

    f.fx->tick();
    f.frame();
    CHECK(text() == "96 spl");
    REQUIRE(f.run("history.undo", json::Value::object()));
    REQUIRE(f.run("fx.restart", f.args({{"fx", effect.str()}})));
    f.fx->tick();
    f.frame();
    CHECK(text() == "0 spl");
}

TEST_CASE("render cancellation retains partial files and labels them cancelled") {
    Fixture f;
    REQUIRE(f.ok);
    // Script the bus responses so completion ordering is deterministic.
    auto response = [&](const char* name, json::Value result) {
        cmd::Command<json::Value, json::Value> command;
        command.name = name;
        command.version = 2;
        command.kind = cmd::Kind::Query;
        command.exposure = cmd::Exposure::Internal;
        command.args = schema::any();
        command.result = schema::any();
        command.run = [result](const json::Value&, cmd::Context&) -> Result<json::Value> {
            return result;
        };
        f.registry.add(std::move(command));
    };
    response("render.mix", json::parse(R"({"job":"test-job"})").value());
    response(
        "job.get",
        json::parse(
            R"({"status":"cancelled","result":{"files":[{"path":"partial.wav","seconds":1}]}})")
            .value());
    f.render->open();
    f.render->run();
    f.render->tick();
    CHECK(f.render->job().empty());
    CHECK(f.render->statusText().starts_with("cancelled"));
    CHECK(f.render->resultFiles() == std::vector<std::string>{"partial.wav"});
}

TEST_CASE("audit3: a reopened render dialog shows the stored settings, not unsaved edits") {
    Fixture f;
    REQUIRE(f.ok);
    f.render->open();
    f.render->setAddToProject(true);
    f.render->close();  // nothing rendered: nothing written
    CHECK(f.app->head().project->render.find("add_to_project") == nullptr);
    f.render->open();
    CHECK_FALSE(f.render->addToProject());
    f.render->close();
    REQUIRE(f.run("project.set",
                  f.args({{"render", json::parse(R"({"add_to_project":true})").value()}})));
    f.render->open();
    CHECK(f.render->addToProject());
    CHECK(f.uiRoot().find("render.add")->valueText() == "on");
}
TEST_CASE("audit3: active render refuses additional launches") {
    Fixture f;
    REQUIRE(f.ok);
    int starts = 0;
    for (const char* name : {"render.mix", "render.queue.run"}) {
        cmd::Command<json::Value, json::Value> c;
        c.name = name;
        c.version = 2;
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::Internal;
        c.args = schema::any();
        c.result = schema::any();
        c.run = [&](const json::Value&, cmd::Context&) -> Result<json::Value> {
            auto result = json::Value::object();
            result["job"] = "probe-" + std::to_string(++starts);
            return result;
        };
        f.registry.add(std::move(c));
    }
    f.render->open();
    SUBCASE("render job") { f.render->run(); }
    SUBCASE("queue job") { f.render->runQueue(); }
    CHECK(f.render->job() == "probe-1");
    f.render->run();
    CHECK(f.render->job() == "probe-1");
    CHECK(starts == 1);
    f.render->runQueue();
    CHECK(f.render->job() == "probe-1");
    CHECK(starts == 1);
    CHECK(f.uiRoot().find("render.status")->valueText().find("active") != std::string::npos);
}

TEST_CASE("render preview has one deadline only while an open dialog is dirty") {
    Fixture f;
    if (!f.ok) return;
    CHECK_FALSE(f.render->nextDeadline());
    f.render->open();
    CHECK_FALSE(f.render->nextDeadline());
    f.render->setField("render.pattern", "next-{counter:02}");
    REQUIRE(f.render->nextDeadline());
    const auto deadline = *f.render->nextDeadline();
    f.render->setDebounceMs(0);
    CHECK(*f.render->nextDeadline() == deadline - std::chrono::milliseconds(40));
    f.render->tick();
    CHECK_FALSE(f.render->nextDeadline());
    f.render->setField("render.pattern", "later-{counter:02}");
    REQUIRE(f.render->nextDeadline());
    f.render->close();
    CHECK_FALSE(f.render->nextDeadline());
}

TEST_CASE("an effect's presets menu loads factory presets and saves and deletes the user's") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) {
        MESSAGE("skipped: no GUI or stock plugins");
        return;
    }
    const auto track = f.addTrack("Source");
    REQUIRE(f.run("fx.add", f.args({{"track", track.str()}, {"plugin", f.pluginId("Hickory")}})));
    REQUIRE(f.fx->open(track));
    auto factory = f.run("fx.patch.list", f.args({{"fx", "fx_1"}}));
    REQUIRE(factory);
    const std::size_t factoryCount = (*factory)["patches"].size();
    REQUIRE(factoryCount > 0);
    // Rows: "Factory", its presets, ["Saved", the user's,] a rule, "Save preset…".
    auto row = [&](std::size_t i, float x = 30) {
        auto* menu = f.uiRoot().find("fx.preset.menu");
        REQUIRE(menu);
        const auto& b = menu->bounds();
        return ui::Point{x < 0 ? b.right() + x : b.x + x, b.y + 26 * static_cast<float>(i) + 13};
    };
    auto press = [&](ui::Point at, int button = 1) {
        f.frame();
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, at, 0});
        f.uiRoot().pointer({ui::PointerEvent::Type::Down, at, button});
        f.uiRoot().pointer({ui::PointerEvent::Type::Up, at, button});
        f.frame();
    };

    // The button toggles the menu; a click on a factory preset loads it in one step.
    REQUIRE(f.click("fx.presets.fx_1"));
    REQUIRE(f.uiRoot().find("fx.preset.menu"));
    REQUIRE(f.click("fx.presets.fx_1"));
    CHECK_FALSE(f.uiRoot().find("fx.preset.menu"));
    REQUIRE(f.click("fx.presets.fx_1"));
    const auto beforeLoad = f.historyCount();
    press(row(1));
    CHECK_FALSE(f.uiRoot().find("fx.preset.menu"));
    CHECK(f.historyCount() == beforeLoad + 1);
    const std::string first = (*factory)["patches"].at(0).find("name")->asString();
    CHECK(f.fxStatus() == "Loaded " + first);
    // A factory preset has no delete: a right click offers nothing.
    REQUIRE(f.click("fx.presets.fx_1"));
    press(row(1), 3);
    CHECK_FALSE(f.uiRoot().find("fx.preset.menu.context"));
    f.key("Escape");

    // "Save preset…" shows the name field; a taken name needs a second Save.
    REQUIRE(f.run("fx.param.set", f.args({{"fx", "fx_1"}, {"values", f.args({{"gain", -6}})}})));
    if (!f.uiRoot().find("fx.preset.menu")) REQUIRE(f.click("fx.presets.fx_1"));
    press(row(factoryCount + 2));
    CHECK(f.uiRoot().find("fx.preset.prompt")->visible);
    REQUIRE(f.uiRoot().focused()->id() == "fx.preset.name");
    const auto beforeSave = f.historyCount();
    f.type("Mine");
    f.key("Return");
    CHECK(f.fxStatus() == "Saved Mine");
    CHECK_FALSE(f.uiRoot().find("fx.preset.prompt")->visible);
    CHECK(f.historyCount() == beforeSave);
    REQUIRE(f.click("fx.presets.fx_1"));
    press(row(factoryCount + 4));  // past the Saved heading and Mine
    f.type("Mine");
    f.snapshot("fx-presets-save.png");
    f.key("Return");
    CHECK(f.fxStatus() == "“Mine” exists: Save again to replace it");
    f.key("Return");
    CHECK(f.fxStatus() == "Saved Mine");
    auto listed = f.run("fx.preset.list", f.args({{"fx", "fx_1"}}));
    REQUIRE(listed);
    REQUIRE((*listed)["presets"].size() == factoryCount + 1);
    const std::string file = (*listed)["presets"].at(factoryCount).find("path")->asString();
    CHECK(file.starts_with((f.dir / "presets").string()));

    // Loading it back; then its delete mark asks, and Delete removes the file.
    REQUIRE(f.run("fx.param.set", f.args({{"fx", "fx_1"}, {"values", f.args({{"gain", 0}})}})));
    REQUIRE(f.click("fx.presets.fx_1"));
    press(row(factoryCount + 2));
    CHECK(f.fxStatus() == "Loaded Mine");
    auto gain = f.run("fx.param.get", f.args({{"fx", "fx_1"}, {"param", "gain"}}));
    REQUIRE(gain);
    CHECK((*gain)["value"].asDouble() == doctest::Approx(-6));
    REQUIRE(f.click("fx.presets.fx_1"));
    f.uiRoot().pointer({ui::PointerEvent::Type::Move, row(factoryCount + 2, -19), 0});
    f.snapshot("fx-presets.png");
    press(row(factoryCount + 2, -19));
    REQUIRE(f.click("fx.preset.menu.context"));
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(f.fxStatus() == "Deleted Mine");
    auto* menu = dynamic_cast<ui::List*>(f.uiRoot().find("fx.preset.menu"));
    REQUIRE(menu);
    CHECK(menu->count() == factoryCount + 3);  // the Saved heading went with it
}

TEST_CASE("double clicking empty FX chain space adds to its main, input or take chain") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) return;
    const auto track = f.addTrack("Empty chain");
    REQUIRE(f.fx->open(track));
    auto doubleClick = [&](ui::Point p, int clicks = 2, int button = 1) {
        f.panel->root().pointer({ui::PointerEvent::Type::Down, p, button, clicks, 0});
        f.panel->root().pointer({ui::PointerEvent::Type::Up, p, button, clicks, 0});
        f.frame();
    };
    f.frame();
    const auto b = f.panel->root().find("fx.empty")->bounds();
    const ui::Point empty{b.x + 20, b.y + b.h / 2};
    doubleClick(empty, 1);
    CHECK_FALSE(f.fx->browser().isOpen());
    doubleClick(empty, 2, 3);
    CHECK_FALSE(f.fx->browser().isOpen());
    doubleClick(empty);
    REQUIRE(f.fx->browser().isOpen());
    CHECK(f.panel->root().find("browser"));
    CHECK_FALSE(f.root->find("browser"));
    f.fx->browser().onAdd()(f.pluginId("Hickory"));
    f.fx->browser().close();
    CHECK(f.fx->chain().size() == 1);
    f.frame();
    const auto list = f.panel->root().find("fx.body")->bounds();
    doubleClick({list.x + 20, list.bottom() - 20});
    REQUIRE(f.fx->browser().isOpen());  // below the final effect row
    f.fx->browser().close();
    f.frame();
    const auto input = f.panel->root().find("ifx.empty")->bounds();
    doubleClick({input.x + 20, input.y + input.h / 2});
    REQUIRE(f.fx->browser().isOpen());
    f.fx->browser().onAdd()(f.pluginId("Hickory"));
    f.fx->browser().close();
    CHECK(f.fx->inputChain().size() == 1);
    auto made = f.run(
        "item.add",
        f.args(
            {{"track", track.str()}, {"position", 0.0}, {"length", 4.0}, {"notation", "1: C4:w"}}));
    REQUIRE(made);
    const auto take = *model::Id::parse((*made)["take"].asString());
    REQUIRE(f.fx->openTake(take));
    f.frame();
    const auto takeEmpty = f.panel->root().find("fx.empty")->bounds();
    doubleClick({takeEmpty.x + 20, takeEmpty.y + takeEmpty.h / 2});
    REQUIRE(f.fx->browser().isOpen());
    f.fx->browser().onAdd()(f.pluginId("Hickory"));
    f.fx->browser().close();
    CHECK(f.fx->chain().size() == 1);
    CHECK(f.app->head().tracks.find(track)->chain.size() == 1);
    CHECK(f.app->head().tracks.find(track)->inputChain.size() == 1);
    gui::UiState ui;
    REQUIRE(f.fx->openToAdd(ui, track.str()));
    CHECK(f.root->find("browser"));  // the strip's standalone browser stays on main
    CHECK_FALSE(f.panel->root().find("browser"));
}

TEST_CASE("FX header arrows and menu switch among the strips in mixer order") {
    Fixture f;
    if (!f.ok) return;
    const auto first = f.addTrack("First");
    const auto added = f.run("track.add", f.args({{"type", "buss"}, {"name", "Bus"}}));
    REQUIRE(added);
    const auto bus = *model::Id::parse(added->find("track")->asString());
    const auto second = f.addTrack("Second");
    const model::Id master = f.app->head().project->masters.front();
    REQUIRE(f.fx->open(first));
    REQUIRE(f.click("fx.tab.params"));
    // Channels, then busses, then masters, wrapping at both ends.
    for (model::Id want : {second, bus, master, first}) {
        REQUIRE(f.click("fx.track.next"));
        CHECK(f.fx->track() == want);
    }
    REQUIRE(f.click("fx.track.prev"));
    CHECK(f.fx->track() == master);
    CHECK(f.panel->root().find("fx.params.chooser"));  // still on Parameters
    REQUIRE(f.click("fx.title"));
    const auto* menu = f.panel->root().find("fx.track.menu");
    REQUIRE(menu);
    f.snapshot("fx-track-menu.png");
    std::vector<std::string> rows;
    for (const auto& item : ui::menuItems(*menu))
        rows.push_back(item.separator ? "-" : item.label + (item.checked ? "*" : ""));
    CHECK(rows == std::vector<std::string>{"First", "Second", "-", "Bus", "-", "Master*"});
    const auto pick = ui::menuItems(*menu)[1].run;  // the menu goes as it runs
    pick();
    CHECK(f.fx->track() == second);
    CHECK_FALSE(f.panel->root().popupOpen());
}

TEST_CASE("FX native title follows track switches, renames and take headers") {
    Fixture f;
    if (!f.ok) return;
    const auto first = f.addTrack("First"), second = f.addTrack("Second");
    REQUIRE(f.fx->open(first));
    const auto window = f.panel->window()->id();
    auto title = [&] { return std::string(SDL_GetWindowTitle(f.panel->window()->sdl())); };
    auto matchesHeader = [&] { CHECK(title() == f.panel->root().find("fx.title")->valueText()); };
    CHECK(title() == "FX: First");
    matchesHeader();
    REQUIRE(f.fx->open(second));
    CHECK(f.panel->window()->id() == window);
    CHECK(title() == "FX: Second");
    matchesHeader();
    REQUIRE(f.run("track.set", f.args({{"track", second.str()}, {"name", "Renamed"}})));
    f.fx->tick();
    CHECK(title() == "FX: Renamed");
    matchesHeader();
    const auto item = f.run("item.add", f.args({{"track", second.str()},
                                                {"position", 0.0},
                                                {"length", 4.0},
                                                {"notation", "1: C4:w"}}));
    REQUIRE(item);
    const auto take = *model::Id::parse(item->find("take")->asString());
    REQUIRE(f.fx->openTake(take));
    CHECK(title() == "Take FX: Renamed / " + take.str());
    matchesHeader();
    REQUIRE(f.run("track.set", f.args({{"track", second.str()}, {"name", "Take track"}})));
    f.fx->tick();
    CHECK(title() == "Take FX: Take track / " + take.str());
    matchesHeader();
}

TEST_CASE("FX project replacement closes overlays and clears pending prompts") {
    Fixture f;
    if (!f.ok) return;
    const auto track = f.addTrack("Old project");
    REQUIRE(f.fx->open(track));
    REQUIRE(f.click("fx.chain.save"));
    f.type("Old chain");
    REQUIRE(f.click("fx.add"));
    REQUIRE(f.panel->root().find("browser"));
    bool applied = false;
    gui::LearnRequest request;
    request.title = "Old parameter";
    request.onOk = [&](const gui::LearnChoice&) { applied = true; };
    gui::openLearnDialog(f.panel->root(), *f.actions, *f.app, std::move(request));
    REQUIRE(f.panel->root().find("learn"));
    REQUIRE(f.app->learning());
    REQUIRE(f.run("project.new", json::Value::object()));
    CHECK(f.addTrack("Replacement") == track);  // the old captured id is reused
    f.fx->syncProject();                        // before another event, without waiting for a frame
    CHECK(f.fx->isOpen());
    CHECK_FALSE(f.panel->root().popupOpen());
    CHECK_FALSE(f.panel->root().find("browser"));
    CHECK_FALSE(f.panel->root().find("learn"));
    CHECK_FALSE(f.panel->root().find("fx.chain.prompt")->visible);
    CHECK_FALSE(f.panel->root().find("fx.preset.prompt")->visible);
    CHECK(f.panel->root().find("fx.chain.name")->valueText().empty());
    CHECK_FALSE(f.fx->browser().onAdd());
    CHECK_FALSE(f.app->learning());
    CHECK_FALSE(f.actions->learnSink);
    CHECK_FALSE(applied);
}

TEST_CASE("simultaneous plugin browsers preserve each other's favourite toggles") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) return;
    REQUIRE(f.panel->ensure());
    const auto file = gui::Favourites::defaultFile(gui::test::slug());
    gui::PluginBrowser first(*f.app, *f.dispatcher, *f.root, file);
    gui::PluginBrowser second(*f.app, *f.dispatcher, f.panel->root(), file);
    first.open([](const std::string&) {});
    second.open([](const std::string&) {});
    auto toggle = [&](ui::Root& root, const char* name) {
        root.frame(false);
        root.focus(root.find("browser.search"));
        root.key(SDLK_A, ui::kCtrl);
        root.text(name);
        root.frame(false);
        REQUIRE(root.click("browser.fav"));
    };
    const auto a = f.pluginId("Hickory"), b = f.pluginId("Yarrow");
    toggle(*f.root, "Hickory");
    toggle(f.panel->root(), "Yarrow");
    auto saved = gui::Favourites::load(file);
    REQUIRE(saved);
    CHECK(saved->has(a));
    CHECK(saved->has(b));
    toggle(*f.root, "Hickory");  // remove A through its older browser cache
    saved = gui::Favourites::load(file);
    REQUIRE(saved);
    CHECK_FALSE(saved->has(a));
    CHECK(saved->has(b));
    first.close();
    second.close();
}

TEST_CASE("render dialog settings: written once when changed; presets; File → Mix as a job") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));
    auto wait = [&] {
        for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
            f.render->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        f.dispatcher->flushJobEvents();
    };
    f.render->open();
    f.render->setField("render.end", "2");
    f.render->setField("render.pattern", "once-{counter:02}");
    const std::size_t before = f.historyCount();
    f.render->run();
    wait();
    CHECK(f.historyCount() == before + 1);  // "Render settings"
    f.render->run();                        // nothing changed: nothing written
    wait();
    CHECK(f.historyCount() == before + 1);
    // A preset loads into the dialog only; Render writes it.
    REQUIRE(f.render->loadPreset("WAV 24-bit"));
    json::Value shown = f.render->settings();
    CHECK(shown["outputs"].asArray()[0]["bit_depth"].asInt() == 24);
    CHECK(f.app->head().project->render.find("outputs") == nullptr);
    // Save as default: render.settings then shows it for what the project leaves out.
    REQUIRE(f.render->saveAsDefault());
    auto settings = f.run("render.settings", json::Value::object());
    REQUIRE(settings);
    CHECK((*settings)["default"]["outputs"].asArray()[0]["bit_depth"].asInt() == 24);
    f.render->close();
    // File → Mix runs as a job and reports the file it wrote.
    std::string reported;
    f.render->onStatus = [&](const std::string& message) { reported = message; };
    f.render->mixJob();
    REQUIRE_FALSE(f.render->job().empty());
    CHECK(f.render->nextDeadline());  // polled while closed
    wait();
    CHECK(reported.find("Mixed") == 0);
    CHECK(reported.find("once-01.wav") != std::string::npos);
}

TEST_CASE("file URLs: POSIX, drive and UNC paths, with reserved characters encoded") {
    CHECK(gui::fileUrl("/home/me/Mixes #2/a b?%") == "file:///home/me/Mixes%20%232/a%20b%3F%25");
    CHECK(gui::fileUrl("C:/Users/Me/Renders") == "file:///C:/Users/Me/Renders");
    CHECK(gui::fileUrl("//server/share/Mixes") == "file://server/share/Mixes");
    CHECK(gui::fileUrl("/r\xC3\xA9sum\xC3\xA9") == "file:///r%C3%A9sum%C3%A9");
}

TEST_CASE("render dialog: regions checklist, the matrix grid and the chain rate reach the render") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));
    REQUIRE(f.run("region.add", f.args({{"start", 0.0}, {"end", 2.0}, {"name", "One"}})));
    REQUIRE(f.run("region.add", f.args({{"start", 2.0}, {"end", 4.0}, {"name", "Two"}})));
    f.render->open();
    f.render->setSource(gui::RenderDialog::Source::Regions);
    f.frame();
    // The checked regions are rendered (a one-off selector).
    CHECK_FALSE(f.render->renderArguments().find("regions"));  // all of them
    REQUIRE(f.click("render.regions.list.none"));
    REQUIRE(f.click("render.regions.list.item.0"));
    json::Value args = f.render->renderArguments();
    REQUIRE(args.find("regions"));
    CHECK(args["regions"].size() == 1);
    CHECK(f.app->head().project->render.find("regions") == nullptr);
    // A cell of the matrix grid becomes a matrix member of the settings.
    f.render->openMatrix();
    f.frame();
    REQUIRE(f.click("render.matrix.rg_1.tr_1"));
    args = f.render->renderArguments();
    REQUIRE(args.find("matrix"));
    CHECK(args["matrix"]["rg_1"].asArray()[0] == "tr_1");
    f.click("render.matrix.close");
    f.render->setSource(gui::RenderDialog::Source::Master);
    // A new sample rate gives a file at that rate (outputs follow the chain).
    f.render->setField("render.end", "2");
    f.render->setField("render.sample_rate", "44100");
    f.render->setField("render.pattern", "rate-{counter:02}");
    f.render->run();
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.render->resultFiles().size() == 1);
    auto audio = audio::readAudio(f.render->resultFiles()[0]);
    REQUIRE(audio);
    CHECK(audio->sampleRate == 44100);
}

TEST_CASE("render dialog: with several masters the mix lists them, a file each (ADR-0056)") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));
    f.render->open();
    f.frame();
    CHECK(f.root->find("render.masters") == nullptr);  // one master: nothing to choose
    f.render->close();
    REQUIRE(f.run("track.add", f.args({{"name", "Cue"}, {"type", "master"}})));
    f.render->open();
    f.render->setSource(gui::RenderDialog::Source::Master);
    f.frame();
    REQUIRE(f.root->find("render.masters") != nullptr);
    CHECK_FALSE(f.render->renderArguments().find("masters"));  // the main master
    REQUIRE(f.click("render.masters.all"));
    const json::Value args = f.render->renderArguments();
    REQUIRE(args.find("masters"));
    CHECK(args.find("masters")->size() == 2);
    f.render->setField("render.end", "1");
    f.render->refreshPreview();
    CHECK(f.render->preview().find("Cue") != std::string::npos);  // its own file
}

TEST_CASE("render dialog: Time selection renders the ruler's loop range, looping on or off") {
    Fixture f;
    if (!f.ok) return;
    gui::test::linkGeneratedMedia(f.app->mediaDir());
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));
    const auto range = [&]() -> ui::Dropdown& {
        auto* d = dynamic_cast<ui::Dropdown*>(f.root->find("render.range"));
        REQUIRE(d);
        return *d;
    };
    f.render->open();
    f.frame();
    CHECK(range().choices().size() == 3);       // no separate Loop choice
    CHECK_FALSE(range().choices()[1].enabled);  // no time selection yet
    f.render->close();
    REQUIRE(f.run("transport.loop",
                  f.args({{"start_seconds", 1.0}, {"end_seconds", 3.0}, {"enabled", false}})));
    f.render->open();
    f.frame();
    CHECK(range().choices()[1].enabled);
    range().choose("loop");
    f.frame();
    CHECK(range().label() == "Time selection");
    CHECK(f.render->renderArguments()["bounds"].asString() == "loop");
    json::Value args = f.render->renderArguments();
    args["operation"] = "mix";
    auto dry = f.run("render.dry-run", std::move(args));
    REQUIRE(dry);
    CHECK((*dry)["files"].asArray()[0].find("seconds")->asDouble() == doctest::Approx(2.0));
}

TEST_CASE(
    "a channel fed by a rack shows a Rack device: its Macro knobs, named after what they drive") {
    Fixture f;
    if (!f.ok) return;
    std::vector<model::Id> opened;
    f.fx = std::make_unique<gui::FxWindow>(
        *f.app, *f.dispatcher, *f.root, *f.panel,
        gui::FxWindow::Hooks{.openRack = [&](model::Id rack) { opened.push_back(rack); }},
        gui::Favourites::defaultFile(gui::test::slug()));
    auto exec = [&](const char* command, const std::string& args) {
        auto r = f.run(command, json::parse(args).value());
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    json::Value made = exec("rack.add", R"({"name":"Lead"})");
    const std::string rack = made["rack"].asString(), track = made["track"].asString();
    auto add = [&](const char* type, const char* label) {
        return exec("module.add", R"({"rack":")" + rack + R"(","type":")" + type +
                                      R"(","label":")" + label + R"("})")["module"]
            .asString();
    };
    const std::string filter = add("filter", "Filter"), amp = add("vca", "Amp");
    const std::string macro = add("macro", "Performance");
    add("macro", "Spare");  // nothing cabled: not shown
    exec("cable.add",
         R"({"from":")" + macro + R"(.out_1","to":")" + filter + R"(.cutoff","amount":24})");
    exec("cable.add", R"({"from":")" + macro + R"(.out_2","to":")" + amp + R"(.cv"})");
    exec("cable.add",
         R"({"from":")" + macro + R"(.out_2","to":")" + filter + R"(.resonance","amount":0.3})");
    exec("module.set", R"({"module":")" + macro + R"(","params":{"macro_2":40}})");
    REQUIRE(f.fx->open(*model::Id::parse(track)));
    f.frame();
    auto devices = f.fx->rackDevices();
    REQUIRE(devices.size() == 1);
    gui::RackDevice& device = *devices.front();
    auto shown = device.shown();
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].group == "Performance");
    CHECK(shown[0].name == "Filter cutoff");
    CHECK(shown[0].key == "macro_1");
    CHECK(shown[0].value == "0 %");
    CHECK(shown[1].name == "Amp CV +1");  // a name in capitals stays
    CHECK(shown[1].value == "40 %");
    f.snapshot("fx-rack-device.png");
    // Open: the rack window on that rack.
    REQUIRE(f.click("fx.rack.open." + rack));
    CHECK(opened == std::vector<model::Id>{*model::Id::parse(rack)});
    // So does a double-click anywhere else on the device (here its name),
    // as on a plug-in's row; a single press opens nothing.
    const ui::Point name{device.bounds().x + 8, device.bounds().y + 8};
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, name, 1, 1});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, name, 1, 1});
    CHECK(opened.size() == 1);
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, name, 1, 2});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, name, 1, 2});
    CHECK(opened.size() == 2);
    CHECK(opened.back() == *model::Id::parse(rack));
    // A drag turns module.set, one undo step.
    const auto at = device.knobAt("Filter cutoff");
    REQUIRE(at);
    const auto history = f.historyCount();
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, *at, 1});
    for (int i = 1; i <= 5; ++i) {
        f.uiRoot().pointer({ui::PointerEvent::Type::Move, {at->x, at->y - 12.0f * i}, 1});
        f.fx->tick();
        f.uiRoot().frame(false);
    }
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, {at->x, at->y - 60}, 1});
    f.fx->tick();
    f.frame();
    CHECK(f.historyCount() == history + 1);
    const auto* m = f.app->head().modules.find(*model::Id::parse(macro));
    REQUIRE(m);
    const json::Value* v = m->params.find("macro_1");
    REQUIRE(v);
    CHECK(v->asDouble() > 20);
    // Its right-click: what a rack knob offers.
    devices = f.fx->rackDevices();
    REQUIRE(devices.size() == 1);
    const auto knob = devices.front()->knobAt("Filter cutoff");
    REQUIRE(knob);
    f.uiRoot().pointer({ui::PointerEvent::Type::Down, *knob, 3});
    f.uiRoot().pointer({ui::PointerEvent::Type::Up, *knob, 3});
    f.frame();
    const ui::Node* menu =
        f.root->popupOpen() ? f.root->find("fx.rack.menu") : f.panel->root().find("fx.rack.menu");
    REQUIRE(menu);
    std::vector<std::string> labels;
    for (const auto& item : ui::menuItems(*menu)) labels.push_back(item.label);
    CHECK(std::find(labels.begin(), labels.end(), "Show lane") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "MIDI learn…") != labels.end());
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Show lane") {
            auto run = item.run;
            f.panel->root().closePopups();
            run();
            break;  // closing the popup freed the menu and its items
        }
    bool automated = false;
    for (const auto& e : f.app->head().envelopes)
        automated = automated || (e.value->target.object == *model::Id::parse(macro) &&
                                  e.value->target.param == "macro_1");
    CHECK(automated);
}

TEST_CASE("a Rack device without Macro modules shows its name and Open; knobs follow playback") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* command, const std::string& args) {
        auto r = f.run(command, json::parse(args).value());
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    json::Value made = exec("rack.add", R"({"name":"Plain"})");
    const std::string rack = made["rack"].asString(), track = made["track"].asString();
    const std::string vco =
        exec("module.add", R"({"rack":")" + rack + R"(","type":"vco"})")["module"].asString();
    REQUIRE(f.fx->open(*model::Id::parse(track)));
    f.frame();
    REQUIRE(f.fx->rackDevices().size() == 1);
    CHECK(f.fx->rackDevices().front()->shown().empty());
    CHECK(f.uiRoot().find("fx.rack.open." + rack));
    CHECK_FALSE(f.fx->nextDeadline());  // nothing to follow
    f.snapshot("fx-rack-plain.png");
    // A Macro whose first knob is automated from 0 to 100 % over 4 beats.
    const std::string macro =
        exec("module.add", R"({"rack":")" + rack + R"(","type":"macro"})")["module"].asString();
    exec("cable.add", R"({"from":")" + macro + R"(.out_1","to":")" + vco + R"(.exp_fm"})");
    exec("env.add",
         R"({"module":")" + macro +
             R"(","param":"macro_1","points":[{"beat":0,"value":0},{"beat":4,"value":100}]})");
    f.fx->tick();
    f.frame();
    REQUIRE(f.fx->rackDevices().size() == 1);
    REQUIRE(f.fx->rackDevices().front()->shown().size() == 1);
    CHECK(f.fx->nextDeadline());
    engine::DummyBackend backend(
        engine::DummyBackend::Options{.mode = engine::DummyBackend::Mode::Manual});
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 512},
                               {.anticipative = false}));
    f.app->engine()->play();
    backend.run(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    f.fx->tick();  // the running Macro is watched from here
    // Two beats at 120 bpm: a second.
    for (int i = 0; i < 93; ++i) backend.run(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    f.fx->tick();
    const auto shown = f.fx->rackDevices().front()->shown();
    REQUIRE(shown.size() == 1);
    const double value = std::stod(shown[0].value);
    CHECK(value > 30);
    CHECK(value < 70);
    f.snapshot("fx-rack-playing.png");
    f.app->stopEngine();
}
