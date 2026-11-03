// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Render settings, presets and the processing chain (ADR-0055, engine.md §3).
#include <doctest/doctest.h>

#include <unistd.h>
#include <cmath>
#include <filesystem>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/app.h>
#include <supple/model/schemas.h>

#include "render_commands.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct Workdir {
    fs::path path =
        fs::temp_directory_path() / ("supple-engine-render-" + std::to_string(::getpid()));
    Workdir() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Workdir() { fs::remove_all(path); }
};

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "render", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }

struct Rig {
    Workdir dir;
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    Rig() {
        AppOptions options;
        options.workdir = dir.path;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    json::Value run(std::string name, std::string_view args) {
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    Error fails(std::string name, std::string_view args) {
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().hint.empty());
        return result.error();
    }
    // One tone, two beats on tr_1 "Synth".
    void song() {
        run("project.set", R"({"name":"Song"})");
        run("track.add", R"({"name":"Synth"})");
        run("fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
        run("item.add", R"({"track":"tr_1","position":0,"length":2,"notation":"1: C4:h r:h"})");
    }
};

double peakOf(const audio::AudioData& data) {
    double max = 0;
    for (const auto& channel : data.channels)
        for (float x : channel) max = std::max(max, std::fabs(double(x)));
    return max;
}
}  // namespace

TEST_CASE("render settings resolve per member: arguments, project, default, built-in") {
    Rig r;
    r.song();
    auto settings = r.run("render.settings", "{}");
    CHECK(settings.find("default")->isNull());
    CHECK(settings["from"]["outputs"] == "built-in");
    CHECK(settings["effective"]["outputs"] == j(R"([{"format":"wav","bit_depth":32,
        "dither":"none","sample_rate":48000}])"));
    CHECK(settings["effective"]["pattern"]["mix"] == "{project}-v{version}");
    // The user's default fills what the project leaves out; objects are whole.
    r.run("render.preset.save",
          R"({"default":true,"settings":{"tail_s":1,"normalize":{"mode":"lufs_i"},
              "stems":{"tracks":["tr_1"]},"start_beat":3}})");
    r.run("project.set", R"({"render":{"normalize":{"mode":"peak","target":-1}}})");
    settings = r.run("render.settings", R"({"channels":1})");
    CHECK(settings["from"]["tail_s"] == "default");
    CHECK(settings["from"]["normalize"] == "project");
    CHECK(settings["from"]["channels"] == "args");
    CHECK(settings["effective"]["tail_s"] == 1.0);
    CHECK(settings["effective"]["normalize"]["mode"] == "peak");
    CHECK(settings["effective"]["normalize"]["target"] == -1.0);
    // Presets never keep project references.
    CHECK(settings["default"].find("stems")->find("tracks") == nullptr);
    CHECK(settings["default"].find("start_beat") == nullptr);
    // Arguments are never written back.
    CHECK(r.app->head().project->render == j(R"({"normalize":{"mode":"peak","target":-1}})"));
    // File → Mix (no arguments) follows them: a 1 s tail, peak-normalized.
    auto mix = r.run("render.mix", "{}");
    CHECK(fs::path(mix["path"].asString()).filename() == "Song-v5.wav");
    CHECK(mix["branch"] == "main");
    CHECK(mix["version"] == 5);
    auto audio = audio::readAudio(mix["path"].asString());
    REQUIRE(audio);
    CHECK(audio->seconds() == doctest::Approx(2.0));  // 2 beats at 120 bpm, plus the 1 s tail
    CHECK(20 * std::log10(peakOf(*audio)) == doctest::Approx(-1.0).epsilon(0.01));
}

