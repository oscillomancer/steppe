// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Timecode and sync in the GUI (gui.md §4.8): the project timecode dialog.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <filesystem>
#include <memory>
#include <string>

#include "app_actions.h"
#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/ui/forms.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "timecode_dialog.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Test, "sync-test", {}};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<ui::Root> root;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-sync-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"sync-test", 1200, 800, true});
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
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        root->setContent(std::move(frame));
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    ui::Node* node(const std::string& id) {
        ui::Node* n = root->find(id);
        REQUIRE_MESSAGE(n, id);
        return n;
    }
    const sync::TimecodeMapping& timecode() { return app->head().project->timecode; }
};

}  // namespace

TEST_CASE("project timecode: the dialog edits the project's rate and start") {
    Fixture f;
    if (!f.ok) return;
    gui::TimecodeDialog dialog(*f.root, *f.dispatcher, *f.app);
    std::vector<std::string> errors;
    dialog.onError = [&](const std::string& e) { errors.push_back(e); };
    dialog.open();
    f.root->frame(false);
    REQUIRE(dialog.isOpen());
    // The defaults: 30 fps from 00:00:00:00.
    CHECK(f.node("timecode.rate.set")->valueText() == "30 fps");
    CHECK(f.node("timecode.start.set")->valueText() == "00:00:00:00");
    // The rate menu: one edit.
    auto* rate = dynamic_cast<ui::Dropdown*>(f.node("timecode.rate.set"));
    REQUIRE(rate);
    rate->choose("25");
    f.root->frame(false);
    CHECK(f.timecode().rate == sync::FrameRate::Fps25);
    CHECK(f.node("timecode.rate.set")->valueText() == "25 fps");
    // The start: Enter commits it.
    auto* start = dynamic_cast<ui::TextField*>(f.node("timecode.start.set"));
    REQUIRE(start);
    REQUIRE(start->onSubmit);
    start->onSubmit("01:00:00:00");
    f.root->frame(false);
    CHECK(f.timecode().start == sync::Timecode{1, 0, 0, 0});
    CHECK(errors.empty());
    // A label the rate does not have is refused, shown, and changes nothing.
    start->onSubmit("01:00:00:25");
    f.root->frame(false);
    CHECK(f.timecode().start == sync::Timecode{1, 0, 0, 0});
    REQUIRE(errors.size() == 1);
    CHECK(f.node("timecode.status")->valueText().find("out of range") != std::string::npos);
    // Drop-frame refuses a start it skips; an agent's edit shows at once.
    CHECK(dialog.setRate("30"));  // 01:00:00:00 is a label at 30 too
    f.exec("project.set", R"({"timecode": {"rate": "29.97df", "start": "00:10:00;00"}})");
    dialog.refresh();
    f.root->frame(false);
    CHECK(f.node("timecode.rate.set")->valueText() == "29.97 fps drop-frame");
    CHECK(f.node("timecode.start.set")->valueText() == "00:10:00;00");
    // project.get reports it; undo restores the previous setting.
    auto info = f.exec("project.get", "{}");
    CHECK(info["timecode"]["rate"].asString() == "29.97df");
    CHECK(info["timecode"]["start"].asString() == "00:10:00;00");
    f.exec("history.undo", "{}");
    dialog.refresh();
    f.root->frame(false);
    CHECK(f.node("timecode.rate.set")->valueText() == "30 fps");
}
