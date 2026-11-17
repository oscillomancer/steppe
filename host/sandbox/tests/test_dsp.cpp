// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The DSP script harness (dsp.test) through the real script player: what it
// measures is what the script does, and what goes wrong is said in words.
// And dsp.export: a copy of the player that plays one script anywhere.
#include <doctest/doctest.h>
#include "../../../tests/file_platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>

#include "../../dsp-commands/artifact_name.h"
#include "../../dsp-commands/dsp_export_platform.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/sandbox/commands.h"

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kStock = STOCK_CLAP_DIR;
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

struct Root {
    fs::path path = fs::temp_directory_path() /
                    ("supple-sandbox-dsp-" + std::to_string(supple::test::processId()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::string script(const std::string& name, const std::string& id, const std::string& body) {
    return "#include <supple/script.h>\n\nstruct " + name +
           " : Effect {\n    static constexpr Info info = {.name = \"" + name + "\", .id = \"" +
           id + "\"};\n" + body + "\n};\n";
}

// New harness cases share the real player and the small host fixtures.
struct Harness {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<SandboxHost> host;
    std::unique_ptr<cmd::Dispatcher> bus;

    Harness() {
        SessionOptions options;
        options.workdir = root.path;
        options.pluginPath = {fs::path(FIXTURE_CLAP_PATH), kStock / "Script Player.clap"};
        auto session = Session::open(options);
        REQUIRE(session);
        host = std::make_unique<SandboxHost>(std::move(*session), fs::path{});
        registerCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *host);
    }
    Result<json::Value> test(std::string_view args) {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        return bus->execute({"dsp.test", *parsed, kTest});
    }
    json::Value measured(std::string_view args) {
        auto result = test(args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    void write(const std::string& name, const std::string& source) {
        auto args = json::Value::object();
        args["name"] = name;
        args["source"] = source;
        auto result = bus->execute({"dsp.write", args, kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        REQUIRE_MESSAGE((*result)["ok"].asBool(), json::writeCompact((*result)["diagnostics"]));
    }
};

}  // namespace

TEST_CASE("DSP artifact names are bounded and distinguish altered plugin IDs") {
    using dsp_commands::artifactId;
    CHECK(artifactId("com.example-safe_1.0") == "com.example-safe_1.0");
    CHECK(artifactId("com.suppleaudio.marram") == "com.suppleaudio.marram");
    CHECK(artifactId("com.example:g") == "com.example_g-cc81f5fbfb637d01");
    CHECK(artifactId("com.example:g") != artifactId("com.example_g-cc81f5fbfb637d01"));
    CHECK(artifactId("x-0123456789ABCDEF") != "x-0123456789ABCDEF");
    CHECK(artifactId("x-0123456789abcdeg") == "x-0123456789abcdeg");
    CHECK(artifactId("x-0123456789abcde") == "x-0123456789abcde");
    CHECK(artifactId("a/b") != artifactId("a_b"));
    CHECK(artifactId("a/b") != artifactId("a\\b"));
    const std::string prefix(64, 'a');
    CHECK(artifactId(prefix + "b") != artifactId(prefix + "c"));
    for (const std::string id : {"vst3:abc", "../../escape", "caf\xc3\xa9", ""}) {
        const auto name = artifactId(id);
        CHECK(name.size() <= 81);
        CHECK(name.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") ==
              std::string::npos);
        CHECK(artifactId(id) == name);
    }
}

TEST_CASE("DSP saved artifacts retain long plugin IDs only in metadata") {
    Harness h;
    const std::string id = "com.example." + std::string(240, 'a');
    h.write("long_id", script("LongId", id, "void process(Block& io) {}"));
    auto result =
        h.measured(R"({"script":"long_id","signals":["silence"],"seconds":0.1,"save":true})");
    CHECK(result["plugin"].asString() == id);
    const fs::path saved = result["signals"].at(0).find("path")->asString();
    CHECK(saved.parent_path() == h.root.path / "renders");
    CHECK(saved.filename().string().size() < 120);
    CHECK(fs::is_regular_file(saved));
}

TEST_CASE("DSP render commands locate invalid patch values inside the patch argument") {
    Harness h;
    h.write("gain",
            script("Gain", "com.example.patch-errors",
                   "Param gain = decibels(\"Gain\", -24, 24, 0); void process(Block& io) {}"));
    for (const std::string command : {"dsp.test", "dsp.preview"}) {
        if (command == "dsp.preview" && !SUPPLE_EDITORS) continue;
        auto args = json::parse(R"({"script":"gain","patch":{"params":{"gain":1000}}})").value();
        auto result = h.bus->execute({command, args, kTest});
        REQUIRE_FALSE(result);
        CHECK(result.error().path == "/args/patch/params/gain");
        CHECK_FALSE(result.error().code.empty());
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
    }
}

#if SUPPLE_EDITORS
TEST_CASE("the automatic editor lists a script's routes and edits them as a patch would") {
    Harness h;
    h.write("routed", R"(#include <supple/script.h>
struct Routed : Instrument {
    static constexpr Info info = {.name = "Routed", .id = "com.example.routed", .voices = 2};
    Param level = decibels("Level", -48, 0, -12).modulated();
    void startVoice(int v) {}
    void releaseVoice(int v) { voiceFinished(v); }
    void process(Block& io) {}
};
)");
    auto preview = [&](const std::string& pointer) {
        auto args = json::parse(R"({"script":"routed","seconds":0,)"
                                R"("patch":{"routes":{"modwheel>level":{"amount":"+6 dB"}}},)"
                                R"("pointer":)" +
                                pointer + "}")
                        .value();
        auto result = h.bus->execute({"dsp.preview", args, kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    // The route's amount bar, dragged right; its remove button, clicked.
    auto dragged = preview(R"([{"action":"drag","x":470,"y":302,"to_x":520,"to_y":302}])");
    CHECK(dragged["problems"].size() == 0);
    INFO(json::writeCompact(dragged["edits"]));
    CHECK(dragged["edits"]["routes"]["modwheel>level"]["amount"].asDouble() > 6.0);
    auto removed = preview(R"([{"action":"click","x":668,"y":302}])");
    CHECK(json::writeCompact(removed["edits"]) == R"({"routes":{"modwheel>level":null}})");
}
#endif

TEST_CASE("results name the parameters that do not apply with the values they leave") {
    Harness h;
    h.write("sometimes", script("Sometimes", "com.example.sometimes",
                                "    Param on = toggle(\"On\", true);\n"
                                "    Param gain = decibels(\"Gain\", -24, 24, 0).activeWhen(on);\n"
                                "    void process(Block& io) {}"));
    auto run = [&](const char* command, const std::string& args) {
        auto result = h.bus->execute({command, json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    const std::string instance =
        run("plugin.load", R"({"plugin":"com.example.sometimes"})")["instance"].asString();
    const std::string at = R"({"instance":")" + instance + R"(")";
    CHECK(run("plugin.describe", at + "}").find("inactive") == nullptr);
    auto merged = run("patch.merge", at + R"(,"patch":{"params":{"on":false,"gain":-6}}})");
    REQUIRE(merged.find("inactive"));
    CHECK(json::writeCompact(merged["inactive"]) == R"(["gain"])");
    auto summary = run("plugin.describe", at + "}");
    CHECK(json::writeCompact(summary["inactive"]) == R"(["gain"])");
    CHECK(json::writeCompact(summary["params"]).find("gain") == std::string::npos);
    auto set = run("param.set", at + R"(,"values":{"gain":-3}})");
    CHECK_FALSE(set["values"]["gain"]["active"].asBool());
    CHECK(run("param.set", at + R"(,"values":{"on":true}})")["values"]["on"].find("active") ==
          nullptr);
}

TEST_CASE("two scripts with one id are refused: the player would play only one") {
    Harness h;
    const std::string body = "    void process(Block& io) {}";
    h.write("first", script("First", "com.example.twin", body));
    auto args = json::Value::object();
    args["name"] = "second";
    args["source"] = script("Second", "com.example.twin", body);
    auto refused = h.bus->execute({"dsp.write", args, kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.id_taken");
    CHECK(refused.error().path == "/args/source");
    // One saved some other way is refused by name too.
    auto list = h.bus->execute({"dsp.list", json::Value::object(), kTest});
    REQUIRE(list);
    const fs::path library((*list)["library"].asString());
    CHECK_FALSE(fs::exists(library / "second"));
    fs::create_directories(library / "second");
    std::ofstream(library / "second" / "second.cpp") << args["source"].asString();
    auto tested = h.test(R"({"script":"second","signals":["sine"],"seconds":0.1})");
    REQUIRE_FALSE(tested);
    CHECK(tested.error().code == "dsp.id_taken");
    CHECK(tested.error().path == "/args/script");
}

TEST_CASE("a source export's tests move a parameter whose name survives CMake and C") {
    Harness h;
    auto exportSource = [&](const std::string& params) {
        h.write("named",
                script("Named", "com.example.named", params + "    void process(Block& io) {}"));
        auto args = json::Value::object();
        args["script"] = "named";
        args["format"] = "source";
        args["folder"] = (h.root.path / "named-source").string();
        return h.bus->execute({"dsp.export", args, kTest});
    };
    auto refused = exportSource("    Param gain = decibels(\"Out \\\"dB\\\"\", -24, 24, 0);\n");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.no_parameter");
    CHECK(refused.error().path == "/args/script");
}

TEST_CASE("dsp.test automation uses physical values and detects abrupt steps") {
    Harness h;
    // The command's value reaches the renderer at sample 12012, not the next block.
    auto value = h.measured(R"({"plugin":"com.suppleaudio.test.gain","signals":["sine"],
        "save":true,"frequency_hz":1000,"automation":[{"param":"gain","at_s":0.25025,"value":-6}]})");
    auto audio = audio::readAudio(value["signals"].at(0).find("path")->asString());
    REQUIRE(audio);
    const auto& x = audio->channels[0];
    const double amplitude = std::pow(10.0, -12.0 / 20.0);
    for (std::size_t i : {12011u, 12012u}) {
        const double input = amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                  static_cast<double>(i) / 48000.0);
        CHECK(
            static_cast<double>(x[i]) ==
            doctest::Approx(input * (i < 12012 ? 1.0 : std::pow(10.0, -6.0 / 20.0))).epsilon(1e-5));
    }
    CHECK_FALSE(value["signals"].at(0).find("thd_percent"));
    for (const std::string name : {"abrupt", "smooth"}) {
        h.write(name, script("Gain", "com.example." + name,
                             "Param gain = decibels(\"Gain\", -60, 0, -60)" +
                                 std::string(name == "smooth" ? ".smoothed(20)" : "") + R"(;
            void process(Block& io) {
                for (int c = 0; c < io.channels; ++c)
                    for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
            })"));
    }
    auto abrupt = h.measured(R"({"script":"abrupt","signals":["sine","sine"],"frequency_hz":100,
        "automation":[{"param":"gain","at_s":0.2525,"value":0}]})");
    auto smooth = h.measured(R"({"script":"smooth","signals":["sine"],"frequency_hz":100,
        "automation":[{"param":"gain","at_s":0.2525,"value":0}]})");
    const auto& step = *abrupt["signals"].at(0).find("automation");
    CHECK(step.find("max_step")->asDouble() > 0.2);
    const auto& smoothedStep = *smooth["signals"].at(0).find("automation");
    CHECK(smoothedStep.find("max_step")->asDouble() < step.find("max_step")->asDouble() * 0.1);
    CHECK(abrupt["problems"].at(0).asString().find("zipper noise") != std::string::npos);
    CHECK(abrupt["signals"].at(0) == abrupt["signals"].at(1));  // a fresh initial value per pass
}

TEST_CASE(
    "dsp.test compressor ducks under a timed key, measures a silent reference, and keys from its "
    "input without one") {
    Harness h;
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/compressor.cpp");
    std::stringstream source;
    source << in.rdbuf();
    h.write("compressor", source.str());
    auto active = h.measured(R"({"script":"compressor","signals":["sine"],"level_db":-30,
        "patch":{"params":{"attack":1}},"sidechain":{"level_db":-6}})");
    const auto& duck = *active["signals"].at(0).find("sidechain");
    CHECK(duck.find("gain_change_db")->asDouble() < -10.0);
    CHECK(duck.find("reference_rms_dbfs")->asDouble() == doctest::Approx(-33.01).epsilon(0.001));
    CHECK_FALSE(active["signals"].at(0).find("thd_percent"));
    auto silent = h.measured(R"({"script":"compressor","signals":["sine","silence"],
        "level_db":-30,"sidechain":{"signal":"silence"}})");
    const auto& silentSine = *silent["signals"].at(0).find("sidechain");
    const auto& silentMain = *silent["signals"].at(1).find("sidechain");
    CHECK(silentSine.find("gain_change_db")->asDouble() == 0.0);
    CHECK(silentMain.find("gain_change_db")->isNull());
    // With no key routed, the key port is deactivated and the compressor
    // listens to its own input: a loud sine is turned down.
    auto own = h.measured(R"({"script":"compressor","signals":["sine"],"level_db":-6,
        "patch":{"params":{"attack":1}}})");
    CHECK(own["signals"].at(0).find("gain_db")->asDouble() < -5.0);
}

TEST_CASE("dsp.test custom notes keep exact timing, velocity and their own final release") {
    Harness h;
    h.write("gates", R"(#include <supple/script.h>
        struct Gates : Instrument {
            static constexpr Info info = {.name="Gates", .id="com.example.gates", .voices=4};
            Array<int, 4> remaining;
            void startVoice(int v) { remaining[v] = -1; }
            void releaseVoice(int v) { remaining[v] = 240; }
            void process(Block& io) {
                for (int v = 0; v < voiceCount(); ++v) {
                    if (!voicePlaying(v)) continue;
                    for (int i = 0; i < io.frames; ++i) {
                        if (remaining[v] == 0) { voiceFinished(v); break; }
                        for (int c = 0; c < io.channels; ++c) io[c][i] += 0.1 * voiceVelocity(v);
                        if (remaining[v] > 0) --remaining[v];
                    }
                }
            }
        };)");
    auto result = h.measured(R"({"script":"gates","save":true,
        "notes":[{"key":60,"velocity":0.4,"start_s":0.013,"length_s":0.05},
                 {"key":60,"velocity":0.8,"start_s":0.043,"length_s":0.06}]})");
    REQUIRE(result["signals"].size() == 2);
    const auto& notes = result["signals"].at(0);
    CHECK(notes.find("signal")->asString() == "notes");
    CHECK_FALSE(notes.find("pitch_hz"));
    CHECK(notes.find("release_ms")->asDouble() == 5.0);
    auto audio = audio::readAudio(notes.find("path")->asString());
    REQUIRE(audio);
    const auto& x = audio->channels[0];
    CHECK(x[623] == 0.0f);
    CHECK(static_cast<double>(x[624]) == doctest::Approx(0.04));
    CHECK(static_cast<double>(x[2064]) == doctest::Approx(0.12));
    CHECK(static_cast<double>(x[3263]) == doctest::Approx(0.12));
    CHECK(static_cast<double>(x[3264]) == doctest::Approx(0.08));
    CHECK(static_cast<double>(x[5183]) == doctest::Approx(0.08));
    CHECK(x[5184] == 0.0f);
    CHECK(result["signals"].at(1).find("peak_dbfs")->asDouble() == -200.0);
}

TEST_CASE("dsp.test rejects invalid automation, key and custom-note arguments at their paths") {
    Harness h;
    auto check = [&](std::string_view args, std::string_view path) {
        auto result = h.test(args);
        REQUIRE_FALSE(result);
        if (result.error().code == "command.invalid_args") {
            REQUIRE(result.error().details.size() == 1);
            CHECK(result.error().details[0].path == "/args" + std::string(path));
        } else {
            CHECK(result.error().path == "/args" + std::string(path));
        }
        CHECK_FALSE(result.error().code.empty());
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
    };
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"missing","at_s":0.2,"value":0}]})",
        "/automation/0/param");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"gain","at_s":0.2,"value":999}]})",
        "/automation/0/value");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"gain","at_s":1,"value":0}]})",
        "/automation/0/at_s");
    check(
        R"({"plugin":"com.suppleaudio.test.keymix","automation":[{"param":"gain","at_s":0.2,"value":0}]})",
        "/automation");
    h.write(
        "meter",
        script("Meter", "com.example.readonly",
               "Param level = decibels(\"Level\", -60, 0, 0).meter(); void process(Block& io) {}"));
    check(R"({"script":"meter","automation":[{"param":"level","at_s":0.2,"value":0}]})",
          "/automation/0/param");
    check(R"({"plugin":"com.suppleaudio.test.gain","sidechain":{}})", "/sidechain");
    check(R"({"plugin":"com.suppleaudio.test.keymix","sidechain":{"start_s":1}})",
          "/sidechain/start_s");
    check(R"({"plugin":"com.suppleaudio.test.keymix","sidechain":{"length_s":0.000000001}})",
          "/sidechain/length_s");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","notes":[{"key":60,"start_s":0.2,"length_s":1}]})",
        "/notes/0/length_s");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","notes":[{"key":128,"start_s":0.2,"length_s":0.1}]})",
        "/notes/0/key");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","signals":["note"],"notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","signals":["silence"],"notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
}

