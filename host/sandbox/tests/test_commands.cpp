// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/product.h>
#include <algorithm>
#include <array>
#include <set>
#include "../../../tests/file_platform.h"
#include "../../../tests/fuzz/command_inputs.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/sandbox/commands.h>

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;

struct Root {
    fs::path path = fs::temp_directory_path() /
                    ("supple-sandbox-cmd-" + std::to_string(supple::test::processId()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<SandboxHost> makeHost(const fs::path& dir, fs::path log = {}, bool player = false) {
    SessionOptions o;
    o.workdir = dir;
    o.pluginPath = {kFixture};
    if (player) o.pluginPath.push_back(fs::path(STOCK_CLAP_DIR) / "Script Player.clap");
    auto s = Session::open(o);
    REQUIRE(s);
    return std::make_unique<SandboxHost>(std::move(*s), std::move(log));
}

json::Value j(std::string_view text) { return json::parse(text).value(); }

const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

TEST_CASE("every sandbox command group has its line, and every line a group") {
    cmd::Registry registry;
    registerCommands(registry);
    std::set<std::string> prefixes;
    for (const auto& group : registry.groupInfos()) {
        INFO("group " << group.name << " has no line: add it to registerCommands");
        CHECK_FALSE(group.summary.empty());
        CHECK(group.summary.size() <= cmd::Registry::kMaxGroupSummary);
        prefixes.insert(group.name);
    }
    for (const auto& [name, line] : registry.groups()) {
        INFO("group line " << name << " has no commands: remove it or restore them");
        CHECK(prefixes.count(name) == 1);
    }
    CHECK(registry.groups().size() == prefixes.size());
}
TEST_CASE("the sandbox's describe levels stay inside their budgets") {
    cmd::Registry registry;
    registerCommands(registry);
    // The canonical writer pads, so these bytes are an upper bound of the compact JSON an agent
    // reads.
    const auto groups = cmd::groupIndex(registry.groupsResult());
    CHECK(json::writeCanonical(groups).size() < 3000);
    for (const auto& group : registry.groupInfos()) {
        INFO("group " << group.name << " is over its budget: split it or shorten summaries");
        CHECK(json::writeCanonical(cmd::rowIndex(registry.listResult("", "", "", group.name)))
                  .size() < 2500);
    }
}
TEST_CASE("registered sandbox argument mutations return structured errors") {
    constexpr std::uint64_t seed = 0xc017a01;
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "input-fuzz");
    cmd::Dispatcher bus(registry, *host);
    const auto before = bus.execute({"session.get", json::Value::object(), kTest});
    REQUIRE(before);
    test::CommandInputs inputs(seed);
    std::size_t generated = 0, rejected = 0, accepted = 0;
    for (const auto* command : registry.all()) {
        std::size_t commandRejected = 0;
        inputs.generate(command->argsSchema, [&](const json::Value& args, const std::string& path) {
            ++generated;
            INFO("seed=" << seed << " case=" << generated << " command=" << command->name << "@"
                         << command->version << " target=" << path
                         << " args=" << json::writeCompact(args));
            if (command->validate(args)) {
                REQUIRE_MESSAGE(inputs.allows(command->argsSchema, args),
                                "decoder accepted an input outside its exported contract");
                ++accepted;
                return;
            }
            ++rejected;
            ++commandRejected;
            const auto result =
                bus.execute({command->name + "@" + std::to_string(command->version), args, kTest});
            REQUIRE_FALSE(result);
            CHECK(result.error().code == "command.invalid_args");
            CHECK_FALSE(result.error().message.empty());
            CHECK(result.error().path == "/args");
            CHECK_FALSE(result.error().hint.empty());
            CHECK_FALSE(result.error().details.empty());
            const auto after = bus.execute({"session.get", json::Value::object(), kTest});
            REQUIRE(after);
            CHECK(*after == *before);
        });
        CHECK_MESSAGE(commandRejected > 0, "no rejected inputs for " << command->name);
    }
    std::cout << "sandbox command-inputs seed=" << seed << " commands=" << registry.all().size()
              << " generated=" << generated << " rejected=" << rejected
              << " schema-accepted-not-executed=" << accepted << '\n';
}

TEST_CASE("audio filenames beginning with rd_ are not render identifiers") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s");
    cmd::Dispatcher dispatcher(registry, *host);
    REQUIRE(
        dispatcher.execute({"plugin.load", j(R"({"plugin":"com.suppleaudio.test.sine"})"), kTest}));
    auto rendered = dispatcher.execute(
        {"render", j(R"({"notation":"1: A4:w", "seconds": 1, "tail_s": 0})"), kTest});
    REQUIRE(rendered);
    struct File {
        fs::path path = "rd_reference-" + std::to_string(supple::test::processId()) + ".wav";
        bool created = false;
        ~File() {
            if (created) fs::remove(path);
        }
    } file;
    REQUIRE_FALSE(fs::exists(file.path));
    file.created = fs::copy_file((*rendered)["path"].asString(), file.path);
    auto args = json::Value::object();
    args["source"] = file.path.string();
    auto analyzed = dispatcher.execute({"analyze", std::move(args), kTest});
    REQUIRE_MESSAGE(analyzed, (analyzed ? "" : analyzed.error().toString()));
    CHECK((*analyzed)["report"]["source"]["seconds"].asDouble() == 1);
}

TEST_CASE("a surround file is analyzed with all its channels; other counts are refused") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s");
    cmd::Dispatcher dispatcher(registry, *host);
    auto analyzeChannels = [&](std::size_t count) {
        audio::AudioData audio;
        audio.sampleRate = 48000.0;
        for (std::size_t c = 0; c < count; ++c) {
            std::vector<float> channel(48000);
            for (std::size_t i = 0; i < channel.size(); ++i)  // only the surrounds sound
                channel[i] = c >= count - 2 ? 0.3f * static_cast<float>(std::sin(0.05 * i)) : 0.0f;
            audio.channels.push_back(std::move(channel));
        }
        const fs::path file = root.path / ("ch" + std::to_string(count) + ".wav");
        REQUIRE(audio::writeWav(file, audio, {}));
        auto args = json::Value::object();
        args["source"] = file.string();
        return dispatcher.execute({"analyze", std::move(args), kTest});
    };
    auto surround = analyzeChannels(6);
    REQUIRE_MESSAGE(surround, (surround ? "" : surround.error().toString()));
    CHECK((*surround)["report"]["source"]["channels"].asInt() == 6);
    CHECK((*surround)["report"]["source"]["layout"].asString() == "5.1");
    CHECK((*surround)["report"]["loudness"]["integrated_lufs"].isNumber());  // not silence
    auto odd = analyzeChannels(3);
    REQUIRE_FALSE(odd);
    CHECK(odd.error().code == "analysis.layout");
}

