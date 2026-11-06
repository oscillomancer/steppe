// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The rack window (rack.md §8) driven like a user: a patch made through the
// commands, drawn offscreen, moved, zoomed, cabled and turned through the
// root's pointer path, with the session read back.
#include <doctest/doctest.h>

#include "product_fixture.h"
#include "test_support.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <filesystem>

#include "app_actions.h"
#include "floating_panel.h"
#include "plugin_scales.h"
#include "rack_engine.h"
#include "rack_window.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/script_compiler.hpp"
#include "supple/script_module.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_area.h"
#include "supple/ui/widgets.h"
#include "supple/views/track_layout.h"

#include <stb_image.h>

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Ui, "test", {}};

// Two module types of the test's own, compiled by the JIT, so the faces
// show every kind of control whatever the stock modules become.
constexpr const char* kOsc = R"(#include <supple/script.h>
struct Osc : Module {
    static constexpr ModuleInfo info = {.name = "Osc", .id = "com.example.test.osc",
        .description = "A test oscillator.", .tags = {"oscillator"}};
    Input pitch = pitchIn("Pitch").help("1 per octave.");
    Input fm = cvIn("FM").help("Frequency modulation.");
    Input sync = gateIn("Sync").help("Restarts the cycle.");
    Param tune = semitones("Tune", -24, 24, 0).help("Coarse.");
    Param fine = cents("Fine", -100, 100, 12).help("Fine.");
    Param shape = choice("Shape", {"Sine", "Triangle", "Saw", "Square"}, 2).help("The wave.");
    Param level = percent("Level", 80).help("Output level.");
    Output out = audioOut("Out").help("The wave.");
    Output sub = audioOut("Sub").help("An octave down.");
    double phase = 0;
    void process(int frames) {
        for (int i = 0; i < frames; ++i) {
            phase += 220.0 * pow(2.0, pitch[0][i]) / sampleRate();
            if (phase >= 1) phase -= 1;
            out[0][i] = (2 * phase - 1) * level.at(i);
            sub[0][i] = phase < 0.5 ? 0.5 : -0.5;
        }
    }
};
)";

constexpr const char* kScope = R"(#include <supple/script.h>
struct Scope : Module {
    static constexpr ModuleInfo info = {.name = "Scope", .id = "com.example.test.scope",
        .description = "A test display.", .tags = {"utility", "visual"}};
    Input in = audioIn("In").help("What it shows.");
    Param time = milliseconds("Time", 1, 1000, 20).help("The window.");
    Param hold = toggle("Hold", false).help("Freezes it.");
    Output out = audioOut("Thru").help("The input.");
    SharedArray<float, 64> wave;
    int at = 0;
    void process(int frames) {
        for (int i = 0; i < frames; ++i) {
            out[0][i] = in[0][i];
            wave[at] = float(in[0][i]);
            at = (at + 1) % 64;
        }
    }
    void draw(Canvas& g) {
        g.well(10, 32, 130, 64);
        g.levelGrid(10, 32, 130, 64, -1, 1, 0.5);
        g.strokeStyle(Theme::accent2);
        g.lineWidth(1.5);
        g.beginPath();
        for (int k = 0; k < 64; ++k) {
            const double v = wave[k] != 0 ? wave[k] : 0.8 * sin(twoPi * 3 * k / 63.0);
            const double x = 12 + 126 * k / 63.0;
            const double y = 64 - 28 * v;
            if (k == 0) g.moveTo(x, y);
            else g.lineTo(x, y);
        }
        g.stroke();
        g.fillStyle(Theme::textMuted);
        g.font(8);
        g.fillText(format("%.0f ms", time.value()), 14, 92);
    }
    void panel(Panel& p) {
        p.size(150, 196);
        p.display(10, 32, 130, 64);
        p.knob(time, 40, 124, 24);
        p.toggle(hold, 80, 114, 56, 20);
        p.input(in, 40, 168);
        p.output(out, 110, 168);
    }
};
)";

std::shared_ptr<const rack::ModuleType> compiled(const char* source) {
    auto r = scriptc::compile(source);
    INFO(scriptc::toText(r.diagnostics));
    REQUIRE(r.program);
    auto code = scriptc::moduleCode(r.program);
    REQUIRE(code);
    return rack::ModuleType::fromCode(code, rack::Origin::Library, source);
}