TEST_CASE("dsp.test renders at the sample rate it is given") {
    Harness h;
    h.write("rate", script("Rate", "com.example.rate",
                           "    void prepare() { setLatency(roundToInt(sampleRate() / 1000)); }\n"
                           "    void process(Block& io) {}"));
    auto session = h.measured(R"({"script":"rate","signals":["sine"],"seconds":0.1})");
    CHECK(session["sample_rate"].asDouble() == 48000.0);
    CHECK(session["latency"].asInt() == 48);
    auto given =
        h.measured(R"({"script":"rate","signals":["sine"],"seconds":0.1,"sample_rate":96000})");
    CHECK(given["sample_rate"].asDouble() == 96000.0);
    CHECK(given["latency"].asInt() == 96);
    CHECK(std::abs(given["signals"].at(0).find("gain_db")->asDouble()) < 0.01);
    auto aliased =
        h.test(R"({"script":"rate","signals":["sine"],"frequency_hz":5000,"sample_rate":8000})");
    REQUIRE_FALSE(aliased);
    CHECK(aliased.error().code == "dsp.frequency");
    CHECK(aliased.error().path == "/args/frequency_hz");
}

TEST_CASE("dsp.test reports meters as they stood when the signal ended") {
    Harness h;
    // Each block's peak: the sine's while it plays, -60 once the tail is silent.
    h.write("peak", script("Peak", "com.example.peak",
                           "    Param level = decibels(\"Level\", -60, 0, -60).meter();\n"
                           "    void process(Block& io) {\n"
                           "        double peak = 0.0;\n"
                           "        for (int i = 0; i < io.frames; ++i) peak = max(peak, "
                           "abs(io[0][i]));\n"
                           "        level.set(max(-60.0, gainToDb(peak)));\n"
                           "    }"));
    auto result = h.measured(R"({"script":"peak","signals":["sine"],"level_db":-12})");
    const json::Value* meters = result["signals"].at(0).find("meters");
    REQUIRE(meters);
    CHECK(meters->find("level")->asDouble() == doctest::Approx(-12.0).epsilon(0.01));
}