TEST_CASE("every sandbox command example runs against its fixture" * doctest::description("rt")) {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    int n = 0;
    // The dsp_player fixtures' examples need the script player built, and
    // dsp_player_editor's its editor, which builds without a GUI lack.
    auto st = cmd::runExamples(
        registry,
        [&](std::string_view fixture) -> std::unique_ptr<cmd::Host> {
            return makeHost(root.path / std::to_string(n++), {}, fixture.starts_with("dsp_player"));
        },
        [](std::string_view fixture) { return SUPPLE_EDITORS || fixture != "dsp_player_editor"; });
    INFO((st ? std::string() : st.error().toString()));
    CHECK(st);
    CHECK(n >= (SUPPLE_EDITORS ? 16 : 14));
}

#if SUPPLE_EDITORS
TEST_CASE("DSP previews draw deterministic native PNGs and report pointer edits") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "preview", {}, true);
    cmd::Dispatcher dispatcher(registry, *host);
    host->setupFixture(dispatcher, "dsp_player_editor");
    auto preview = [&](std::string_view args) {
        auto result = dispatcher.execute({"dsp.preview", j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    auto first = preview(R"({"script":"gain","seconds":0})");
    const fs::path path = first["path"].asString();
    const auto bytes = readFile(path);
    REQUIRE(bytes);
    REQUIRE(bytes->size() > 24);
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    CHECK(std::equal(signature.begin(), signature.end(), bytes->begin()));
    CHECK(first["width"].asInt() > 0);
    CHECK(first["height"].asInt() > 0);
    CHECK(first["edits"].size() == 0);
    CHECK(first["problems"].size() == 0);
    const char* artifacts = std::getenv("SUPPLE_PICTURE_OUTPUT_DIR");
    auto keep = [&](const char* name) {
        if (!artifacts || !*artifacts) return;
        fs::create_directories(artifacts);
        fs::copy_file(path, fs::path(artifacts) / name, fs::copy_options::overwrite_existing);
    };
    keep("dsp-preview.png");
    auto again = preview(R"({"script":"gain","seconds":0})");
    CHECK(again["width"] == first["width"]);
    CHECK(again["height"] == first["height"]);
    const auto repeated = readFile(path);
    REQUIRE(repeated);
    CHECK(*repeated == *bytes);
    auto dragged = preview(R"({"script":"gain","seconds":0,"pointer":[
        {"action":"drag","x":86,"y":132,"to_x":86,"to_y":92}]})");
    CHECK(dragged["edits"]["params"]["gain"].asDouble() > 0);
    CHECK(dragged["problems"].size() == 0);
    const auto changed = readFile(path);
    REQUIRE(changed);
    CHECK(*changed != *bytes);
    keep("dsp-preview-dragged.png");
}
#endif

