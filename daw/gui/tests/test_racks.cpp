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
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <tuple>

#include "app_actions.h"
#include "floating_panel.h"
#include "plugin_scales.h"
#include "rack_device.h"
#include "rack_engine.h"
#include "rack_labels.h"
#include "rack_plugin_params.h"
#include "rack_window.h"
#include "supple/engine/realtime.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/script_compiler.hpp"
#include "supple/script_module.h"
#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
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
        if (over) {
            g.strokeStyle(Theme::accent);
            g.lineWidth(1);
            g.strokeRect(10, 32, 130, 64);
        }
    }
    // The display sets the window: across it, or by the wheel. A hover may
    // not edit (that edit is ignored).
    bool over = false;
    void pointer(const Pointer& p) {
        if (p.action() == PointerAction::Press || p.action() == PointerAction::Drag)
            time.editNormalized((p.x() - 10) / 130);
        if (p.action() == PointerAction::Wheel) time.edit(time.value() + 10 * p.wheel());
        if (p.action() == PointerAction::Move) {
            over = true;
            time.edit(999);
        }
        if (p.action() == PointerAction::Leave) over = false;
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
                     std::vector<std::filesystem::path> plugins = {}, int width = 1100,
                     int height = 560,
                     const std::function<void(engine::AppOptions&)>& configure = {}) {
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
        auto w = gui::Window::open(*platform, {"rack-test", width, height, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        if (!plugins.empty()) options.pluginPath = std::move(plugins);
        if (configure) configure(options);
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
            *platform, *window, *root, SUPPLE_GUI_FONTS_DIR, "Racks", std::max(1180, width),
            std::max(720, height), 640, 400);
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

// A realistic big patch (28 modules, 36 cables) as an agent makes it with
// rack.patch: two voices, drums, effects and modulation, laid out by the engine.
std::string bigPatch(Fixture& f, const std::string& rack, const std::string& out) {
    const std::string patch = R"({"rack":")" + rack + R"(","modules":{
        "clock":{"type":"clock"}, "seq":{"type":"sequencer"}, "quant":{"type":"quantizer"},
        "osc1":{"type":"vco"}, "osc2":{"type":"vco"}, "lfo1":{"type":"lfo"},
        "lfo2":{"type":"lfo"}, "env1":{"type":"adsr"}, "env2":{"type":"adsr"},
        "filt1":{"type":"filter"}, "filt2":{"type":"ladder"}, "amp1":{"type":"vca"},
        "amp2":{"type":"vca"}, "euc1":{"type":"euclid"}, "euc2":{"type":"euclid"},
        "kick":{"type":"drum"}, "snare":{"type":"drum"}, "hat":{"type":"drum"},
        "noise":{"type":"noise"}, "rnd":{"type":"random"}, "slew":{"type":"slew"},
        "shift":{"type":"turing"}, "mix":{"type":"mixer"}, "drive":{"type":"drive"},
        "chorus":{"type":"chorus"}, "delay":{"type":"delay"}, "verb":{"type":"reverb"},
        "atten":{"type":"scale"}},
      "cables":[
        ["clock.out_1","seq.clock"], ["clock.out_2","euc1.clock"], ["clock.out_1","euc2.clock"],
        ["clock.out_3","kick.trigger"], ["euc1.gate","snare.trigger"], ["euc2.gate","hat.trigger"],
        ["clock.out_4","rnd.clock"], ["clock.out_1","shift.clock"],
        ["seq.pitch","quant.pitch"], ["quant.out","osc1.pitch"], ["quant.out","filt1.pitch"],
        ["seq.gate","env1.gate"], ["shift.out","osc2.pitch"], ["shift.bit_1","env2.gate"],
        ["osc1.saw","filt1.in"], ["osc1.pulse","filt1.in"], ["osc2.saw","filt2.in"],
        ["lfo1.out","osc1.pulse_width",20], ["lfo2.out","filt2.cutoff",1],
        ["env1.envelope","amp1.cv"], ["env1.envelope","filt1.cutoff",2],
        ["env2.envelope","amp2.cv"], ["filt1.low","amp1.in"], ["filt2.out","amp2.in"],
        ["rnd.smooth","slew.in"], ["slew.out","filt1.fm"], ["rnd.gate_a","env2.retrigger"],
        ["amp1.out","mix.in_1"], ["amp2.out","mix.in_2"], ["kick.out","mix.in_3"],
        ["snare.out","mix.in_4"], ["hat.out","drive.in"], ["drive.out","chorus.in_l"],
        ["mix.left","delay.in_l"], ["mix.right","delay.in_r"], ["delay.out_l","verb.in_l"],
        ["delay.out_r","verb.in_r"], ["chorus.out_l","verb.in_l"], ["lfo1.out","atten.in_1"],
        ["atten.out_1","chorus.rate",0.5], ["noise.pink","snare.accent"],
        {"from":"verb.out_l","to":")" +
                              out + R"(.l"}, {"from":"verb.out_r","to":")" + out + R"(.r"}]})";
    f.exec("rack.patch", patch);
    return rack;
}

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
    "rack window: a drag moves the selection, one undo step; the wheel zooms at the pointer") {
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
    wheel.wheelY = 3;
    f.r().pointer(wheel);
    CHECK(space.view().zoom > zoom);
    CHECK(space.toSpace(at).x == doctest::Approx(static_cast<double>(under.x)).epsilon(0.001));
    CHECK(space.toSpace(at).y == doctest::Approx(static_cast<double>(under.y)).epsilon(0.001));
    for (int k = 0; k < 40; ++k) f.r().pointer(wheel);
    CHECK(space.view().zoom == doctest::Approx(gui::RackSpace::kMaxZoom));
    // Ctrl scrolls vertically; Shift scrolls horizontally without zooming.
    const auto scrolled = space.view();
    wheel.mods = ui::kCtrl;
    wheel.wheelY = 1;
    f.r().pointer(wheel);
    CHECK(space.view().zoom == scrolled.zoom);
    CHECK(space.view().y == doctest::Approx(scrolled.y - 40 / scrolled.zoom));
    const auto shifted = space.view();
    wheel.mods = ui::kShift;
    f.r().pointer(wheel);
    CHECK(space.view().zoom == shifted.zoom);
    CHECK(space.view().x == doctest::Approx(shifted.x - 40 / shifted.zoom));
    CHECK(space.view().y == shifted.y);
    f.snapshot("rack-wheel-navigation.png");
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

TEST_CASE("rack window: a display hears the pointer; its edits are one undo step a press") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const model::Id scope = *model::Id::parse(p.scope);
    auto time = [&]() -> double {
        const json::Value* t = f.app->head().modules.find(scope)->params.find("time");
        return t ? t->asDouble() : 20.0;
    };
    // The display is at (10, 32) 130 x 64 on the module at (620, 300).
    auto at = [&](double x, double y) {
        return space.toWindow({620.0f + float(x), 300.0f + float(y)});
    };
    const std::size_t steps = f.exec("history.list", "{}")["versions"].size();
    // A hover is heard (the display outlines itself) but edits nothing.
    f.pointer(ui::PointerEvent::Type::Move, at(70, 60), 0);
    CHECK(time() == doctest::Approx(20));
    f.snapshot("rack-display-hover.png");
    // A drag across it sets Time, and the module stays where it is.
    f.drag(at(12, 60), at(138, 60));
    CHECK(time() > 800);  // 98 % across, on its log scale
    CHECK(f.app->head().modules.find(scope)->x == doctest::Approx(620));
    CHECK(f.exec("history.list", "{}")["versions"].size() == steps + 1);
    f.exec("history.undo", "{}");
    f.frame();
    CHECK(time() == doctest::Approx(20));
    // The wheel over it, one step; Ctrl+wheel scrolls the space.
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = at(70, 60);
    wheel.wheelY = 1;
    f.r().pointer(wheel);
    f.frame();
    CHECK(time() == doctest::Approx(30));
    const auto beforeScroll = space.view();
    wheel.mods = ui::kCtrl;
    f.r().pointer(wheel);
    CHECK(space.view().zoom == beforeScroll.zoom);
    CHECK(space.view().y == doctest::Approx(beforeScroll.y - 40 / beforeScroll.zoom));
    CHECK(time() == doctest::Approx(30));
    // Elsewhere on the module, a drag still moves it.
    f.drag(at(75, 10), at(95, 10));
    CHECK(f.app->head().modules.find(scope)->x > 620);
}

TEST_CASE("rack window: the browser's tags fold into one row, opened by +N") {
    Fixture f;
    if (!f.ok) return;
    f.racks->open();
    f.frame();
    auto click = [&](const std::string& label) {
        const auto at = f.racks->tagChipAt(label);
        REQUIRE(at);
        f.pointer(ui::PointerEvent::Type::Down, *at);
        f.pointer(ui::PointerEvent::Type::Up, *at);
    };
    // Folded: a row of the most used tags, then "+N" for the rest.
    auto folded = f.racks->tagChips();
    REQUIRE(folded.size() >= 2);
    const std::string more = folded.back();
    REQUIRE(more.front() == '+');
    const std::size_t hidden = std::stoul(more.substr(1));
    CHECK(f.racks->tagChipAt(folded.front())->y == f.racks->tagChipAt(more)->y);  // one row
    // Open: every tag, alphabetically, and "Less".
    click(more);
    auto open = f.racks->tagChips();
    CHECK(open.size() == folded.size() - 1 + hidden + 1);
    CHECK(open.back() == "Less");
    CHECK(std::is_sorted(open.begin(), open.end() - 1));
    f.snapshot("rack-tags-open.png");
    // Picking one filters the list and folds the tags, the pick first.
    click("visual");
    folded = f.racks->tagChips();
    CHECK(folded.front() == "visual");
    CHECK(folded.back().front() == '+');
    const auto shows = f.racks->browserShows();
    CHECK(std::find(shows.begin(), shows.end(), "com.example.test.scope") != shows.end());
    CHECK(std::find(shows.begin(), shows.end(), "com.example.test.osc") == shows.end());
    // Again: the filter off.
    click("visual");
    CHECK(f.racks->browserShows().size() > shows.size());
    f.snapshot("rack-tags-folded.png");
}