TEST_CASE("dsp.test plays a file through an effect, at the render's rate, as the only signal") {
    Harness h;
    h.write("peak", script("Peak", "com.example.peak",
                           "    Param level = decibels(\"Level\", -60, 0, -60).meter();\n"
                           "    Param right = decibels(\"Right\", -60, 0, -60).meter();\n"
                           "    void process(Block& io) {\n"
                           "        double l = 0.0, r = 0.0;\n"
                           "        for (int i = 0; i < io.frames; ++i) {\n"
                           "            l = max(l, abs(io[0][i]));\n"
                           "            r = max(r, abs(io[1][i]));\n"
                           "        }\n"
                           "        level.set(max(-60.0, gainToDb(l)));\n"
                           "        right.set(max(-60.0, gainToDb(r)));\n"
                           "    }"));
    // A mono 44.1 kHz file of half a second at -6 dBFS: it plays on both
    // channels at 48 kHz.
    audio::AudioData tone;
    tone.sampleRate = 44100.0;
    tone.channels.assign(1, std::vector<float>(22050, 0.0f));
    for (size_t i = 0; i < 22050; ++i)
        tone.channels[0][i] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / 44100.0));
    const fs::path file = h.root.path / "tone.wav";
    REQUIRE(audio::writeWav(file, tone));
    auto args = json::Value::object();
    args["script"] = "peak";
    args["audio_file"] = file.string();
    args["sample_rate"] = 48000;
    auto result = h.measured(json::writeCompact(args));
    REQUIRE(result["signals"].size() == 1);
    json::Value signal = result["signals"].at(0);
    CHECK(signal["signal"].asString() == "file");
    CHECK(signal["meters"]["level"].asDouble() == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(signal["meters"]["right"].asDouble() == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(json::writeCompact(result["log"]).find("resampled to 48000 Hz") != std::string::npos);
    // The file signal needs its file.
    auto missing = h.test(R"({"script":"peak","signals":["file"]})");
    REQUIRE(!missing);
    CHECK(missing.error().code == "dsp.file");
    // A relative path would name a file wherever the host happens to run.
    auto relative = h.test(R"({"script":"peak","audio_file":"tone.wav"})");
    REQUIRE(!relative);
    CHECK(relative.error().code == "dsp.file");
}

TEST_CASE("dsp.test gives what a script traced, interval by interval, and says what was NaN") {
    Harness h;
    // Every frame's count since prepare(), at that frame: each interval's
    // last is the frame before the next interval starts.
    h.write("counter", script("Counter", "com.example.counter",
                              "    int n = 0;\n"
                              "    void prepare() { n = 0; }\n"
                              "    void process(Block& io) {\n"
                              "        for (int i = 0; i < io.frames; ++i) {\n"
                              "            trace(\"frame\", n, i);\n"
                              "            n += 1;\n"
                              "        }\n"
                              "        trace(\"broken\", 0.0 / (n - n));\n"
                              "    }"));
    auto result = h.measured(
        R"({"script":"counter","signals":["sine"],"seconds":0.5,"sample_rate":48000,"trace_points":5})");
    json::Value signal = result["signals"].at(0);
    CHECK(signal["trace_interval_s"].asDouble() == doctest::Approx(0.1));
    json::Value traces = signal["traces"];
    REQUIRE(traces.size() == 2);
    json::Value frame = traces.at(0), broken = traces.at(1);
    CHECK(frame["name"].asString() == "frame");
    for (size_t k = 0; k < 5; ++k) {
        CHECK(frame["last"].at(k).asDouble() == 4800.0 * static_cast<double>(k + 1) - 1);
        CHECK(frame["low"].at(k).asDouble() == 4800.0 * static_cast<double>(k));
    }
    CHECK(broken["name"].asString() == "broken");
    CHECK(broken["last"].at(0).isNull());
    CHECK(broken["non_finite"].asInt() > 0);
    REQUIRE(result["problems"].size() == 1);
    CHECK(result["problems"].at(0).asString().find("\"broken\"") != std::string::npos);
}

TEST_CASE("dsp.edit replaces text in a script in order, and saves nothing when an edit misses") {
    Harness h;
    h.write("gain", script("Gain", "com.example.edit",
                           "    Param gain = decibels(\"Gain\", -24, 24, 0);\n"
                           "    void process(Block& io) {}"));
    auto edit = [&](std::string_view edits) {
        auto args = json::Value::object();
        args["name"] = "gain";
        args["edits"] = json::parse(edits).value();
        return h.bus->execute({"dsp.edit", args, kTest});
    };
    // The second edit sees what the first left.
    auto edited = edit(R"([{"old":"-24, 24, 0","new":"-36, 36, 0"},
                           {"old":"\"Gain\", -36","new":"\"Level\", -36"}])");
    REQUIRE_MESSAGE(edited, (edited ? "" : edited.error().toString()));
    CHECK((*edited)["ok"].asBool());
    json::Value param = (*edited)["plugin"]["params"].at(0);
    CHECK(param["min"].asDouble() == -36.0);
    const fs::path file = (*edited)["path"].asString();
    const std::string after = readTextFile(file).value();
    CHECK(after.find("decibels(\"Level\", -36, 36, 0)") != std::string::npos);
    // Text that is nowhere, or in two places: an error at the edit, and the
    // script as it was, though the edit before it matched.
    auto missing = edit(R"([{"old":"void process","new":"void process "},
                            {"old":"decibels(\"Level\", -36, 24","new":"x"}])");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "dsp.edit");
    CHECK(missing.error().path == "/args/edits/1/old");
    CHECK(missing.error().message.find("at line 5") != std::string::npos);
    auto twice = edit(R"([{"old":"36","new":"48"}])");
    REQUIRE_FALSE(twice);
    CHECK(twice.error().message.find("2 times") != std::string::npos);
    CHECK(readTextFile(file).value() == after);
    // A script saved with Windows line ends takes text copied without them,
    // and keeps its own.
    std::string windows;
    for (char ch : after) windows += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    REQUIRE(writeFileAtomic(file, windows));
    auto lines = edit(R"([{"old":"36, 0);\n    void process","new":"36, 6);\n    void process"}])");
    REQUIRE_MESSAGE(lines, (lines ? "" : lines.error().toString()));
    CHECK(readTextFile(file).value().find("36, 6);\r\n    void process") != std::string::npos);
}