TEST_CASE("an LLM-style session: load, program, render, batch with references") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s", root.path / "commands.ndjson");
    host->setFixedClock(1000);
    cmd::Dispatcher d(registry, *host);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };

    auto load = call("plugin.load", R"({"plugin": "com.suppleaudio.test.sine"})");
    REQUIRE(load);
    CHECK((*load)["instance"].asString() == "in_1");

    // One atomic batch: add the effect, set it with a reference to its id, render.
    auto batch = call("batch", R"({
        "label": "quieter phrase",
        "steps": [
          {"command": "plugin.load", "args": {"plugin": "com.suppleaudio.test.gain"}, "as": "fx"},
          {"command": "param.set", "args": {"instance": "${fx.instance}", "values": {"gain": "-6 dB"}}},
          {"command": "render", "args": {"notation": "voice lead; 1: A4:q r:q r:h", "name": "take 1"}, "as": "r1"}
        ]})");
    INFO((batch ? std::string()
                : batch.error().toString() + " / " +
                      (batch.error().details.empty() ? std::string()
                                                     : batch.error().details[0].toString())));
    REQUIRE(batch);
    json::Value set = (*batch)["results"].at(1), rendered = (*batch)["results"].at(2);
    CHECK(set["values"]["gain"]["value"].asDouble() == doctest::Approx(-6.0));
    CHECK(fs::exists(rendered["path"].asString()));
    // Notation's default velocity 96 -> 0.25 * 96/127, then -6 dB.
    CHECK(rendered["peak_dbfs"].asDouble() ==
          doctest::Approx(20 * std::log10(0.25 * 96 / 127) - 6).epsilon(0.001));

    // A failing batch rolls back everything, including its render.
    auto failed = call("batch", R"({
        "steps": [
          {"command": "render", "args": {"notation": "voice lead; 1: A4:h r:h"}, "as": "r2"},
          {"command": "param.set", "args": {"instance": "in_2", "values": {"gian": 1}}}
        ]})");
    REQUIRE(!failed);
    CHECK(failed.error().code == "batch.step_failed");
    REQUIRE(failed.error().details.size() == 1);
    CHECK(failed.error().details[0].path == "/steps/1/args/values/gian");
    auto session = call("session.get", "{}");
    REQUIRE(session);
    CHECK((*session)["renders"].size() == 1);
    CHECK(!fs::exists(root.path / "s" / "renders" / "rd_2.wav"));

    // Errors carry argument paths and, for notation, a text location.
    auto bad =
        call("render", R"({"notation": "voice lead; 1: C4:q D4:q"})");  // bar 1 does not add up
    REQUIRE(!bad);
    CHECK(bad.error().code.rfind("notation.", 0) == 0);
    CHECK(bad.error().path == "/args/notation");
    CHECK(bad.error().loc.has_value());
    auto unknown = call("param.get", R"({"instance": "in_9"})");
    REQUIRE(!unknown);
    CHECK(unknown.error().code == "sandbox.unknown_instance");
    CHECK(unknown.error().path == "/args/instance");
    auto wrongArgs = call("render", R"({"notation": 5})");
    REQUIRE(!wrongArgs);
    CHECK(wrongArgs.error().code == "command.invalid_args");

    // The command log has one NDJSON line per record, batch steps included.
    auto text = readTextFile(root.path / "commands.ndjson");
    REQUIRE(text);
    std::size_t lines = 0;
    for (char ch : *text) lines += ch == '\n';
    CHECK(lines == host->records().size());
    CHECK(lines >= 10);
    auto first = json::parse(text->substr(0, text->find('\n')));
    REQUIRE(first);
    CHECK((*first)["command"].asString() == "plugin.load");
    CHECK((*first)["time_ms"].asInt() == 1000);
}