TEST_CASE("legacy render arguments override the first outputs and the stems members") {
    Rig r;
    r.song();
    r.run("project.set",
          R"({"render":{"outputs":[{"format":"wav","bit_depth":24},{"format":"flac"}]}})");
    // format resets the bit depth to the new format's default.
    auto settings = r.run("render.settings", R"({"format":"flac"})");
    CHECK(settings["effective"]["outputs"].asArray()[0]["bit_depth"] == 24);
    CHECK(settings["effective"]["outputs"].size() == 2);
    settings = r.run("render.settings", R"({"secondary":"none","mode":"through_master"})");
    CHECK(settings["effective"]["outputs"].size() == 1);
    CHECK(settings["effective"]["stems"]["mode"] == "through_master");
    CHECK(settings["from"]["stems"] == "args");
    auto error = r.fails("render.mix", R"({"outputs":[{"format":"wav"}],"format":"flac"})");
    CHECK(error.code == "render.args");
    error = r.fails("render.mix", R"({"format":"flac","bit_depth":32})");
    CHECK(error.code == "render.args");
    CHECK(error.path == "/args/outputs/0/bit_depth");
    // The project's second output is named by its depth when the extension repeats.
    r.run(
        "project.set",
        R"({"render":{"outputs":[{"format":"flac","bit_depth":24},{"format":"flac","bit_depth":16}]}})");
    auto mix = r.run("render.mix", R"({"name":"x"})");
    CHECK(fs::path(mix["secondary_path"].asString()).filename() == "x.16.flac");
    CHECK(mix["outputs"].size() == 2);
    CHECK(mix["outputs"].asArray()[1]["bit_depth"] == 16);
}

TEST_CASE("contradictory settings are refused with the offending member") {
    Rig r;
    r.song();
    CHECK(r.fails("render.mix", R"({"seamless_loop":true})").path == "/args/seamless_loop");
    CHECK(r.fails("render.mix", R"({"seamless_loop":true,"tail_s":1,"trim":{"end":true}})").path ==
          "/args/trim");
    CHECK(r.fails("render.stems",
                  R"({"normalize":{"mode":"lufs_i","scope":"master"},"limit_true_peak_db":-1})")
              .path == "/args/limit_true_peak_db");
    CHECK(r.fails("render.mix", R"({"outputs":[{"format":"mp3","sample_rate":96000}]})").path ==
          "/args/outputs/0/sample_rate");
    CHECK(r.fails("render.mix", R"({"bounds":"loop"})").code == "render.empty");
    // A contradiction that comes from the project says so.
    r.run("project.set", R"({"render":{"seamless_loop":true}})");
    auto error = r.fails("render.mix", "{}");
    CHECK(error.code == "render.args");
    CHECK(error.message.find("project") != std::string::npos);
}

TEST_CASE("bounds: loop and custom; a seamless loop folds its tail into the start") {
    Rig r;
    r.song();
    r.run("transport.loop", R"({"start_beat":1,"end_beat":2,"enabled":true})");
    auto loop = r.run("render.mix", R"({"name":"loop","bounds":"loop"})");
    CHECK(loop["seconds"].asDouble() == doctest::Approx(0.5));
    CHECK(loop["start_beat"].asDouble() == doctest::Approx(1.0));
    auto plain = r.run("render.mix", R"({"name":"plain","end_beat":1,"tail_s":0.5})");
    auto folded =
        r.run("render.mix", R"({"name":"folded","end_beat":1,"tail_s":0.5,"seamless_loop":true})");
    CHECK(folded["seconds"].asDouble() == doctest::Approx(0.5));
    auto a = audio::readAudio(plain["path"].asString());
    auto b = audio::readAudio(folded["path"].asString());
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(b->frames() == 24000);
    // Each folded sample is the range sample plus the tail sample one range later.
    for (std::size_t i : {0u, 100u, 23999u})
        CHECK(double(b->channels[0][i]) ==
              doctest::Approx(double(a->channels[0][i]) + double(a->channels[0][i + 24000])));
}

TEST_CASE("the limiter runs without normalization; only_reduce never raises the level") {
    Rig r;
    r.song();
    auto loud = r.run("render.mix", R"({"name":"loud","end_beat":2})");
    auto limited =
        r.run("render.mix", R"({"name":"limited","end_beat":2,"limit_true_peak_db":-30})");
    CHECK(limited["outputs"].asArray()[0]["checks"]["true_peak"]["ok"] == true);
    CHECK(limited["outputs"].asArray()[0]["checks"]["true_peak"]["measured"].asDouble() <= -29.9);
    CHECK(limited["peak_dbfs"].asDouble() < loud["peak_dbfs"].asDouble() - 3);
    auto kept = r.run("render.mix", R"({"name":"kept","end_beat":2,
        "normalize":{"mode":"peak","target":0,"only_reduce":true}})");
    CHECK(kept["peak_dbfs"] == loud["peak_dbfs"]);
    // A loudness target is checked on the file as written, and reported.
    auto mastered = r.run("render.mix", R"({"name":"mastered","end_beat":2,"report":true,
        "outputs":[{"format":"wav","bit_depth":16}],"normalize":{"mode":"lufs_i","target":-20}})");
    auto& checks = mastered["outputs"].asArray()[0]["checks"];
    CHECK(checks["loudness"]["ok"] == true);
    CHECK(checks["loudness"]["measured"].asDouble() == doctest::Approx(-20).epsilon(0.05));
    auto report = readTextFile(mastered["outputs"].asArray()[0]["report"].asString());
    REQUIRE(report);
    CHECK(report->find("Integrated loudness: -20.0 LUFS") != std::string::npos);
    CHECK(report->find("Check loudness: pass") != std::string::npos);
}