TEST_CASE("every patch in the synth guide plays on Marram") {
    if (!fs::exists(kStock / "Marram.clap")) {
        MESSAGE("skipped: no Marram in " << kStock.string());
        return;
    }
    std::ifstream in(fs::path(SUPPLE_SKILL_GUIDES) / "sound-design.md");
    REQUIRE(in);
    std::stringstream guide;
    guide << in.rdbuf();
    const std::string text = guide.str();
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {kStock};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, std::string_view args) {
        auto r = bus.execute({std::move(name), json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("plugin.load", R"({"plugin":"com.suppleaudio.marram"})");
    int patches = 0;
    for (size_t at = text.find("```json\n"); at != std::string::npos;
         at = text.find("```json\n", at)) {
        at += 8;
        const std::string patch = text.substr(at, text.find("\n```", at) - at);
        INFO(patch.substr(0, 120));
        call("patch.set", R"({"instance":"in_1","patch":)" + patch + "}");
        CHECK(call("render", R"({"notation":"1: C3:q r:*","tail_s":0})")["peak_dbfs"].asDouble() >
              -40);
        ++patches;
    }
    CHECK(patches == 11);  // the anatomy and ten recipes (the evaluation tasks' answers)
}

TEST_CASE("dsp.test measures a script through the player and names what goes wrong") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {kStock};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        auto r = bus.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](const std::string& name, const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = name;
        a["source"] = source;
        json::Value written = call("dsp.write", std::move(a));
        REQUIRE(written["ok"].asBool());
    };
    auto test = [&](const std::string& name) {
        json::Value a = json::Value::object();
        a["script"] = name;
        return call("dsp.test", std::move(a));
    };

    // +6 dB, clean: the numbers say so, and nothing is wrong.
    write("boost",
          script("Boost", "com.example.boost",
                 "    Param gain = decibels(\"Gain\", -12, 12, 6);\n"
                 "    void factoryPatches(Patches& p) { p.add(\"Unity\").set(gain, 0); }\n"
                 "    void process(Block& io) {\n"
                 "        for (int c = 0; c < io.channels; ++c)\n"
                 "            for (int i = 0; i < io.frames; ++i) io[c][i] *= gain.gain();\n"
                 "    }"));
    json::Value boost = test("boost");
    CHECK(boost["plugin"].asString() == "com.example.boost");
    std::string clean;
    for (const json::Value& p : boost["problems"].asArray()) clean += p.asString() + "\n";
    CHECK_MESSAGE(clean.empty(), clean);
    REQUIRE(boost["signals"].size() == 5);
    // Inspection addresses the loaded instance, independently of dsp.test.
    auto loaded = call("plugin.load", json::parse(R"({"plugin":"com.example.boost"})").value());
    json::Value inspect = json::Value::object();
    inspect["instance"] = loaded["instance"];
    auto described = call("plugin.describe", inspect);
    CHECK(described["status_available"].asBool());
    CHECK(described["problems"].size() == 0);
    auto factories = call(
        "preset.list", json::parse(R"({"plugin":"com.example.boost","source":"factory"})").value());
    REQUIRE(factories["presets"].size() == 1);
    CHECK(factories["presets"].at(0).find("read_only")->asBool());
    auto load = inspect;
    load["name"] = "Unity";
    load["source"] = "factory";
    call("preset.load", std::move(load));
    CHECK(call("patch.get", inspect)["patch"]["params"]["gain"].asDouble() == 0.0);
    const json::Value& sine = boost["signals"].at(0);
    CHECK(sine.find("signal")->asString() == "sine");
    CHECK(sine.find("gain_db")->asDouble() == doctest::Approx(6.0).epsilon(0.002));
    CHECK(sine.find("thd_percent")->asDouble() < 0.01);
    CHECK(sine.find("peak_dbfs")->asDouble() == doctest::Approx(-6.0).epsilon(0.01));
    const json::Value& impulse = boost["signals"].at(2);
    REQUIRE(impulse.find("response_db")->size() >= 9);
    for (const json::Value& point : impulse.find("response_db")->asArray())
        CHECK(point.find("db")->asDouble() == doctest::Approx(6.0).epsilon(0.01));
    CHECK(boost["signals"].at(4).find("peak_dbfs")->asDouble() < -150.0);

    // Sound from silence, a DC offset, and a NaN the framework silenced.
    write("broken", script("Broken", "com.example.broken",
                           "    void process(Block& io) {\n"
                           "        for (int c = 0; c < io.channels; ++c)\n"
                           "            for (int i = 0; i < io.frames; ++i)\n"
                           "                io[c][i] = io[c][i] > 0.2 ? log(0.0) : 0.5;\n"
                           "    }"));
    json::Value broken = test("broken");
    std::string problems;
    for (const json::Value& p : broken["problems"].asArray()) problems += p.asString() + "\n";
    INFO(problems);
    CHECK(problems.find("sound from silence") != std::string::npos);
    CHECK(problems.find("DC offset") != std::string::npos);
    CHECK(problems.find("NaN or infinity") != std::string::npos);
}

