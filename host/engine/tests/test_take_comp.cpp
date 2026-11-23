// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// take.comp and take.comp.get (session-model §1.5.1): a take stack comped into pieces with
// seam crossfades, from a loop-recorded item.
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include "realtime_common.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/model/fade_shape.h"
#include "supple/model/schemas.h"
#include "supple/time/timeline.h"

using namespace supple;
using namespace supple::engine;

namespace {
const cmd::Origin origin{cmd::Origin::Type::Test, "take-comp", {}};
constexpr std::int64_t kPass = 96000;  // frames of one pass: 2 s, 4 beats at 120 bpm

// One piece of the comp as stored: its samples at the project rate and its active take.
struct Piece {
    model::Id item;
    std::int64_t start = 0, end = 0;
    std::size_t index = 0;
    double fadeIn = 0, fadeOut = 0;
    model::FadeShape inShape = model::FadeShape::Linear, outShape = model::FadeShape::Linear;
};

struct CompFixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("take-comp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;

    // `passes` loop passes of 2 s on tr_1, each a different sine: one item, one take per pass.
    explicit CompFixture(std::size_t passes = 3) {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"Audio"})");
        record("tr_1", "passes.wav", 300.0, passes);
    }
    // A loop recording of `passes` passes of 2 s on `track`, each a different sine from `hz` up.
    void record(const std::string& track, const std::string& name, double hz0, std::size_t passes) {
        const double rate = app->head().project->sampleRate;
        audio::AudioData data;
        data.sampleRate = rate;
        data.channels.assign(2, std::vector<float>(passes * kPass));
        for (std::size_t p = 0; p < passes; ++p)
            for (std::int64_t n = 0; n < kPass; ++n) {
                const double hz = hz0 + 150.0 * static_cast<double>(p);
                const auto at = static_cast<std::size_t>(p * kPass + n);
                data.channels[0][at] = data.channels[1][at] = static_cast<float>(
                    0.5 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(n) / rate));
            }
        const auto file = app->mediaDir() / name;
        REQUIRE(audio::writeWav(file, data));
        auto args = *json::parse(R"({"takes":[{"track":"tr_1","hash":"","start_sample":0,
            "loop_start":0,"overflow_frames":0,"midi":false,"events":[]}],
            "punch":{"enabled":false,"start":0,"end":0}})");
        auto& take = args["takes"].asArray()[0];
        take["file"] = file.string();
        take["track"] = track;
        take["frames"] = static_cast<std::int64_t>(passes) * kPass;
        take["loop_end"] = kPass;
        take["sample_rate"] = rate;
        auto segments = json::Value::array();
        for (std::size_t p = 0; p < passes; ++p) {
            auto segment = json::Value::object();
            segment["file_frame"] = static_cast<std::int64_t>(p) * kPass;
            segment["timeline"] = 0;
            segments.push_back(std::move(segment));
        }
        take["segments"] = std::move(segments);
        auto done = bus->execute({"record.commit", args, origin});
        REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    }
    ~CompFixture() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    Result<json::Value> call(const char* command, std::string_view args = "{}") {
        auto parsed = json::parse(args);
        // A path built into the text on Windows has backslashes that are not JSON: say so.
        REQUIRE_MESSAGE(parsed, "arguments of " << std::string(command)
                                                << " are not JSON: " << std::string(args));
        return bus->execute({command, *parsed, origin});
    }
    // For arguments that carry a path or other text: built as a value, never spliced in.
    Result<json::Value> callValue(const char* command, json::Value args) {
        return bus->execute({command, std::move(args), origin});
    }
    json::Value run(const char* command, std::string_view args = "{}") {
        auto result = call(command, args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    // The error of a refused call, checked for code, path and a hint.
    void refused(const char* command, std::string_view args, std::string_view code,
                 std::string_view path) {
        auto result = call(command, args);
        REQUIRE_MESSAGE(!result, command << " " << args << " should be refused");
        CHECK(result.error().code == code);
        CHECK(result.error().path == "/args" + std::string(path));
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
    }
    double rate() const { return app->head().project->sampleRate; }
    // Items of tr_1 in time order.
    std::vector<Piece> pieces(std::optional<model::Id> track = std::nullopt) const {
        const model::Session& s = app->head();
        std::vector<Piece> out;
        for (const auto& e : s.items) {
            const auto& it = *e.value;
            if (track && it.track != *track) continue;
            const auto span = model::itemSpan(s, it);
            Piece p;
            p.item = e.id;
            p.start = time::samplesAt(span.startSec, rate());
            p.end = time::samplesAt(span.endSec, rate());
            const auto at = std::find(it.takes.begin(), it.takes.end(), *it.activeTake);
            p.index = static_cast<std::size_t>(at - it.takes.begin());
            p.fadeIn = it.fadeIn.length;
            p.fadeOut = it.fadeOut.length;
            p.inShape = it.fadeIn.shape;
            p.outShape = it.fadeOut.shape;
            out.push_back(p);
        }
        std::sort(out.begin(), out.end(), [](const Piece& a, const Piece& b) {
            return a.start == b.start ? a.item < b.item : a.start < b.start;
        });
        return out;
    }
    // The offline render of the master, left channel, over the first two seconds.
    std::vector<float> render() const {
        auto pool = makePool();
        auto out = renderOffline(app->head(), pool,
                                 {.startSeconds = 0, .endSeconds = 2.0, .projectDir = dir});
        REQUIRE(out);
        return out->audio.channels[0];
    }
    // What each take sounds on its own over the whole item, by index.
    std::vector<std::vector<float>> references() {
        const model::Session s = app->head();
        const auto& it = *s.items.begin()->value;
        const std::string item = s.items.begin()->id.str();
        std::vector<std::vector<float>> out;
        for (std::size_t k = 0; k < it.takes.size(); ++k) {
            run("take.activate", R"({"item":")" + item + R"(","index":)" + std::to_string(k) + "}");
            out.push_back(render());
        }
        return out;
    }
};

// The comp's core seams (samples) and the take index each stretch plays, from the arguments.
struct Seam {
    std::int64_t at;
    std::size_t left, right;
};

// What the master should be: each stretch's take, crossfaded over `half` samples either side of
// every seam with the shape's gains, from the takes' own renders.
std::vector<double> expected(const std::vector<std::vector<float>>& refs,
                             const std::vector<Seam>& seams, std::size_t first, std::int64_t half,
                             model::FadeShape shape) {
    std::vector<double> out(refs[0].size());
    for (std::size_t n = 0; n < out.size(); ++n) {
        std::size_t index = first;
        for (const Seam& s : seams)
            if (static_cast<std::int64_t>(n) >= s.at) index = s.right;
        out[n] = static_cast<double>(refs[index][n]);
        for (const Seam& s : seams) {
            const std::int64_t j = static_cast<std::int64_t>(n) - (s.at - half);
            if (half > 0 && j >= 0 && j < 2 * half) {
                const double f = static_cast<double>(2 * half);
                out[n] = static_cast<double>(refs[s.left][n]) *
                             model::fadeGain(shape, 0, (f - static_cast<double>(j)) / f) +
                         static_cast<double>(refs[s.right][n]) *
                             model::fadeGain(shape, 0, static_cast<double>(j) / f);
            }
        }
    }
    return out;
}

double worst(const std::vector<float>& got, const std::vector<double>& want) {
    double w = 0;
    for (std::size_t n = 0; n < want.size(); ++n)
        w = std::max(w, std::fabs(static_cast<double>(got[n]) - want[n]));
    return w;
}

const char* const kSpans = R"({"item":"it_1","spans":[
    {"from":0.5,"to":1.5,"index":0},{"from":1.5,"to":2.5,"index":1}]})";
}  // namespace