TEST_CASE("rack window: a knob learns a MIDI controller from its menu, shows it and forgets it") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    const model::Id vca = *model::Id::parse(p.vca);
    auto menu = [&] {
        const ui::Point level = *f.racks->space()->controlAt(vca, "level");
        f.pointer(ui::PointerEvent::Type::Down, level, 3);
        f.pointer(ui::PointerEvent::Type::Up, level, 3);
        REQUIRE(f.r().find("rack.param.menu"));
    };
    menu();
    REQUIRE(f.pick("rack.param.menu", "MIDI learn"));
    REQUIRE(f.r().find("learn"));
    engine::EngineApp::ControlEvent e;
    e.midi.bytes[0] = 0xb0;
    e.midi.bytes[1] = 9;
    e.midi.bytes[2] = 127;
    f.actions->control({e});
    f.frame();
    REQUIRE(f.r().click("learn.ok"));
    f.frame();
    const auto& learn = f.app->head().modules.find(vca)->learn;
    REQUIRE(learn.size() == 1);
    CHECK(learn[0].param == "level");
    CHECK(learn[0].control.number == 9);
    CHECK(f.racks->space()->scene().module(vca)->learned.contains("level"));
    f.racks->space()->setView({420, 40, 2.0});
    f.snapshot("rack-learned.png");
    // The menu names the controller, and removes it.
    menu();
    const auto items = ui::menuItems(*f.r().find("rack.param.menu"));
    CHECK(std::any_of(items.begin(), items.end(),
                      [](const ui::MenuItem& m) { return m.label.rfind("MIDI: CC 9", 0) == 0; }));
    REQUIRE(f.pick("rack.param.menu", "Remove MIDI learn"));
    CHECK(f.app->head().modules.find(vca)->learn.empty());
    CHECK_FALSE(f.racks->space()->scene().module(vca)->learned.contains("level"));
}

TEST_CASE(
    "rack window: a cable dropped on a plugin module modulates a parameter picked from a list") {
    Fixture f(ui::Theme::dark(), {FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    Patch p(f);
    const std::string gain =
        f.exec("module.add",
               R"({"rack":")" + p.rack +
                   R"(","plugin":"com.suppleaudio.test.gain","x":840,"y":300})")["module"]
            .asString();
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    space.frameAll();
    f.frame();
    const model::Id id = *model::Id::parse(gain), osc = *model::Id::parse(p.osc);
    const model::Id fx = *f.app->head().modules.find(id)->fx;
    const std::size_t cables = f.app->head().cables.size();
    // Dropped on the body: the plug-in's modulatable parameters (Gain; not
    // the stepped Latency), searchable.
    f.drag(*space.jackAt(osc, "out", true), *space.bodyAt(id));
    const ui::Node* picker = f.r().find("rack.params");
    REQUIRE(picker);
    CHECK(picker->valueText() == "Gain");
    f.snapshot("rack-plugin-params.png");
    f.r().text("lat");
    f.frame();
    CHECK(f.r().find("rack.params")->valueText().empty());
    f.r().key(SDLK_BACKSPACE);
    f.r().key(SDLK_BACKSPACE);
    f.r().key(SDLK_BACKSPACE);
    f.r().text("ga");
    f.frame();
    CHECK(f.r().find("rack.params")->valueText() == "Gain");
    f.r().key(SDLK_RETURN);
    f.frame();
    CHECK_FALSE(f.r().find("rack.params"));
    REQUIRE(f.app->head().cables.size() == cables + 1);
    std::optional<model::Id> made;
    for (const auto& c : f.app->head().cables)
        if (c.value->to.object == fx) made = c.id;
    REQUIRE(made);
    const model::Cable& cable = *f.app->head().cables.find(*made);
    CHECK(cable.to.key == "gain");
    CHECK(cable.amount == 0.5);
    // It ends in a socket on the module's edge, named on hover.
    const auto* m = space.scene().module(id);
    REQUIRE(m);
    CHECK(m->mods == std::vector<std::string>{"gain"});
    const ui::Point socket = *space.socketAt(id, "gain");
    f.pointer(ui::PointerEvent::Type::Move, socket, 0);
    CHECK(space.hoverText() == "Gain · +50 %");
    space.setView({780, 220, 2.0});
    f.frame();
    f.pointer(ui::PointerEvent::Type::Move, *space.socketAt(id, "gain"), 0);
    f.snapshot("rack-plugin-mod.png");
    // Its amount drags like a knob's ring: one undo step.
    {
        const ui::Point at = *space.socketAt(id, "gain");
        f.drag(at, {at.x, at.y - 40});
    }
    CHECK(f.app->head().cables.find(*made)->amount == doctest::Approx(0.75));
    REQUIRE(f.exec("history.undo", "{}").isObject());
    f.frame();
    CHECK(f.app->head().cables.find(*made)->amount == doctest::Approx(0.5));
    // The cable's right-click names the parameter, sets the amount, removes it.
    auto menu = [&] {
        const ui::Point at = *space.socketAt(id, "gain");
        f.pointer(ui::PointerEvent::Type::Down, at, 3);
        f.pointer(ui::PointerEvent::Type::Up, at, 3);
        REQUIRE(f.r().find("rack.cable.menu"));
    };
    menu();
    const auto items = ui::menuItems(*f.r().find("rack.cable.menu"));
    REQUIRE(items.size() >= 2);
    CHECK(items[0].label == "Gain on Test Gain");
    CHECK(items[1].label == "Amount: +50 %…");
    REQUIRE(f.pick("rack.cable.menu", "Amount"));
    auto* amount = dynamic_cast<ui::TextField*>(f.r().find("rack.cable.amount"));
    REQUIRE(amount);
    amount->setText("-25 %");
    f.r().key(SDLK_RETURN);
    f.frame();
    CHECK(f.app->head().cables.find(*made)->amount == doctest::Approx(-0.25));
    // Right-clicking the cord itself opens the same menu.
    space.frameAll();
    f.frame();
    const ui::Point cord = *space.cableAt(*made);
    f.pointer(ui::PointerEvent::Type::Move, cord, 0);
    f.pointer(ui::PointerEvent::Type::Down, cord, 3);
    f.pointer(ui::PointerEvent::Type::Up, cord, 3);
    REQUIRE(f.pick("rack.cable.menu", "Remove cable"));
    CHECK_FALSE(f.app->head().cables.find(*made));
    CHECK(space.scene().module(id)->mods.empty());
    // A VST3 plug-in says it takes no modulation.
    model::FxInstance vst3 = *f.app->head().fx.find(fx);
    vst3.plugin.format = "vst3";
    const gui::racks::PluginParams none = gui::racks::pluginParams(*f.app, vst3);
    CHECK(none.note == "VST3 plug-ins take no modulation.");
    CHECK(std::none_of(none.params.begin(), none.params.end(),
                       [](const gui::racks::PluginParam& q) { return q.modulatable; }));
}

TEST_CASE("rack window: a plug-in without semantic keys is modulated by its parameter ids") {
    Fixture f(ui::Theme::dark(), {FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    Patch p(f);
    // A third-party-style plug-in: CLAP parameters and no semantic keys.
    const std::string raw =
        f.exec("module.add",
               R"({"rack":")" + p.rack +
                   R"(","plugin":"com.suppleaudio.test.raw-gain","x":840,"y":300})")["module"]
            .asString();
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    space.frameAll();
    f.frame();
    const model::Id id = *model::Id::parse(raw), osc = *model::Id::parse(p.osc);
    const model::Id fx = *f.app->head().modules.find(id)->fx;
    // The list names its parameters; the cable reaches one by its CLAP id.
    f.drag(*space.jackAt(osc, "out", true), *space.bodyAt(id));
    const ui::Node* picker = f.r().find("rack.params");
    REQUIRE(picker);
    CHECK(picker->valueText() == "Gain");
    f.r().key(SDLK_RETURN);
    f.frame();
    CHECK_FALSE(f.r().find("rack.params"));
    std::optional<model::Id> made;
    for (const auto& c : f.app->head().cables)
        if (c.value->to.object == fx) made = c.id;
    REQUIRE(made);
    CHECK(f.app->head().cables.find(*made)->to.key == "0");
    CHECK(f.racks->status().rfind("Bells", 0) == 0);  // no error
    // The engine takes it: no problems with the rack.
    const json::Value got = f.exec("rack.get", R"({"rack":")" + p.rack + R"("})");
    const json::Value* problems = got.find("problems");
    CHECK((!problems || problems->size() == 0));
    // Its socket and its menu name the parameter.
    const auto* m = space.scene().module(id);
    REQUIRE(m);
    CHECK(m->mods == std::vector<std::string>{"0"});
    f.pointer(ui::PointerEvent::Type::Move, *space.socketAt(id, "0"), 0);
    CHECK(space.hoverText() == "Gain · +50 %");
    // Saved and reopened: the same cable, still named.
    const std::string path = (f.dir / "raw.json").generic_string();
    f.exec("project.save", R"({"path":")" + path + R"("})");
    f.exec("project.open", R"({"path":")" + path + R"("})");
    f.frame();
    REQUIRE(f.app->head().cables.find(*made));
    CHECK(f.app->head().cables.find(*made)->to.key == "0");
    REQUIRE(f.racks->space()->scene().module(id));
    CHECK(f.racks->space()->scene().module(id)->modName("0") == "Gain");
    // A Macro driving it is named after the parameter too.
    const std::string macro = f.add(p.rack, "macro", 40, 520);
    f.cable(macro + ".out_1", fx.str() + ".0");
    gui::racks::PluginParamCache cache(*f.app);
    gui::RackDevice::Hooks hooks;
    hooks.paramName = [&](model::Id at, const std::string& key) { return cache.name(at, key); };
    const auto shown = gui::RackDevice(*f.app, *model::Id::parse(p.rack), hooks).shown();
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].name == "Test Raw Gain Gain");
}