TEST_CASE("dsp.export makes a plugin that plays its version of the script, and no other") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    struct Host {
        std::unique_ptr<SandboxHost> host;
        std::unique_ptr<cmd::Dispatcher> bus;
    };
    auto open = [&](const std::string& name, const fs::path& plugins) {
        SessionOptions o;
        o.workdir = root.path / name;
        o.pluginPath = {plugins};
        auto session = Session::open(o);
        REQUIRE(session);
        Host h{std::make_unique<SandboxHost>(std::move(*session), fs::path{}), nullptr};
        h.bus = std::make_unique<cmd::Dispatcher>(registry, *h.host);
        return h;
    };
    auto call = [&](Host& h, std::string name, json::Value args) {
        auto r = h.bus->execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](Host& h, const std::string& name, const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = name;
        a["source"] = source;
        json::Value written = call(h, "dsp.write", std::move(a));
        REQUIRE_MESSAGE(written["ok"].asBool(), json::writeCompact(written["diagnostics"]));
    };
    auto exportTo = [&](Host& h, const fs::path& folder) {
        json::Value a = json::Value::object();
        a["script"] = "boost";
        const auto utf8 = folder.u8string();
        a["folder"] = std::string(utf8.begin(), utf8.end());
        return call(h, "dsp.export", std::move(a));
    };
    auto gainDb = [&](Host& h) {
        json::Value a = json::Value::object();
        a["plugin"] = "com.example.boost";
        a["signals"] = json::Value::array();
        a["signals"].push_back("sine");
        return call(h, "dsp.test", std::move(a))["signals"].at(0).find("gain_db")->asDouble();
    };
    auto boost = [](int db) {
        return script("Boost", "com.example.boost",
                      "    Param gain = decibels(\"Gain\", -24, 24, " + std::to_string(db) +
                          ");\n"
                          "    void process(Block& io) {\n"
                          "        for (int c = 0; c < io.channels; ++c)\n"
                          "            for (int i = 0; i < io.frames; ++i) io[c][i] *= "
                          "gain.gain();\n"
                          "    }");
    };

    // From the library's player: Boost.clap, with the notices to ship.
    Host library = open("library", player);
    write(library, "boost", boost(6));
