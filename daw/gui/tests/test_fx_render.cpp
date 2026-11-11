// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The FX chain window, the plugin browser and the parameter panel, and the
// render dialog: driven by keys and clicks through the bus (gui.md §10).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>
#include "generated_media.h"
#include "product_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include "test_support.h"

#include "app_actions.h"
#include "fx_window.h"
#include "generate.h"
#include "learn_dialog.h"
#include "plugin_browser.h"
#include "render_dialog.h"
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
    CHECK(browser.detail().find(dup.string()) != std::string::npos);
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
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process");
    f.click("fx.offline." + fx.str());
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process · offline");
    f.click("fx.offline." + fx.str());  // back on
    CHECK(f.uiRoot().find("fx.badge." + fx.str())->valueText() == "in-process");
    // The isolation modes and a failed instance compose the same badge.
    model::FxInstance inst;
    inst.isolation = model::Isolation::Dedicated;
    CHECK(gui::FxWindow::badgeText(inst, false) == "isolated");
    inst.isolation = model::Isolation::SharedBridge;
    inst.offline = true;
    CHECK(gui::FxWindow::badgeText(inst, true) == "shared bridge · offline · failed");
    // A healthy engine reports no failure: no Restart button, and tick() keeps
    // the set empty.
    f.fx->tick();
    CHECK(f.fx->failed().empty());
    CHECK(f.uiRoot().find("fx.restart." + fx.str()) == nullptr);
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
    CHECK(std::find(labels.begin(), labels.end(), "Automate") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "MIDI learn…") != labels.end());
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Automate") {
            auto run = item.run;
            f.panel->root().closePopups();
            run();
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