TEST_CASE("rack window: a knob learned in the window turns on screen when its controller moves") {
    // A device the test plays (engine/tests/test_learn.cpp's rig): a manual
    // dummy backend with a MIDI input whose messages the test queues.
    std::mutex lock;
    std::deque<engine::MidiInputEvent> queued;
    engine::DummyBackend backend(engine::DummyBackend::Options{
        .mode = engine::DummyBackend::Mode::Manual,
        .midiPorts = {"nanoKONTROL2 CTRL"},
        .midi = [&](std::vector<engine::MidiInputEvent>& out, std::uint32_t, std::int64_t) {
            std::lock_guard<std::mutex> g(lock);
            while (!queued.empty()) {
                out.push_back(queued.front());
                queued.pop_front();
            }
        }});
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.app->attachCommands(*f.dispatcher);
    f.racks->open();
    f.frame();
    const model::Id vca = *model::Id::parse(p.vca);
    // Bound through the knob's menu and the learn dialog.
    const ui::Point level = *f.racks->space()->controlAt(vca, "level");
    f.pointer(ui::PointerEvent::Type::Down, level, 3);
    f.pointer(ui::PointerEvent::Type::Up, level, 3);
    REQUIRE(f.pick("rack.param.menu", "MIDI learn"));
    engine::EngineApp::ControlEvent heard;
    heard.midi.bytes[0] = 0xb0;
    heard.midi.bytes[1] = 21;
    heard.midi.bytes[2] = 64;
    f.actions->control({heard});
    f.frame();
    REQUIRE(f.r().click("learn.ok"));
    f.frame();
    REQUIRE(f.app->head().modules.find(vca)->learn.size() == 1);
    // The controller turns it; the window's knob follows, along the same travel.
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                               {.anticipative = false}));
    auto turn = [&](std::uint8_t value) {
        {
            std::lock_guard<std::mutex> g(lock);
            engine::MidiInputEvent e;
            e.size = 3;
            e.bytes[0] = 0xb0;
            e.bytes[1] = 21;
            e.bytes[2] = value;
            queued.push_back(e);
        }
        backend.run(1);
        f.app->pollControl();
        f.frame();
        const auto* m = f.racks->space()->scene().module(vca);
        REQUIRE(m);
        const int index = m->type->param("level");
        REQUIRE(index >= 0);
        const auto& param = m->type->params()[static_cast<std::size_t>(index)];
        return std::pair{param, m->values[static_cast<std::size_t>(index)]};
    };
    for (std::uint8_t value : {std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{90}}) {
        const auto [param, shown] = turn(value);
        INFO("CC value " << int(value));
        CHECK(shown == doctest::Approx(rack::knobValue(param, value / 127.0)));
        CHECK(rack::knobPosition(param, shown) == doctest::Approx(value / 127.0));
    }
    f.app->stopEngine();
}

TEST_CASE("rack window: a big patch at 1400x900 and at a small size") {
    for (const auto& [w, h, name] :
         {std::tuple{1400, 900, "rack-big.png"}, std::tuple{800, 500, "rack-big-small.png"}}) {
        Fixture f(ui::Theme::dark(), {}, w, h);
        if (!f.ok) return;
        auto r = f.exec("rack.add", R"({"name":"Big"})");
        bigPatch(f, r["rack"].asString(), r["out"].asString());
        f.racks->open();
        f.frame();
        f.racks->space()->frameAll();
        f.frame();
        gui::RackSpace& space = *f.racks->space();
        CHECK(space.scene().modules.size() >= 29);
        CHECK(space.scene().cables.size() >= 40);
        // Everything fits, however small the window: names stand in for faces.
        const ui::Rect all = space.scene().bounds();
        const ui::Rect seen = space.visibleSpace();
        CHECK(seen.contains({all.x, all.y}));
        CHECK(seen.contains({all.right(), all.bottom()}));
        f.snapshot(name);
        // Over a module, its cables stand out and the rest step back.
        for (const auto& m : space.scene().modules)
            if (m.title == "Mixer") {
                f.pointer(ui::PointerEvent::Type::Move, *space.bodyAt(m.id), 0);
                break;
            }
        f.snapshot(w > 1000 ? "rack-big-hover.png" : "rack-big-small-hover.png");
        if (w > 1000) {  // at 100 %, on the voices
            for (const auto& m : space.scene().modules)
                if (m.title == "Filter") space.select({m.id});
            space.frame(space.selection());
            space.setView({space.view().x - 300, space.view().y - 100, 1.0});
            f.frame();
            f.snapshot("rack-big-100.png");
        }
    }
}

TEST_CASE("rack window: typing adds a module at the pointer; a cable let go on nothing picks one") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const ui::Rect b = space.bounds();
    const ui::Point empty{b.x + b.w - 90, b.y + 60};
    f.pointer(ui::PointerEvent::Type::Move, empty, 0);
    const std::size_t modules = f.app->head().modules.size();
    // A letter typed over the space opens the list there, that letter in it.
    f.r().text("v");
    f.frame();
    auto* search = dynamic_cast<ui::TextField*>(f.r().find("rack.add.search"));
    REQUIRE(search);
    CHECK(search->text() == "v");
    f.r().text("co");
    f.frame();
    CHECK(f.r().find("rack.add")->valueText().rfind("VCO", 0) == 0);
    f.snapshot("rack-quick-add.png");
    f.r().key(SDLK_RETURN);
    f.frame();
    CHECK_FALSE(f.r().find("rack.add"));
    REQUIRE(f.app->head().modules.size() == modules + 1);
    REQUIRE(space.selection().size() == 1);
    const auto* made = space.scene().module(*space.selection().begin());
    REQUIRE(made);
    CHECK(made->typeId == "com.suppleaudio.rack.vco");
    const ui::Point at = space.toSpace(empty);
    CHECK(std::abs(made->x + made->w / 2 - double(at.x)) < 1);
    // A double-click on nothing opens it too; Escape closes it.
    const ui::Point other{b.x + b.w - 90, b.y + b.h - 40};
    f.pointer(ui::PointerEvent::Type::Move, other, 0);
    f.pointer(ui::PointerEvent::Type::Down, other, 1, 0, 2);
    f.pointer(ui::PointerEvent::Type::Up, other, 1, 0, 2);
    REQUIRE(f.r().find("rack.add"));
    f.r().key(SDLK_ESCAPE);
    f.frame();
    CHECK_FALSE(f.r().find("rack.add"));
    // A new cable let go on nothing: the module chosen takes it, in one step.
    const model::Id osc = *model::Id::parse(p.osc);
    const std::size_t cables = f.app->head().cables.size();
    f.drag(*space.jackAt(osc, "out", true), other);
    REQUIRE(f.r().find("rack.add"));
    f.r().text("filter");
    f.r().key(SDLK_RETURN);
    f.frame();
    REQUIRE(f.app->head().cables.size() == cables + 1);
    REQUIRE(space.selection().size() == 1);
    const model::Id filter = *space.selection().begin();
    bool wired = false;
    for (const auto& c : f.app->head().cables)
        wired = wired || (c.value->from == model::CableEnd{osc, "out"} &&
                          c.value->to == model::CableEnd{filter, "in"});
    CHECK(wired);
    REQUIRE(f.exec("history.undo", "{}").isObject());
    f.frame();
    CHECK(f.app->head().cables.size() == cables);
    CHECK_FALSE(f.app->head().modules.find(filter));
}