TEST_CASE("trim removes silence at the ends and moves start_beat") {
    Rig r;
    r.song();
    r.run("item.move", R"({"item":"it_1","position":1})");  // silence for beat 0..1
    auto trimmed = r.run("render.mix", R"({"name":"t","end_beat":4,
        "trim":{"start":true,"end":true}})");
    CHECK(trimmed["start_beat"].asDouble() == doctest::Approx(1.0).epsilon(0.02));
    CHECK(trimmed["seconds"].asDouble() < 1.2);
    CHECK(trimmed["seconds"].asDouble() > 0.9);
}

TEST_CASE("file names: version and branch tokens, subfolders and if_exists") {
    Rig r;
    r.song();
    r.run("branch.create", R"({"name":"agent/me/try"})");
    auto mix = r.run("render.mix", R"({"pattern":"{project}/{branch}-v{version}"})");
    const fs::path first = mix["path"].asString();
    CHECK(first.parent_path().filename() == "Song");
    CHECK(first.filename() == "agent-me-try-v4.wav");
    CHECK(mix["branch"] == "agent/me/try");
    auto dry = r.run("render.dry-run", R"({"pattern":"{project}/{branch}-v{version}"})");
    CHECK(dry["files"].asArray()[0]["exists"] == true);
    auto numbered = r.run("render.mix", R"({"pattern":"{project}/{branch}-v{version}",
        "if_exists":"number"})");
    CHECK(fs::path(numbered["path"].asString()).filename() == "agent-me-try-v4-2.wav");
    auto skipped = r.run("render.mix", R"({"pattern":"{project}/{branch}-v{version}",
        "if_exists":"skip"})");
    CHECK(skipped.find("path") == nullptr);
    CHECK(skipped["skipped"].asArray()[0]["reason"] == "exists");
    CHECK(r.fails("render.mix", R"({"pattern":"{project}/{branch}-v{version}",
        "if_exists":"error"})")
              .code == "render.exists");
    CHECK(r.fails("render.mix", R"({"pattern":"../out"})").code == "render.path");
}

TEST_CASE("stems: shared master gain, silent stems skipped, mono tracks as mono") {
    Rig r;
    r.song();
    r.run("track.add", R"({"name":"Empty","channels":1})");
    r.run("track.add", R"({"name":"Mono","channels":1})");
    r.run("fx.add", R"({"track":"tr_3","plugin":"Test Sine"})");
    r.run("item.add", R"({"track":"tr_3","position":0,"length":2,"notation":"1: C4:h r:h"})");
    auto stems = r.run("render.stems", R"({"end_beat":2,"channels":"track",
        "stems":{"skip_silent":true},"normalize":{"mode":"peak","target":-6,"scope":"master"}})");
    REQUIRE(stems["files"].size() == 2);
    CHECK(stems["skipped"].asArray()[0]["track"] == "tr_2");
    CHECK(stems["skipped"].asArray()[0]["reason"] == "silent");
    auto synth = audio::readAudio(stems["files"].asArray()[0]["path"].asString());
    auto mono = audio::readAudio(stems["files"].asArray()[1]["path"].asString());
    REQUIRE(synth);
    REQUIRE(mono);
    CHECK(synth->channelCount() == 2);
    CHECK(mono->channelCount() == 1);
    // One gain for both: their peaks keep their ratio, below the master's -6 dB.
    CHECK(20 * std::log10(peakOf(*synth)) < -6.0);
}