TEST_CASE("session.set validates, render uses notation tempo and bars") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s");
    cmd::Dispatcher d(registry, *host);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    REQUIRE(call("plugin.load", R"({"plugin": "com.suppleaudio.test.sine"})"));
    auto bad = call("session.set", R"({"meter": "99999999999/4"})");
    REQUIRE(!bad);
    CHECK(bad.error().path == "/args/meter");
    CHECK(call("session.set", R"({"sample_rate": "44.1 kHz"})"));
    auto r =
        call("render",
             R"({"notation": "voice lead; 1: A4:q r:q r:h", "bars": 2, "tempo": 60, "tail_s": 0})");
    INFO((r ? std::string() : r.error().toString()));
    REQUIRE(r);
    CHECK((*r)["seconds"].asDouble() == doctest::Approx(8.0));  // 2 bars of 4/4 at 60 BPM
    CHECK(host->session().renders().at("rd_1").sampleRate == 44100.0);
}

TEST_CASE("registry exports are deterministic and name every sandbox tool") {
    cmd::Registry a, b;
    registerCommands(a);
    registerCommands(b);
    CHECK(json::writeCanonical(a.exportJson()) == json::writeCanonical(b.exportJson()));
    const auto tools = cmd::mcpToolList(a);
    std::string names = json::writeCompact(tools);
    for (const char* t : {"plugin_load", "param_set", "patch_merge", "render", "session_get"})
        CHECK(names.find(t) != std::string::npos);
}