TEST_CASE("rack window: copy, paste, cut and duplicate keep the cables among the modules") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    const model::Id osc = *model::Id::parse(p.osc), vca = *model::Id::parse(p.vca);
    auto count = [&](model::Id rack) {
        std::size_t modules = 0, cables = 0;
        for (const auto& m : f.app->head().modules) modules += m.value->rack == rack;
        for (const auto& c : f.app->head().cables)
            if (const auto* m = f.app->head().modules.find(c.value->from.object))
                cables += m->rack == rack;
        return std::pair{modules, cables};
    };
    const model::Id rack = *model::Id::parse(p.rack);
    const auto [modules, cables] = count(rack);
    // Osc and VCA, with the cable between them (not the ones from outside).
    space.select({osc, vca});
    f.r().key(SDLK_C, ui::kCtrl);
    const ui::Rect b = space.bounds();
    const ui::Point to{b.x + b.w - 260, b.y + b.h - 220};
    f.pointer(ui::PointerEvent::Type::Move, to, 0);
    f.r().key(SDLK_V, ui::kCtrl);
    f.frame();
    CHECK(count(rack) == std::pair{modules + 2, cables + 1});
    REQUIRE(space.selection().size() == 2);
    double left = 1e9, top = 1e9;
    for (model::Id id : space.selection()) {
        left = std::min(left, space.scene().module(id)->x);
        top = std::min(top, space.scene().module(id)->y);
    }
    const ui::Point at = space.toSpace(to);
    CHECK(std::abs(left - double(at.x)) < 1);
    CHECK(std::abs(top - double(at.y)) < 1);
    // Ctrl+D duplicates what is selected, cables too.
    f.r().key(SDLK_D, ui::kCtrl);
    f.frame();
    CHECK(count(rack) == std::pair{modules + 4, cables + 2});
    // Into another rack, from the engine's clipboard (rack.paste).
    REQUIRE(f.racks->addRack());
    f.frame();
    const model::Id second = *f.racks->rack();
    const auto [modules2, cables2] = count(second);
    f.r().key(SDLK_V, ui::kCtrl);
    f.frame();
    CHECK(count(second) == std::pair{modules2 + 2, cables2 + 1});
    // Cut takes them away; paste brings them back.
    f.r().key(SDLK_X, ui::kCtrl);
    f.frame();
    CHECK(count(second) == std::pair{modules2, cables2});
    f.r().key(SDLK_V, ui::kCtrl);
    f.frame();
    CHECK(count(second) == std::pair{modules2 + 2, cables2 + 1});
    // Arrows nudge the selection (Shift: finely), one undo step a press.
    const model::Id one = *space.selection().begin();
    space.select({one});
    const double x = space.scene().module(one)->x;
    f.r().key(SDLK_RIGHT);
    f.frame();
    CHECK(f.app->head().modules.find(one)->x == doctest::Approx(x + 10));
    f.r().key(SDLK_LEFT, ui::kShift);
    f.frame();
    CHECK(f.app->head().modules.find(one)->x == doctest::Approx(x + 9));
    // Shift+Home fills the view with the selection.
    space.setView({-2000, -2000, 0.5});
    f.r().key(SDLK_HOME, ui::kShift);
    f.frame();
    const ui::Rect seen = space.visibleSpace();
    const auto* m = space.scene().module(one);
    CHECK(seen.contains({float(m->x), float(m->y)}));
    CHECK(space.view().zoom > 1.0);  // filled (at most 150 %)
    CHECK(space.view().zoom <= 1.5);
}

TEST_CASE("rack window: Delete, Backspace and Alt+C delete the selection, and stop there") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    // What the window leaves unhandled reaches the app's keys (the arrange
    // view's edit.delete): none of these.
    std::vector<int> leaked;
    f.r().onUnhandledKey = [&](const ui::KeyEvent& e) {
        leaked.push_back(e.key);
        return false;
    };
    const struct {
        int key;
        unsigned mods;
        const std::string* module;
    } presses[] = {
        {SDLK_DELETE, 0, &p.osc}, {SDLK_BACKSPACE, 0, &p.vca}, {SDLK_C, ui::kAlt, &p.scope}};
    for (const auto& press : presses) {
        const model::Id id = *model::Id::parse(*press.module);
        REQUIRE(f.app->head().modules.find(id));
        space.select({id});
        f.r().key(press.key, press.mods);
        f.frame();
        CHECK_FALSE(f.app->head().modules.find(id));
        CHECK(space.selection().empty());
        // With nothing selected, the key deletes nothing and goes no further.
        const auto version = f.app->head().version;
        f.r().key(press.key, press.mods);
        f.frame();
        CHECK(f.app->head().version == version);
    }
    CHECK(leaked.empty());
}

TEST_CASE(
    "a Rack device names a Macro's plug-in parameter target after the parameter, "
    "asking the plug-in once") {
    Fixture f(ui::Theme::dark(), {FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    Patch p(f);
    const std::string gain =
        f.exec("module.add",
               R"({"rack":")" + p.rack +
                   R"(","plugin":"com.suppleaudio.test.gain","x":840,"y":300})")["module"]
            .asString();
    const std::string macro = f.add(p.rack, "macro", 40, 520, R"(,"label":"Hands")");
    const model::Id fx = *f.app->head().modules.find(*model::Id::parse(gain))->fx;
    // By its key: named as the plug-in writes it.
    f.cable(macro + ".out_1", fx.str() + ".gain");
    f.cable(macro + ".out_2", p.vca + ".level");
    gui::racks::PluginParamCache cache(*f.app);
    gui::RackDevice::Hooks hooks;
    hooks.paramName = [&](model::Id id, const std::string& key) { return cache.name(id, key); };
    const model::Id rack = *model::Id::parse(p.rack);
    {
        gui::RackDevice device(*f.app, rack, hooks);
        const auto shown = device.shown();
        REQUIRE(shown.size() == 2);
        CHECK(shown[0].name == "Test Gain Gain");
        CHECK(shown[1].name == "VCA level");
    }
    CHECK(cache.asked() == 1);
    // Built again, and the rack window's sockets named: the plug-in is not
    // asked again (the views share the answers).
    gui::RackDevice again(*f.app, rack, hooks);
    CHECK(again.shown()[0].name == "Test Gain Gain");
    f.racks->open();
    f.frame();
    const auto* m = f.racks->space()->scene().module(*model::Id::parse(gain));
    REQUIRE(m);
    CHECK(m->modName("gain") == "Gain");
    CHECK(cache.asked() == 1);
    // Without the names: the key.
    CHECK(gui::RackDevice(*f.app, rack, {}).shown()[0].name == "Test Gain gain");
    // The same fx id holding another plug-in (a new project): asked again.
    REQUIRE(f.exec("project.new", "{}").isObject());
    const std::string rack2 = f.exec("rack.add", "{}")["rack"].asString();
    const std::string edge =
        f.exec("module.add",
               R"({"rack":")" + rack2 + R"(","plugin":"com.suppleaudio.test.edge"})")["module"]
            .asString();
    REQUIRE(*f.app->head().modules.find(*model::Id::parse(edge))->fx == fx);
    CHECK(cache.of(fx).find("gain") == nullptr);
    CHECK(cache.asked() == 2);
}

TEST_CASE("rack window: a cut plugin module pastes into another rack with its settings") {
    Fixture f(ui::Theme::dark(), {FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    Patch p(f);
    const std::string gain =
        f.exec("module.add",
               R"({"rack":")" + p.rack +
                   R"(","plugin":"com.suppleaudio.test.gain","x":840,"y":300})")["module"]
            .asString();
    const model::Id id = *model::Id::parse(gain), osc = *model::Id::parse(p.osc);
    f.cable(p.osc + ".out", gain + ".in_l");
    const std::string fx = f.app->head().modules.find(id)->fx->str();
    f.exec("fx.param.set", R"({"fx":")" + fx + R"(","values":{"gain":-17}})");
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    // Nothing copied yet: the space's Paste is off.
    auto pasteEnabled = [&] {
        const ui::Rect b = space.bounds();
        const ui::Point empty{b.x + 20, b.y + 20};
        f.pointer(ui::PointerEvent::Type::Down, empty, 3);
        f.pointer(ui::PointerEvent::Type::Up, empty, 3);
        const ui::Node* menu = f.r().find("rack.space.menu");
        REQUIRE(menu);
        bool on = false;
        for (const auto& item : ui::menuItems(*menu))
            if (item.label == "Paste") on = item.enabled;
        f.r().closePopups();
        f.frame();
        return on;
    };
    CHECK_FALSE(pasteEnabled());
    space.select({id, osc});
    f.r().key(SDLK_X, ui::kCtrl);
    f.frame();
    CHECK_FALSE(f.app->head().modules.find(id));
    CHECK(pasteEnabled());
    REQUIRE(f.racks->addRack());
    f.frame();
    const model::Id second = *f.racks->rack();
    f.r().key(SDLK_V, ui::kCtrl);
    f.frame();
    REQUIRE(space.selection().size() == 2);
    std::optional<model::Id> pasted;
    for (model::Id m : space.selection()) {
        const auto* rm = f.app->head().modules.find(m);
        REQUIRE(rm);
        CHECK(rm->rack == second);
        if (rm->fx) pasted = rm->fx;
    }
    REQUIRE(pasted);
    // The plug-in keeps its gain, and the cable between them comes too.
    const auto value =
        f.exec("fx.param.get", R"({"fx":")" + pasted->str() + R"(","param":"gain"})")["value"];
    CHECK(value.asDouble() == doctest::Approx(-17));
    std::size_t cables = 0;
    for (const auto& c : f.app->head().cables)
        cables += space.selection().contains(c.value->from.object) &&
                  space.selection().contains(c.value->to.object);
    CHECK(cables == 1);
    // One undo step takes the paste away.
    const std::set<model::Id> made = space.selection();
    f.exec("history.undo", "{}");
    f.frame();
    for (model::Id m : made) CHECK_FALSE(f.app->head().modules.find(m));
    CHECK_FALSE(f.app->head().fx.find(*pasted));
    CHECK_FALSE(f.app->head().modules.find(id));  // the cut stays cut
}

