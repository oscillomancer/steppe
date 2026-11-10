// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Output gestures through the shell, Root pointer capture and the real bus.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include "supple/engine/realtime.h"

#include "routing_matrix.h"
#include "shell.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"

using namespace supple;

namespace {
const cmd::Origin kSendUi{cmd::Origin::Type::Ui, "gui", {}};
using Pointer = ui::PointerEvent::Type;

struct SendsFixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState state;
    gui::MeterFeed meters;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::Shell> shell;
    model::Id a{}, b{};
    std::vector<std::string> errors;
    std::vector<std::pair<model::Id, ui::Rect>> opened;

    explicit SendsFixture(bool busses = false) {
        dir = std::filesystem::path(SUPPLE_SKILL_DIR).parent_path() / "build" / "p5-sends" /
              ("fixture-" + std::to_string(testProcessId()));
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        REQUIRE(p);
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"send-test", 1100, 600, true});
        REQUIRE(w);
        window = std::move(*w);
        auto fonts = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        REQUIRE(fonts);
        engine::AppOptions options;
        options.workdir = dir;
        auto engineApp = engine::EngineApp::create(options);
        REQUIRE(engineApp);
        app = std::move(*engineApp);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        a = add(busses ? "buss" : "channel", "A");
        b = add("buss", "B");
        actions = std::make_unique<gui::AppActions>(registry, *dispatcher, *app, state,
                                                    dir / "keys.json");
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, *fonts, ui::Theme::dark());
        shell = std::make_unique<gui::Shell>(*app, *dispatcher, state, "", *peaks, meters);
        shell->onError = [this](const std::string& error) { errors.push_back(error); };
        shell->onRouting = [this](model::Id track, const ui::Rect& anchor) {
            opened.emplace_back(track, anchor);
        };
        root->setContent(shell->build(*root, *actions));
        shell->installOverlay(*root);
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            return actions->key(e.key, e.mods, false);
        };
        sync();
    }
    ~SendsFixture() {
        root.reset();  // meters detach while the shell and feed still exist
        shell.reset();
        peaks.reset();
        actions.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, json::Value args = json::Value::object()) {
        auto r = dispatcher->execute({command, std::move(args), kSendUi});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    }
    model::Id add(const std::string& type, const std::string& name) {
        json::Value args = json::Value::object();
        args["type"] = type;
        args["name"] = name;
        const auto result = exec("track.add", std::move(args));
        const auto id = model::Id::parse(result.find("track")->asString());
        REQUIRE(id);
        return *id;
    }
    void sync() {
        shell->sync();
        root->frame(false);
    }
    views::ArrangeView& view() { return *shell->arrange(); }
    ui::Node& out(model::Id track) {
        auto* node = view().tcpPanel().controlNode(track, "out");
        REQUIRE(node);
        return *node;
    }
    static ui::Point centre(const ui::Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }
    ui::Point target(model::Id track, bool lanes = false) {
        const auto row = view().trackLayout().indexOf(track);
        REQUIRE(row);
        return centre(lanes ? view().rowRect(*row) : view().tcpRowRect(*row));
    }
    std::size_t history() { return exec("history.list").find("versions")->size(); }
    json::Value routes() { return *exec("route.list").find("routes"); }
    void pointer(Pointer type, ui::Point p) {
        ui::PointerEvent e;
        e.type = type;
        e.button = 1;
        e.pos = p;
        root->pointer(e);
    }
    void drag(model::Id from, ui::Point to) {
        const auto before = history();
        const auto count = routes().size();
        pointer(Pointer::Down, centre(out(from).bounds()));
        pointer(Pointer::Move, to);
        root->frame(false);
        CHECK(history() == before);
        CHECK(routes().size() == count);
        REQUIRE(root->find("tcp.send-ghost"));
        pointer(Pointer::Up, to);
        sync();
    }
    void send(model::Id from, model::Id to) {
        json::Value args = json::Value::object();
        args["from"] = from.str();
        args["to"] = to.str();
        exec("route.add", std::move(args));
        sync();
    }
};
}  // namespace

TEST_CASE("TCP sends: output drag creates one default send and one undo step, never a second") {
    SendsFixture f;
    const auto before = f.history();
    CHECK(f.out(f.a).visible);
    CHECK(f.out(f.a).bounds().w == 28);
    CHECK(f.out(f.a).bounds().right() <= f.view().tcp().bounds().right());
    f.drag(f.a, f.target(f.b));
    const auto routes = f.routes();
    REQUIRE(routes.size() == 1);
    CHECK(routes.asArray()[0].find("from")->asString() == f.a.str());
    CHECK(routes.asArray()[0].find("to")->asString() == f.b.str());
    CHECK(routes.asArray()[0].find("tap")->asString() == "post_fader");
    CHECK(routes.asArray()[0].find("volume_db")->asDouble() == 0);
    CHECK(f.history() == before + 1);
    // A second drag onto a track it already sends to adds nothing.
    f.drag(f.a, f.target(f.b));
    f.drag(f.a, f.target(f.b, true));
    CHECK(f.routes().size() == 1);
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    CHECK(f.routes().size() == 0);
}