TEST_CASE("presets: factory ones are read-only; a loaded preset replaces the settings") {
    Rig r;
    r.song();
    auto list = r.run("render.preset.list", "{}");
    CHECK(list["presets"].asArray()[0]["name"] == "WAV 24-bit");
    CHECK(list["presets"].asArray()[0]["factory"] == true);
    CHECK(r.fails("render.preset.save", R"({"name":"WAV 24-bit"})").code ==
          "render.preset_factory");
    CHECK(r.fails("render.preset.delete", R"({"name":"WAV 24-bit"})").code ==
          "render.preset_factory");
    CHECK(r.fails("render.preset.save", R"({"name":"a/b"})").code == "render.preset_name");
    r.run("project.set", R"({"render":{"tail_s":3,"matrix":{"project":["tr_1"]}}})");
    r.run("render.preset.save", R"({"name":"Mine"})");  // the effective settings
    list = r.run("render.preset.list", "{}");
    json::Value mine = list["presets"].asArray().back();
    CHECK(mine["name"] == "Mine");
    CHECK(mine["settings"]["tail_s"] == 3.0);
    CHECK(mine["settings"].find("matrix") == nullptr);
    auto before = r.app->head().version;
    r.run("render.preset.load", R"({"name":"WAV 24-bit"})");
    CHECK(r.app->head().version == before + 1);
    CHECK(r.app->head().project->render ==
          j(R"({"outputs":[{"format":"wav","bit_depth":24,"dither":"tpdf"}]})"));
    r.run("history.undo", "{}");
    CHECK(r.app->head().project->render.find("tail_s"));
    r.run("render.preset.delete", R"({"name":"Mine"})");
    CHECK(r.fails("render.preset.load", R"({"name":"Mine"})").code == "render.preset_unknown");
}

TEST_CASE("renders are byte-identical for the same session and settings") {
    Rig r;
    r.song();
    const char* args = R"({"name":"a","end_beat":2,"outputs":[{"format":"wav","bit_depth":16},
        {"format":"flac","bit_depth":24,"dither":"tpdf"}],"normalize":{"mode":"lufs_i"}})";
    auto one = r.run("render.mix", args);
    auto first = readFile(one["path"].asString());
    auto firstFlac = readFile(one["secondary_path"].asString());
    REQUIRE(first);
    REQUIRE(firstFlac);
    auto two = r.run("render.mix", args);
    CHECK(readFile(two["path"].asString()).value() == *first);
    CHECK(readFile(two["secondary_path"].asString()).value() == *firstFlac);
}

TEST_CASE("branch.create starts a branch at a render's version") {
    Rig r;
    r.song();
    auto mix = r.run("render.mix", R"({"name":"old"})");
    r.run("track.set", R"({"track":"tr_1","volume_db":-12})");
    const std::string args =
        R"({"name":"from-old","version":)" + std::to_string(mix["version"].asInt()) + "}";
    r.run("branch.create", args);
    CHECK(r.app->versions().branch() == "from-old");
    CHECK(r.app->head().version == static_cast<std::uint64_t>(mix["version"].asInt()));
    CHECK(r.fails("branch.create", R"({"name":"later","version":99})").code == "history.version");
}

namespace {
bool holds(const fs::path& path, std::string_view text) {
    auto bytes = readFile(path);
    REQUIRE(bytes);
    return std::search(bytes->begin(), bytes->end(), text.begin(), text.end()) != bytes->end();
}
}  // namespace