TEST_CASE("take.comp splits a take stack at the spans, crossfades the seams, and plays each take") {
    CompFixture f;
    REQUIRE(f.app->head().items.size() == 1);
    REQUIRE(f.app->head().items.find({model::Kind::Item, 1})->takes.size() == 3);
    const auto refs = f.references();
    f.run("take.activate", R"({"item":"it_1","index":2})");
    const auto result = f.run("take.comp", kSpans);
    const auto& list = result.find("pieces")->asArray();
    REQUIRE(list.size() == 4);
    // The first stretch keeps the current take (2); the spans take 0 and 1; the rest stays 2.
    const std::size_t take[] = {2, 0, 1, 2};
    const double from[] = {0, 0.5, 1.5, 2.5}, to[] = {0.5, 1.5, 2.5, 4};
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(list[i].find("index")->asInt() == static_cast<std::int64_t>(take[i]));
        CHECK(list[i].find("from")->asDouble() == doctest::Approx(from[i]));
        CHECK(list[i].find("to")->asDouble() == doctest::Approx(to[i]));
    }
    // 120 bpm, 48 kHz: a beat is 24000 samples, so the seams are at 12000, 36000 and 60000, and
    // the default crossfade of 10 ms is 480 samples, 240 each side.
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 4);
    const std::int64_t seam[] = {12000, 36000, 60000};
    CHECK(pieces[0].start == 0);
    CHECK(pieces[3].end == 96000);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(pieces[i].end == seam[i] + 240);
        CHECK(pieces[i + 1].start == seam[i] - 240);
        CHECK(pieces[i].fadeOut == doctest::Approx(480.0 / 48000.0));
        CHECK(pieces[i + 1].fadeIn == doctest::Approx(480.0 / 48000.0));
        CHECK(pieces[i].outShape == model::FadeShape::Cos);
        CHECK(pieces[i + 1].inShape == model::FadeShape::Cos);
    }
    CHECK(pieces[0].fadeIn == 0);
    CHECK(pieces[3].fadeOut == 0);
    for (std::size_t i = 0; i < 4; ++i) CHECK(pieces[i].index == take[i]);
    // Every piece keeps all three takes, in order.
    for (const auto& p : pieces) CHECK(f.app->head().items.find(p.item)->takes.size() == 3);
    const std::vector<Seam> seams = {{12000, 2, 0}, {36000, 0, 1}, {60000, 1, 2}};
    CHECK(worst(f.render(), expected(refs, seams, 2, 240, model::FadeShape::Cos)) < 1e-5);
}

TEST_CASE("take.comp seams are whole project samples and the pieces tile") {
    CompFixture f;
    // Spans off the sample grid: the seams snap to samples, extensions and fades are whole
    // samples, and each seam's centre is the sample its beat falls on.
    f.run("take.comp",
          R"({"item":"it_1","crossfade":0.0123,"spans":[
              {"from":0.3333333,"to":1.7777777,"index":1}]})");
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 3);
    const auto at = [&](double beat) {
        return time::samplesAt(f.app->head().timeMap->secondsAt(beat), f.rate());
    };
    const std::int64_t seams[] = {at(0.3333333), at(1.7777777)};
    // 0.0123 s is 590.4 samples: half of it rounds to 295, so the overlap is 590.
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(pieces[i].end - pieces[i + 1].start == 590);
        CHECK(pieces[i + 1].start + 295 == seams[i]);
        CHECK(pieces[i].end - 295 == seams[i]);
        CHECK(std::llround(pieces[i].fadeOut * f.rate()) == 590);
        CHECK(std::llround(pieces[i + 1].fadeIn * f.rate()) == 590);
    }
    CHECK(pieces[0].start == 0);
    CHECK(pieces[2].end == 96000);
}

TEST_CASE("take.comp twice with the same arguments gives the same model as once") {
    CompFixture f;
    f.run("take.comp", kSpans);
    const std::string once = model::serializeProject(f.app->head());
    const auto result = f.run("take.comp", kSpans);
    CHECK(result.find("pieces")->asArray().size() == 4);
    CHECK(model::serializeProject(f.app->head()) == once);
    // Through any piece, and a third time.
    f.run("take.comp", R"({"item":"it_3","spans":[
        {"from":0.5,"to":1.5,"index":0},{"from":1.5,"to":2.5,"index":1}]})");
    CHECK(model::serializeProject(f.app->head()) == once);
}

TEST_CASE("take.comp twice at an odd tempo, with off-grid spans, still gives the same model") {
    CompFixture f;
    f.run("tempo.set", R"({"points":[{"beat":0,"bpm":133.37},{"beat":2,"bpm":97.11}]})");
    const char* spans = R"({"item":"it_1","crossfade":0.0137,"spans":[
        {"from":0.3141,"to":1.2718,"index":0},{"from":1.4142,"to":2.7182,"index":1}]})";
    f.run("take.comp", spans);
    const std::string once = model::serializeProject(f.app->head());
    f.run("take.comp", spans);
    CHECK(model::serializeProject(f.app->head()) == once);
    f.run("take.comp", spans);
    CHECK(model::serializeProject(f.app->head()) == once);
    // The seams are still whole samples after the third call.
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 5);
    for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
        CHECK(pieces[i].end - pieces[i + 1].start == 2 * std::llround(0.0137 * f.rate() / 2));
}

TEST_CASE(
    "take.comp whose spans all name the current takes changes nothing, bar identical splits") {
    CompFixture f;
    const std::string before = model::serializeProject(f.app->head());
    // The item plays take 2: a span of take 2 inside it splits nothing.
    f.run("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":3,"index":2}]})");
    CHECK(model::serializeProject(f.app->head()) == before);
    // After a comp, spans that name each piece's own take change nothing either.
    f.run("take.comp", kSpans);
    const std::string comped = model::serializeProject(f.app->head());
    f.run("take.comp", R"({"item":"it_1","spans":[
        {"from":0,"to":0.5,"index":2},{"from":0.5,"to":1.5,"index":0},
        {"from":1.5,"to":2.5,"index":1},{"from":2.5,"to":4,"index":2}]})");
    CHECK(model::serializeProject(f.app->head()) == comped);
}

