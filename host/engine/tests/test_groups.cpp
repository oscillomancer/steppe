// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/model/groups.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace {
struct GroupsHost {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("supple-groups-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    GroupsHost() {
        std::filesystem::create_directories(root / "plugins");
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {root / "plugins"};
        auto created = EngineApp::create(options);
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~GroupsHost() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(root);
    }
    Result<json::Value> execute(const char* name, std::string_view args = "{}") {
        return bus->execute(
            {name, json::parse(args).value(), {cmd::Origin::Type::Test, "groups", {}}});
    }
    json::Value run(const char* name, std::string_view args = "{}") {
        auto r = execute(name, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    const model::Track& track(unsigned n) {
        return *app->head().tracks.find({model::Kind::Track, n});
    }
    void tracks() {
        run("track.add", R"({"name":"A"})");
        run("track.add", R"({"name":"B"})");
        run("track.add", R"({"name":"C"})");
    }
};
}  // namespace
TEST_CASE("groups commands: relative overlap, bypass, suspension and one undo step") {
    GroupsHost h;
    h.tracks();
    h.run("track.set", R"({"track":"tr_2","volume_db":-9,"pan":0.2})");
    h.run("group.add",
          R"({"name":"AB","members":["tr_2","tr_1"],"links":["volume","pan","mute"]})");
    h.run("group.add", R"({"name":"BC","members":["tr_2","tr_3"],"links":["volume"]})");
    const auto history = h.app->versions().history().size();
    h.run("track.set", R"({"track":"tr_1","volume_db":-3,"pan":0.1,"mute":true})");
    CHECK(h.app->versions().history().size() == history + 1);
    CHECK(h.track(2).volumeDb == -12);
    CHECK(h.track(3).volumeDb == -3);
    CHECK(h.track(2).pan == doctest::Approx(.3));
    CHECK(h.track(3).pan == 0);
    CHECK(h.track(2).mute);
    CHECK_FALSE(h.track(3).mute);
    h.run("history.undo");
    CHECK(h.track(2).volumeDb == -9);
    CHECK(h.track(1).volumeDb == 0);
    h.run("track.set", R"({"track":"tr_1","volume_db":-6,"bypass_groups":true})");
    CHECK(h.track(2).volumeDb == -9);
    h.run("group.set", R"({"group":"gr_1","active":false})");
    h.run("track.set", R"({"track":"tr_1","volume_db":-12})");
    CHECK(h.track(2).volumeDb == -9);
    h.run("group.set", R"({"group":"gr_1","active":true})");
    h.run("track.set", R"({"track":"tr_1","volume_db":-150})");
    CHECK(h.track(2).volumeDb == -150);
    h.run("track.set", R"({"track":"tr_1","volume_db":-10})");
    CHECK(h.track(2).volumeDb == -150);
    h.run("project.save", R"({"path":"groups.json"})");
    const auto saved = model::serializeProject(h.app->head());
    h.run("group.remove", R"({"group":"gr_1"})");
    h.run("project.open", R"({"path":"groups.json"})");
    CHECK(model::serializeProject(h.app->head()) == saved);
}
TEST_CASE("groups commands: nested VCA queries, cycle rollback and removal cleanup") {
    GroupsHost h;
    h.tracks();
    h.run("group.add", R"({"name":"Child","members":["tr_1"]})");
    h.run("group.add", R"({"name":"Parent"})");
    h.run("vca.add", R"({"group":"gr_1","volume_db":12})");
    h.run("vca.add", R"({"group":"gr_2","volume_db":12,"followers":["gr_1"]})");
    auto d = h.run("track.describe", R"({"track":"tr_1","at":0})");
    CHECK(d["requested_effective_volume_db"].asDouble() == 24);
    CHECK(d["effective_volume_db"].asDouble() == doctest::Approx(20 * std::log10(4.0)));
    CHECK(h.track(1).volumeDb == 0);
    const auto saved = model::serializeProject(h.app->head());
    auto cycle = h.execute("vca.members.set", R"({"group":"gr_1","vcas":["gr_2"]})");
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "vca.cycle");
    CHECK_FALSE(cycle.error().hint.empty());
    CHECK(model::serializeProject(h.app->head()) == saved);
    h.run(
        "env.add",
        R"({"group":"gr_1","param":"volume","points":[{"beat":0,"value":-3},{"beat":4,"value":-9}]})");
    h.run("group.set", R"({"group":"gr_1","active":false})");
    d = h.run("track.describe", R"({"track":"tr_1","at":1})");
    CHECK(d["requested_effective_volume_db"].asDouble() == 6);
    h.run("vca.set", R"({"group":"gr_2","mute":true})");
    CHECK(h.run("track.describe", R"({"track":"tr_1"})")["effective_mute"].asBool());
    h.run("vca.remove", R"({"group":"gr_1"})");
    CHECK(h.app->head().envelopes.empty());
    CHECK(h.app->head().groups.find({model::Kind::Group, 2})->vca->followers.empty());
    h.run("track.remove", R"({"track":"tr_1"})");
    CHECK(h.app->head().groups.find({model::Kind::Group, 1})->members.empty());
}
TEST_CASE("groups commands: matching edits are atomic, respect locks and copy whole takes") {
    GroupsHost h;
    h.tracks();
    h.run("group.add", R"({"name":"Edit","members":["tr_1","tr_2"],"links":["edit"]})");
    for (const char* args : {R"({"track":"tr_1","notation":"voice a; 1: C4:w"})",
                             R"({"track":"tr_2","notation":"voice a; 1: E4:w"})",
                             R"({"track":"tr_3","notation":"voice a; 1: G4:w"})"})
        h.run("item.add", args);
    h.run("item.set", R"({"item":"it_2","locked":true})");
    h.run("item.set", R"({"item":"it_1","name":"Local"})");
    const auto saved = model::serializeProject(h.app->head());
    CHECK_FALSE(h.execute("item.split", R"({"item":"it_1","at":2})"));
    CHECK(model::serializeProject(h.app->head()) == saved);
    h.run("item.set", R"({"item":"it_2","locked":false})");
    const auto history = h.app->versions().history().size();
    auto split = h.run("item.split", R"({"item":"it_1","at":2})");
    CHECK(h.app->versions().history().size() == history + 1);
    REQUIRE(split["linked_right"].size() == 1);
    CHECK(h.app->head().items.size() == 5);
    h.run("history.undo");
    CHECK(h.app->head().items.size() == 3);
    auto dup = h.run("item.duplicate", R"({"item":"it_1","to":4})");
    REQUIRE(dup["linked"].size() == 1);
    CHECK(h.app->head().items.size() == 5);
    h.run("history.undo");
    CHECK_FALSE(h.execute("item.move", R"({"item":"it_1","track":"tr_3","position":1})"));
    h.run("item.move", R"({"item":"it_1","track":"tr_1","position":2})");
    CHECK(h.app->head().items.find({model::Kind::Item, 2})->position == 2);
    CHECK(h.app->head().items.find({model::Kind::Item, 2})->track ==
          model::Id{model::Kind::Track, 2});
    CHECK(h.app->head().items.find({model::Kind::Item, 3})->position == 0);
    h.run("item.remove", R"({"item":"it_1"})");
    CHECK(h.app->head().items.size() == 1);
}