#if defined(__linux__)
    const fs::path output = root.path / "out";
#else
    const fs::path output = root.path / fs::path(u8"out café λ");
#endif
    auto utf8 = [](const fs::path& path) {
        const auto bytes = path.u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    const auto plugin = output / "Boost.clap";
    const auto notices = output / "Boost-NOTICES.txt";
    json::Value first = exportTo(library, output);
    CHECK(first["path"].asString() == utf8(plugin));
    CHECK(first["id"].asString() == "com.example.boost");
    CHECK(fs::canonical(dsp_commands::binary_export::path(first["player"].asString())) ==
          fs::canonical(player));
#if defined(__APPLE__)
    CHECK(fs::is_directory(plugin));
    CHECK(fs::is_regular_file(plugin / "Contents/Resources/supple-script.json"));
    CHECK(first["bytes"].asInt() > 0);
#else
    CHECK(fs::file_size(plugin) == static_cast<std::uintmax_t>(first["bytes"].asInt()));
    const auto original = readFile(player);
    const auto exported = readFile(plugin);
    REQUIRE(original);
    REQUIRE(exported);
    REQUIRE(exported->size() > original->size());
    CHECK(std::equal(original->begin(), original->end(), exported->begin()));
#endif
    CHECK(first["notices"].asString() == utf8(notices));
    CHECK(fs::exists(notices));

    // The library changes and grows; the export keeps its own version and
    // offers nothing else.
    write(library, "boost", boost(12));
    write(library, "other", script("Other", "com.example.other", "    void process(Block& io) {}"));
    Host anywhere = open("anywhere", output);
    auto listed = anywhere.host->session().plugins(true);
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().id == "com.example.boost");
    CHECK(gainDb(anywhere) == doctest::Approx(6.0).epsilon(0.002));

    // The exported file also loads through the generic host and bridge, with
    // no library lookup and identical sample output in either process.
    std::array<float, 64> input{}, left{}, right{}, expected{};
    input.fill(0.25f);
    const float* inputs[] = {input.data(), input.data()};
    float* outputs[] = {left.data(), right.data()};
    for (bool isolated : {false, true}) {
        auto instance = host::Instance::create(listed->front(), {}, isolated);
        REQUIRE_MESSAGE(instance, (instance ? "" : instance.error().toString()));
        REQUIRE((*instance)->activate(48000, 64, 64));
        REQUIRE((*instance)->startProcessing());
        host::ProcessBlock block;
        block.inputs = inputs;
        block.inputChannels = 2;
        block.outputs = outputs;
        block.outputChannels = 2;
        block.frames = 64;
        CHECK((*instance)->process(block));
        (*instance)->stopProcessing();
        (*instance)->deactivate();
        if (!isolated) expected = left;
        CHECK(left == expected);
        CHECK(right == expected);
        CHECK(left.front() == doctest::Approx(0.25 * std::pow(10.0, 6.0 / 20)).epsilon(0.002));
    }

    // An export exported again starts from the player, with the new script.
    write(anywhere, "boost", boost(12));
    json::Value second = exportTo(anywhere, root.path / "again");
    CHECK(fs::canonical(dsp_commands::binary_export::path(second["player"].asString())) ==
          fs::canonical(dsp_commands::binary_export::path(first["path"].asString())));