TEST_CASE("TCP sends: arrange lanes and automation lanes accept output drops") {
    SendsFixture f;
    auto before = f.history();
    f.drag(f.a, f.target(f.b, true));
    REQUIRE(f.routes().size() == 1);
    CHECK(f.routes().asArray()[0].find("to")->asString() == f.b.str());
    CHECK(f.history() == before + 1);
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    json::Value args = json::Value::object();
    args["track"] = f.b.str();
    args["param"] = "volume";
    const auto result = f.exec("env.add", std::move(args));
    const auto env = model::Id::parse(result.find("env")->asString());
    REQUIRE(env);
    f.sync();
    const auto row = f.view().trackLayout().indexOfEnv(*env);
    REQUIRE(row);
    before = f.history();
    f.drag(f.a, SendsFixture::centre(f.view().rowRect(*row)));
    REQUIRE(f.routes().size() == 1);
    CHECK(f.routes().asArray()[0].find("to")->asString() == f.b.str());
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: own strip, empty space and outside targets do nothing") {
    SendsFixture f;
    const auto before = f.history();
    f.drag(f.a, f.target(f.a));
    f.drag(f.a, f.target(f.a, true));
    const auto tcp = f.view().tcp().bounds();
    f.drag(f.a, {tcp.x + 10, tcp.bottom() - 5});
    f.drag(f.a, {1200, f.target(f.b).y});
    f.drag(f.a, {tcp.x + 10, f.view().ruler().bounds().y + 10});
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    CHECK(f.opened.empty());
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: cycles and strip-type rules are reported without edits") {
    SendsFixture f(true);
    f.send(f.b, f.a);
    const auto before = f.history();
    f.drag(f.a, f.target(f.b));
    CHECK(f.routes().size() == 1);
    CHECK(f.history() == before);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().find("route.cycle") != std::string::npos);
    const auto channel = f.add("channel", "C");
    f.sync();
    const auto n = f.history();
    f.drag(f.a, f.target(channel));
    CHECK(f.routes().size() == 1);
    CHECK(f.history() == n);
    REQUIRE(f.errors.size() == 2);
    CHECK(f.errors.back().find("route.strip_type") != std::string::npos);
}

TEST_CASE("TCP sends: a send into a child is refused by the parent path") {
    SendsFixture f(true);
    json::Value args = json::Value::object();
    args["track"] = f.b.str();
    args["parent"] = f.a.str();
    f.exec("track.move", std::move(args));
    f.sync();
    const auto before = f.history();
    f.drag(f.a, f.target(f.b, true));
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().find("route.cycle") != std::string::npos);
}

TEST_CASE("TCP sends: click anchors the routing callback; short rows hide output") {
    SendsFixture f;
    const auto before = f.history();
    const auto anchor = f.out(f.a).bounds();
    REQUIRE(f.root->click(f.out(f.a).id()));
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0].first == f.a);
    CHECK(f.opened[0].second.x == anchor.x);
    CHECK(f.opened[0].second.y == anchor.y);
    CHECK(f.opened[0].second.w == anchor.w);
    CHECK(f.history() == before);
    CHECK(f.routes().size() == 0);
    f.view().trackLayout().setTrackHeight(f.a, 30);
    f.root->relayout();
    f.root->frame(false);
    CHECK_FALSE(f.out(f.a).visible);
}