TEST_CASE("take.comp rebuilds a comp from any piece, as building it from scratch") {
    CompFixture a, b;
    const char* whole = R"({"item":"it_1","spans":[
        {"from":0,"to":1,"index":1},{"from":1,"to":3,"index":0},{"from":3,"to":4,"index":2}]})";
    a.run("take.comp", kSpans);
    a.run("take.comp", whole);
    b.run("take.comp", whole);
    // The first comp's seams stay (hard cuts where both sides now play one take); what plays
    // is what a comp built straight from these spans plays.
    const auto got = a.render();
    const auto want = b.render();
    double diff = 0;
    for (std::size_t n = 0; n < got.size(); ++n)
        diff =
            std::max(diff, std::fabs(static_cast<double>(got[n]) - static_cast<double>(want[n])));
    CHECK(diff < 1e-5);
    // A piece's take changes by take.activate and keeps its seams.
    const auto pieces = a.pieces();
    const auto geometry = a.pieces();
    a.run("take.activate", R"({"item":")" + pieces[1].item.str() + R"(","index":2})");
    const auto after = a.pieces();
    REQUIRE(after.size() == geometry.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i].start == geometry[i].start);
        CHECK(after[i].end == geometry[i].end);
    }
}

TEST_CASE("take.comp: crossfade 0 is a hard cut, linear is linear, long ones are clamped") {
    SUBCASE("a hard cut") {
        CompFixture f;
        const auto refs = f.references();
        f.run("take.activate", R"({"item":"it_1","index":2})");
        f.run("take.comp",
              R"({"item":"it_1","crossfade":0,"spans":[{"from":0.5,"to":1.5,"index":0}]})");
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() == 3);
        CHECK(pieces[0].end == pieces[1].start);
        CHECK(pieces[0].fadeOut == 0);
        CHECK(pieces[1].fadeIn == 0);
        CHECK(worst(f.render(), expected(refs, {{12000, 2, 0}, {36000, 0, 2}}, 2, 0,
                                         model::FadeShape::Linear)) < 1e-6);
    }
    SUBCASE("linear") {
        CompFixture f;
        const auto refs = f.references();
        f.run("take.activate", R"({"item":"it_1","index":2})");
        f.run("take.comp",
              R"({"item":"it_1","crossfade":0.02,"crossfade_shape":"linear",
                  "spans":[{"from":1,"to":2,"index":0}]})");
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() == 3);
        CHECK(pieces[0].outShape == model::FadeShape::Linear);
        CHECK(pieces[1].inShape == model::FadeShape::Linear);
        CHECK(worst(f.render(), expected(refs, {{24000, 2, 0}, {48000, 0, 2}}, 2, 480,
                                         model::FadeShape::Linear)) < 1e-5);
    }
    SUBCASE("a crossfade longer than the pieces beside it is shortened, with a warning") {
        CompFixture f;
        // A piece of 50 samples between two seams: each crossfade is at most half of it (25
        // samples either side), however long the one asked for.
        const auto result = f.run("take.comp",
                                  R"({"item":"it_1","crossfade":0.5,"spans":[
                                      {"from":1,"to":1.00208333333,"index":0}]})");
        bool clamped = false;
        for (const auto& w : result.find("warnings")->asArray())
            clamped = clamped || w.find("code")->asString() == "comp.crossfade_clamped";
        CHECK(clamped);
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() == 3);
        CHECK(pieces[0].end - pieces[1].start == 2 * 25);
        CHECK(pieces[1].end - pieces[2].start == 2 * 25);
    }
}

TEST_CASE("take.comp follows a take's playrate and a trimmed item") {
    SUBCASE("playrate 2") {
        CompFixture f;
        for (const char* take : {"tk_1", "tk_2", "tk_3"})
            f.run("take.set", std::string(R"({"take":")") + take + R"(","playrate":2})");
        const auto refs = f.references();
        f.run("take.activate", R"({"item":"it_1","index":2})");
        f.run("take.comp", kSpans);
        const std::vector<Seam> seams = {{12000, 2, 0}, {36000, 0, 1}, {60000, 1, 2}};
        CHECK(worst(f.render(), expected(refs, seams, 2, 240, model::FadeShape::Cos)) < 1e-5);
    }
    SUBCASE("an item trimmed at the front") {
        CompFixture f;
        f.run("item.split", R"({"item":"it_1","at":1})");
        f.run("item.remove", R"({"item":"it_1"})");
        REQUIRE(f.app->head().items.size() == 1);
        const std::string item = f.app->head().items.begin()->id.str();
        const auto refs = f.references();
        f.run("take.activate", R"({"item":")" + item + R"(","index":2})");
        f.run("take.comp", R"({"item":")" + item + R"(","spans":[{"from":2,"to":3,"index":0}]})");
        const std::vector<Seam> seams = {{48000, 2, 0}, {72000, 0, 2}};
        CHECK(worst(f.render(), expected(refs, seams, 2, 240, model::FadeShape::Cos)) < 1e-5);
    }
}

TEST_CASE("take.comp: a comp renders in real time as offline, in both executors") {
    CompFixture f;
    f.run("take.comp", kSpans);
    const model::Session session = f.app->head();
    auto pool = makePool();
    auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 2.0, .projectDir = f.dir});
    REQUIRE(offline);
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session, f.dir));
        engine.play();
        const std::int64_t cycles = (96000 + 4096) / 128;
        if (options.anticipative)
            runAhead(engine, backend, cycles, 128);
        else
            backend.run(cycles);
        engine.stop();
        const std::uint32_t latency = engine.outputLatency();
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], latency, offline->audio.channels[0], 96000) ==
              0.0);
        CHECK(maxDiff(backend.capture().channels[1], latency, offline->audio.channels[1], 96000) ==
              0.0);
    }
}