#if !defined(__APPLE__)
    CHECK(second["bytes"].asInt() == first["bytes"].asInt() + 1);  // "12" for "6"
#endif
    CHECK(fs::file_size(root.path / "again/Boost-NOTICES.txt") == fs::file_size(notices));
    Host again = open("again", root.path / "again");
    CHECK(gainDb(again) == doctest::Approx(12.0).epsilon(0.002));

    // Replacing an existing, unloaded export publishes the new payload too.
    (void)exportTo(anywhere, root.path / "replacement");
    write(anywhere, "boost", boost(6));
    (void)exportTo(anywhere, root.path / "replacement");
    Host replacement = open("replacement", root.path / "replacement");
    CHECK(gainDb(replacement) == doctest::Approx(6.0).epsilon(0.002));

    // An installed copy missing its notices must not yield an export that
    // cannot be redistributed with the required licenses.
    REQUIRE(fs::remove(notices));
    json::Value missing = json::Value::object();
    missing["script"] = "boost";
    missing["folder"] = (root.path / "without-notices").string();
    auto refused = anywhere.bus->execute({"dsp.export", missing, kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.notices");
    CHECK(refused.error().path == "/args/script");
    CHECK_FALSE(refused.error().hint.empty());
    CHECK_FALSE(fs::exists(root.path / "without-notices" / "Boost.clap"));
}

#if defined(__APPLE__)
TEST_CASE("failed bundle signing preserves an existing export and removes staging files") {
    Root root;
    const auto broken = root.path / "broken.clap";
    const auto output = root.path / "output.clap";
    REQUIRE(writeFileAtomic(broken / "Contents/MacOS/broken", "not a Mach-O"));
    REQUIRE(writeFileAtomic(output / "keep", "previous valid export"));
    auto result = dsp_commands::binary_export::writeBundle(broken, output, "{}");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "dsp.sign");
    CHECK(result.error().path == "/folder");
    CHECK_FALSE(result.error().hint.empty());
    CHECK(fs::is_regular_file(output / "keep"));
    for (const auto& entry : fs::directory_iterator(root.path))
        CHECK_FALSE(entry.path().filename().string().starts_with(".script-export-"));

    // A copied resource-directory link must not write back into the source.
    const auto outside = root.path / "outside";
    fs::create_directory(outside);
    fs::create_directory_symlink(outside, broken / "Contents/Resources");
    auto linked = dsp_commands::binary_export::writeBundle(broken, output, "{}");
    REQUIRE_FALSE(linked);
    CHECK(linked.error().code == "dsp.player");
    CHECK(fs::is_empty(outside));
    CHECK(fs::is_regular_file(output / "keep"));
}
#endif