TEST_CASE("TCP sends: Escape cancels the overlay gesture and later drags still work") {
    SendsFixture f;
    const auto before = f.history();
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    f.pointer(Pointer::Move, f.target(f.b, true));
    f.root->frame(false);
    f.root->key(SDLK_ESCAPE);
    f.pointer(Pointer::Up, f.target(f.b, true));
    f.sync();
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    f.drag(f.a, f.target(f.b));
    CHECK(f.routes().size() == 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: pooling during a captured drag keeps the original source") {
    SendsFixture f;
    model::Id destination = f.b;
    for (int i = 0; i < 12; ++i) destination = f.add("buss", "Extra " + std::to_string(i));
    f.sync();
    const auto before = f.history();
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    auto v = f.view().viewport();
    v.scrollY = 10000;
    f.view().setViewport(v);
    f.root->frame(false);
    CHECK(f.view().tcpPanel().rowNode(f.a) == nullptr);
    const auto target = f.target(destination, true);
    f.pointer(Pointer::Move, target);
    f.root->frame(false);
    CHECK(f.history() == before);
    f.pointer(Pointer::Up, target);
    f.sync();
    const auto routes = f.routes();
    REQUIRE(routes.size() == 1);
    CHECK(routes.asArray()[0].find("from")->asString() == f.a.str());
    CHECK(routes.asArray()[0].find("to")->asString() == destination.str());
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: overlay survives replacement of its owning view") {
    SendsFixture f;
    const auto overlays = f.root->overlay().children().size();
    f.view().installOverlay(*f.root);
    CHECK(f.root->overlay().children().size() == overlays);
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    f.pointer(Pointer::Move, f.target(f.b, true));
    f.root->frame(false);
    f.root->setContent(std::make_unique<ui::Node>("replacement"));
    CHECK(f.root->overlay().children().empty());
    f.root->frame(false);
    CHECK(f.routes().size() == 0);
}

TEST_CASE("TCP pooled gestures cancel when scrolling rebinds their row") {
    SendsFixture f;
    for (int i = 0; i < 12; ++i) f.add("channel", "Extra " + std::to_string(i));
    f.sync();
    ui::Point press{};
    SUBCASE("fader") {
        press = SendsFixture::centre(f.view().tcpPanel().controlNode(f.a, "fader")->bounds());
    }
    SUBCASE("pan") {
        press = SendsFixture::centre(f.view().tcpPanel().controlNode(f.a, "pan")->bounds());
    }
    SUBCASE("resize") {
        const auto bounds = f.view().tcpPanel().rowNode(f.a)->bounds();
        press = {bounds.x + 10, bounds.bottom() - 2};
    }
    SUBCASE("rename") {
        REQUIRE(f.view().tcpPanel().rename(f.a));
        f.root->text("Renamed");
    }
    const auto before = f.history();
    if (press.x != 0) f.pointer(Pointer::Down, press);
    auto viewport = f.view().viewport();
    viewport.scrollY = 10000;
    f.view().setViewport(viewport);
    f.root->frame(false);
    CHECK(f.view().tcpPanel().rowNode(f.a) == nullptr);
    if (press.x != 0) {
        f.pointer(Pointer::Move, {press.x + 30, press.y + 20});
        f.pointer(Pointer::Up, {press.x + 30, press.y + 20});
    } else
        f.root->key(SDLK_RETURN);
    CHECK(f.history() == before);
    CHECK(f.app->head().tracks.find(f.a)->name == "A");
}

TEST_CASE("shell selection sync replaces equal-sized selections and drops deleted items") {
    SendsFixture f;
    auto add = [&] {
        json::Value args = json::Value::object();
        args["track"] = f.a.str();
        args["notation"] = "voice a; 1: C4:w";
        const auto result = f.exec("item.add", std::move(args));
        auto id = model::Id::parse(result.find("item")->asString());
        REQUIRE(id);
        return *id;
    };
    const auto a = add(), b = add();
    f.state.selectedItems = {a.str()};
    f.sync();
    CHECK(f.view().gestures().selection() == std::vector<model::Id>{a});
    f.state.selectedItems = {b.str()};
    f.sync();
    CHECK(f.view().gestures().selection() == std::vector<model::Id>{b});
    json::Value args = json::Value::object();
    args["item"] = b.str();
    f.exec("item.remove", std::move(args));
    f.sync();
    CHECK(f.view().gestures().selection().empty());
    CHECK(f.state.selectedItems.empty());
    f.exec("history.undo", json::Value::object());
    f.sync();
    REQUIRE(f.app->head().items.find(b));
    CHECK(f.state.selectedItems.empty());
    f.state.selectedTracks = {f.b.str()};
    args = json::Value::object();
    args["track"] = f.b.str();
    f.exec("track.remove", std::move(args));
    f.sync();
    CHECK(f.state.selectedTracks.empty());
    f.exec("history.undo", json::Value::object());
    f.sync();
    CHECK(f.state.selectedTracks.empty());
}

TEST_CASE("TCP rows refresh strip type and depth while keeping the same identity") {
    SendsFixture f(true);  // two busses: a channel would sort ahead of them
    json::Value args = json::Value::object();
    args["track"] = f.b.str();
    args["index"] = 0;
    f.exec("track.move", std::move(args));
    f.sync();
    auto* name = f.view().tcpPanel().controlNode(f.a, "name");
    REQUIRE(name);
    const float before = name->bounds().x;
    args = json::Value::object();
    args["track"] = f.a.str();
    args["parent"] = f.b.str();
    f.exec("track.move", std::move(args));
    f.sync();
    CHECK(f.view().tcpPanel().controlNode(f.a, "name") == name);
    CHECK(name->bounds().x == doctest::Approx(static_cast<double>(before + 14)));
    auto* arm = f.view().tcpPanel().controlNode(f.a, "arm");
    REQUIRE(arm);
    CHECK_FALSE(arm->visible);
    args = json::Value::object();
    args["track"] = f.a.str();
    args["type"] = "channel";
    f.exec("track.set", std::move(args));
    f.sync();
    CHECK(arm->visible);
}

TEST_CASE("TCP: a selection survives a right-click; Delete removes all of it in one step") {
    SendsFixture f;
    const model::Id c = f.add("channel", "C");
    f.sync();
    auto press = [&](model::Id track, int button, unsigned mods) {
        const auto row = f.view().trackLayout().indexOf(track);
        REQUIRE(row);
        const ui::Rect r = f.view().tcpRowRect(*row);
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.button = button;
        e.mods = mods;
        e.pos = {r.x + 60, r.y + 10};
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.sync();
    };
    press(f.a, 1, 0);
    press(c, 1, ui::kCtrl);
    CHECK(f.state.selectedTracks == std::vector<std::string>{f.a.str(), c.str()});
    // A right-click on one of them keeps both; its menu deletes both, one step.
    press(c, 3, 0);
    CHECK(f.state.selectedTracks == std::vector<std::string>{f.a.str(), c.str()});
    const std::string menu = "track.menu." + c.str();
    ui::Node* m = f.root->find(menu);
    REQUIRE(m);
    const auto before = f.history();
    const ui::Point del{
        m->bounds().x + 40,
        m->bounds().y + static_cast<float>(ui::menuItems(*m).size() - 1) * 26 + 13};  // Delete
    f.pointer(Pointer::Move, del);
    f.pointer(Pointer::Down, del);
    f.pointer(Pointer::Up, del);
    f.sync();
    CHECK_FALSE(f.app->head().tracks.find(f.a));
    CHECK_FALSE(f.app->head().tracks.find(c));
    CHECK(f.app->head().tracks.find(f.b));
    CHECK(f.history() == before + 1);
    CHECK(f.state.selectedTracks.empty());
    f.exec("history.undo");
    f.sync();
    CHECK(f.app->head().tracks.find(f.a));
    CHECK(f.app->head().tracks.find(c));
    // A right-click on a track outside the selection selects it alone.
    press(f.a, 1, 0);
    press(c, 1, ui::kCtrl);
    press(f.b, 3, 0);
    CHECK(f.state.selectedTracks == std::vector<std::string>{f.b.str()});
    f.root->closePopup();
    // The Delete key removes the selected tracks (tracks were selected last).
    press(f.a, 1, 0);
    press(c, 1, ui::kShift);  // a range: A, C (B, a buss, lists after them)
    CHECK(f.state.selectedTracks.size() == 2);
    const auto keyed = f.history();
    f.root->key(SDLK_DELETE);
    f.sync();
    CHECK_FALSE(f.app->head().tracks.find(f.a));
    CHECK_FALSE(f.app->head().tracks.find(c));
    CHECK(f.history() == keyed + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP: the right-click menu renames, colours and deletes a track (one undo step each)") {
    SendsFixture f;
    const auto row = f.view().trackLayout().indexOf(f.a);
    REQUIRE(row);
    const ui::Rect r = f.view().tcpRowRect(*row);
    auto rightClick = [&] {
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.button = 3;
        e.pos = {r.x + 60, r.y + 10};
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.root->frame(false);
    };
    // Menu rows are 26 px; track choices are located by label.
    auto pickRow = [&](const std::string& menu, int index) {
        ui::Node* m = f.root->find(menu);
        REQUIRE(m);
        const ui::Point p{m->bounds().x + 40, m->bounds().y + static_cast<float>(index) * 26 + 13};
        f.pointer(Pointer::Move, p);
        f.pointer(Pointer::Down, p);
        f.pointer(Pointer::Up, p);
        f.sync();
    };
    const std::string menu = "track.menu." + f.a.str();
    auto pickTrack = [&](std::string_view label) {
        auto* m = f.root->find(menu);
        REQUIRE(m);
        const auto items = ui::menuItems(*m);
        auto at = std::find_if(items.begin(), items.end(),
                               [&](const auto& item) { return item.label == label; });
        REQUIRE(at != items.end());
        pickRow(menu, static_cast<int>(at - items.begin()));
    };
    // Color… opens the swatches; teal is the fifth. A selected track's
    // colour goes to the whole selection.
    f.state.selectedTracks = {f.a.str(), f.b.str()};
    rightClick();
    REQUIRE(f.root->find(menu));
    auto before = f.history();
    pickTrack("Color…");
    REQUIRE(f.root->find("track.color." + f.a.str()));
    pickRow("track.color." + f.a.str(), 4);
    CHECK(f.app->head().tracks.find(f.a)->color == "teal");
    CHECK(f.app->head().tracks.find(f.b)->color == "teal");
    CHECK(f.history() == before + 1);
    f.state.selectedTracks = {f.a.str()};
    // Rename edits the name in place.
    rightClick();
    pickTrack("Rename");
    auto* field = dynamic_cast<ui::TextField*>(f.view().tcpPanel().controlNode(f.a, "rename"));
    REQUIRE(field);
    CHECK(field->visible);
    field->setText("Renamed");
    f.root->key(SDLK_RETURN);
    f.sync();
    CHECK(f.app->head().tracks.find(f.a)->name == "Renamed");
    // Timebase… (channels): what its items follow; "Follow time" is the third.
    before = f.history();
    rightClick();
    pickTrack("Timebase…");
    REQUIRE(f.root->find("track.timebase." + f.a.str()));
    pickRow("track.timebase." + f.a.str(), 2);
    CHECK(f.app->head().tracks.find(f.a)->timebase == model::TrackTimebase::Time);
    CHECK(f.history() == before + 1);
    // Channels…: four gives it aux channels 3/4 (the third choice).
    before = f.history();
    rightClick();
    pickTrack("Channels…");
    REQUIRE(f.root->find("track.channels." + f.a.str()));
    pickRow("track.channels." + f.a.str(), 2);
    CHECK(f.app->head().tracks.find(f.a)->channels == 4);
    CHECK(f.history() == before + 1);
    // A channel's racks (ADR-0062): Add rack, then Show rack and Add another.
    std::vector<std::string> added, shown;
    auto record = [](std::vector<std::string>& into) {
        return [&into](const json::Value& v) -> Status {
            into.push_back(v.asString());
            return {};
        };
    };
    f.actions->addUi("ui.rack.add", "Racks: add rack", "track", record(added));
    f.actions->addUi("ui.rack.show", "Racks: show rack", "track", record(shown));
    rightClick();
    pickTrack("Add rack");
    CHECK(added == std::vector<std::string>{f.a.str()});
    json::Value rack = json::Value::object();
    rack["track"] = f.a.str();
    f.exec("rack.add", std::move(rack));
    f.sync();
    rightClick();
    pickTrack("Show rack");
    CHECK(shown == std::vector<std::string>{f.a.str()});
    rightClick();
    pickTrack("Add another rack");
    CHECK(added == std::vector<std::string>{f.a.str(), f.a.str()});
    // Delete removes it.
    before = f.history();
    rightClick();
    pickTrack("Delete");
    CHECK_FALSE(f.app->head().tracks.find(f.a));
    CHECK(f.history() == before + 1);
}

TEST_CASE("TCP: \"fx\" opens the chain and \"+\" the plugin browser, even for an empty chain") {
    SendsFixture f;
    std::vector<std::string> added, chains;
    f.actions->addUi("ui.fx.add", "FX: add plugin…", "track", [&](const json::Value& v) -> Status {
        added.push_back(v.asString());
        return {};
    });
    f.actions->addUi("ui.fx.chain", "FX: chain…", "track", [&](const json::Value& v) -> Status {
        chains.push_back(v.asString());
        return {};
    });
    auto* fx = f.view().tcpPanel().controlNode(f.a, "fx");
    auto* plus = f.view().tcpPanel().controlNode(f.a, "addfx");
    REQUIRE(fx);
    REQUIRE(plus);
    CHECK(fx->valueText() == "fx");
    CHECK(plus->valueText() == "+");
    REQUIRE(f.root->click(fx->id()));
    CHECK(chains == std::vector<std::string>{f.a.str()});
    CHECK(added.empty());
    REQUIRE(f.root->click(plus->id()));
    CHECK(added == std::vector<std::string>{f.a.str()});
}

TEST_CASE("TCP: the wheel over the tracks scrolls them and the lanes together, never a control") {
    SendsFixture f;
    std::vector<model::Id> tracks;
    for (int i = 0; i < 30; ++i) tracks.push_back(f.add("channel", "T" + std::to_string(i)));
    f.sync();
    auto wheel = [&](ui::Point p, float dy, unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = Pointer::Wheel;
        e.pos = p;
        e.wheelY = dy;
        e.mods = mods;
        f.root->pointer(e);
        f.sync();
    };
    auto volumes = [&] {
        std::vector<double> v;
        for (const auto& t : f.app->head().tracks) v.push_back(t.value->volumeDb);
        return v;
    };
    // What the eye checks: every shown track row sits level with its lane row.
    auto aligned = [&] {
        int shown = 0;
        for (const model::Id& t : tracks) {
            ui::Node* row = f.view().tcpPanel().rowNode(t);
            if (!row) continue;
            const auto index = f.view().trackLayout().indexOf(t);
            REQUIRE(index);
            INFO("track " << t.str());
            CHECK(static_cast<double>(row->bounds().y) ==
                  doctest::Approx(static_cast<double>(f.view().rowRect(*index).y)));
            ++shown;
        }
        return shown;
    };
    const auto before = volumes();
    const ui::Rect tcp = f.view().tcpPanel().bounds();
    float last = f.view().viewport().scrollY;
    for (float fy : {0.15f, 0.3f, 0.45f, 0.6f, 0.75f})
        for (float fx : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.98f}) {
            wheel({tcp.x + tcp.w * fx, tcp.y + tcp.h * fy}, -1.0f);
            const float now = f.view().viewport().scrollY;
            INFO("at " << fx << "," << fy << " scroll " << last << " -> " << now);
            CHECK(now > last);
            CHECK(aligned() > 0);
            if (now > 600) wheel({tcp.x + 20, tcp.y + 40}, 30.0f);  // back to the top
            last = f.view().viewport().scrollY;
        }
    CHECK(volumes() == before);  // no fader or pan moved

    // Over the lanes the wheel keeps the time under the pointer in place (with
    // following off; following holds the cursor instead).
    f.view().setFollow(views::Follow::Off);
    f.view().setPlayhead(4.0);
    f.sync();
    const ui::Rect lanes = f.view().lanesRect();
    const float scroll = f.view().viewport().scrollY;
    const double pps = f.view().viewport().pixelsPerSecond;
    const float x = lanes.x + lanes.w / 2;
    const double time = f.view().lanes().timeAt(x);
    wheel({x, lanes.y + lanes.h / 2}, 1.0f);
    CHECK(f.view().viewport().pixelsPerSecond > pps);
    CHECK(f.view().lanes().timeAt(x) == doctest::Approx(time).epsilon(0.001));
    CHECK(f.view().viewport().scrollY == scroll);
    // Ctrl scrolls the tracks from the lanes too, still aligned.
    wheel({lanes.x + lanes.w / 2, lanes.y + lanes.h / 2}, -1.0f, ui::kCtrl);
    CHECK(f.view().viewport().scrollY > scroll);
    CHECK(aligned() > 0);
}

TEST_CASE("TCP sends: output click opens the existing routing matrix action") {
    SendsFixture f;
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    f.shell->onRouting = {};
    const auto before = f.history();
    REQUIRE(f.root->click(f.out(f.a).id()));
    f.root->frame(false);
    CHECK(matrix.isOpen());
    REQUIRE(panel.root().find("routing"));
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    panel.root().key(SDLK_ESCAPE);
    CHECK_FALSE(matrix.isOpen());
}

TEST_CASE("the Delete key removes the selected items") {
    SendsFixture f;
    json::Value args = json::Value::object();
    args["track"] = f.a.str();
    args["notation"] = "voice a; 1: C4:w";
    const auto item = f.exec("item.add", std::move(args));
    const std::string id = item.find("item")->asString();
    f.state.selectedItems = {id};
    f.sync();
    f.root->key(SDLK_DELETE);
    f.sync();
    CHECK(f.app->head().items.size() == 0);
}

TEST_CASE("TCP: dragging a row's bottom edge sets that track's height; a double-click resets it") {
    SendsFixture f;
    const auto row = f.view().trackLayout().indexOf(f.a);
    REQUIRE(row);
    const ui::Rect r = f.view().tcpRowRect(*row);
    const float before = r.h;
    const ui::Point edge{r.x + 100, r.bottom() - 2};
    const auto history = f.history();
    f.pointer(Pointer::Down, edge);
    f.pointer(Pointer::Move, {edge.x, edge.y + 40});
    f.pointer(Pointer::Up, {edge.x, edge.y + 40});
    f.sync();
    CHECK(f.view().trackLayout().hasTrackHeight(f.a));
    CHECK(double(f.view().tcpRowRect(*row).h) == doctest::Approx(double(before) + 40));
    // The lanes follow: the next row starts lower by as much.
    CHECK(double(f.view().rowRect(*row + 1).y) == doctest::Approx(double(r.y + before) + 40));
    CHECK(f.history() == history);  // UI state, not an undo step
    ui::PointerEvent twice{Pointer::Down, {edge.x, r.y + before + 40 - 2}};
    twice.button = 1;
    twice.clicks = 2;
    f.root->pointer(twice);
    twice.type = Pointer::Up;
    f.root->pointer(twice);
    f.sync();
    CHECK_FALSE(f.view().trackLayout().hasTrackHeight(f.a));
    CHECK(double(f.view().tcpRowRect(*row).h) == doctest::Approx(double(before)));
}

namespace {
std::vector<model::Id> trackRows(SendsFixture& f) {
    std::vector<model::Id> out;
    for (const views::TrackRow& r : f.view().trackLayout().rows())
        if (!r.env) out.push_back(r.track);
    return out;
}
std::vector<model::Id> masterChildren(SendsFixture& f) {
    return f.app->head().tracks.find(model::kMaster)->children;
}
// A press on the row's empty part (its name area), then the move and release.
ui::Point emptyPartOf(SendsFixture& f, model::Id track) {
    const ui::Rect r = f.view().tcpRowRect(*f.view().trackLayout().indexOf(track));
    return {r.x + 90, r.y + 10};
}
ui::Point above(SendsFixture& f, model::Id track) {
    const ui::Rect r = f.view().tcpRowRect(*f.view().trackLayout().indexOf(track));
    return {r.x + 90, r.y + 4};
}
ui::Point below(SendsFixture& f, model::Id track) {
    const ui::Rect r = f.view().tcpRowRect(*f.view().trackLayout().indexOf(track));
    return {r.x + 90, r.bottom() - 8};
}
}  // namespace

TEST_CASE("TCP: busses are listed after the channels, whatever the model's order") {
    SendsFixture f;  // a channel A, a buss B
    const model::Id c = f.add("channel", "C");
    const model::Id d = f.add("buss", "D");
    f.sync();
    CHECK(masterChildren(f) ==
          std::vector<model::Id>{f.a, f.b, c, d});  // the model keeps its order
    CHECK(trackRows(f) == std::vector<model::Id>{f.a, c, f.b, d});
}

TEST_CASE("TCP: dragging a row's empty part reorders it among its own type, one undo step") {
    SendsFixture f;  // A channel, B buss
    const model::Id c = f.add("channel", "C");
    const model::Id d = f.add("channel", "D");
    const model::Id e = f.add("buss", "E");
    f.sync();
    REQUIRE(trackRows(f) == std::vector<model::Id>{f.a, c, d, f.b, e});
    const auto before = f.history();

    // Drag D to the top of the list: it goes first among the channels.
    f.pointer(Pointer::Down, emptyPartOf(f, d));
    f.pointer(Pointer::Move, above(f, f.a));
    f.root->frame(false);
    CHECK(f.root->find("tcp.reorder-ghost"));
    CHECK(f.history() == before);  // nothing is committed until the release
    f.pointer(Pointer::Up, above(f, f.a));
    f.sync();
    CHECK_FALSE(f.root->find("tcp.reorder-ghost"));
    CHECK(trackRows(f) == std::vector<model::Id>{d, f.a, c, f.b, e});
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    CHECK(trackRows(f) == std::vector<model::Id>{f.a, c, d, f.b, e});

    // Dropped below the busses, a channel only goes to the end of the channels.
    f.pointer(Pointer::Down, emptyPartOf(f, f.a));
    f.pointer(Pointer::Move, below(f, e));
    f.pointer(Pointer::Up, below(f, e));
    f.sync();
    CHECK(trackRows(f) == std::vector<model::Id>{c, d, f.a, f.b, e});

    // A buss moves among busses, never in between channels.
    f.pointer(Pointer::Down, emptyPartOf(f, e));
    f.pointer(Pointer::Move, above(f, c));
    f.pointer(Pointer::Up, above(f, c));
    f.sync();
    CHECK(trackRows(f) == std::vector<model::Id>{c, d, f.a, e, f.b});
    CHECK(f.errors.empty());
}

TEST_CASE("TCP: dragging one of several selected tracks moves them all, one undo step") {
    SendsFixture f;  // A channel, B buss
    const model::Id c = f.add("channel", "C");
    const model::Id d = f.add("channel", "D");
    f.sync();
    REQUIRE(trackRows(f) == std::vector<model::Id>{f.a, c, d, f.b});
    f.state.selectedTracks = {f.a.str(), c.str()};
    f.sync();
    const auto before = f.history();
    // The press keeps the selection; the drop puts both after D, in order.
    f.pointer(Pointer::Down, emptyPartOf(f, f.a));
    CHECK(f.state.selectedTracks == std::vector<std::string>{f.a.str(), c.str()});
    f.pointer(Pointer::Move, below(f, d));
    f.pointer(Pointer::Up, below(f, d));
    f.sync();
    CHECK(trackRows(f) == std::vector<model::Id>{d, f.a, c, f.b});
    CHECK(f.history() == before + 1);
    // A click without a drag selects the track alone.
    f.pointer(Pointer::Down, emptyPartOf(f, c));
    f.pointer(Pointer::Up, emptyPartOf(f, c));
    f.sync();
    CHECK(f.state.selectedTracks == std::vector<std::string>{c.str()});
    CHECK(f.errors.empty());
}

TEST_CASE("TCP: Escape cancels a row drag; a plain click or a no-op drop commits nothing") {
    SendsFixture f;
    const model::Id c = f.add("channel", "C");
    f.sync();
    const auto before = f.history();
    f.pointer(Pointer::Down, emptyPartOf(f, c));
    f.pointer(Pointer::Move, above(f, f.a));
    f.root->frame(false);
    REQUIRE(f.root->find("tcp.reorder-ghost"));
    f.root->key(SDLK_ESCAPE);
    f.pointer(Pointer::Up, above(f, f.a));
    f.sync();
    CHECK_FALSE(f.root->find("tcp.reorder-ghost"));
    CHECK(trackRows(f) == std::vector<model::Id>{f.a, c, f.b});
    // A click selects, and nothing moves.
    f.pointer(Pointer::Down, emptyPartOf(f, c));
    f.pointer(Pointer::Up, emptyPartOf(f, c));
    // Dropping a track where it already is (just below itself) is not an edit.
    f.pointer(Pointer::Down, emptyPartOf(f, c));
    f.pointer(Pointer::Move, below(f, c));
    f.pointer(Pointer::Up, below(f, c));
    f.sync();
    CHECK(f.history() == before);
    // A buss with no other buss has nowhere to go among the channels.
    f.pointer(Pointer::Down, emptyPartOf(f, f.b));
    f.pointer(Pointer::Move, above(f, f.a));
    f.pointer(Pointer::Up, above(f, f.a));
    f.sync();
    CHECK(f.history() == before);
    CHECK(trackRows(f) == std::vector<model::Id>{f.a, c, f.b});
}

TEST_CASE("TCP: a selected row's controls act on every selected track; Space stays play/stop") {
    SendsFixture f;
    const model::Id c = f.add("channel", "C");
    const model::Id d = f.add("channel", "D");
    f.state.selectedTracks = {f.a.str(), f.b.str(), c.str()};
    f.sync();
    auto* mute = f.view().tcpPanel().controlNode(f.a, "mute");
    REQUIRE(mute);
    const auto before = f.history();
    REQUIRE(f.root->click(mute->id()));
    CHECK(f.app->head().tracks.find(f.a)->mute);
    CHECK(f.app->head().tracks.find(f.b)->mute);
    CHECK(f.app->head().tracks.find(c)->mute);
    CHECK_FALSE(f.app->head().tracks.find(d)->mute);
    CHECK(f.history() == before + 1);
    f.sync();
    // Arm: the channels of the selection (B is a buss).
    REQUIRE(f.root->click(f.view().tcpPanel().controlNode(c, "arm")->id()));
    CHECK(f.app->head().tracks.find(f.a)->record.armed);
    CHECK(f.app->head().tracks.find(c)->record.armed);
    CHECK_FALSE(f.app->head().tracks.find(d)->record.armed);
    f.sync();
    // Space after a click never presses that control again.
    const bool muted = f.app->head().tracks.find(f.a)->mute;
    f.root->key(SDLK_SPACE);
    CHECK(f.app->head().tracks.find(f.a)->mute == muted);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP: a click anywhere else ends a rename and keeps the edit") {
    SendsFixture f;
    REQUIRE(f.view().tcpPanel().rename(f.a));
    f.sync();
    auto* field = dynamic_cast<ui::TextField*>(f.view().tcpPanel().controlNode(f.a, "rename"));
    REQUIRE(field);
    REQUIRE(f.root->focused() == field);
    f.root->text("Guitar");
    const ui::Rect lanes = f.view().lanesRect();
    f.root->pointer({ui::PointerEvent::Type::Down, {lanes.right() - 20, lanes.bottom() - 20}, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {lanes.right() - 20, lanes.bottom() - 20}, 1});
    CHECK(f.app->head().tracks.find(f.a)->name == "Guitar");
    CHECK_FALSE(field->visible);
    CHECK(f.root->focused() != field);
}

TEST_CASE("a master's outputs menu lists the device's outputs and sets hw_output (ADR-0056)") {
    SendsFixture f;
    const model::Id cue = f.add("master", "Cue");
    f.sync();
    // Stopped: the device's outputs are unknown; None is still offered.
    f.shell->openOutputMenu(cue, {10, 10, 40, 18});
    const auto* menu = f.root->find("output." + cue.str());
    REQUIRE(menu);
    CHECK(ui::menuItems(*menu).front().label == "None");
    CHECK(ui::menuItems(*menu).front().checked);  // a new master plays nowhere
    CHECK(f.shell->outputLabel(cue) == "out \u2014");
    CHECK(f.shell->outputLabel(model::kMaster) == "out 1/2");
    f.root->closePopup();
    json::Value start = json::Value::object();
    start["backend"] = "dummy";
    start["outputs"] = std::int64_t{4};
    f.exec("engine.start", std::move(start));
    f.shell->openOutputMenu(cue, {10, 10, 40, 18});
    menu = f.root->find("output." + cue.str());
    REQUIRE(menu);
    std::vector<std::string> rows;
    for (const auto& item : ui::menuItems(*menu))
        rows.push_back(item.separator ? "-" : item.shortcut.empty() ? item.label : item.shortcut);
    CHECK(rows == std::vector<std::string>{"None", "-", "stereo 1-2", "stereo 3-4", "-", "mono 1",
                                           "mono 2", "mono 3", "mono 4"});
    const auto pick = ui::menuItems(*menu)[3].run;  // the menu goes as it runs
    pick();
    const auto* track = f.app->head().tracks.find(cue);
    REQUIRE(track);
    CHECK(model::hardwareOutputs(*track, cue) == std::vector<int>{3, 4});
    CHECK(f.shell->outputLabel(cue) == "out 3/4");
    f.exec("engine.stop");
}

TEST_CASE("a strip's envelope menu automates its sends; their lanes sit under the strip") {
    SendsFixture f;
    f.send(f.a, f.b);
    const auto route = model::Id::parse(f.routes().asArray()[0].find("id")->asString());
    REQUIRE(route);
    f.shell->openEnvelopeMenu(f.a, {10, 10, 40, 18});
    const auto* menu = f.root->find("tcp.env." + f.a.str());
    REQUIRE(menu);
    std::function<void()> pick;
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Send → B · Volume envelope") pick = item.run;
    REQUIRE(pick);
    pick();
    f.sync();
    REQUIRE(f.app->head().envelopes.size() == 1);
    const auto& env = *f.app->head().envelopes.begin();
    CHECK(env.value->target.object == *route);
    CHECK(env.value->target.param == "volume");
    const auto lane = f.view().trackLayout().indexOfEnv(env.id);
    const auto strip = f.view().trackLayout().indexOf(f.a);
    REQUIRE(lane);
    REQUIRE(strip);
    CHECK(*lane == *strip + 1);
}

TEST_CASE("TCP: a right-click on a strip's fader or button learns a MIDI control for it") {
    SendsFixture f;
    auto rightClick = [&](const char* what) {
        ui::Node* node = f.view().tcpPanel().controlNode(f.a, what);
        REQUIRE(node);
        REQUIRE(node->visible);
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.button = 3;
        e.pos = SendsFixture::centre(node->bounds());
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.root->frame(false);
    };
    // The menu's one row opens the learn dialog; a moved control and OK bind it.
    auto learn = [&](const char* what, const std::string& entry, std::uint8_t cc) {
        rightClick(what);
        ui::Node* menu = f.root->find("strip.learn.menu");
        REQUIRE(menu);
        CHECK(ui::menuItems(*menu)[0].label == entry);
        const ui::Point p{menu->bounds().x + 40, menu->bounds().y + 13};
        f.pointer(Pointer::Move, p);
        f.pointer(Pointer::Down, p);
        f.pointer(Pointer::Up, p);
        f.root->frame(false);
        REQUIRE(f.root->find("learn"));
        engine::EngineApp::ControlEvent e;
        e.midi.bytes[0] = 0xb0;
        e.midi.bytes[1] = cc;
        e.midi.bytes[2] = 127;
        f.actions->control({e});
        const std::string mode = f.root->find("learn.mode.set")->valueText();
        const auto before = f.history();
        REQUIRE(f.root->click("learn.ok"));
        CHECK(f.history() == before + 1);
        return mode;
    };
    // A fader follows a knob; a button toggles on each press.
    CHECK(learn("fader", "MIDI learn: Volume…", 7) == "Absolute");
    CHECK(learn("mute", "MIDI learn: Mute…", 20).starts_with("Toggle"));
    const model::Track& a = *f.app->head().tracks.find(f.a);
    REQUIRE(a.learn.size() == 2);
    CHECK(a.learn[0].param == "mute");
    CHECK(a.learn[0].mode == model::ControlMode::Toggle);
    CHECK(a.learn[1].param == "volume");
    CHECK(a.learn[1].control.number == 7);
    // The menu names what is bound.
    rightClick("fader");
    REQUIRE(f.root->find("strip.learn.menu"));
    CHECK(ui::menuItems(*f.root->find("strip.learn.menu"))[0].label ==
          "MIDI learn: Volume… (CC 7 · ch 1)");
    f.root->closePopup();
    // Elsewhere on the row, the track's menu as before.
    const auto row = f.view().trackLayout().indexOf(f.a);
    REQUIRE(row);
    const ui::Rect r = f.view().tcpRowRect(*row);
    ui::PointerEvent e;
    e.type = Pointer::Down;
    e.button = 3;
    e.pos = {r.x + 60, r.y + 10};
    f.root->pointer(e);
    f.root->frame(false);
    CHECK(f.root->find("track.menu." + f.a.str()));
    CHECK_FALSE(f.root->find("strip.learn.menu"));
}

TEST_CASE("pan controls: channel and buss envelope menus create and hide pan lanes") {
    SendsFixture f;
    for (const model::Id track : {f.a, f.b}) {
        const auto open = [&]() {
            f.shell->openEnvelopeMenu(track, {});
            f.root->frame(false);
            auto* menu = f.root->find("tcp.env." + track.str());
            REQUIRE(menu);
            return menu;
        };
        const auto find = [](const auto& items, std::string_view label) {
            return std::find_if(items.begin(), items.end(),
                                [&](const auto& item) { return item.label == label; });
        };
        auto items = ui::menuItems(*open());
        for (const char* label : {"Width envelope", "Pan left envelope", "Pan right envelope",
                                  "Gain compensation envelope"})
            CHECK(find(items, label) != items.end());
        const auto width = find(items, "Width envelope");
        REQUIRE(width != items.end());
        auto create = width->run;
        f.root->closePopup();
        create();
        f.sync();
        const auto lane = std::find_if(
            f.app->head().envelopes.begin(), f.app->head().envelopes.end(), [&](const auto& e) {
                return e.value->target.object == track && e.value->target.param == "width";
            });
        REQUIRE(lane != f.app->head().envelopes.end());
        CHECK(lane->value->points.front().value == 100);
        const auto laneId = lane->id;
        items = ui::menuItems(*open());
        const auto shown = find(items, "Width envelope");
        REQUIRE(shown != items.end());
        CHECK(shown->checked);
        auto hide = shown->run;
        f.root->closePopup();
        hide();
        f.sync();
        CHECK_FALSE(f.app->head().envelopes.find(laneId)->visible);
    }
}

TEST_CASE("pan controls: a TCP second bar records its writing envelope") {
    SendsFixture f;
    auto args = json::Value::object();
    args["track"] = f.a.str();
    args["pan_mode"] = "width";
    f.exec("track.set", args);
    args.asObject().erase("pan_mode");
    args["param"] = "width";
    const auto lane = *model::Id::parse(f.exec("env.add", args)["env"].asString());
    args = json::Value::object();
    args["env"] = lane.str();
    args["mode"] = "latch";
    f.exec("env.set", args);
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    struct Stop {
        engine::EngineApp& app;
        ~Stop() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                               {.anticipative = false, .workers = 0}));
    f.exec("transport.play");
    backend.run(4);
    f.sync();
    const auto* control = f.view().tcpPanel().controlNode(f.a, "pan");
    REQUIRE(control);
    const auto bar = control->bounds();
    f.pointer(Pointer::Down, {bar.x + .9f * bar.w, bar.y + bar.h / 2});
    f.pointer(Pointer::Move, {bar.x + .65f * bar.w, bar.y + bar.h / 2});
    f.pointer(Pointer::Up, {bar.x + .65f * bar.w, bar.y + bar.h / 2});
    f.sync();
    const auto* track = f.app->head().tracks.find(f.a);
    REQUIRE(track);
    CHECK(track->width < 100);
    const auto* env = f.app->head().envelopes.find(lane);
    REQUIRE(env);
    REQUIRE_FALSE(env->points.empty());
    CHECK(env->points.back().value == track->width);
    const model::EnvTarget target{f.a, "width"};
    CHECK(f.app->engine()->writeHolds() == std::vector<model::EnvTarget>{target});
}