struct Fixture {
    std::filesystem::path dir;
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
    std::unique_ptr<gui::RackWindow> racks;
    bool ok = false;

    explicit Fixture(ui::Theme theme = ui::Theme::dark(),
                     std::vector<std::filesystem::path> plugins = {}) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-rack-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"rack-test", 1100, 560, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        if (!plugins.empty()) options.pluginPath = std::move(plugins);
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, std::move(theme));
        root->setContent(std::make_unique<ui::Node>("window"));
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        auto c = std::make_shared<rack::Catalog>(*gui::racks::catalog(*app));
        REQUIRE(c->add(compiled(kOsc)).empty());
        REQUIRE(c->add(compiled(kScope)).empty());
        gui::racks::setCatalog(*app, c);
        panel = std::make_unique<gui::FloatingPanel>(
            *platform, *window, *root, SUPPLE_GUI_FONTS_DIR, "Racks", 1180, 720, 640, 400);
        racks = std::make_unique<gui::RackWindow>(*panel, *actions, *dispatcher, *app, ui);
        ok = true;
    }
    ~Fixture() {
        racks.reset();
        panel.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        INFO(command << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    }
    std::string add(const std::string& rack, const std::string& type, int x, int y,
                    const std::string& extra = "") {
        return exec("module.add", R"({"rack":")" + rack + R"(","type":")" + type + R"(","x":)" +
                                      std::to_string(x) + R"(,"y":)" + std::to_string(y) + extra +
                                      "}")["module"]
            .asString();
    }
    void cable(const std::string& from, const std::string& to, const std::string& extra = "") {
        exec("cable.add", R"({"from":")" + from + R"(","to":")" + to + "\"" + extra + "}");
    }
    ui::Root& r() { return panel->root(); }
    void frame() {
        racks->sync();
        r().frame(false);
    }
    void pointer(ui::PointerEvent::Type type, ui::Point p, int button = 1, unsigned mods = 0,
                 int clicks = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = button;
        e.mods = mods;
        e.clicks = clicks;
        r().pointer(e);
        frame();
    }
    void drag(ui::Point a, ui::Point b, int button = 1, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Move, a, 0, mods);
        pointer(ui::PointerEvent::Type::Down, a, button, mods);
        for (int k = 1; k <= 4; ++k)
            pointer(ui::PointerEvent::Type::Move,
                    {a.x + (b.x - a.x) * static_cast<float>(k) / 4,
                     a.y + (b.y - a.y) * static_cast<float>(k) / 4},
                    0, mods);
        pointer(ui::PointerEvent::Type::Up, b, button, mods);
    }
    // Runs the row of the open menu `menu` whose label starts with `label`.
    bool pick(const std::string& menu, const std::string& label) {
        const ui::Node* m = r().find(menu);
        if (!m) return false;
        for (const auto& item : ui::menuItems(*m))
            if (item.label.rfind(label, 0) == 0 && item.run && item.enabled) {
                auto run = item.run;
                r().closePopups();
                run();
                frame();
                return true;
            }
        return false;
    }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        r().damageAll();
        r().frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, r().readBack());
    }
    // Regenerate with a full run of the suite (SUPPLE_UPDATE_GOLDEN=1).
    void golden(const char* name) {
        r().damageAll();
        r().frame(false);
        const gui::Image image = r().readBack();
#if !defined(__linux__)
        CHECK(image.width > 0);
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
            if (d > 24) ++off;
        }
        stbi_image_free(data);
        // Font rasterization and AA differ slightly between drivers.
        CHECK(off < image.rgba.size() / 4 / 200);
    }
};

// A patch: Notes into an oscillator, through a VCA to the Out; the
// transport's beat phase modulating the oscillator's fine tune; a scope.
struct Patch {
    std::string rack, track, out, notes, osc, vca, transport, scope;
    explicit Patch(Fixture& f) {
        auto r = f.exec("rack.add", R"({"name":"Bells"})");
        rack = r["rack"].asString();
        track = r["track"].asString();
        out = r["out"].asString();
        f.exec("module.set", R"({"module":")" + out + R"(","x":620,"y":80,"label":"Bells"})");
        notes = f.add(rack, "notes", 40, 40);
        osc = f.add(rack, "com.example.test.osc", 220, 40);
        vca = f.add(rack, "vca", 440, 60, R"(,"params":{"level":-6})");
        transport = f.add(rack, "transport", 40, 380);
        scope = f.add(rack, "com.example.test.scope", 620, 300);
        f.cable(notes + ".pitch", osc + ".pitch");
        f.cable(notes + ".gate", vca + ".cv");
        f.cable(osc + ".out", vca + ".in");
        f.cable(vca + ".out", out + ".l");
        f.cable(vca + ".out", scope + ".in");
        f.cable(transport + ".beat_phase", osc + ".fine", R"(,"amount":40)");
    }
};

}  // namespace