TEST_CASE("rack window: modules saved as a patch load into another rack from the browser") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.racks->open();
    f.frame();
    const model::Id osc = *model::Id::parse(p.osc), vca = *model::Id::parse(p.vca);
    // Two modules, saved under a name asked for.
    f.racks->space()->select({osc, vca});
    f.racks->savePreset(std::vector<model::Id>{osc, vca});
    f.frame();
    auto* name = dynamic_cast<ui::TextField*>(f.r().find("rack.preset.name"));
    REQUIRE(name);
    name->setText("Voice");
    f.r().key(SDLK_RETURN);
    f.frame();
    // Not among the modules: on the Patches view, the user's before the factory's.
    auto shows = f.racks->browserShows();
    CHECK(std::find(shows.begin(), shows.end(), "preset:Voice") == shows.end());
    f.racks->showPatches(true);
    f.frame();
    shows = f.racks->browserShows();
    REQUIRE_FALSE(shows.empty());
    CHECK(shows.front() == "preset:Voice");
    CHECK(std::find(shows.begin(), shows.end(), "preset:Init Voice") != shows.end());
    f.snapshot("rack-presets-browser.png");
    // The same name again: replaced only when the user says so.
    f.racks->savePreset(std::vector<model::Id>{osc});
    f.frame();
    name = dynamic_cast<ui::TextField*>(f.r().find("rack.preset.name"));
    REQUIRE(name);
    name->setText("Voice");
    f.r().key(SDLK_RETURN);
    f.frame();
    REQUIRE(f.pick("rack.preset.replace", "Replace"));
    json::Value list = f.exec("rack.preset.list", R"({"factory": false})");
    REQUIRE(list["presets"].size() == 1);
    CHECK(list["presets"].asArray()[0].find("modules")->asInt() == 1);
    // A new rack: the patch loads into it, in view.
    REQUIRE(f.racks->addRack());
    f.frame();
    const std::size_t before = f.racks->space()->scene().modules.size();
    REQUIRE(f.racks->addModule("preset:Voice"));
    f.frame();
    CHECK(f.racks->space()->scene().modules.size() == before + 1);
    for (const auto& m : f.racks->space()->scene().modules) {
        const ui::Rect seen = f.racks->space()->visibleSpace(), r = m.rect();
        CHECK(seen.contains({r.x, r.y}));
        CHECK(seen.contains({r.right(), r.bottom()}));
    }
    // In a rack with more than its Out: added beside, selected and shown.
    REQUIRE(f.racks->addModule("preset:Voice"));
    f.frame();
    CHECK(f.racks->space()->scene().modules.size() == before + 2);
    CHECK(f.racks->space()->selection().size() == 1);
    // The space's menu: Add module… asks which at the pointer, and puts it there.
    const ui::Rect b = f.racks->space()->bounds();
    const ui::Point empty{b.x + b.w - 60, b.y + b.h - 60};
    f.pointer(ui::PointerEvent::Type::Down, empty, 3);
    f.pointer(ui::PointerEvent::Type::Up, empty, 3);
    REQUIRE(f.pick("rack.space.menu", "Add module"));
    REQUIRE(f.r().find("rack.add"));
    const ui::Point at = f.racks->space()->toSpace(empty);
    f.r().text("vca");
    f.r().key(SDLK_RETURN);
    f.frame();
    bool placed = false;
    for (const auto& m : f.racks->space()->scene().modules)
        placed = placed || (m.typeId == "com.suppleaudio.rack.vca" &&
                            std::abs(m.x + m.w / 2 - double(at.x)) < 1 &&
                            std::abs(m.y - double(at.y) + 12) < 1);
    CHECK(placed);
    // Deleted from the browser's menu; a factory patch cannot be.
    f.racks->browserMenu("preset:Init Voice", {b.x + 40, b.y + 40});
    CHECK_FALSE(f.pick("rack.browser.menu", "Delete patch"));
    f.r().closePopups();
    f.racks->browserMenu("preset:Voice", {b.x + 40, b.y + 40});
    REQUIRE(f.pick("rack.browser.menu", "Delete patch"));
    shows = f.racks->browserShows();
    CHECK(std::find(shows.begin(), shows.end(), "preset:Voice") == shows.end());
}

TEST_CASE("rack window: patches have a view of their own, searchable, with a card on hover") {
    Fixture f;
    if (!f.ok) return;
    Patch p(f);
    f.exec("rack.preset.save",
           R"({"rack":")" + p.rack + R"(","name":"My Bass","modules":[")" + p.osc + R"("]})");
    f.racks->open();
    f.frame();
    // The default view lists module types only, so patches never move it.
    auto shows = f.racks->browserShows();
    CHECK(std::none_of(shows.begin(), shows.end(),
                       [](const std::string& id) { return id.starts_with("preset:"); }));
    CHECK(std::find(shows.begin(), shows.end(), "com.suppleaudio.rack.vco") != shows.end());
    CHECK_FALSE(f.racks->tagChips().empty());
    // Patches: the user's, then the factory's; no module types, no tags.
    const auto view = f.racks->browserViewAt(true);
    REQUIRE(view);
    f.pointer(ui::PointerEvent::Type::Down, *view, 1);
    f.pointer(ui::PointerEvent::Type::Up, *view, 1);
    f.frame();
    CHECK(f.racks->patchesShown());
    shows = f.racks->browserShows();
    REQUIRE(shows.size() >= 2);
    CHECK(shows.front() == "preset:My Bass");
    CHECK(std::all_of(shows.begin(), shows.end(),
                      [](const std::string& id) { return id.starts_with("preset:"); }));
    CHECK(f.racks->tagChips().empty());
    // Searchable, by name or description.
    auto* search = dynamic_cast<ui::TextField*>(f.r().find("rack.browser.search"));
    REQUIRE(search);
    CHECK(search->placeholder() == "Search patches");
    f.r().focus(search);
    f.r().text("subtractive");
    f.frame();
    CHECK(f.racks->browserShows() == std::vector<std::string>{"preset:Init Voice"});
    // A hovered row shows its whole description beside the browser.
    const auto row = f.racks->browserRowAt("preset:Init Voice");
    REQUIRE(row);
    f.pointer(ui::PointerEvent::Type::Move, *row, 0);
    f.frame();
    const ui::Node* card = f.r().find("rack.browser.card");
    REQUIRE(card);
    CHECK(card->valueText().find("A plain subtractive voice") != std::string::npos);
    CHECK(card->valueText().find("Factory patch") != std::string::npos);
    CHECK(card->bounds().x >= f.racks->space()->bounds().x);
    f.snapshot("rack-patches.png");
    // Away from it, the card goes; back to Modules, the search is the modules'.
    const ui::Rect sb = f.racks->space()->bounds();
    f.pointer(ui::PointerEvent::Type::Move, {sb.x + sb.w / 2, sb.y + sb.h / 2}, 0);
    f.frame();
    CHECK_FALSE(f.r().find("rack.browser.card"));
    f.racks->showPatches(false);
    f.frame();
    CHECK(search->placeholder() == "Search modules");
    CHECK(search->text().empty());
    CHECK(f.racks->browserShows().size() > 40);
}