TEST_CASE("take.comp skips the crossfade for a warped take and for MIDI, and says so") {
    auto skipped = [](const json::Value& result) {
        for (const auto& w : result.find("warnings")->asArray())
            if (w.find("code")->asString() == "comp.crossfade_skipped") return true;
        return false;
    };
    SUBCASE("a take with stretch markers") {
        CompFixture f;
        f.run("take.stretch.add", R"({"take":"tk_2","source_pos":2.3,"item_pos":0.25})");
        const auto result = f.run("take.comp", kSpans);
        CHECK(skipped(result));
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() == 4);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(pieces[i].end == pieces[i + 1].start);
            CHECK(pieces[i].fadeOut == 0);
            CHECK(pieces[i + 1].fadeIn == 0);
        }
    }
    SUBCASE("MIDI takes") {
        CompFixture f;
        f.run("track.add", R"({"name":"Keys"})");
        f.run("item.add",
              R"({"track":"tr_2","bar":1,"notation":"voice keys; 1: C3:q D3:q E3:q F3:q"})");
        f.run("take.add", R"({"item":"it_2","from":"tk_4"})");
        f.run("take.activate", R"({"item":"it_2","index":1})");
        f.run("midi.write", R"({"item":"it_2","notation":"voice keys; 1: G3:q A3:q B3:q C4:q"})");
        const auto result =
            f.run("take.comp", R"({"item":"it_2","spans":[{"from":1,"to":3,"index":0}]})");
        CHECK(skipped(result));
        const auto& list = result.find("pieces")->asArray();
        REQUIRE(list.size() == 3);
        const model::Session& s = f.app->head();
        for (const auto& p : list) {
            const auto& it = *s.items.find(*model::Id::parse(p.find("item")->asString()));
            CHECK(it.fadeIn.length == 0);
            CHECK(it.fadeOut.length == 0);
        }
        // The pieces tile exactly: no overlap, no gap.
        const auto a = model::itemSpan(s, *s.items.find({model::Kind::Item, 2}));
        CHECK(a.endSec == doctest::Approx(0.5));
        // What each piece plays: take 1 (G A B C) outside, take 0 (C D E F) from beat 1 to 3.
        auto plays = [&](const json::Value& piece) {
            return f
                .run("midi.get",
                     R"({"item":")" + piece.find("item")->asString() + R"(","sounding":true})")
                .find("text")
                ->asString();
        };
        const auto first = plays(list[0]), middle = plays(list[1]), last = plays(list[2]);
        CHECK(first.find("G3") != std::string::npos);
        CHECK(first.find("A3") == std::string::npos);
        CHECK(middle.find("D3") != std::string::npos);
        CHECK(middle.find("E3") != std::string::npos);
        CHECK(middle.find("C3") == std::string::npos);
        CHECK(middle.find("F3") == std::string::npos);
        CHECK(last.find("C4") != std::string::npos);
        CHECK(last.find("B3") == std::string::npos);
    }
}

TEST_CASE("take.comp.get reads a comp, and a saved project reads it alike") {
    CompFixture f;
    f.run("take.comp", kSpans);
    const auto made = f.run("take.comp.get", R"({"item":"it_2"})");
    const auto& list = made.find("pieces")->asArray();
    REQUIRE(list.size() == 4);
    const std::size_t take[] = {2, 0, 1, 2};
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(list[i].find("index")->asInt() == static_cast<std::int64_t>(take[i]));
    // Save, reopen, read again.
    // A backslash in the name (a Windows path has them) must survive as a JSON value.
    const auto file = f.dir / "comp\\dir.supple";
    auto where = json::Value::object();
    where["path"] = file.string();
    auto saved = f.callValue("project.save", where);
    REQUIRE_MESSAGE(saved, (saved ? "" : saved.error().toString()));
    REQUIRE(f.callValue("project.open", where));
    const auto again = f.run("take.comp.get", R"({"item":"it_2"})");
    CHECK(json::writeCompact(again) == json::writeCompact(made));
}

TEST_CASE("take.comp is one undo step") {
    CompFixture f;
    // Undo keeps the id counters, so compare the objects, not the whole file.
    const model::Session before = f.app->head();
    f.run("take.comp", kSpans);
    CHECK(f.app->head().items.size() == 4);
    f.run("history.undo");
    const model::Session& after = f.app->head();
    REQUIRE(after.items.size() == 1);
    CHECK(json::writeCompact(model::toJson(*after.items.find({model::Kind::Item, 1}))) ==
          json::writeCompact(model::toJson(*before.items.find({model::Kind::Item, 1}))));
    CHECK(after.takes.size() == before.takes.size());
    for (const auto& e : before.takes)
        CHECK(json::writeCompact(model::toJson(*after.takes.find(e.id))) ==
              json::writeCompact(model::toJson(*e.value)));
}

TEST_CASE("take.comp refusals carry a code, message, path and hint") {
    CompFixture f;
    SUBCASE("which item") {
        f.refused("take.comp", R"({"spans":[{"from":1,"to":2,"index":0}]})", "comp.item", "/item");
        f.refused("take.comp",
                  R"({"item":"it_1","items":["it_1"],"spans":[{"from":1,"to":2,"index":0}]})",
                  "comp.item", "/item");
        f.refused("take.comp", R"({"item":"it_9","spans":[{"from":1,"to":2,"index":0}]})",
                  "it.unknown", "/item");
        f.refused("take.comp", R"({"item":"cl_1","spans":[{"from":1,"to":2,"index":0}]})",
                  "item.kind", "/item");
    }
    SUBCASE("spans") {
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":2,"to":2,"index":0}]})",
                  "comp.span", "/spans/0/to");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":3,"to":2,"index":0}]})",
                  "comp.span", "/spans/0/to");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":5,"index":0}]})",
                  "comp.span", "/spans/0/to");
        f.refused("take.comp",
                  R"({"item":"it_1","spans":[{"from":1,"to":3,"index":0},
                                             {"from":2,"to":3.5,"index":1}]})",
                  "comp.span", "/spans/1/from");
    }
    SUBCASE("takes") {
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2}]})", "comp.take",
                  "/spans/0");
        f.refused("take.comp",
                  R"({"item":"it_1","spans":[{"from":1,"to":2,"index":0,"take":"tk_1"}]})",
                  "comp.take", "/spans/0");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":3}]})",
                  "comp.take", "/spans/0/index");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"take":"tk_9"}]})",
                  "comp.take", "/spans/0/take");
    }
    SUBCASE("a single take") {
        f.run("take.remove", R"({"take":"tk_2"})");
        f.run("take.remove", R"({"take":"tk_3"})");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":0}]})",
                  "comp.takes", "/item");
    }
    SUBCASE("a looped item") {
        f.run("item.set", R"({"item":"it_1","loop":true})");
        f.refused("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":0}]})",
                  "comp.loop", "/item");
    }
    SUBCASE("a locked item") {
        f.run("item.set", R"({"item":"it_1","locked":true})");
        auto result =
            f.call("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":0}]})");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "item.locked");
    }
    SUBCASE("items that are not one comp") {
        f.run("take.comp", kSpans);
        f.refused("take.comp", R"({"items":["it_1","it_3"],"spans":[{"from":1,"to":2,"index":0}]})",
                  "comp.mismatch", "/items/1");
        // Pieces whose takes differ are not one comp.
        f.run("take.set", R"({"take":"tk_5","name":"renamed"})");
        f.refused("take.comp", R"({"items":["it_1","it_2"],"spans":[{"from":1,"to":2,"index":0}]})",
                  "comp.mismatch", "/items/1");
    }
}