namespace {
InstancePool groupsPool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
        host::ScanOptions options;
        options.forceInProcess = true;
        auto plugins = host::scan(std::span(&fixture, 1), options);
        if (!plugins) return plugins.error();
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("test.plugin", ref.id);
    });
}
model::Session audioSession(const std::filesystem::path& path, bool console, bool bridge,
                            bool automated) {
    model::EditTxn edit(model::Session::create({.console9 = console}), 1);
    model::Track track;
    track.parent = model::kMaster;
    track.volumeDb = -6;
    const auto id = edit.create(track);
    edit.modify<model::Track>(model::kMaster, [&](auto& master) { master.children.push_back(id); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    if (bridge) fx.isolation = model::Isolation::Dedicated;
    if (automated)
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                .value();
    const auto effect = edit.create(fx);
    edit.modify<model::Track>(id, [&](auto& tr) { tr.chain.push_back(effect); });
    const auto media = edit.create(model::MediaSource{path.string(), "", "wav", 48000, 2, 16384});
    model::Take take;
    take.source = media;
    const auto taken = edit.create(take);
    model::Item item;
    item.track = id;
    item.length = 16384. / 24000.;
    item.takes = {taken};
    item.activeTake = taken;
    edit.create(item);
    model::TrackGroup child;
    child.members = {id};
    child.vca = model::Vca{-3, false, false, {}, {}};
    const auto first = edit.create(child);
    model::TrackGroup parent;
    parent.vca = model::Vca{-6, false, false, {first}, {}};
    const auto second = edit.create(parent);
    if (automated) {
        model::Envelope volume;
        volume.target = {first, "volume"};
        volume.timebase = model::Timebase::Time;
        volume.points = {{0, -3}, {.1, -12}, {.25, 6}};
        edit.create(volume);
        volume.target = {id, "volume"};
        volume.points = {{0, -6}, {.1, 0}, {.25, -9}};
        edit.create(volume);
        model::Envelope gate;
        gate.target = {second, "mute"};
        gate.timebase = model::Timebase::Time;
        gate.points = {
            {0, 0, model::PointShape::Square}, {.1, 1, model::PointShape::Square}, {.15, 0}};
        edit.create(gate);
    }
    auto result = edit.commit();
    REQUIRE(result);
    return *result;
}
}  // namespace
TEST_CASE(
    "VCA audio: nested automation is deterministic and identical in both executors and bridge" *
    doctest::description("rt")) {
    GroupsHost h;
    audio::AudioData data;
    data.sampleRate = 48000;
    data.channels.assign(2, std::vector<float>(16384));
    for (std::size_t i = 0; i < data.frames(); ++i)
        data.channels[0][i] = data.channels[1][i] =
            static_cast<float>(.1 * std::sin(double(i) * .07));
    const auto path = h.root / "signal.wav";
    REQUIRE(audio::writeWav(path, data));
    host::setBridgeExecutable(BRIDGE_PATH);
    for (double rate : {1., 1.5})
        for (bool console : {false, true})
            for (bool bridge : {false, true}) {
                CAPTURE(console);
                CAPTURE(bridge);
                CAPTURE(rate);
                const auto base = audioSession(path, console, bridge, true);
                model::EditTxn edit(base, 2);
                auto project = edit.project();
                project.playrate.rate = rate;
                edit.setProject(project);
                auto changed = edit.commit();
                REQUIRE(changed);
                const auto& s = *changed;
                auto pool = groupsPool();
                auto reference = renderOffline(s, pool, {.endSeconds = .25, .threads = 1});
                REQUIRE(reference);
                CHECK(reference->warnings.empty());
                auto repeated = renderOffline(s, pool, {.endSeconds = .25, .threads = 1});
                REQUIRE(repeated);
                CHECK((reference->audio.channels == repeated->audio.channels));
                for (bool ahead : {false, true}) {
                    CAPTURE(ahead);
                    auto livePool = groupsPool();
                    MediaCache media;
                    DummyBackend backend(
                        {.mode = DummyBackend::Mode::Manual, .captureFrames = 16384});
                    Engine engine(livePool, media, {.anticipative = ahead, .workers = 1});
                    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
                    REQUIRE(engine.load(s));
                    engine.play();
                    for (int i = 0; i < 128; ++i) {
                        if (ahead) REQUIRE(engine.awaitRendered((backend.cycles() + 1) * 128));
                        backend.run(1);
                    }
                    engine.stop();
                    for (std::size_t c = 0; c < 2; ++c)
                        CHECK(std::equal(
                            reference->audio.channels[c].begin(),
                            reference->audio.channels[c].end(),
                            backend.capture().channels[c].begin() + reference->outputLatency));
                    CHECK(engine.processErrors() == 0);
                    CHECK(engine.anticipationMisses() == 0);
                }
            }
}
TEST_CASE(
    "VCA audio: static sum matches member fader, silence absorbs boosts and native top clamps" *
    doctest::description("rt")) {
    GroupsHost h;
    audio::AudioData data;
    data.sampleRate = 48000;
    data.channels.assign(2, std::vector<float>(16384, .01f));
    const auto path = h.root / "signal.wav";
    REQUIRE(audio::writeWav(path, data));
    for (bool console : {false, true})
        for (double level : {-15., 24., -150.}) {
            auto s = audioSession(path, console, false, false);
            model::EditTxn edit(s, 2);
            edit.modify<model::Track>({model::Kind::Track, 1}, [&](auto& tr) {
                tr.volumeDb = level == -150. ? -150. : level == 24. ? 12. : -6.;
            });
            edit.modify<model::TrackGroup>({model::Kind::Group, 1}, [&](auto& g) {
                g.vca->volumeDb = level == 24. ? 6. : -3.;
            });
            edit.modify<model::TrackGroup>({model::Kind::Group, 2}, [&](auto& g) {
                g.vca->volumeDb = level == 24. ? 6. : -6.;
            });
            auto grouped = edit.commit();
            REQUIRE(grouped);
            model::EditTxn plain(*grouped, 3);
            plain.remove<model::TrackGroup>({model::Kind::Group, 1});
            plain.remove<model::TrackGroup>({model::Kind::Group, 2});
            plain.modify<model::Track>({model::Kind::Track, 1},
                                       [&](auto& tr) { tr.volumeDb = level == 24. ? 12. : level; });
            auto direct = plain.commit();
            REQUIRE(direct);
            auto pool = groupsPool();
            auto a = renderOffline(*grouped, pool, {.endSeconds = .1, .threads = 1});
            REQUIRE(a);
            auto b = renderOffline(*direct, pool, {.endSeconds = .1, .threads = 1});
            REQUIRE(b);
            if (level != 24.)
                CHECK((a->audio.channels == b->audio.channels));
            else {
                // Boosting more must never change an already saturated fader.
                model::EditTxn boosted(*grouped, 4);
                boosted.modify<model::TrackGroup>({model::Kind::Group, 1},
                                                  [](auto& g) { g.vca->volumeDb = 12; });
                auto more = boosted.commit();
                REQUIRE(more);
                auto c = renderOffline(*more, pool, {.endSeconds = .1, .threads = 1});
                REQUIRE(c);
                CHECK((a->audio.channels == c->audio.channels));
            }
        }
}

TEST_CASE("VCA audio: independent sends and exclusive stems retain the right controls" *
          doctest::description("rt")) {
    GroupsHost h;
    audio::AudioData data;
    data.sampleRate = 48000;
    data.channels.assign(2, std::vector<float>(16384, .02f));
    const auto path = h.root / "signal.wav";
    REQUIRE(audio::writeWav(path, data));
    for (bool clipped : {false, true})
        for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader}) {
            CAPTURE(clipped);
            CAPTURE(static_cast<int>(tap));
            auto base = audioSession(path, true, false, false);
            model::EditTxn edit(base, 2);
            model::Track bus;
            bus.type = model::StripType::Buss;
            bus.parent = model::kMaster;
            const auto dest = edit.create(bus);
            edit.modify<model::Track>(model::kMaster, [&](auto& m) { m.children.push_back(dest); });
            edit.modify<model::Track>({model::Kind::Track, 1},
                                      [](auto& tr) { tr.parentSend.enabled = false; });
            model::Route route;
            route.from.track = model::Id{model::Kind::Track, 1};
            route.to.track = dest;
            route.tap = tap;
            route.volumeDb = clipped ? -12 : -3;
            route.pan = 25;
            const auto send = edit.create(route);
            if (clipped) {
                edit.modify<model::Track>({model::Kind::Track, 1},
                                          [](auto& tr) { tr.volumeDb = 12; });
                for (unsigned g : {1u, 2u})
                    edit.modify<model::TrackGroup>({model::Kind::Group, g},
                                                   [](auto& group) { group.vca->volumeDb = 6; });
            }
            auto grouped = edit.commit();
            REQUIRE(grouped);
            model::EditTxn plain(*grouped, 3);
            plain.remove<model::TrackGroup>({model::Kind::Group, 1});
            plain.remove<model::TrackGroup>({model::Kind::Group, 2});
            plain.modify<model::Track>({model::Kind::Track, 1},
                                       [&](auto& tr) { tr.volumeDb = clipped ? 12 : -15; });
            if (clipped && tap == model::Tap::PostFader)
                plain.modify<model::Route>(
                    send, [](auto& value) { value.volumeDb = 20 * std::log10(4.) - 24; });
            auto direct = plain.commit();
            REQUIRE(direct);
            auto pool = groupsPool();
            auto a = renderOffline(*grouped, pool, {.endSeconds = .1, .threads = 1});
            auto b = renderOffline(*direct, pool, {.endSeconds = .1, .threads = 1});
            REQUIRE(a);
            REQUIRE(b);
            CHECK((a->audio.channels == b->audio.channels));
        }
    auto base = audioSession(path, false, false, false);
    model::EditTxn edit(base, 2);
    model::Track other;
    other.parent = model::kMaster;
    const auto otherId = edit.create(other);
    edit.modify<model::Track>(model::kMaster, [&](auto& m) { m.children.push_back(otherId); });
    auto item = *base.items.find({model::Kind::Item, 1});
    item.track = otherId;
    const auto taken = edit.create(*base.takes.find(*item.activeTake));
    item.takes = {taken};
    item.activeTake = taken;
    edit.create(item);
    model::TrackGroup solo;
    solo.members = {otherId};
    solo.vca = model::Vca{0, false, true, {}, {}};
    const auto gid = edit.create(solo);
    auto soloed = edit.commit();
    REQUIRE(soloed);
    auto pool = groupsPool();
    OfflineSettings settings;
    settings.endSeconds = .1;
    settings.threads = 1;
    settings.throughMaster = model::Id{model::Kind::Track, 1};
    auto a = renderOffline(*soloed, pool, settings);
    REQUIRE(a);
    model::EditTxn clearSolo(*soloed, 3);
    clearSolo.modify<model::TrackGroup>(gid, [](auto& g) { g.vca->solo = false; });
    auto cleared = clearSolo.commit();
    REQUIRE(cleared);
    auto b = renderOffline(*cleared, pool, settings);
    REQUIRE(b);
    CHECK((a->audio.channels == b->audio.channels));
    settings.throughMaster.reset();
    settings.track = model::Id{model::Kind::Track, 1};
    auto stem = renderOffline(*cleared, pool, settings);
    REQUIRE(stem);
    model::EditTxn silent(*cleared, 4);
    silent.modify<model::TrackGroup>({model::Kind::Group, 2}, [](auto& g) { g.vca->mute = true; });
    auto muted = silent.commit();
    REQUIRE(muted);
    auto quiet = renderOffline(*muted, pool, settings);
    REQUIRE(quiet);
    double loud = 0, low = 0;
    for (float x : stem->audio.channels[0]) loud += std::fabs(double(x));
    for (float x : quiet->audio.channels[0]) low += std::fabs(double(x));
    CHECK(loud > 1);
    CHECK(low == 0);
}