TEST_CASE("rack window: a Notes module's menu picks its MIDI input and the track it plays") {
    // A keyboard the test plays: a manual dummy backend with one MIDI port.
    std::mutex lock;
    std::deque<engine::MidiInputEvent> queued;
    engine::DummyBackend backend(engine::DummyBackend::Options{
        .mode = engine::DummyBackend::Mode::Manual,
        .captureFrames = 48000 * 12,
        .midiPorts = {"Keys"},
        .midi = [&](std::vector<engine::MidiInputEvent>& out, std::uint32_t, std::int64_t) {
            std::lock_guard<std::mutex> g(lock);
            while (!queued.empty()) {
                out.push_back(queued.front());
                queued.pop_front();
            }
        }});
    Fixture f;
    if (!f.ok) return;
    f.app->attachCommands(*f.dispatcher);
    auto made = f.exec("rack.add", R"({"name":"Voice"})");
    const std::string rack = made["rack"].asString();
    const model::Id home = *model::Id::parse(made["track"].asString());
    f.exec("rack.preset.load", R"({"rack":")" + rack + R"(","name":"Init Voice"})");
    auto other = f.exec("track.add", R"({"name":"Part"})");
    const model::Id part = *model::Id::parse(other["track"].asString());
    std::optional<model::Id> found;
    for (const auto& e : f.app->head().modules)
        if (e.value->type == model::kNotesModule) found = e.id;
    REQUIRE(found);
    const model::Id notes = *found;
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                               {.anticipative = false}));
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    auto openMenu = [&]() -> const ui::Node* {
        f.r().closePopups();
        f.frame();
        const auto at = space.bodyAt(notes);
        REQUIRE(at);
        f.pointer(ui::PointerEvent::Type::Down, *at, 3);
        f.pointer(ui::PointerEvent::Type::Up, *at, 3);
        return f.r().find("rack.module.menu");
    };
    auto item = [](const ui::Node& menu, const std::string& label) -> const ui::MenuItem* {
        for (const auto& i : ui::menuItems(menu))
            if (i.label == label) return &i;
        return nullptr;
    };
    auto checked = [&](const char* id) {
        std::vector<std::string> out;
        if (const ui::Node* m = f.r().find(id))
            for (const auto& i : ui::menuItems(*m))
                if (i.checked) out.push_back(i.label);
        return out;
    };
    // Played: a C4 held for a fifth of a second, what reaches the output
    // meanwhile (the output's peak over those cycles; stopped, as a player
    // tries a patch).
    std::int64_t played = 0;
    auto play = [&] {
        for (int k = 0; k < 4; ++k) {
            f.app->tick();
            backend.run(1);
            played += 256;
        }
        {
            std::lock_guard<std::mutex> g(lock);
            queued.push_back({.frame = 0, .port = 0, .size = 3, .bytes = {0x90, 60, 100}});
        }
        const std::int64_t from = played;
        for (int k = 0; k < 40; ++k) backend.run(1);
        played += 40 * 256;
        {
            std::lock_guard<std::mutex> g(lock);
            queued.push_back({.frame = 0, .port = 0, .size = 3, .bytes = {0x80, 60, 0}});
        }
        for (int k = 0; k < 160; ++k) backend.run(1);  // the release dies away
        played += 160 * 256;
        float peak = 0;
        const auto& out = backend.capture().channels[0];
        for (std::int64_t i = from; i < from + 40 * 256 && i < std::int64_t(out.size()); ++i)
            peak = std::max(peak, std::fabs(out[static_cast<std::size_t>(i)]));
        return peak;
    };
    // At first it has no device of its own and plays its home channel.
    const ui::Node* menu = openMenu();
    REQUIRE(menu);
    const ui::MenuItem* input = item(*menu, "MIDI input…");
    const ui::MenuItem* plays = item(*menu, "Plays track…");
    REQUIRE(input);
    REQUIRE(plays);
    CHECK(input->shortcut == "none");
    CHECK(plays->shortcut == "home");
    CHECK(play() < 1e-4f);  // nothing selected anywhere: silent
    // A separate MIDI channel sends to Notes; the rack keeps its Out input.
    f.exec("route.add",
           R"({"from":")" + part.str() + R"(","to":")" + notes.str() + R"(","midi":{}})");
    f.exec("track.set",
           R"({"track":")" + part.str() + R"(","record_midi":"Keys","armed":true,"monitor":"on"})");
    f.frame();
    CHECK(play() > 0.01f);
    REQUIRE(f.app->head().tracks.find(home)->record.input);
    // Unarmed, the channel's input is not heard; the module's own device is.
    f.exec("track.set", R"({"track":")" + part.str() + R"(","armed":false})");
    f.frame();
    CHECK(play() < 1e-4f);
    REQUIRE(openMenu());
    REQUIRE(f.pick("rack.module.menu", "MIDI input"));
    CHECK(checked("rack.notes.input") == std::vector<std::string>{"None"});
    REQUIRE(f.r().find("rack.notes.input"));
    CHECK(item(*f.r().find("rack.notes.input"), "Keys"));
    f.snapshot("rack-notes-input.png");
    REQUIRE(f.pick("rack.notes.input", "Keys"));
    CHECK(f.app->head().modules.find(notes)->device == std::optional<std::string>("Keys"));
    CHECK(play() > 0.01f);
    REQUIRE(f.pick("rack.module.menu", "MIDI input") == false);  // the menu closed
    menu = openMenu();
    REQUIRE(menu);
    CHECK(item(*menu, "MIDI input…")->shortcut == "Keys");
    REQUIRE(f.pick("rack.module.menu", "MIDI input"));
    REQUIRE(f.pick("rack.notes.input", "Any MIDI input"));
    CHECK(f.app->head().modules.find(notes)->device == std::optional<std::string>(""));
    CHECK(play() > 0.01f);
    REQUIRE(openMenu());
    REQUIRE(f.pick("rack.module.menu", "MIDI input"));
    CHECK(checked("rack.notes.input") == std::vector<std::string>{"Any MIDI input"});
    REQUIRE(f.pick("rack.notes.input", "None"));  // device: null
    CHECK_FALSE(f.app->head().modules.find(notes)->device);
    CHECK(play() < 1e-4f);
    // Plays track: another channel's MIDI, then Home again (source: null).
    REQUIRE(openMenu());
    REQUIRE(f.pick("rack.module.menu", "Plays track"));
    CHECK(checked("rack.notes.source") == std::vector<std::string>{"Home", "Hear MIDI sends"});
    f.snapshot("rack-notes-source.png");
    REQUIRE(f.pick("rack.notes.source", model::trackLabel(f.app->head(), part)));
    CHECK(f.app->head().modules.find(notes)->source == part);
    menu = openMenu();
    REQUIRE(menu);
    CHECK(item(*menu, "Plays track…")->shortcut == model::trackLabel(f.app->head(), part));
    REQUIRE(f.pick("rack.module.menu", "Plays track"));
    REQUIRE(f.pick("rack.notes.source", "Home"));
    CHECK_FALSE(f.app->head().modules.find(notes)->source);
    // One undo step each.
    f.exec("history.undo", "{}");
    CHECK(f.app->head().modules.find(notes)->source == part);
    // Hear MIDI sends: on by default, checkable at the menu's end.
    REQUIRE(openMenu());
    REQUIRE(f.pick("rack.module.menu", "Plays track"));
    REQUIRE(f.pick("rack.notes.source", "Hear MIDI sends"));
    CHECK_FALSE(f.app->head().modules.find(notes)->sourceSends);
    CHECK(f.app->head().modules.find(notes)->source == part);
    REQUIRE(openMenu());
    REQUIRE(f.pick("rack.module.menu", "Plays track"));
    CHECK(checked("rack.notes.source") ==
          std::vector<std::string>{model::trackLabel(f.app->head(), part)});
    REQUIRE(f.pick("rack.notes.source", "Hear MIDI sends"));
    CHECK(f.app->head().modules.find(notes)->sourceSends);
    f.app->stopEngine();
}

TEST_CASE("rack window: stopped, a patch that follows the song says to press play") {
    Fixture f;
    if (!f.ok) return;
    auto made = f.exec("rack.add", R"({"name":"Bells"})");
    const std::string rack = made["rack"].asString();
    f.racks->open();
    f.frame();
    const std::string hint = "Stopped: press play to start the patch";
    auto hinted = [&] { return f.racks->status().find(hint) != std::string::npos; };
    CHECK_FALSE(hinted());  // nothing in it follows the song
    // An instrument plays from notes: no hint.
    f.exec("rack.preset.load", R"({"rack":")" + rack + R"(","name":"Init Voice"})");
    f.frame();
    CHECK_FALSE(hinted());
    // Rain Bells runs from its Transport module and sequencers.
    f.exec("rack.remove", R"({"rack":")" + rack + R"("})");
    made = f.exec("rack.add", R"({"name":"Bells"})");
    f.exec("rack.preset.load",
           R"({"rack":")" + made["rack"].asString() + R"(","name":"Rain Bells"})");
    f.racks->open(model::Id::parse(made["rack"].asString()));
    f.frame();
    CHECK(hinted());
    f.snapshot("rack-stopped-hint.png");
    // Playing, it goes, without any edit; stopped again, it is back.
    engine::DummyBackend backend(
        engine::DummyBackend::Options{.mode = engine::DummyBackend::Mode::Manual});
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                               {.anticipative = false}));
    auto now = std::chrono::steady_clock::now();
    f.racks->tick(now += std::chrono::seconds(1));
    CHECK(hinted());
    f.app->engine()->play();
    backend.run(2);
    CHECK(f.racks->tick(now += std::chrono::seconds(1)));
    CHECK_FALSE(hinted());
    const auto* status = dynamic_cast<const ui::Label*>(f.r().find("rack.status"));
    REQUIRE(status);
    CHECK(status->valueText().find(hint) == std::string::npos);
    f.app->engine()->pause();
    backend.run(1);
    f.racks->tick(now += std::chrono::seconds(1));
    CHECK(hinted());
    CHECK(status->valueText().find(hint) != std::string::npos);
    f.app->stopEngine();
}

TEST_CASE("rack window: an empty rack invites a start, and a patch lands in it whole") {
    Fixture f;
    if (!f.ok) return;
    auto made = f.exec("rack.add", R"({"name":"Start"})");
    const model::Id out = *model::Id::parse(made["out"].asString());
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    REQUIRE(space.startHint());
    f.snapshot("rack-start.png");  // the golden is rack-empty.png
    // "start from a patch" opens the Patches view.
    const auto link = space.startLinkAt(true);
    REQUIRE(link);
    f.pointer(ui::PointerEvent::Type::Down, *link, 1);
    f.pointer(ui::PointerEvent::Type::Up, *link, 1);
    f.frame();
    CHECK(f.racks->patchesShown());
    // A double-click loads one: the rack's Out is its Out (nothing replaced),
    // moved into the patch's own layout, all in view, one undo step.
    const auto row = f.racks->browserRowAt("preset:Init Voice");
    REQUIRE(row);
    f.pointer(ui::PointerEvent::Type::Down, *row, 1);
    f.pointer(ui::PointerEvent::Type::Up, *row, 1);
    f.pointer(ui::PointerEvent::Type::Down, *row, 1, 0, 2);
    f.pointer(ui::PointerEvent::Type::Up, *row, 1);
    f.frame();
    const auto& modules = space.scene().modules;
    CHECK(modules.size() == 6);
    std::size_t outs = 0;
    for (const auto& m : modules) outs += m.out;
    CHECK(outs == 1);
    CHECK(space.scene().module(out));
    CHECK(space.scene().cables.size() == 9);
    // Init Voice's own places: its Out 792 right of and 98 under its Keys.
    const auto* keys = [&]() -> const gui::RackScene::Module* {
        for (const auto& m : modules)
            if (m.title == "Keys") return &m;
        return nullptr;
    }();
    REQUIRE(keys);
    CHECK(space.scene().module(out)->x - keys->x == doctest::Approx(792));
    CHECK(space.scene().module(out)->y - keys->y == doctest::Approx(98));
    const ui::Rect seen = space.visibleSpace();
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const ui::Rect r = modules[i].rect();
        CHECK(seen.contains({r.x, r.y}));
        CHECK(seen.contains({r.right(), r.bottom()}));
        for (std::size_t j = i + 1; j < modules.size(); ++j)
            CHECK(r.intersect(modules[j].rect()).empty());
    }
    CHECK_FALSE(space.startHint());
    f.snapshot("rack-start-loaded.png");
    f.exec("history.undo", "{}");
    f.frame();
    CHECK(space.scene().modules.size() == 1);
    CHECK(space.startHint());
    // "Add a module" opens the Modules view, its search focused.
    f.frame();
    const auto add = space.startLinkAt(false);
    REQUIRE(add);
    f.pointer(ui::PointerEvent::Type::Down, *add, 1);
    f.pointer(ui::PointerEvent::Type::Up, *add, 1);
    f.frame();
    CHECK_FALSE(f.racks->patchesShown());
}