TEST_CASE("take.comp does not pull in a copy of the item placed right after it") {
    CompFixture f;
    f.run("item.set", R"({"item":"it_1","fade_out":0.2})");
    f.run("item.duplicate", R"({"item":"it_1"})");
    f.run("take.activate", R"({"item":"it_2","index":1})");
    const model::Session before = f.app->head();
    const auto original =
        json::writeCanonical(model::toJson(*before.items.find({model::Kind::Item, 1})));
    // The copy has the same takes and touches the original, but its source does not continue
    // the original's at the boundary: it is another item, not the next piece.
    const auto got = f.run("take.comp.get", R"({"item":"it_2"})");
    REQUIRE(got.find("pieces")->asArray().size() == 1);
    CHECK(got.find("pieces")->asArray()[0].find("item")->asString() == "it_2");
    CHECK(f.run("take.comp.get", R"({"item":"it_1"})").find("pieces")->asArray().size() == 1);
    f.run("take.comp", R"({"item":"it_2","spans":[{"from":5,"to":6,"index":2}]})");
    const model::Session after = f.app->head();
    CHECK(json::writeCanonical(model::toJson(*after.items.find({model::Kind::Item, 1}))) ==
          original);
    for (model::Id take : before.items.find({model::Kind::Item, 1})->takes)
        CHECK(json::writeCanonical(model::toJson(*after.takes.find(take))) ==
              json::writeCanonical(model::toJson(*before.takes.find(take))));
    // The copy was comped alone: three pieces, and the original's 0.2 s fade-out is still there.
    CHECK(after.items.size() == 4);
    CHECK(after.items.find({model::Kind::Item, 1})->fadeOut.length == doctest::Approx(0.2));
    // Naming both as one comp is refused rather than guessed.
    f.refused("take.comp", R"({"items":["it_1","it_2"],"spans":[{"from":1,"to":2,"index":0}]})",
              "comp.mismatch", "/items/1");
}

namespace {
// The comp as a person reads it, up to ids: each piece's place, fades, take index and the
// source offsets of its takes, in time order.
struct Shape {
    double position, length, fadeIn, fadeOut;
    std::size_t index;
    std::vector<double> offsets;
};
std::vector<Shape> shapeOf(const model::Session& s, std::optional<model::Id> track = std::nullopt) {
    std::vector<std::pair<std::int64_t, Shape>> out;
    for (const auto& e : s.items) {
        const auto& it = *e.value;
        if (track && it.track != *track) continue;
        Shape p{it.position, it.length, it.fadeIn.length, it.fadeOut.length, 0, {}};
        for (std::size_t k = 0; k < it.takes.size(); ++k) {
            if (it.takes[k] == *it.activeTake) p.index = k;
            p.offsets.push_back(s.takes.find(it.takes[k])->startOffset);
        }
        out.emplace_back(time::samplesAt(model::itemSpan(s, it).startSec, s.project->sampleRate),
                         std::move(p));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Shape> shapes;
    for (auto& [at, p] : out) shapes.push_back(std::move(p));
    return shapes;
}
std::string beats(std::int64_t sample) {
    char text[40];
    std::snprintf(text, sizeof text, "%.17g", static_cast<double>(sample) / 24000.0);
    return text;
}
}  // namespace

TEST_CASE("take.comp moving a seam gives what the same spans give from scratch") {
    // 120 bpm, 48 kHz: a beat is 24000 samples. A seam at beat 1, moved by a few samples either
    // way: the old boundary goes, so the new crossfade is not clamped to the sliver between.
    const std::int64_t base = 24000;
    for (const std::int64_t move : {24, 1, -1, -24, 600}) {
        CAPTURE(move);
        CompFixture a, b;
        a.run("take.activate", R"({"item":"it_1","index":0})");
        b.run("take.activate", R"({"item":"it_1","index":0})");
        a.run("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":1}]})");
        const std::string seam = beats(base + move);
        const std::string moved = R"({"item":"it_1","spans":[{"from":0,"to":)" + seam +
                                  R"(,"index":0},{"from":)" + seam + R"(,"to":2,"index":1}]})";
        const std::string scratch =
            R"({"item":"it_1","spans":[{"from":)" + seam + R"(,"to":2,"index":1}]})";
        a.run("take.comp", moved);
        b.run("take.comp", scratch);
        const auto got = shapeOf(a.app->head()), want = shapeOf(b.app->head());
        REQUIRE(got.size() == want.size());
        CHECK(got.size() == 3);
        CHECK(a.app->head().items.size() == b.app->head().items.size());
        CHECK(a.app->head().takes.size() == b.app->head().takes.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].position == want[i].position);
            CHECK(got[i].length == want[i].length);
            CHECK(got[i].fadeIn == want[i].fadeIn);
            CHECK(got[i].fadeOut == want[i].fadeOut);
            CHECK(got[i].index == want[i].index);
            REQUIRE(got[i].offsets.size() == want[i].offsets.size());
            for (std::size_t k = 0; k < got[i].offsets.size(); ++k)
                CHECK(got[i].offsets[k] == want[i].offsets[k]);
        }
        // The crossfade is the whole 480 samples, however small the move.
        CHECK(got[0].fadeOut * 48000.0 == doctest::Approx(480.0));
        CHECK(got[1].fadeIn * 48000.0 == doctest::Approx(480.0));
        CHECK(a.render() == b.render());
    }
}

TEST_CASE("take.comp keeps a boundary where the pieces differ, and a seam between other takes") {
    CompFixture f;
    f.run("take.activate", R"({"item":"it_1","index":0})");
    f.run("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":1}]})");
    // A piece with its own gain is not the same piece cut in two: painting the neighbours the
    // same take leaves the boundary.
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 3);
    f.run("item.set", R"({"item":")" + pieces[2].item.str() + R"(","gain_db":-6})");
    f.run("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":4,"index":1}]})");
    CHECK(f.pieces().size() == 3);
    // Painting both the same take, with nothing different between them, joins them.
    f.run("item.set", R"({"item":")" + pieces[2].item.str() + R"(","gain_db":0})");
    f.run("take.comp", R"({"item":"it_1","spans":[{"from":0,"to":4,"index":1}]})");
    CHECK(f.pieces().size() == 1);
    CHECK(f.app->head().items.size() == 1);
    CHECK(f.app->head().takes.size() == 3);
}

namespace {
std::string beatsText(double beat) {
    char text[40];
    std::snprintf(text, sizeof text, "%.17g", beat);
    return text;
}
}  // namespace

TEST_CASE("take.comp refusals name the piece they mean in /items/k") {
    CompFixture f;
    f.run("take.comp", kSpans);
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 4);
    const std::string all = R"({"items":[")" + pieces[0].item.str() + R"(",")" +
                            pieces[1].item.str() + R"(",")" + pieces[2].item.str() + R"(",")" +
                            pieces[3].item.str() + R"("],"spans":[{"from":1,"to":2,"index":0}]})";
    f.run("item.set", R"({"item":")" + pieces[2].item.str() + R"(","locked":true})");
    f.refused("take.comp", all, "item.locked", "/items/2");
    f.run("item.set", R"({"item":")" + pieces[2].item.str() + R"(","locked":false})");
    // An edit-linked peer beside only one piece: the group's other track does not line up.
    f.run("track.add", R"({"name":"Other"})");
    f.run("group.add", R"({"name":"Edit","members":["tr_1","tr_2"],"links":["edit"]})");
    const std::string at = beatsText(f.app->head().items.find(pieces[1].item)->position);
    f.run("item.duplicate", R"({"item":")" + pieces[1].item.str() + R"(","to":)" + at + "}");
    const std::string copy = "it_" + std::to_string(f.app->head().items.size());
    f.run("item.move", R"({"item":")" + copy + R"(","track":"tr_2","position":)" + at +
                           R"(,"bypass_groups":true})");
    f.refused("take.comp", all, "comp.group_mismatch", "/items/1");
}