TEST_CASE("outputs: AIFF, FLAC, MP3 and Ogg Vorbis of one render, each checked as written") {
    Rig r;
    r.song();
    const char* args = R"({"name":"all","end_beat":2,"limit_true_peak_db":-1,
        "normalize":{"mode":"lufs_i","target":-18},
        "outputs":[{"format":"aiff","bit_depth":16,"dither":"shaped"},{"format":"flac"},
                   {"format":"mp3","mp3":{"mode":"vbr","vbr_quality":4}},
                   {"format":"vorbis","vorbis_quality":4}]})";
    auto mix = r.run("render.mix", args);
    auto& outputs = mix["outputs"].asArray();
    REQUIRE(outputs.size() == 4);
    std::vector<std::string> names;
    for (auto& o : outputs) names.push_back(fs::path(o["path"].asString()).filename().string());
    CHECK(names == std::vector<std::string>{"all.aif", "all.flac", "all.mp3", "all.ogg"});
    for (auto& o : outputs) {
        INFO(o["path"].asString());
        CHECK(o["checks"]["true_peak"]["ok"] == true);
        CHECK(o["checks"]["loudness"]["ok"] == true);
        CHECK(o["checks"]["loudness"]["measured"].asDouble() == doctest::Approx(-18).epsilon(0.06));
    }
    CHECK(outputs[2].find("bit_depth") == nullptr);
    CHECK(outputs[0]["sample_rate"] == 48000);
    // Lossy encoders give the same bytes for the same render.
    auto again = r.run("render.mix", args);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(readFile(again["outputs"].asArray()[i]["path"].asString()).value() ==
              readFile(outputs[i]["path"].asString()).value());
    // Formats hold only what they can.
    CHECK(r.fails("render.mix", R"({"outputs":[{"format":"mp3","bit_depth":16}]})").path ==
          "/args/outputs/0/bit_depth");
    CHECK(r.fails("render.mix", R"({"outputs":[{"format":"aiff","bit_depth":32}]})").path ==
          "/args/outputs/0/bit_depth");
    // An MP3 of a 96 kHz chain is written at 48 kHz.
    auto high = r.run("render.mix", R"({"name":"high","end_beat":1,"sample_rate":96000,
        "outputs":[{"format":"wav"},{"format":"mp3"}]})");
    CHECK(high["outputs"].asArray()[1]["sample_rate"] == 48000);
    CHECK(audio::readAudio(high["path"].asString())->sampleRate == 96000);
}

TEST_CASE("metadata: tokens in tags, BWF timecode and loudness, markers, a cover") {
    Rig r;
    r.song();
    r.run("marker.add", R"({"pos":1,"name":"Drop"})");
    r.run("region.add", R"({"start":0,"end":1,"name":"Intro"})");
    auto error = r.fails("render.mix", R"({"metadata":{"cover":"cover.png"}})");
    CHECK(error.path == "/args/metadata/cover");  // an unsaved project has no folder
    r.run(
        "project.save",
        "{\"path\":" + json::writeCompact(json::Value((r.dir.path / "song.json").string())) + "}");
    const fs::path folder = r.app->projectDir();
    const std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0};
    REQUIRE(writeFileAtomic(folder / "cover.png", png));
    auto mix = r.run("render.mix", R"({"name":"tagged","end_beat":2,
        "outputs":[{"format":"wav","bit_depth":24},{"format":"flac"},{"format":"mp3"}],
        "metadata":{"title":"{project} v{version}","artist":"Me","isrc":"USRC17607839",
                    "date":"2026-11-03","cover":"cover.png","bwf":true,"markers":true}})");
    auto& o = mix["outputs"].asArray();
    const fs::path wav = o[0]["path"].asString(), flac = o[1]["path"].asString(),
                   mp3 = o[2]["path"].asString();
    for (const char* id : {"bext", "INAM", "Song v", "ISFT", "cue ", "Drop", "Intro", "ltxt"})
        CHECK_MESSAGE(holds(wav, id), id);
    CHECK(holds(flac, "TITLE=Song v"));
    CHECK(holds(flac, "ISRC=USRC17607839"));
    CHECK(holds(mp3, "TSRC"));
    CHECK(holds(mp3, "APIC"));
    CHECK_FALSE(holds(flac, "Drop"));  // markers are WAV and AIFF only
    CHECK(r.fails("render.mix", R"({"metadata":{"cover":"../x.png"}})").path ==
          "/args/metadata/cover");
    CHECK(r.fails("render.mix", R"({"metadata":{"title":"{nope}"}})").path ==
          "/args/metadata/title");
}

TEST_CASE("factory presets cover the delivery targets") {
    Rig r;
    auto list = r.run("render.preset.list", "{}");
    std::vector<std::string> names;
    for (auto& preset : list["presets"].asArray()) names.push_back(preset["name"].asString());
    CHECK(names.size() == 6);
    CHECK(std::find(names.begin(), names.end(), "Podcast (MP3, −16 LUFS)") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Game loop (Ogg Vorbis, seamless)") != names.end());
}