TEST_CASE("rack window: a cable glows with its source's signal, even from out of view") {
    Fixture f;
    if (!f.ok) return;
    const std::string rack = f.exec("rack.add", R"({"name":"Glow"})")["rack"].asString();
    const std::string lfo = f.add(rack, "lfo", 0, 0), vca = f.add(rack, "vca", 1600, 0);
    const std::string far = f.add(rack, "drift", -2000, 0);
    f.cable(lfo + ".out", vca + ".in");
    f.racks->open();
    f.frame();
    const model::Id lfoId = *model::Id::parse(lfo), vcaId = *model::Id::parse(vca);
    const model::Id farId = *model::Id::parse(far);
    auto watched = [&](model::Id id) {
        const rack::Instance* in = gui::racks::instance(*f.app, id);
        REQUIRE(in);
        return in->watched();
    };

    SUBCASE("the source of a cable across the view is measured, not watched") {
        engine::DummyBackend backend(
            engine::DummyBackend::Options{.mode = engine::DummyBackend::Mode::Manual});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                                   {.anticipative = false}));
        backend.run(1);
        // Neither end in view, the cable's middle is.
        f.racks->space()->setView({600, -40, 1.0});
        f.frame();
        const auto sources = f.racks->space()->cableSources(f.racks->space()->visibleSpace());
        CHECK(sources == std::vector<model::Id>{lfoId});
        auto now = std::chrono::steady_clock::now();
        auto lfoModule = [&] { return f.racks->space()->scene().module(lfoId); };
        // Stopped, there is no level to read.
        f.racks->tick(now += std::chrono::seconds(1));
        CHECK_FALSE(lfoModule()->measured);
        f.app->engine()->play();
        backend.run(8);
        f.racks->tick(now += std::chrono::seconds(1));
        CHECK_FALSE(watched(lfoId));  // no telemetry for it
        CHECK_FALSE(watched(vcaId));  // takes it, feeds nothing
        CHECK_FALSE(watched(farId));
        CHECK_FALSE(lfoModule()->live);
        REQUIRE(lfoModule()->measured);
        const int out = lfoModule()->type->output("out");
        REQUIRE(out >= 0);
        REQUIRE(static_cast<std::size_t>(out) < lfoModule()->levels.size());
        CHECK(lfoModule()->levels[static_cast<std::size_t>(out)] ==
              doctest::Approx(static_cast<double>(f.app->moduleOutputLevel(lfoId, "out"))));
        CHECK(lfoModule()->levels[static_cast<std::size_t>(out)] > 0.0f);
        CHECK_FALSE(f.racks->space()->scene().module(farId)->measured);
        // Out of view altogether, it is not measured.
        f.racks->space()->setView({-6000, -40, 1.0});
        f.frame();
        f.racks->tick(now += std::chrono::seconds(1));
        CHECK_FALSE(lfoModule()->measured);
        // In view, it is watched again.
        f.racks->space()->setView({-20, -40, 1.0});
        f.frame();
        f.racks->tick(now += std::chrono::seconds(1));
        CHECK(watched(lfoId));
        CHECK_FALSE(lfoModule()->measured);
        f.app->stopEngine();
    }

    SUBCASE("the glow follows the level") {
        f.racks->space()->setView({-20, -40, 1.0});
        f.frame();
        auto* m = f.racks->space()->scene().module(lfoId);
        REQUIRE(m);
        REQUIRE(m->type);
        const auto from =
            f.racks->space()->scene().endPoint(f.racks->space()->scene().cables.at(0).from, true);
        REQUIRE(from);
        const std::size_t n = m->type->outputs().size();
        const int out = m->type->output("out");
        REQUIRE(out >= 0);
        // The light around the cable, away from its jack (whose own glow
        // follows the same level).
        auto light = [&](double level, bool measured = false) {
            // From its telemetry, or measured (out of view): the same glow.
            m->live = !measured;
            m->measured = measured;
            m->levels.assign(n, 0.0f);
            m->levels[static_cast<std::size_t>(out)] = static_cast<float>(level);
            m->telemetry.outputs.assign(n, 0.0);
            m->telemetry.peaks.assign(n, 0.0f);
            m->telemetry.channels.assign(n, 1);
            m->telemetry.outputs[static_cast<std::size_t>(out)] = level;
            m->telemetry.peaks[static_cast<std::size_t>(out)] = static_cast<float>(level);
            f.racks->space()->telemetryChanged();
            f.r().damageAll();
            f.r().frame(false);
            const gui::Image image = f.r().readBack();
            const ui::Point a = f.racks->space()->toWindow(*from);
            const ui::Rect seen = f.racks->space()->bounds();
            long sum = 0;
            for (int y = static_cast<int>(seen.y); y < static_cast<int>(seen.bottom()); ++y)
                for (int x = static_cast<int>(a.x) + 120; x < static_cast<int>(a.x) + 320; ++x) {
                    if (x < 0 || y < 0 || x >= image.width || y >= image.height) continue;
                    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
                    sum += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2];
                }
            return sum;
        };
        const long dark = light(0), lit = light(1);
        f.snapshot("rack-cable-glow.png");
        CHECK(lit > dark + dark / 50);
        CHECK(light(0) == dark);
        f.snapshot("rack-cable-quiet.png");
        CHECK(light(1, true) == lit);
        CHECK(light(0, true) == dark);
    }
}

// Job barriers for capture tests: a worker can be held at its start, and
// the test waits for it to finish (the engine's job hooks).
struct JobGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false, entered = false, finished = false;
    void configure(engine::AppOptions& o) {
        o.jobStartedHook = [this] {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return !hold; });
        };
        o.jobFinishedHook = [this] {
            std::lock_guard lock(mutex);
            finished = true;
            cv.notify_all();
        };
    }
    void holdNext() {
        std::lock_guard lock(mutex);
        hold = true;
        entered = finished = false;
    }
    void release() {
        std::lock_guard lock(mutex);
        hold = false;
        cv.notify_all();
    }
    bool waitEntered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(30), [this] { return entered; });
    }
    bool waitFinished() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(30), [this] { return finished; });
    }
};

// Releases a held worker when a test ends early (declared after the fixture).
struct Unhold {
    JobGate& gate;
    ~Unhold() { gate.release(); }
};

TEST_CASE("rack window: a sequencer's notes are captured to a track from its menu") {
    JobGate gate;
    Fixture f(ui::Theme::dark(), {}, 1100, 560, [&](engine::AppOptions& o) { gate.configure(o); });
    Unhold unhold{gate};
    if (!f.ok) return;
    const std::string rack = f.exec("rack.add", R"({"name":"Seq"})")["rack"].asString();
    const std::string seq = f.add(rack, "sequencer", 40, 40), vca = f.add(rack, "vca", 400, 40);
    const model::Id track = f.app->head().racks.find(*model::Id::parse(rack))->track;
    f.racks->open();
    f.frame();
    gui::RackSpace& space = *f.racks->space();
    auto menuOf = [&](const std::string& module) {
        const auto at = space.bodyAt(*model::Id::parse(module));
        REQUIRE(at);
        f.pointer(ui::PointerEvent::Type::Down, *at, 3);
        f.pointer(ui::PointerEvent::Type::Up, *at, 3);
        const ui::Node* menu = f.r().find("rack.module.menu");
        REQUIRE(menu);
        std::vector<std::string> labels;
        for (const auto& item : ui::menuItems(*menu)) labels.push_back(item.label);
        return labels;
    };
    auto has = [](const std::vector<std::string>& labels, const std::string& label) {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    };
    // A VCA plays no notes; a sequencer has pitch and gate outputs.
    CHECK_FALSE(has(menuOf(vca), "Capture notes to track\u2026"));
    f.r().closePopups();
    f.frame();
    REQUIRE(has(menuOf(seq), "Capture notes to track\u2026"));
    REQUIRE(f.pick("rack.module.menu", "Capture notes to track"));
    REQUIRE(f.pick("rack.capture.track", model::trackLabel(f.app->head(), track)));
    const ui::Node* lengths = f.r().find("rack.capture.length");
    REQUIRE(lengths);
    CHECK(ui::menuItems(*lengths).front().label == "From bar 1");
    f.snapshot("rack-capture-menu.png");
    REQUIRE(f.pick("rack.capture.length", "1 bar"));
    // It renders as a job: the status bar says so, with Stop, and the loop
    // is asked to follow it.
    const auto statusText = [&] {
        const ui::Node* status = f.r().find("rack.status");
        REQUIRE(status);
        return static_cast<const ui::Label*>(status)->text();
    };
    CHECK(f.racks->capturing());
    CHECK(statusText().find("Capturing notes\u2026") != std::string::npos);
    const ui::Node* stop = f.r().find("rack.capture.stop");
    REQUIRE(stop);
    CHECK(stop->visible);
    CHECK(f.racks->nextDeadline());
    // A second capture waits for it.
    {
        const auto at = space.bodyAt(*model::Id::parse(seq));
        f.pointer(ui::PointerEvent::Type::Down, *at, 3);
        f.pointer(ui::PointerEvent::Type::Up, *at, 3);
        const ui::Node* menu = f.r().find("rack.module.menu");
        REQUIRE(menu);
        for (const auto& item : ui::menuItems(*menu))
            if (item.label == "Capture notes to track\u2026") CHECK_FALSE(item.enabled);
        f.r().closePopups();
        f.frame();
    }
    f.snapshot("rack-capturing.png");
    REQUIRE(gate.waitFinished());
    f.dispatcher->flushJobEvents();  // its edit lands on the model thread
    CHECK(f.racks->tick());
    f.frame();
    CHECK_FALSE(f.racks->capturing());
    CHECK_FALSE(f.r().find("rack.capture.stop")->visible);
    // A MIDI item of a bar (4/4) on the channel, from the song's start:
    // the sequencer's sixteenths, each step a note.
    const model::Session& s = f.app->head();
    const model::Item* item = nullptr;
    for (const auto& e : s.items)
        if (e.value->track == track) item = e.value.get();
    REQUIRE(item);
    REQUIRE(item->activeTake);
    const model::Take* take = s.takes.find(*item->activeTake);
    REQUIRE(take);
    const model::MidiSource* midi = take->source ? s.midi.find(*take->source) : nullptr;
    REQUIRE(midi);
    CHECK(midi->sequence.notes.size() == 16);
    CHECK(midi->sequence.length == 4 * midi->sequence.ppq);
    CHECK(statusText().find("Captured 16 notes to ") != std::string::npos);
    f.snapshot("rack-capture.png");
    // One undo step takes it away.
    f.exec("history.undo", "{}");
    for (const auto& e : f.app->head().items) CHECK(e.value->track != track);
    f.frame();

    // Stop: nothing is added, also when the render ends afterwards.
    auto capture = [&] {
        REQUIRE(has(menuOf(seq), "Capture notes to track\u2026"));
        REQUIRE(f.pick("rack.module.menu", "Capture notes to track"));
        REQUIRE(f.pick("rack.capture.track", model::trackLabel(f.app->head(), track)));
        REQUIRE(f.pick("rack.capture.length", "1 bar"));
        REQUIRE(f.racks->capturing());
        REQUIRE(gate.waitEntered());
    };
    auto items = [&] {
        std::size_t n = 0;
        for (const auto& e : f.app->head().items) n += e.value->track == track;
        return n;
    };
    gate.holdNext();
    capture();
    {
        const ui::Rect b = f.r().find("rack.capture.stop")->bounds();
        f.pointer(ui::PointerEvent::Type::Down, {b.x + b.w / 2, b.y + b.h / 2});
        f.pointer(ui::PointerEvent::Type::Up, {b.x + b.w / 2, b.y + b.h / 2});
    }
    CHECK_FALSE(f.racks->capturing());
    CHECK(statusText().find("Capture stopped") != std::string::npos);
    f.snapshot("rack-capture-stopped.png");
    gate.release();
    REQUIRE(gate.waitFinished());
    f.dispatcher->flushJobEvents();
    f.racks->tick();
    f.frame();
    CHECK(items() == 0);

    // The rack changes while it renders: nothing is added, and it says so.
    gate.holdNext();
    capture();
    f.add(rack, "vca", 400, 300);
    f.frame();
    gate.release();
    REQUIRE(gate.waitFinished());
    f.dispatcher->flushJobEvents();
    CHECK(f.racks->tick());
    f.frame();
    CHECK_FALSE(f.racks->capturing());
    CHECK(items() == 0);
    CHECK(statusText() == "The rack or the channel changed while capturing; capture again");
    f.snapshot("rack-capture-stale.png");
}