namespace {
// A two-take MIDI item on a new track (tr_2, it_2): take 0 plays C4 and E4 as whole notes in
// bars 1 and 2, take 1 G4 and B4.
void midiStack(CompFixture& f) {
    f.run("track.add", R"({"name":"Keys"})");
    f.run("item.add", R"({"track":"tr_2","bar":1,"notation":"voice keys; 1: C4:w; 2: E4:w"})");
    f.run("take.add", R"({"item":"it_2","from":"tk_4"})");
    f.run("take.activate", R"({"item":"it_2","index":1})");
    f.run("midi.write", R"({"item":"it_2","notation":"voice keys; 1: G4:w; 2: B4:w"})");
    f.run("take.activate", R"({"item":"it_2","index":0})");
}
// What each piece of the comp sounds, in time order.
std::vector<std::string> sounding(CompFixture& f, const json::Value& result) {
    std::vector<std::string> out;
    for (const auto& p : result.find("pieces")->asArray())
        out.push_back(
            f.run("midi.get", R"({"item":")" + p.find("item")->asString() + R"(","sounding":true})")
                .find("text")
                ->asString());
    return out;
}
}  // namespace

TEST_CASE("take.comp moving a MIDI seam again and again gives what the same spans give") {
    CompFixture a, b;
    midiStack(a);
    midiStack(b);
    a.run("take.comp", R"({"item":"it_2","spans":[{"from":3,"to":8,"index":1}]})");
    json::Value last;
    for (const char* seam : {"2.5", "2", "1.5", "1", "2.25"}) {
        last = a.run("take.comp", std::string(R"({"item":"it_2","spans":[{"from":0,"to":)") + seam +
                                      R"(,"index":0},{"from":)" + seam + R"(,"to":8,"index":1}]})");
        // However the seam moves, there is one piece for each take: two, not one per move.
        CHECK(last.find("pieces")->asArray().size() == 2);
    }
    const auto want =
        b.run("take.comp", R"({"item":"it_2","spans":[{"from":2.25,"to":8,"index":1}]})");
    const auto got = shapeOf(a.app->head(), model::Id{model::Kind::Track, 2});
    const auto expect = shapeOf(b.app->head(), model::Id{model::Kind::Track, 2});
    REQUIRE(got.size() == expect.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].position == expect[i].position);
        CHECK(got[i].length == expect[i].length);
        CHECK(got[i].index == expect[i].index);
        CHECK(got[i].offsets == expect[i].offsets);
    }
    CHECK(a.app->head().items.size() == b.app->head().items.size());
    CHECK(a.app->head().takes.size() == b.app->head().takes.size());
    CHECK(a.app->head().midi.size() == b.app->head().midi.size());
    // What sounds is the same, and a held note is one note: C4 once, then G4 once.
    const auto heard = sounding(a, last), wanted = sounding(b, want);
    CHECK(heard == wanted);
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].find("C4") != std::string::npos);
    CHECK(heard[1].find("G4") != std::string::npos);
    for (const auto& w : last.find("warnings")->asArray())
        CHECK(w.find("code")->asString() != "comp.midi_boundary");
}

TEST_CASE("take.comp warns where MIDI pieces of one take cannot be joined") {
    CompFixture f;
    midiStack(f);
    f.run("take.comp", R"({"item":"it_2","spans":[{"from":4,"to":8,"index":1}]})");
    // The second half now has a gain of its own: not the first half cut in two.
    const auto pieces = f.run("take.comp.get", R"({"item":"it_2"})").find("pieces")->asArray();
    REQUIRE(pieces.size() == 2);
    f.run("item.set", R"({"item":")" + pieces[1].find("item")->asString() + R"(","gain_db":-3})");
    const auto result =
        f.run("take.comp", R"({"item":"it_2","spans":[{"from":0,"to":8,"index":0}]})");
    CHECK(result.find("pieces")->asArray().size() == 2);
    bool warned = false;
    for (const auto& w : result.find("warnings")->asArray())
        if (w.find("code")->asString() == "comp.midi_boundary") {
            warned = true;
            CHECK(w.find("message")->asString().find("4") != std::string::npos);
        }
    CHECK(warned);
}

TEST_CASE(
    "take.comp joins identical same-take pieces anywhere in the run, not only at a moved seam") {
    CompFixture f;
    f.run("take.activate", R"({"item":"it_1","index":0})");
    const char* spans = R"({"item":"it_1","spans":[{"from":1,"to":2,"index":1}]})";
    f.run("take.comp", spans);
    REQUIRE(f.pieces().size() == 3);
    // A split of the last piece in two identical halves: the same take, nothing different.
    const std::string last = f.pieces()[2].item.str();
    f.run("item.split", R"({"item":")" + last + R"(","at":3})");
    REQUIRE(f.pieces().size() == 4);
    // The same spans again join them (activate a take on a half after the re-comp, not before).
    f.run("take.comp", spans);
    CHECK(f.pieces().size() == 3);
    // A half with another take is not identical, and its split stays.
    f.run("item.split", R"({"item":")" + last + R"(","at":3})");
    f.run("take.activate", R"({"item":")" + f.pieces()[3].item.str() + R"(","index":2})");
    f.run("take.comp", spans);
    CHECK(f.pieces().size() == 4);
}

TEST_CASE("take.comp.get reads back the crossfade the comp was made with") {
    SUBCASE("a run of one piece reads the default") {
        CompFixture f;
        const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        CHECK(got.find("crossfade")->asDouble() == doctest::Approx(0.01));
        CHECK(got.find("crossfade_shape")->asString() == "cos");
    }
    SUBCASE("hard cuts read 0") {
        CompFixture f;
        f.run("take.comp",
              R"({"item":"it_1","crossfade":0,"spans":[{"from":1,"to":2,"index":0}]})");
        const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        CHECK(got.find("crossfade")->asDouble() == 0.0);
    }
    SUBCASE("a seam clamped by a short piece does not shrink it") {
        CompFixture f;
        // 20 ms asked for. The first piece is 48 samples (2 ms), so the seam at its end is
        // clamped to 24 samples either side; the seams at beats 2 and 3 have room: 480 each.
        const auto made = f.run("take.comp",
                                R"({"item":"it_1","crossfade":0.02,"crossfade_shape":"linear",
                                    "spans":[{"from":0,"to":0.002,"index":1},
                                             {"from":2,"to":3,"index":0}]})");
        bool clamped = false;
        for (const auto& w : made.find("warnings")->asArray())
            clamped = clamped || w.find("code")->asString() == "comp.crossfade_clamped";
        CHECK(clamped);
        const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        CHECK(got.find("crossfade")->asDouble() == doctest::Approx(0.02).epsilon(1e-9));
        CHECK(got.find("crossfade_shape")->asString() == "linear");
        // take.comp itself answers the same.
        CHECK(made.find("crossfade")->asDouble() == got.find("crossfade")->asDouble());
    }
}

