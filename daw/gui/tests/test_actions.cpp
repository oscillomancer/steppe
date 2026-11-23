// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <SDL3/SDL_keycode.h>

#include <filesystem>
#include "test_support.h"

#include "supple/actions/actions.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/write.h"

// The engine waits for a device to act on a transport request (1 s, then
// engine.transport); on a starved runner a healthy dummy device took longer
// (Windows CI 45186989443), so this binary waits as long as a test may.
namespace {
[[maybe_unused]] const bool kTransportPatient = [] {
    supple::engine::setTransportWaitSeconds(supple::engine::kPatientTransportSeconds);
    return true;
}();
}  // namespace

using namespace supple;
using namespace supple::actions;

namespace {

struct Engine {
    std::filesystem::path dir;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    Engine() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-actions-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        engine::AppOptions options;
        options.workdir = dir;
        auto created = engine::EngineApp::create(options);
        REQUIRE(created);
        app = std::move(*created);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
    }
    ~Engine() {
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, json::Value args = json::Value::object()) {
        auto r =
            dispatcher->execute({command, std::move(args), {cmd::Origin::Type::Test, "test", {}}});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
};

}  // namespace

TEST_CASE("every exposed command becomes an action; selection arguments bind, others prompt") {
    Engine e;
    ActionRegistry reg;
    reg.addCommands(e.registry);
    CHECK(reg.all().size() >= 90);
    const Action* undo = reg.find("history.undo");
    REQUIRE(undo);
    CHECK(undo->name == "History: undo");
    CHECK(undo->prompts.empty());
    const Action* split = reg.find("item.split");
    REQUIRE(split);
    CHECK(split->args.find("item")->asString() == "$selection.item");
    CHECK(split->prompts == std::vector<std::string>{"at"});
}

TEST_CASE("fuzzy search ranks word starts and runs first") {
    CHECK(fuzzyScore("tradd", "Track: add").has_value());
    CHECK_FALSE(fuzzyScore("xyz", "Track: add").has_value());
    CHECK(*fuzzyScore("undo", "History: undo") > *fuzzyScore("undo", "Something: un dot"));
    Engine e;
    ActionRegistry reg;
    reg.addCommands(e.registry);
    const auto hits = reg.search("undo", 5);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().action->id == "history.undo");
    const auto play = reg.search("tr play", 5);
    REQUIRE_FALSE(play.empty());
    CHECK(play.front().action->id == "transport.play");
}

TEST_CASE(
    "the runner resolves the selection, runs per item in one undo step, and runs macros as one "
    "batch") {
    Engine e;
    ActionRegistry reg;
    reg.addCommands(e.registry);
    Action buss;
    buss.id = "track.add:buss";
    buss.name = "Track: add buss";
    buss.command = "track.add";
    buss.args["type"] = "buss";
    reg.add(buss);
    Action channel = buss;
    channel.id = "track.add:channel";
    channel.args["type"] = "channel";
    reg.add(channel);
    Runner runner(reg, *e.dispatcher);
    Context ctx;
    REQUIRE(runner.run("track.add:buss", ctx));
    auto added = runner.run("track.add:channel", ctx);  // only channels hold items
    REQUIRE(added);
    const std::string track = (*added)["track"].asString();

    // Two MIDI items, then split both at beat 2 with one action.
    std::vector<std::string> items;
    for (int bar : {1, 3}) {
        json::Value a = json::Value::object();
        a["track"] = track;
        a["bar"] = bar;
        a["notation"] = "voice v\n1: C4:w\n";
        items.push_back(e.exec("item.add", std::move(a))["item"].asString());
    }
    Action split;
    split.id = "item.split:playhead";
    split.name = "Item: split at playhead";
    split.command = "item.split";
    split.args["item"] = "$each.item";
    split.args["at"] = "$playhead.beats";
    reg.add(split);
    ctx.selectedItems = items;
    ctx.playheadBeats = 2;  // inside the first item only — the second must fail cleanly
    CHECK_FALSE(runner.run("item.split:playhead", ctx));
    const auto before = e.exec("session.describe");
    ctx.selectedItems = {items[0]};
    REQUIRE(runner.run("item.split:playhead", ctx));
    // Undo once restores the session: one undo step.
    e.exec("history.undo");
    CHECK(json::writeCanonical(e.exec("session.describe")) == json::writeCanonical(before));

    // No selection: an actionable error, not a crash.
    ctx.selectedItems.clear();
    auto none = runner.run(*reg.find("item.split"), ctx, json::Value::object());
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("select") != std::string::npos);
    // Prompts are required and applied.
    ctx.selectedItems = {items[1]};
    json::Value prompted = json::Value::object();
    prompted["at"] = 9.0;
    CHECK(runner.run(*reg.find("item.split"), ctx, prompted));

    // A macro: two track adds, one undo step.
    Action macro;
    macro.id = "macro:two-busses";
    macro.name = "Two busses";
    macro.steps = {"track.add:buss", "track.add:buss"};
    reg.add(macro);
    const auto count = [&] { return e.exec("track.list")["tracks"].asArray().size(); };
    const auto n = count();
    REQUIRE(runner.run("macro:two-busses", ctx));
    CHECK(count() == n + 2);
    e.exec("history.undo");
    CHECK(count() == n);
}