TEST_CASE("sandbox invocation hook restores state and discards failed invocation events") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("supple-savepoint-hook-" + std::to_string(supple::test::processId()));
    struct Cleanup {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    fs::remove_all(root);
    SessionOptions options;
    options.workdir = root;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = Session::open(options);
    REQUIRE(opened);
    SandboxHost host(std::move(*opened));
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher dispatcher(registry, host);
    host.attachCommands(dispatcher);
    REQUIRE(host.scripting()->run("watcher", R"(
        seen = {}
        q.on("test.changed", function(payload) seen[#seen + 1] = payload.value end)
    )",
                                  -1.0));
    auto id = host.session().load("com.suppleaudio.test.gain");
    REQUIRE(id);
    auto transaction = host.beginTransaction(cmd::Origin{}, "savepoint");
    REQUIRE(transaction);
    host.emit("test.changed", j(R"({"value":"before"})"));
    auto restore = host.invocationSavepoint();
    REQUIRE(restore);
    REQUIRE(host.session().patchMerge(*id, *json::parse(R"({"params":{"gain":-6}})")));
    host.emit("test.changed", j(R"({"value":"failed"})"));
    REQUIRE((*restore)());
    auto params = host.session().paramGet(*id);
    REQUIRE(params);
    CHECK((*params)["gain"]["value"].asDouble() == doctest::Approx(0));
    const auto events = host.takeEvents();
    REQUIRE(events.size() == 1);
    CHECK(*events[0].second.find("value") == "before");
    CHECK(host.scripting()->pendingEvents() == 1);
    host.commandFinished();
    auto seen = host.scripting()->run("watcher", "return seen", -1.0);
    REQUIRE(seen);
    REQUIRE(seen->result.size() == 1);
    CHECK(seen->result.at(0) == "before");
    host.emit("test.changed", j(R"({"value":"after"})"));
    host.commandFinished();
    seen = host.scripting()->run("watcher", "return seen", -1.0);
    REQUIRE(seen);
    REQUIRE(seen->result.size() == 2);
    CHECK(seen->result.at(1) == "after");
    REQUIRE(host.session().inTransaction());
    REQUIRE(host.rollbackTransaction(*transaction));
}

TEST_CASE("sandbox bus refuses failed invocation snapshots and aborts caught restore failures") {
    Root root;
    auto host = makeHost(root.path);
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Command<cmd::NoArgs, cmd::NoArgs> failure;
    failure.name = "test.poison";
    failure.kind = cmd::Kind::Edit;
    failure.exposure = cmd::Exposure::Internal;
    failure.args = schema::object<cmd::NoArgs>("no_args").build();
    failure.result = failure.args;
    failure.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<cmd::NoArgs> {
        auto instance = ctx.service<Session>().instance("in_1");
        if (!instance) return instance.error();
        const std::array<std::uint8_t, 4> cannotRestore{8, 0, 0, 0};
        SUPPLE_TRY((*instance)->loadState(cannotRestore));
        return Error("test.partial", "failed after changing state", "/instance", "retry");
    };
    registry.add(std::move(failure));
    cmd::Dispatcher bus(registry, *host);
    host->attachCommands(bus);
    auto id = host->session().load("com.suppleaudio.test.edge");
    REQUIRE(id);
    auto instance = host->session().instance(*id);
    REQUIRE(instance);
    const std::array<std::uint8_t, 4> cannotSave{7, 0, 0, 0};
    const std::array<std::uint8_t, 4> good{};
    auto result = bus.transact("capture", kTest, [&]() -> Result<json::Value> {
        REQUIRE((*instance)->loadState(cannotSave));
        auto refused =
            bus.execute({"plugin.load", j(R"({"plugin":"com.suppleaudio.test.gain"})"), kTest});
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "sandbox.snapshot_failed");
        CHECK(refused.error().path == "/instances/in_1");
        CHECK(host->session().chain().size() == 1);
        REQUIRE((*instance)->loadState(good));
        return json::Value();
    });
    REQUIRE(result);
    CHECK_FALSE(host->session().inTransaction());
    result = bus.execute({"script.run", j(R"lua({"code":
        "q.batch('restore', function() local ok,err=pcall(q.test.poison,{}); assert(not ok and err.code=='test.partial'); assert(err.details[1].code=='sandbox.rollback_failed') end)"})lua"),
                          kTest});
    REQUIRE_FALSE(result);
    CHECK_FALSE(host->session().inTransaction());
    // The outer rollback fails too because this fixture now refuses all restores.
    CHECK(result.error().code == "sandbox.rollback_failed");
    std::function<bool(const Error&, std::string_view)> contains = [&](const Error& e,
                                                                       std::string_view code) {
        if (e.code == code) return true;
        for (const auto& detail : e.details)
            if (contains(detail, code)) return true;
        return false;
    };
    CHECK(contains(result.error(), "transaction.restore_failed"));
    CHECK(contains(result.error(), "sandbox.restore_failed"));
    const auto events = host->takeEvents();
    CHECK(std::count_if(events.begin(), events.end(),
                        [](const auto& e) { return e.first == "sandbox.savepoint_failed"; }) == 2);
}