TEST_CASE(
    "take.comp with the read-back crossfade and spans rebuilds the same comp, byte for byte") {
    for (const char* shape : {"cos", "linear"}) {
        CAPTURE(shape);
        CompFixture f;
        f.run("take.comp", std::string(R"({"item":"it_1","crossfade":0.02,"crossfade_shape":")") +
                               shape + R"(","spans":[{"from":0,"to":0.002,"index":1},
                                                     {"from":2,"to":3,"index":0}]})");
        const std::string made = model::serializeProject(f.app->head());
        const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        auto args = json::Value::object();
        args["item"] = "it_1";
        args["crossfade"] = got.find("crossfade")->asDouble();
        args["crossfade_shape"] = got.find("crossfade_shape")->asString();
        auto spans = json::Value::array();
        for (const auto& piece : got.find("pieces")->asArray()) {
            auto span = json::Value::object();
            span["from"] = piece.find("from")->asDouble();
            span["to"] = piece.find("to")->asDouble();
            span["index"] = piece.find("index")->asInt();
            spans.push_back(std::move(span));
        }
        args["spans"] = std::move(spans);
        auto again = f.bus->execute({"take.comp", args, origin});
        REQUIRE_MESSAGE(again, (again ? "" : again.error().toString()));
        CHECK(model::serializeProject(f.app->head()) == made);
    }
}

TEST_CASE("take.comp.get says when the seams of a run are not all alike") {
    const std::string spans = R"({"item":"it_1","crossfade":0.02,"crossfade_shape":"cos",
                                  "spans":[{"from":0,"to":1,"index":1},
                                           {"from":2,"to":3,"index":0}]})";
    auto seamsOf = [](const json::Value& got) {
        std::vector<std::int64_t> out;
        for (const auto& d : got.find("differing_seams")->asArray())
            out.push_back(d.find("seam")->asInt());
        return out;
    };
    SUBCASE("a comp as take.comp made it is uniform, and so is one with a clamped seam") {
        CompFixture f;
        f.run("take.comp", spans);
        auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        CHECK(got.find("uniform")->asBool());
        CHECK(seamsOf(got).empty());
        CompFixture g;
        g.run("take.comp", R"({"item":"it_1","crossfade":0.02,
                               "spans":[{"from":0,"to":0.002,"index":1},{"from":2,"to":3,"index":0}]})");
        got = g.run("take.comp.get", R"({"item":"it_1"})");
        CHECK(got.find("uniform")->asBool());
        // The result of take.comp says the same.
        CHECK(g.run("take.comp", R"({"item":"it_1","crossfade":0.02,
                      "spans":[{"from":0,"to":0.002,"index":1},{"from":2,"to":3,"index":0}]})")
                  .find("uniform")
                  ->asBool());
    }
    SUBCASE("a seam fade set by hand to another length is listed") {
        CompFixture f;
        f.run("take.comp", spans);
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() >= 3);
        f.run("item.set", R"({"item":")" + pieces[1].item.str() + R"(","fade_in":0.05})");
        const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
        CHECK_FALSE(got.find("uniform")->asBool());
        CHECK(seamsOf(got) == std::vector<std::int64_t>{0});
        const auto& first = got.find("differing_seams")->asArray()[0];
        CHECK(first.find("left")->asString() == pieces[0].item.str());
        CHECK(first.find("right")->asString() == pieces[1].item.str());
        CHECK(first.find("at")->asDouble() == doctest::Approx(1.0));
    }
    SUBCASE("a seam of another shape, or a hard cut among crossfaded seams, is listed") {
        CompFixture f;
        f.run("take.comp", spans);
        const auto pieces = f.pieces();
        REQUIRE(pieces.size() >= 3);
        f.run("item.set", R"({"item":")" + pieces[2].item.str() + R"(","fade_in_shape":"linear"})");
        CHECK(seamsOf(f.run("take.comp.get", R"({"item":"it_1"})")) ==
              std::vector<std::int64_t>{1});
        CompFixture g;
        g.run("take.comp", spans);
        const auto gp = g.pieces();
        g.run("item.set", R"({"item":")" + gp[1].item.str() + R"(","fade_in":0})");
        CHECK(seamsOf(g.run("take.comp.get", R"({"item":"it_1"})")) ==
              std::vector<std::int64_t>{0});
    }
}

// Edit groups (a multitrack loop recording): the same call comps every member.
namespace {
struct GroupFixture : CompFixture {
    // tr_1 and tr_2 each hold one item of three takes, from one loop recording; the two are
    // an edit group.
    explicit GroupFixture(std::size_t passes = 3, std::size_t otherPasses = 3)
        : CompFixture(passes) {
        run("track.add", R"({"name":"Second"})");
        record("tr_2", "second.wav", 500.0, otherPasses);
        run("group.add", R"({"name":"Drums","members":["tr_1","tr_2"],"links":["edit"]})");
    }
    static model::Id track(unsigned n) { return {model::Kind::Track, n}; }
    static std::string idOf(const std::vector<Piece>& pieces, std::size_t k) {
        return pieces[k].item.str();
    }
};
const char* const kGroupSpans = R"({"item":"it_1","crossfade":0.02,
    "spans":[{"from":0,"to":1,"index":1},{"from":2,"to":3,"index":0}]})";

// Seams and takes of the pieces of two tracks, side by side.
void sameComp(const std::vector<Piece>& a, const std::vector<Piece>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        CHECK(a[k].start == b[k].start);
        CHECK(a[k].end == b[k].end);
        CHECK(a[k].index == b[k].index);
        CHECK(a[k].fadeIn == b[k].fadeIn);
        CHECK(a[k].fadeOut == b[k].fadeOut);
    }
}
}  // namespace