TEST_CASE("rack window: empty, then a rack's tab with its Out module on a plain space") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.actions->run("ui.rack"));
    f.frame();
    REQUIRE(f.racks->isOpen());
    CHECK(f.racks->status() == "No racks");
    f.snapshot("rack-none.png");
    // "+" adds a rack on a new channel; its Out module is the channel's input.
    REQUIRE(f.racks->addRack());
    f.frame();
    const model::Session& s = f.app->head();
    REQUIRE(s.racks.size() == 1);
    REQUIRE(f.racks->space()->scene().modules.size() == 1);
    const auto& out = f.racks->space()->scene().modules.front();
    CHECK(out.out);
    CHECK(out.subtitle == model::trackLabel(s, s.racks.begin()->value->track));
    CHECK(f.r().find("rack.tabs")->valueText().front() == '*');
    f.golden("rack-empty.png");
}

TEST_CASE("rack window: a patch draws its faces, cables and modulation") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    const auto& scene = f.racks->space()->scene();
    REQUIRE(scene.modules.size() == 6);
    CHECK(scene.cables.size() == 6);
    const auto* osc = scene.module(*model::Id::parse(p.osc));
    REQUIRE(osc);
    CHECK(osc->title == "Osc");
    CHECK(osc->values[1] == doctest::Approx(12));  // fine's default
    // Every jack and knob is somewhere in the window.
    CHECK(f.racks->space()->jackAt(*model::Id::parse(p.osc), "pitch", false));
    CHECK(f.racks->space()->controlAt(*model::Id::parse(p.osc), "fine"));
    f.golden("rack-patch.png");
    f.racks->space()->setView({200, 20, 1.6});
    f.snapshot("rack-patch-zoom.png");
    f.racks->space()->setView({560, 260, 1.8});
    f.snapshot("rack-scope-zoom.png");
}

TEST_CASE("rack window: the light theme") {
    Fixture f(ui::Theme::light(ui::Palette::Warm));
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    f.golden("rack-patch-light.png");
    // Close up: the faces at 160 %.
    f.racks->space()->setView({200, 20, 1.6});
    f.snapshot("rack-patch-light-zoom.png");
}

TEST_CASE(
    "rack window: a drag moves the selection, one undo step; ctrl+wheel zooms at the pointer") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const model::Id vca = *model::Id::parse(p.vca), osc = *model::Id::parse(p.osc);
    const auto before = *f.app->head().modules.find(vca);
    const ui::Point a = *space.bodyAt(vca);
    const double zoom = space.view().zoom;
    f.drag(a, {a.x + static_cast<float>(30 * zoom), a.y + static_cast<float>(20 * zoom)});
    const auto& after = *f.app->head().modules.find(vca);
    CHECK(after.x == doctest::Approx(before.x + 30).epsilon(0.02));
    CHECK(after.y == doctest::Approx(before.y + 20).epsilon(0.02));
    CHECK(space.selection() == std::set<model::Id>{vca});
    // Ctrl+click adds the oscillator; both move together.
    f.pointer(ui::PointerEvent::Type::Down, *space.bodyAt(osc), 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Up, *space.bodyAt(osc), 1, ui::kCtrl);
    CHECK(space.selection().size() == 2);
    const double oscX = f.app->head().modules.find(osc)->x;
    const ui::Point b = *space.bodyAt(vca);
    f.drag(b, {b.x + static_cast<float>(10 * zoom), b.y});
    CHECK(f.app->head().modules.find(osc)->x == doctest::Approx(oscX + 10).epsilon(0.05));
    f.exec("history.undo", "{}");
    f.frame();
    CHECK(f.app->head().modules.find(osc)->x == doctest::Approx(oscX));
    // Zoom at a point keeps that point of the space under it.
    const ui::Point at{space.bounds().x + 300, space.bounds().y + 200};
    const ui::Point under = space.toSpace(at);
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = at;
    wheel.mods = ui::kCtrl;
    wheel.wheelY = 3;
    f.r().pointer(wheel);
    CHECK(space.view().zoom > zoom);
    CHECK(space.toSpace(at).x == doctest::Approx(static_cast<double>(under.x)).epsilon(0.001));
    CHECK(space.toSpace(at).y == doctest::Approx(static_cast<double>(under.y)).epsilon(0.001));
    for (int k = 0; k < 40; ++k) f.r().pointer(wheel);
    CHECK(space.view().zoom == doctest::Approx(gui::RackSpace::kMaxZoom));
    // Middle-drag pans.
    const auto v = space.view();
    f.drag(at, {at.x - 40, at.y}, 2);
    CHECK(space.view().x == doctest::Approx(v.x + 40 / v.zoom));
}