TEST_CASE("sandbox plugin list exposes cached failures even for an empty catalogue") {
    Root root;
#if defined(_WIN32)
    static constexpr const char* cacheVariable = "LOCALAPPDATA";
#else
    static constexpr const char* cacheVariable = "XDG_CACHE_HOME";
#endif
    struct Environment {
        std::optional<std::string> previous;
        ~Environment() {
            if (previous)
                supple::test::environment(cacheVariable, previous->c_str());
            else
                supple::test::environment(cacheVariable, nullptr);
        }
    } env;
    if (const char* previous = std::getenv(cacheVariable)) env.previous = previous;
    const auto cache = root.path / "cache";
    REQUIRE(supple::test::environment(cacheVariable, cache.string().c_str()) == 0);
    fs::create_directories(root.path);
    struct Bridge {
        fs::path previous = BRIDGE_PATH;
        ~Bridge() { host::setBridgeExecutable(previous); }
    } bridge;
    host::setBridgeExecutable(BRIDGE_PATH);
    const auto bad = root.path / "bad.clap";
    REQUIRE(writeFileAtomic(bad, "invalid plugin"));
    SessionOptions options;
    options.workdir = root.path;
    options.pluginPath = {bad};
    auto opened = Session::open(options);
    REQUIRE(opened);
    SandboxHost host(std::move(*opened));
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto first = bus.execute({"plugin.list", j("{}"), kTest});
    REQUIRE(first);
    CHECK((*first)["plugins"].size() == 0);
    auto failure = (*first)["scan"]["failures"].asArray()[0];
    CHECK(failure["module"] == fs::canonical(bad).string());
    CHECK(failure["reason"]["code"] == "host.load");
    CHECK_FALSE(failure["cached"].asBool());
    host::setBridgeExecutable(root.path / "missing-bridge");
    auto second = bus.execute({"plugin.list", j(R"({"rescan":true})"), kTest});
    REQUIRE(second);
    CHECK((*second)["scan"]["warnings"].asArray()[0]["code"] == "host.scan.unsupervised");
    CHECK((*second)["scan"]["failures"].asArray()[0]["cached"].asBool());
    CHECK((*second)["scan"]["cached_modules"].asInt() == 1);
    const std::vector<fs::path> empty;
    REQUIRE(host::scan(empty));
    auto retained = bus.execute({"plugin.list", j("{}"), kTest});
    REQUIRE(retained);
    CHECK((*retained)["scan"] == (*second)["scan"]);
    CHECK((*retained)["scan"]["warnings"].isArray());
}

TEST_CASE("plugin queries report missing current values as contract errors") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "missing");
    cmd::Dispatcher d(registry, *host);
    REQUIRE(
        d.execute({"plugin.load", j(R"({"plugin":"com.suppleaudio.test.missing-value"})"), kTest}));
    for (const auto command : {"param.get", "plugin.describe"}) {
        auto result = d.execute({command, j(R"({"instance":"in_1"})"), kTest});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "sandbox.plugin_contract");
        CHECK_FALSE(result.error().path.empty());
        CHECK_FALSE(result.error().hint.empty());
    }
    auto full = d.execute({"plugin.describe", j(R"({"instance":"in_1","detail":"full"})"), kTest});
    REQUIRE_FALSE(full);
    CHECK(full.error().code == "sandbox.plugin_contract");
    auto selected = d.execute({"param.get", j(R"({"instance":"in_1","keys":["gain"]})"), kTest});
    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == "sandbox.plugin_contract");
}
TEST_CASE("plugin describe falls back when semantic IDs do not match CLAP") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "mismatch");
    cmd::Dispatcher d(registry, *host);
    auto load = d.execute(
        {"plugin.load", j(R"({"plugin":"com.suppleaudio.test.semantic-id-mismatch"})"), kTest});
    REQUIRE(load);
    auto full = d.execute({"plugin.describe", j(R"({"instance":"in_1","detail":"full"})"), kTest});
    REQUIRE(full);
    REQUIRE((*full)["params"].find("0"));
    CHECK((*full)["params"]["0"]["value"].isNumber());
    CHECK_FALSE((*full)["params"].find("gain"));
}