TEST_CASE("chords print and parse; keymaps resolve sections and sequences") {
    const auto z = parseChord("Ctrl+Shift+Z");
    REQUIRE(z);
    CHECK(z->key == SDLK_Z);
    CHECK(z->mods == (1u | 2u));
#if defined(__APPLE__)
    CHECK(describe(*z) == "Cmd+Shift+Z");
#else
    CHECK(describe(*z) == "Ctrl+Shift+Z");
#endif
    CHECK(parseChord(describe(*z)) == z);
    CHECK(parseChord("Shift+/")->key == SDLK_SLASH);
    CHECK_FALSE(parseChord("Ctrl+NoSuchKey"));

    Keymap k = Keymap::defaults();
    CHECK(k.conflicts().empty());
    CHECK(k.press(*parseChord("Ctrl+Z"), {"global"}) == std::optional<std::string>("history.undo"));
    // W (and Home) send the playhead back to the start.
    CHECK(k.press(*parseChord("W"), {"arrange", "global"}) ==
          std::optional<std::string>("transport.seek:start"));
    // A section binding wins inside its section only.
    CHECK(k.press(*parseChord("S"), {"arrange", "global"}) ==
          std::optional<std::string>("item.split:playhead"));
    CHECK_FALSE(k.press(*parseChord("S"), {"mixer", "global"}));
    // The editing and navigation bindings of the arrange view.
    REQUIRE(parseChord("["));
    CHECK(parseChord("[")->key == SDLK_LEFTBRACKET);
    CHECK(k.press(*parseChord("]"), {"arrange", "global"}) ==
          std::optional<std::string>("ui.marker.next"));
    CHECK(k.press(*parseChord("["), {"arrange", "global"}) ==
          std::optional<std::string>("ui.marker.prev"));
    CHECK(k.press(*parseChord("Ctrl+C"), {"arrange", "global"}) ==
          std::optional<std::string>("ui.item.copy"));
    CHECK(k.press(*parseChord("Ctrl+A"), {"arrange", "global"}) ==
          std::optional<std::string>("ui.item.select_all"));
    // A two-chord sequence.
    k.bind({{*parseChord("Ctrl+K"), *parseChord("Ctrl+S")}, "global", "project.save-as"});
    CHECK_FALSE(k.press(*parseChord("Ctrl+K"), {"global"}));
    CHECK(k.pending());
    CHECK(k.press(*parseChord("Ctrl+S"), {"global"}) ==
          std::optional<std::string>("project.save-as"));
    // A broken sequence retries the second chord on its own.
    CHECK_FALSE(k.press(*parseChord("Ctrl+K"), {"global"}));
    CHECK(k.press(*parseChord("Ctrl+Z"), {"global"}) == std::optional<std::string>("history.undo"));
    // W (and Home) send the playhead back to the start.
    CHECK(k.press(*parseChord("W"), {"arrange", "global"}) ==
          std::optional<std::string>("transport.seek:start"));
    // Conflicts are reported: Ctrl+K alone would shadow the sequence.
    k.bind({{*parseChord("Ctrl+K")}, "global", "something"});
    CHECK(k.conflicts().size() == 1);
}

TEST_CASE("keymaps save only the user's changes and load them over the defaults") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("supple-keymap-test-" + std::to_string(testProcessId())) / "keymap.json";
    Keymap k = Keymap::defaults();
    k.unbind({{*parseChord("L")}, "global", "transport.loop.toggle"});
    k.bind({{*parseChord("P")}, "global", "transport.loop.toggle"});
    REQUIRE(k.save(file));
    auto text = readFile(file);
    REQUIRE(text);
    const std::string s(text->begin(), text->end());
    CHECK(s.find("\"P\"") != std::string::npos);
    CHECK(s.find("history.undo") == std::string::npos);  // defaults are not written
    auto loaded = Keymap::load(file);
    REQUIRE(loaded);
    CHECK(loaded->bindingsFor("transport.loop.toggle").size() == 1);
    CHECK(loaded->bindingsFor("transport.loop.toggle")[0].keys[0] == *parseChord("P"));
    CHECK_FALSE(loaded->press(*parseChord("L"), {"global"}));
    REQUIRE(writeFileAtomic(
        file,
        std::string(
            R"({"schema": "supple.keymap/1.0", "bind": [{"keys": ["Ctrl+Nope"], "action": "x"}]})")));
    auto bad = Keymap::load(file);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/bind/0/keys");
    std::filesystem::remove_all(file.parent_path());
}