TEST_CASE("rack window: Tidy up lays out the rack, or the selection, in one undo step") {
    Fixture f;
    if (!f.ok) return;
    auto made = f.exec("rack.add", R"({"name":"Heap"})");
    const std::string rack = made["rack"].asString(), out = made["out"].asString();
    // A heap: every module on the same spot.
    const std::string lfo = f.add(rack, "lfo", 300, 200), vco = f.add(rack, "vco", 300, 200);
    const std::string vca = f.add(rack, "vca", 300, 200), adsr = f.add(rack, "adsr", 300, 200);
    f.exec("module.set", R"({"module":")" + out + R"(","x":300,"y":200})");
    f.cable(lfo + ".out", vco + ".exp_fm");
    f.cable(vco + ".sine", vca + ".in");
    f.cable(adsr + ".envelope", vca + ".cv");
    f.cable(vca + ".out", out + ".l");
    f.racks->open();
    f.frame();
    const std::vector<std::string> all{lfo, vco, vca, adsr, out};
    auto place = [&](const std::string& id) {
        const auto* m = f.app->head().modules.find(*model::Id::parse(id));
        REQUIRE(m);
        return std::pair{m->x, m->y};
    };
    auto rectOf = [&](const std::string& id) {
        const auto* m = f.racks->space()->scene().module(*model::Id::parse(id));
        REQUIRE(m);
        return m->rect();
    };
    auto menu = [&](const std::string& label) {
        const ui::Rect b = f.racks->space()->bounds();
        const ui::Point empty{b.x + 20, b.y + 20};
        f.pointer(ui::PointerEvent::Type::Down, empty, 3);
        f.pointer(ui::PointerEvent::Type::Up, empty, 3);
        return f.pick("rack.space.menu", label);
    };
    SUBCASE("the whole rack") {
        REQUIRE(menu("Tidy up"));
        f.snapshot("rack-tidy.png");
        for (std::size_t i = 0; i < all.size(); ++i)
            for (std::size_t j = i + 1; j < all.size(); ++j) {
                CAPTURE(all[i]);
                CAPTURE(all[j]);
                CHECK(rectOf(all[i]).intersect(rectOf(all[j])).empty());
            }
        // From the heap's top-left, all in view.
        double left = 1e9, top = 1e9;
        for (const auto& id : all) {
            left = std::min(left, place(id).first);
            top = std::min(top, place(id).second);
        }
        CHECK(left == doctest::Approx(300));
        CHECK(top == doctest::Approx(200));
        const ui::Rect seen = f.racks->space()->visibleSpace();
        for (const auto& id : all) CHECK(!rectOf(id).intersect(seen).empty());
        // One undo step puts the heap back.
        f.exec("history.undo", "{}");
        f.frame();
        for (const auto& id : all) CHECK(place(id) == std::pair{300.0, 200.0});
    }

    SUBCASE("the selection") {
        f.racks->space()->select({*model::Id::parse(vco), *model::Id::parse(vca)});
        f.frame();
        REQUIRE(menu("Tidy up selection"));
        CHECK(rectOf(vco).intersect(rectOf(vca)).empty());
        for (const auto& id : {lfo, adsr, out}) CHECK(place(id) == std::pair{300.0, 200.0});
    }
}

TEST_CASE("face labels keep to their own room on every stock and host module") {
    Fixture f;
    if (!f.ok) return;
    const ui::Theme theme = ui::Theme::dark();
    ui::Canvas c(f.window->vg(), f.fonts, theme);
    const auto measure = [&](std::string_view s, float size) {
        return c.textWidth(s, ui::Font::Ui, size);
    };
    // Boxes may touch: a hair is not a collision.
    const auto meets = [](const ui::Rect& a, const ui::Rect& b) {
        const ui::Rect i = a.intersect(b);
        return i.w > 0.25f && i.h > 0.25f;
    };
    int types = 0, labels = 0;
    for (const auto* e : gui::racks::catalog(*f.app)->list()) {
        const rack::ModuleType& type = *e->type;
        if (type.origin() != rack::Origin::Stock && type.origin() != rack::Origin::Host) continue;
        ++types;
        const rack::PanelLayout& face = type.layout();
        const auto out = gui::racks::faceLabels(type, face, measure);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto& l = out[i];
            CAPTURE(type.id());
            CAPTURE(l.text);
            ++labels;
            REQUIRE(!l.lines.empty());
            // Every label is shown whole, at a size that can be read.
            CHECK(l.lines[0].find("…") == std::string::npos);
            CHECK(l.size >= gui::racks::kLabelMin);
            float widest = 0;
            for (const auto& line : l.lines) widest = std::max(widest, measure(line, l.size));
            CHECK(widest <= l.box.w + 0.01f);
            CHECK(l.box.x >= 0);
            CHECK(l.box.right() <= static_cast<float>(face.width));
            for (std::size_t k = 0; k < face.items.size(); ++k)
                if (k != l.item && meets(l.box, gui::racks::footprint(face.items[k]))) {
                    CAPTURE(k);
                    CHECK_MESSAGE(false, "under another control");
                }
            for (std::size_t j = i + 1; j < out.size(); ++j)
                if (meets(l.box, out[j].box)) {
                    CAPTURE(out[j].text);
                    CHECK_MESSAGE(false, "into another label");
                }
        }
    }
    CHECK(types > 40);
    CHECK(labels > 400);
}

TEST_CASE("rack window: every stock module's face, at 100 %") {
    Fixture f(ui::Theme::dark(), {}, 1400, 900);
    if (!f.ok) return;
    auto made = f.exec("rack.add", R"({"name":"Faces"})");
    const std::string rack = made["rack"].asString();
    // In rows as wide as the space in the window, as many whole rows to a
    // page as fit its height.
    struct Row {
        std::vector<const rack::ModuleType*> types;
        double height = 0;
    };
    std::vector<Row> rows(1);
    double x = 0;
    for (const auto* e : gui::racks::catalog(*f.app)->list()) {
        if (e->type->origin() != rack::Origin::Stock) continue;
        const auto& l = e->type->layout();
        if (x > 0 && x + l.width > 1120) {
            rows.emplace_back();
            x = 0;
        }
        rows.back().types.push_back(e->type.get());
        rows.back().height = std::max(rows.back().height, static_cast<double>(l.height));
        x += l.width + 20;
    }
    std::vector<double> pages{0};
    double y = 0;
    int count = 0;
    for (const Row& r : rows) {
        if (y > pages.back() && y + r.height - pages.back() > 760) pages.push_back(y);
        x = 0;
        for (const rack::ModuleType* t : r.types) {
            f.add(rack, t->id(), static_cast<int>(x), static_cast<int>(y));
            x += t->layout().width + 20;
            ++count;
        }
        y += r.height + 30;
    }
    // The rack's own Out is a stock face too: after the rest.
    f.exec("module.set", R"({"module":")" + made["out"].asString() + R"(","x":0,"y":)" +
                             std::to_string(static_cast<int>(y)) + "}");
    if (y > pages.back() && y + 200 - pages.back() > 760) pages.push_back(y);
    CHECK(count > 40);
    f.racks->open();
    f.frame();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        f.racks->space()->setView({-20, pages[i] - 20, 1.0});
        f.frame();
        const std::string name = "rack-faces-" + std::to_string(i + 1) + ".png";
        f.snapshot(name.c_str());
    }
}