TEST_CASE("pan controls: the pan link carries positions, modes and nullable laws") {
    GroupsHost h;
    h.tracks();
    h.run("track.set", R"({"track":"tr_2","width":20,"pan_left":-20,"pan_right":80})");
    h.run("track.set", R"({"track":"tr_3","width":-80,"pan_left":30,"pan_right":-90})");
    h.run("group.add", R"({"name":"AB","members":["tr_0","tr_1","tr_2"],"links":["pan"]})");
    h.run("group.add", R"({"name":"BC","members":["tr_2","tr_3"],"links":["pan"]})");
    h.run(
        "track.set",
        R"({"track":"tr_1","width":50,"pan_left":-60,"pan_right":60,"pan_mode":"dual","pan_law":{"db":-3,"boost":true}})");
    CHECK(h.track(2).width == -30);
    CHECK(h.track(3).width == -100);
    CHECK(h.track(2).panLeft == 20);
    CHECK(h.track(3).panLeft == 70);
    CHECK(h.track(2).panRight == 40);
    CHECK(h.track(3).panRight == -100);
    CHECK(h.track(0).panMode == model::PanMode::Pan);
    CHECK_FALSE(h.track(0).panLaw);
    CHECK(h.track(0).width == 100);
    for (unsigned id : {1u, 2u, 3u}) {
        CHECK(h.track(id).panMode == model::PanMode::Dual);
        REQUIRE(h.track(id).panLaw);
        CHECK(h.track(id).panLaw->boost);
    }
    h.run("track.set", R"({"track":"tr_1","pan_law":null})");
    CHECK_FALSE(h.track(2).panLaw);
    CHECK_FALSE(h.track(3).panLaw);
}

TEST_CASE("pan controls: envelope commands accept units and refuse masters and invalid values") {
    GroupsHost h;
    h.tracks();
    for (const std::string param : {"width", "pan_left", "pan_right", "pan_boost"}) {
        CAPTURE(param);
        const std::string points =
            param == "pan_boost" ? R"([{"beat":0,"value":1}])"
                                 : R"([{"beat":0,"value":"-50 %"},{"beat":4,"value":"50 %"}])";
        const auto args = R"({"track":"tr_1","param":")" + param + R"(","points":)" + points + "}";
        CHECK(h.execute("env.add", args));
        const auto master = h.execute("env.add", R"({"track":"tr_0","param":")" + param + "\"}");
        REQUIRE_FALSE(master);
        CHECK(master.error().code == "track.strip_type");
        const auto invalid = h.execute("env.add", R"({"track":"tr_2","param":")" + param +
                                                      R"(","points":[{"beat":0,"value":101}]})");
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().code == "env.value");
    }
}