TEST_CASE("dsp.test plays notes into an instrument and hears its pitch") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {player};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name), std::move(args), kTest});
    };
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/synth.cpp");
    std::stringstream text;
    text << in.rdbuf();
    json::Value w = json::Value::object();
    w["name"] = "synth";
    w["source"] = text.str();
    auto written = call("dsp.write", std::move(w));
    REQUIRE_MESSAGE(written, (written ? std::string() : written.error().toString()));
    REQUIRE((*written)["ok"].asBool());

    json::Value t = json::Value::object();
    t["script"] = "synth";
    auto tested = call("dsp.test", t);
    REQUIRE_MESSAGE(tested, (tested ? std::string() : tested.error().toString()));
    json::Value result = *tested;
    std::string problems;
    for (const json::Value& p : result["problems"].asArray()) problems += p.asString() + "\n";
    CHECK_MESSAGE(problems.empty(), problems);
    REQUIRE(result["signals"].size() == 3);
    CHECK(result["signals"].at(0).find("peak_dbfs")->asDouble() > -40.0);  // notes
    const json::Value& note = result["signals"].at(1);
    REQUIRE(note.find("pitch_hz"));
    CHECK(note.find("pitch_hz")->asDouble() == doctest::Approx(440.0).epsilon(0.002));
    CHECK(result["signals"].at(2).find("peak_dbfs")->asDouble() < -150.0);  // no notes

    // An instrument takes notes, not audio.
    t["signals"] = json::Value::array();
    t["signals"].push_back("sine");
    auto refused = call("dsp.test", t);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.signal");
}

TEST_CASE("dsp.test gives a convolution reverb its room through a patch") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {player};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name), std::move(args), kTest});
    };
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/reverb.cpp");
    std::stringstream text;
    text << in.rdbuf();
    json::Value w = json::Value::object();
    w["name"] = "reverb";
    w["source"] = text.str();
    auto written = call("dsp.write", std::move(w));
    REQUIRE_MESSAGE(written, (written ? std::string() : written.error().toString()));
    // The script's file, and how to give it one.
    const json::Value* files = (*written)["plugin"].find("files");
    REQUIRE(files);
    CHECK(files->at(0).find("key")->asString() == "room");
    CHECK(files->at(0).find("kind")->asString() == "impulse");

    // Half a second of a fading echo.
    audio::AudioData room;
    room.channels.assign(1, std::vector<float>(24000));
    for (size_t n = 0; n < room.channels[0].size(); ++n)
        room.channels[0][n] = static_cast<float>((n % 997 == 0 ? 0.5 : 0.0) *
                                                 std::exp(-static_cast<double>(n) / 6000.0));
    const fs::path ir = root.path / "room.wav";
    fs::create_directories(root.path);
    REQUIRE(audio::writeWav(ir, room));
    auto test = [&](const std::string& path) {
        json::Value t = json::Value::object();
        t["script"] = "reverb";
        t["signals"] = json::Value::array();
        t["signals"].push_back("impulse");
        t["patch"]["params"]["mix"] = 100;
        t["patch"]["properties"]["room"] = path;
        return call("dsp.test", std::move(t));
    };
    auto dry = test("");
    auto wet = test(ir.string());
    REQUIRE_MESSAGE(wet, (wet ? std::string() : wet.error().toString()));
    REQUIRE(dry);
    // The room's echo comes back: a tail the dry render does not have.
    CHECK((*wet)["signals"].at(0).find("peak_dbfs")->asDouble() > -30.0);
    CHECK((*dry)["signals"].at(0).find("peak_dbfs")->asDouble() < -150.0);
    // A path that names nothing is refused, saying which and why.
    auto missing = test((root.path / "nowhere.wav").string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().toString().find("there is no file there") != std::string::npos);
}

TEST_CASE("plugin-file export defaults to the script's export folder") {
    Harness h;
    h.write("native-export",
            script("Native", "com.example.native-export",
                   "Param gain = decibels(\"Gain\", -24, 24, 0); void process(Block& io) {}"));
    auto args = json::Value::object();
    args["script"] = "native-export";
    auto result = h.bus->execute({"dsp.export", args, kTest});
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    CHECK((*result)["format"].asString() == "clap");
#if !defined(__linux__)
    CHECK(fs::equivalent(dsp_commands::binary_export::path((*result)["path"].asString()),
                         h.host->session().scriptDir() / "native-export/export/Native.clap"));
#else
    CHECK((*result)["path"].asString() ==
          (h.host->session().scriptDir() / "native-export/export/Native.clap").string());
#endif
    CHECK(fs::exists(h.host->session().scriptDir() / "native-export/export/Native.clap"));
}