TEST_CASE("rack window: knobs turn by dragging and reset by double-click; cables are drawn") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const model::Id osc = *model::Id::parse(p.osc), vca = *model::Id::parse(p.vca);
    const ui::Point tune = *space.controlAt(osc, "tune");
    f.drag(tune, {tune.x, tune.y - 40});  // a quarter of the travel up
    const json::Value* t = f.app->head().modules.find(osc)->params.find("tune");
    REQUIRE(t);
    CHECK(t->asDouble() == doctest::Approx(12));
    f.pointer(ui::PointerEvent::Type::Down, tune, 1, 0, 2);
    f.pointer(ui::PointerEvent::Type::Up, tune, 1, 0, 2);
    CHECK(!f.app->head().modules.find(osc)->params.find("tune"));
    // A cable from the oscillator's sub to the VCA's input: it sums with
    // the one there.
    const auto cables = f.app->head().cables.size();
    f.drag(*space.jackAt(osc, "sub", true), *space.jackAt(vca, "in", false));
    CHECK(f.app->head().cables.size() == cables + 1);
    // Picking up the VCA input's last cable and letting go on nothing removes it.
    const ui::Point in = *space.jackAt(vca, "in", false);
    f.drag(in, {in.x + 10, in.y + 120});
    CHECK(f.app->head().cables.size() == cables);
    // A cable dropped on a knob modulates it.
    f.drag(*space.jackAt(osc, "sub", true), *space.controlAt(vca, "level"));
    bool modulates = false;
    for (const auto& e : f.app->head().cables)
        if (e.value->to.object == vca && e.value->to.key == "level")
            modulates = e.value->amount.has_value();
    CHECK(modulates);
}

TEST_CASE("rack window: each rack keeps its view in the UI sidecar") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    f.racks->space()->setView({100, 50, 1.5});
    json::Value saved = f.racks->saveLayout();
    CHECK(saved["rack"].asString() == p.rack);
    CHECK(saved["views"][p.rack]["zoom"].asDouble() == doctest::Approx(1.5));
    f.racks->close();
    f.racks->loadLayout(saved);
    f.racks->open();
    f.frame();
    CHECK(f.racks->space()->view() == gui::RackSpace::View{100, 50, 1.5});
}

TEST_CASE("rack window: a cable that closes a loop is drawn as feedback") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.cable(p.vca + ".out", p.osc + ".fm");  // osc -> vca -> osc
    f.racks->open();
    f.frame();
    int feedback = 0;
    for (const auto& c : f.racks->space()->scene().cables) feedback += c.feedback ? 1 : 0;
    CHECK(feedback == 1);
    f.snapshot("rack-feedback.png");
}