TEST_CASE("take.comp on a group comps every member, whole items first") {
    GroupFixture f;
    REQUIRE(f.pieces(GroupFixture::track(1)).size() == 1);  // whole, unsplit, both
    REQUIRE(f.pieces(GroupFixture::track(2)).size() == 1);
    const model::Session before = f.app->head();
    const auto steps = f.app->versions().history().size();
    const auto result = f.run("take.comp", kGroupSpans);
    const auto lead = f.pieces(GroupFixture::track(1));
    const auto second = f.pieces(GroupFixture::track(2));
    CHECK(lead.size() >= 3);
    sameComp(lead, second);
    // The result: the named run's pieces as before, the other member under `members`.
    CHECK(result.find("pieces")->asArray().size() == lead.size());
    const auto& members = result.find("members")->asArray();
    REQUIRE(members.size() == 1);
    CHECK(members[0].find("track")->asString() == "tr_2");
    REQUIRE(members[0].find("pieces")->asArray().size() == second.size());
    CHECK(members[0].find("pieces")->asArray()[0].find("item")->asString() == second[0].item.str());
    CHECK(members[0].find("crossfade")->asDouble() == result.find("crossfade")->asDouble());
    // One undo step puts both back.
    CHECK(f.app->versions().history().size() == steps + 1);
    f.run("history.undo");
    const model::Session& after = f.app->head();
    REQUIRE(after.items.size() == 2);
    for (const auto& e : before.items)
        CHECK(json::writeCompact(model::toJson(*after.items.find(e.id))) ==
              json::writeCompact(model::toJson(*e.value)));
    CHECK(after.takes.size() == before.takes.size());
}

TEST_CASE("take.comp on a group: each member equals the same comp made on its own") {
    GroupFixture f;
    f.run("take.comp", kGroupSpans);
    GroupFixture alone;  // the same fixture, comped member by member with bypass_groups
    alone.run("take.comp", std::string(kGroupSpans).insert(1, R"("bypass_groups":true,)"));
    alone.run("take.comp", R"({"item":")" + alone.pieces(GroupFixture::track(2))[0].item.str() +
                               R"(","bypass_groups":true,"crossfade":0.02,
                      "spans":[{"from":0,"to":1,"index":1},{"from":2,"to":3,"index":0}]})");
    sameComp(f.pieces(GroupFixture::track(1)), alone.pieces(GroupFixture::track(1)));
    sameComp(f.pieces(GroupFixture::track(2)), alone.pieces(GroupFixture::track(2)));
}

TEST_CASE("take.comp with bypass_groups comps only the named item") {
    GroupFixture f;
    const auto result =
        f.run("take.comp", std::string(kGroupSpans).insert(1, R"("bypass_groups":true,)"));
    CHECK(f.pieces(GroupFixture::track(1)).size() >= 3);
    CHECK(f.pieces(GroupFixture::track(2)).size() == 1);
    CHECK(result.find("members")->asArray().empty());
}

TEST_CASE("take.comp on a group names a member that lacks the take index, and changes nothing") {
    GroupFixture f(3, 2);  // tr_2's recording has two passes: no take index 2
    const std::string before = model::serializeProject(f.app->head());
    auto result = f.call("take.comp", R"({"item":"it_1","crossfade":0.02,
        "spans":[{"from":0,"to":1,"index":1},{"from":2,"to":3,"index":2}]})");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "comp.group_takes");
    CHECK(result.error().message.find("tr_2") != std::string::npos);
    CHECK_FALSE(result.error().hint.empty());
    CHECK(model::serializeProject(f.app->head()) == before);
}

TEST_CASE("take.comp on a group names a member whose run does not line up") {
    GroupFixture f;
    f.run("take.comp", kGroupSpans);
    // tr_2 alone, its seams moved: its first piece still lines up with tr_1's, the rest do not.
    f.run("take.comp", R"({"item":")" + f.pieces(GroupFixture::track(2))[0].item.str() +
                           R"(","bypass_groups":true,"crossfade":0.02,
                               "spans":[{"from":0,"to":1,"index":1},{"from":2.5,"to":3,"index":1}]})");
    const std::string before = model::serializeProject(f.app->head());
    auto result = f.call("take.comp", kGroupSpans);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "comp.group_mismatch");
    CHECK(result.error().message.find("tr_2") != std::string::npos);
    CHECK_FALSE(result.error().hint.empty());
    CHECK(model::serializeProject(f.app->head()) == before);
}

TEST_CASE("take.comp on a group: a member with other start offsets or play rates lines up") {
    SUBCASE("another start_offset on its takes") {
        GroupFixture f;
        for (const auto& e : f.app->head().items)
            if (e.value->track == GroupFixture::track(2))
                for (const auto t : e.value->takes)
                    f.run("take.set", R"({"take":")" + t.str() + R"(","start_offset":0.25})");
        f.run("take.comp", kGroupSpans);
        sameComp(f.pieces(GroupFixture::track(1)), f.pieces(GroupFixture::track(2)));
    }
    SUBCASE("a play rate on its takes") {
        GroupFixture f;
        for (const auto& e : f.app->head().items)
            if (e.value->track == GroupFixture::track(2))
                for (const auto t : e.value->takes)
                    f.run("take.set", R"({"take":")" + t.str() + R"(","playrate":2})");
        const auto result = f.run("take.comp", kGroupSpans);
        const auto lead = f.pieces(GroupFixture::track(1));
        const auto second = f.pieces(GroupFixture::track(2));
        REQUIRE(lead.size() == 4);
        sameComp(lead, second);
        CHECK(result.find("members")->asArray()[0].find("crossfade")->asDouble() ==
              doctest::Approx(0.02));
        // Each member reads as it sounds: the same spans, takes and seams.
        CHECK(f.run("take.comp.get", R"({"item":"it_1"})").find("uniform")->asBool());
    }
}

TEST_CASE("take.comp.get on a group lists the members, and a member that differs") {
    GroupFixture f;
    f.run("take.comp", kGroupSpans);
    auto got = f.run("take.comp.get", R"({"item":"it_1"})");
    REQUIRE(got.find("members")->asArray().size() == 1);
    CHECK(got.find("uniform")->asBool());
    CHECK(got.find("differing_members")->asArray().empty());
    // tr_2's seam fade by hand: the member is not uniform, and the group is not.
    const auto second = f.pieces(GroupFixture::track(2));
    f.run("item.set",
          R"({"item":")" + second[1].item.str() + R"(","fade_in":0.05,"bypass_groups":true})");
    got = f.run("take.comp.get", R"({"item":"it_1"})");
    CHECK_FALSE(got.find("uniform")->asBool());
    CHECK_FALSE(got.find("members")->asArray()[0].find("uniform")->asBool());
    // And a member comped differently from the lead (here: bypass, other spans) is listed.
    GroupFixture g;
    g.run("take.comp", kGroupSpans);
    const auto gs = g.pieces(GroupFixture::track(2));
    g.run("take.comp", R"({"item":")" + gs[0].item.str() + R"(","bypass_groups":true,
                           "crossfade":0.02,"spans":[{"from":0,"to":1,"index":0}]})");
    auto other = g.run("take.comp.get", R"({"item":"it_1"})");
    CHECK_FALSE(other.find("uniform")->asBool());
    REQUIRE(other.find("differing_members")->asArray().size() == 1);
    CHECK(other.find("differing_members")->asArray()[0].asString() == "tr_2");
}