TEST_CASE("rack window: Automate on a knob makes an envelope, a lane on the home channel") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const model::Id vca = *model::Id::parse(p.vca);
    const ui::Point level = *space.controlAt(vca, "level");
    f.pointer(ui::PointerEvent::Type::Down, level, 3);
    f.pointer(ui::PointerEvent::Type::Up, level, 3);
    REQUIRE(f.r().find("rack.param.menu"));
    REQUIRE(f.pick("rack.param.menu", "Automate"));
    const model::Envelope* env = nullptr;
    model::Id envId;
    for (const auto& e : f.app->head().envelopes)
        if (e.value->target.object == vca && e.value->target.param == "level") {
            env = e.value.get();
            envId = e.id;
        }
    REQUIRE(env);
    CHECK(env->visible);
    // It holds the knob's value (-6 dB) from the start.
    REQUIRE(env->points.size() == 1);
    // The arrange shows it as a lane under the rack's home channel, with
    // the knob's own scale.
    views::TrackLayout layout;
    layout.pluginScales = [scales =
                               std::make_shared<gui::PluginScales>(*f.dispatcher, f.app.get())](
                              const model::Envelope& e) { return (*scales)(e); };
    layout.show(f.app->head());
    const auto lane = layout.indexOfEnv(envId);
    REQUIRE(lane);
    const auto home = layout.indexOf(*model::Id::parse(p.track));
    REQUIRE(home);
    CHECK(*lane > *home);
    // The menu now hides (and shows) the lane instead.
    f.pointer(ui::PointerEvent::Type::Down, level, 3);
    f.pointer(ui::PointerEvent::Type::Up, level, 3);
    REQUIRE(f.pick("rack.param.menu", "Hide automation lane"));
    CHECK_FALSE(f.app->head().envelopes.find(envId)->visible);
}

TEST_CASE("rack window: a stock module's source forks into the library, which compiles on save") {
    Fixture f;
    if (!f.ok) return;
    f.racks->open();
    f.frame();
    f.racks->showSource("vca");
    f.frame();
    auto* text = dynamic_cast<ui::TextArea*>(f.r().find("rack.source.text"));
    REQUIRE(text);
    CHECK(text->text().find("struct") != std::string::npos);
    CHECK_FALSE(f.r().find("rack.source.save"));
    f.snapshot("rack-source.png");
    // Make editable copy: com.library.vca, shown with Save.
    REQUIRE(f.r().click("rack.source.fork"));
    f.frame();
    REQUIRE(gui::racks::catalog(*f.app)->find("com.library.vca"));
    REQUIRE(f.r().find("rack.source.save"));
    text = dynamic_cast<ui::TextArea*>(f.r().find("rack.source.text"));
    REQUIRE(text);
    const std::string good = text->text();
    CHECK(good.find("com.library.vca") != std::string::npos);
    // A mistake is marked on its line and said under the code.
    text->setText(good + "\nint broken = ;\n");
    REQUIRE(f.r().click("rack.source.save"));
    f.frame();
    CHECK_FALSE(text->marks().empty());
    auto* note = dynamic_cast<ui::Label*>(f.r().find("rack.source.status"));
    REQUIRE(note);
    CHECK(note->text().rfind("Line ", 0) == 0);
    f.snapshot("rack-source-error.png");
    // Fixed, it compiles.
    text->setText(good);
    REQUIRE(f.r().click("rack.source.save"));
    f.frame();
    CHECK(text->marks().empty());
    CHECK(note->text().rfind("Compiled", 0) == 0);
}

TEST_CASE("rack window: a plugin module shows the plug-in's name and a click opens its editor") {
    Fixture f(ui::Theme::dark(), {FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    Patch p(f);
    const std::string sine =
        f.exec("module.add",
               R"({"rack":")" + p.rack +
                   R"(","plugin":"com.suppleaudio.test.sine","x":440,"y":300})")["module"]
            .asString();
    std::vector<model::Id> opened;
    f.racks->openEditor = [&](model::Id fx) -> Status {
        opened.push_back(fx);
        return {};
    };
    f.racks->open();
    f.frame();
    const model::Id id = *model::Id::parse(sine);
    const auto* m = f.racks->space()->scene().module(id);
    REQUIRE(m);
    CHECK(m->plugin);
    CHECK(m->title == "Test Sine");
    const model::RackModule* rm = f.app->head().modules.find(id);
    REQUIRE(rm);
    REQUIRE(rm->fx);
    // A click (not a drag) on it opens the plug-in's own window.
    const ui::Point at = *f.racks->space()->bodyAt(id);
    f.pointer(ui::PointerEvent::Type::Move, at, 0);
    f.pointer(ui::PointerEvent::Type::Down, at);
    f.pointer(ui::PointerEvent::Type::Up, at);
    CHECK(opened == std::vector<model::Id>{*rm->fx});
    f.snapshot("rack-plugin.png");
}
