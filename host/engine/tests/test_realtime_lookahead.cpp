// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Real time equals offline for a compensated take's look-ahead, at locates and loops.
#include <doctest/doctest.h>

#include "realtime_support.h"

// A take chain compensated for the 64-frame latency after its instrument
// runs its notes that far ahead. After a locate, the notes between the
// locate and that look-ahead are due already: they play at once, late, at
// the first frame, on the unity and the rate path, at the song's start and
// after a seek. A seek plays as the offline render from there.
TEST_CASE("a compensated take's notes inside its look-ahead play at once after a locate" *
          doctest::description("rt")) {
    const auto session = [](double rate) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance synth;
        synth.plugin.id = "com.suppleaudio.test.sine";
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        take.chain = {t.create(synth), t.create(delay)};
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 16 * 960;
        // At the song's start, and 25 frames after 1 s (inside the look-ahead
        // of a seek there).
        src.sequence.notes = {{0, 960, 0, 60, 100, 0}, {1921, 960, 0, 72, 100, 0}};
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    const auto first = [](const std::vector<float>& x) {
        double peak = 0;
        for (std::size_t i = 0; i < 2000; ++i) peak = std::max(peak, std::fabs(double(x[i])));
        return peak;
    };
    constexpr std::int64_t kFrames = 24000;
    for (const double rate : {1.0, 1.25})
        for (const double from : {0.0, 1.0}) {
            INFO("rate " << rate << ", from " << from);
            const auto s = session(rate);
            auto pool = makePool();
            const auto offline =
                renderOffline(s, pool, {.startSeconds = from, .endSeconds = from + 1.0}).value();
            const auto& want = offline.audio.channels[0];
            REQUIRE(want.size() >= static_cast<std::size_t>(kFrames));
            CHECK(first(want) > 0.05);
            for (const EngineOptions& options : kBothExecutors) {
                INFO("anticipative " << options.anticipative << ", workers " << options.workers);
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, kFrames + 4096));
                Engine engine(rtPool, media, options);
                REQUIRE(
                    engine
                        .start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                        .ok());
                REQUIRE(engine.load(s));
                REQUIRE(engine.awaitPlan());
                if (from > 0) engine.seek(from);
                engine.play();
                runFrames(engine, backend, options, kFrames, 256);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                REQUIRE(engine.outputLatency() == 0);
                const auto& got = backend.capture().channels[0];
                CHECK(first(got) > 0.05);
                CHECK(maxDiff(got, 0, want, static_cast<std::size_t>(kFrames)) == 0.0);
            }
        }
}

// A compensated take (64 frames of latency after its instrument) with a note
// just after the loop start, inside its look-ahead: every pass plays it on
// time. The chain takes the note before the wrap, its look-ahead early, at
// unity and under a master playrate. With the loop at 0.5 s the first pass
// reaches the note without a wrap, as the offline render (no loop there)
// does, and every later pass plays as the first. With the loop at the song's
// start the first pass plays the note late (it is due at the first frame);
// every later pass plays it on time, as the other loop's first pass.
TEST_CASE("a compensated take's note just after the loop start plays on time every pass" *
          doctest::description("rt")) {
    // The note `tick`s into the song, 0.125 s long.
    const auto session = [](double rate, int tick) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance synth;
        synth.plugin.id = "com.suppleaudio.test.sine";
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        take.chain = {t.create(synth), t.create(delay)};
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 16 * 960;
        src.sequence.notes = {{tick, 240, 0, 72, 100, 0}};
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    constexpr int kPasses = 5;
    for (const double rate : {1.0, 1.25}) {
        // Device frames: one pass (0.5 s of timeline), and the loop at 0.5 s's
        // first pass, on time, offline (the note 25 frames into it).
        const auto pass = static_cast<std::size_t>(24000 / rate);
        auto pool = makePool();
        const auto offline =
            renderOffline(session(rate, 961), pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTime(
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        double heard = 0;
        for (const float x : onTime) heard = std::max(heard, std::fabs(double(x)));
        REQUIRE(heard > 0.05);
        for (const bool atStart : {false, true})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO("rate " << rate << ", loop at " << (atStart ? 0.0 : 0.5) << ", block "
                                 << block << ", anticipative " << options.anticipative
                                 << ", workers " << options.workers);
                    const auto s = session(rate, atStart ? 1 : 961);
                    const std::size_t start = atStart ? 0 : pass;  // the loop start
                    const std::size_t frames = start + kPasses * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(s));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(atStart ? 0.0 : 0.5, atStart ? 0.5 : 1.0, true);
                    engine.play();
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames), block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    if (!atStart)  // up to the first wrap: the offline render
                        CHECK(maxDiff(L, 0, offline.audio.channels[0], start + pass) == 0.0);
                    // Every pass after the first: on time.
                    for (int k = 1; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, start + static_cast<std::size_t>(k) * pass, onTime,
                                      pass) == 0.0);
                    }
                }
    }
}

// A short item 25 frames after the loop start, inside the look-ahead of its
// take FX (64 frames of latency): an audio take, and a MIDI one through the
// same FX after an instrument. Every pass plays it on time: the chain takes
// its input before the wrap, at unity and under a master playrate. With the
// loop at 0.5 s the first pass reaches the item without a wrap, as offline
// does; with the loop at the song's start every later pass plays as that
// first pass (the song's start itself cannot run ahead).
TEST_CASE("a short take inside its take FX's look-ahead after the loop start plays every pass" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-loop-take-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0));
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));
    // The item at `at` beats (the loop start plus 25 frames), 0.125 s long
    // (item positions are in beats here: 120 BPM, 24000 frames a beat).
    const auto session = [](double rate, bool audio, double at) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        if (audio) {
            take.source = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 48000});
        } else {
            model::FxInstance synth;
            synth.plugin.id = "com.suppleaudio.test.sine";
            take.chain.push_back(t.create(synth));
            model::MidiSource src;
            src.sequence.ppq = 960;
            src.sequence.length = 240;
            src.sequence.notes = {{0, 240, 0, 72, 100, 0}};
            take.source = t.create(src);
        }
        take.chain.push_back(t.create(delay));
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.position = at;
        item.length = 0.25;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    constexpr int kPasses = 5;
    constexpr double kAfter = 25.0 / 24000.0;  // beats
    for (const bool audio : {true, false})
        for (const double rate : {1.0, 1.25}) {
            const auto pass = static_cast<std::size_t>(24000 / rate);  // 0.5 s of timeline
            auto pool = makePool();
            const auto offline =
                renderOffline(session(rate, audio, 1.0 + kAfter), pool,
                              {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir})
                    .value();
            REQUIRE(offline.warnings.empty());
            REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
            const std::vector<float> onTime(
                offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
                offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
            double heard = 0;
            for (const float x : onTime) heard = std::max(heard, std::fabs(double(x)));
            REQUIRE(heard > 0.05);
            for (const bool atStart : {false, true})
                for (const std::uint32_t block : {256u, 37u})
                    for (const EngineOptions& options : kBothExecutors) {
                        INFO(std::string(audio ? "audio" : "midi")
                             << ", rate " << rate << ", loop at " << (atStart ? 0.0 : 0.5)
                             << ", block " << block << ", anticipative " << options.anticipative
                             << ", workers " << options.workers);
                        const auto s = session(rate, audio, (atStart ? 0.0 : 1.0) + kAfter);
                        const std::size_t start = atStart ? 0 : pass;  // the loop start
                        const std::size_t frames = start + kPasses * pass;
                        auto rtPool = makePool();
                        MediaCache media;
                        DummyBackend backend(
                            keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                        Engine engine(rtPool, media, options);
                        REQUIRE(engine
                                    .start(backend, {.sampleRate = 48000,
                                                     .blockSize = block,
                                                     .midiInputs = true})
                                    .ok());
                        REQUIRE(engine.load(s, dir));
                        REQUIRE(engine.awaitPlan());
                        engine.setLoop(atStart ? 0.0 : 0.5, atStart ? 0.5 : 1.0, true);
                        engine.play();
                        runFrames(engine, backend, options, static_cast<std::int64_t>(frames),
                                  block);
                        CHECK(engine.processErrors() == 0);
                        engine.stop();
                        REQUIRE(engine.outputLatency() == 0);
                        const auto& L = backend.capture().channels[0];
                        if (!atStart)  // up to the first wrap: the offline render
                            CHECK(maxDiff(L, 0, offline.audio.channels[0], start + pass) == 0.0);
                        for (int k = 1; k < kPasses; ++k) {
                            INFO("pass " << k);
                            CHECK(maxDiff(L, start + static_cast<std::size_t>(k) * pass, onTime,
                                          pass) == 0.0);
                        }
                    }
        }
    std::filesystem::remove_all(dir);
}

// A compensated take around a loop at [0.5 s, 1 s): every pass after the
// first plays as the first, which plays as offline, at unity and under a
// master playrate. Its item may end before the loop end (before the
// look-ahead wraps) or start at the loop start; notes may lie exactly at
// the loop's start or end (the end's is never heard). A note crossing the
// loop end ends at the wrap: every later pass plays as the second (the
// first has no released note before it).
TEST_CASE("a compensated take around the loop plays every pass as the first" *
          doctest::description("rt")) {
    struct Shape {
        const char* name;
        std::vector<std::pair<int, int>> notes;
        double position, length;
        bool crossing = false;
    };
    const std::vector<Shape> shapes = {
        {"the item ends before the loop end", {{961, 240}}, 0.0, 0.75},
        {"the item starts at the loop start", {{1, 240}}, 0.5, 0.25},
        {"the item ends inside the look-ahead", {{961, 240}}, 0.0, 0.999},
        {"notes at the loop start and end", {{960, 240}, {1920, 240}}, 0.0, 4.0},
        {"a note crossing the loop end", {{1900, 240}}, 0.0, 4.0, true},
    };
    constexpr int kPasses = 5;
    for (const Shape& shape : shapes)
        for (const double rate : {1.0, 1.25}) {
            const auto pass = static_cast<std::size_t>(24000 / rate);
            const auto session =
                compensatedTakeSession(rate, shape.notes, shape.position, shape.length);
            auto pool = makePool();
            const auto offline =
                renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
            REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
            REQUIRE(peakOf(offline.audio.channels[0], pass, pass) > 0.05);
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(std::string(shape.name)
                         << ", rate " << rate << ", block " << block << ", anticipative "
                         << options.anticipative << ", workers " << options.workers);
                    const std::size_t frames = (kPasses + 1) * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(session));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(0.5, 1.0, true);
                    engine.play();
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames), block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    const auto& L = backend.capture().channels[0];
                    // Up to the first wrap: the offline render.
                    CHECK(maxDiff(L, 0, offline.audio.channels[0], 2 * pass) == 0.0);
                    // A note crossing the loop end: the second pass begins with
                    // its release, and the note has ended well before its own
                    // onset again.
                    const std::vector<float> first(
                        shape.crossing
                            ? L.begin() + static_cast<std::ptrdiff_t>(2 * pass)
                            : offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
                        (shape.crossing ? L.begin() : offline.audio.channels[0].begin()) +
                            static_cast<std::ptrdiff_t>((shape.crossing ? 3 : 2) * pass));
                    if (shape.crossing) CHECK(peakOf(first, pass / 4, pass / 2) == 0.0);
                    for (int k = shape.crossing ? 2 : 1; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, (static_cast<std::size_t>(k) + 1) * pass, first, pass) ==
                              0.0);
                    }
                }
        }
}

// The loop at [0.5 s, 1 s), a compensated take's note 25 frames after the
// loop start: playing from inside its look-ahead before the loop end (a seek
// there, before play or, in the callback, while playing), and a plan swap
// there after the take has wrapped early (in the callback, where the swap's
// frame is known), every following pass plays the note on time. A swap that
// also takes the latency away (its take FX's) wraps afresh: the note the
// chain took early ends at the wrap, and the pass plays as the new
// session's. (The seek
// is before the note is due: one later is a locate past it, which plays it
// at once, late.)
TEST_CASE(
    "a compensated take's note after the loop start: a seek or a plan swap in its "
    "look-ahead" *
    doctest::description("rt")) {
    constexpr int kPasses = 4;
    // Timeline frames before the loop end: inside the look-ahead (64 device
    // frames, 80 timeline frames at 1.25), before the note's input is due.
    constexpr std::int64_t kBefore = 60;
    enum class Change { PlayThere, SeekThere, SwapThere, NoLatencyThere };
    for (const double rate : {1.0, 1.25}) {
        const auto pass = static_cast<std::size_t>(24000 / rate);
        const auto session = compensatedTakeSession(rate, {{961, 240}});
        // The plan swap: the same take beside a new, empty track.
        model::EditTxn more(session, 2);
        model::Track extra;
        extra.parent = kMaster;
        const Id extraId = more.create(extra);
        more.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(extraId); });
        const auto edited = more.commit().value();
        model::EditTxn none(session, 2);
        for (const auto& e : session.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.gain")
                none.modify<model::FxInstance>(e.id, [](model::FxInstance& fx) {
                    fx.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                               "params":{"latency":"none"}})")
                                   .value();
                });
        const auto noLatency = none.commit().value();
        auto pool = makePool();
        const auto offline =
            renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTime(
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        REQUIRE(peakOf(onTime, 0, pass) > 0.05);
        const auto uncompensated =
            renderOffline(noLatency, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(uncompensated.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTimeNone(
            uncompensated.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            uncompensated.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        for (const Change change :
             {Change::PlayThere, Change::SeekThere, Change::SwapThere, Change::NoLatencyThere})
            for (const std::uint32_t block : {37u, 32u})
                for (const EngineOptions& options : kBothExecutors) {
                    if (change != Change::PlayThere && options.anticipative) continue;
                    INFO("rate " << rate << ", change " << int(change) << ", block " << block
                                 << ", anticipative " << options.anticipative << ", workers "
                                 << options.workers);
                    const std::size_t frames = 4 * pass + kPasses * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(session));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(0.5, 1.0, true);
                    const double there = double(48000 - kBefore) / 48000.0;
                    std::int64_t played = 0;
                    if (change == Change::PlayThere) engine.seek(there);
                    engine.play();
                    // Device frames from here to the output's next wrap.
                    std::int64_t toWrap = static_cast<std::int64_t>(std::llround(kBefore / rate));
                    if (change == Change::SeekThere) {
                        played = static_cast<std::int64_t>(pass + pass / 2) / block * block;
                        backend.run(played / block);
                        engine.seek(there);
                    } else if (change == Change::SwapThere || change == Change::NoLatencyThere) {
                        // The second wrap's look-ahead: its last block boundary
                        // there, after the take has wrapped early.
                        const auto wrap = static_cast<std::int64_t>(3 * pass);
                        played = (wrap - 1) / block * block;
                        REQUIRE(played > wrap - static_cast<std::int64_t>(64 / rate));
                        backend.run(played / block);
                        REQUIRE(engine.load(change == Change::SwapThere ? edited : noLatency));
                        REQUIRE(engine.awaitPlan());
                        toWrap = wrap - played;
                    }
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames) - played,
                              block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    const auto& L = backend.capture().channels[0];
                    const auto wrapped = static_cast<std::size_t>(played + toWrap);
                    for (int k = 0; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, wrapped + static_cast<std::size_t>(k) * pass,
                                      change == Change::NoLatencyThere ? onTimeNone : onTime,
                                      pass) == 0.0);
                    }
                }
    }
}

// The callback's executor takes a loop edit with no locate (the
// anticipative one restarts its epoch). Edited after a compensated take has
// wrapped early, before the output's wrap, the loop the take wrapped for is
// gone: the take locates (the loop start's note it took ends), and a new
// loop's notes play every pass as offline. Turned off and later set again,
// or moved there at once.
TEST_CASE("a loop edit inside a compensated take's look-ahead is a locate in the callback" *
          doctest::description("rt")) {
    const auto session = compensatedTakeSession(1.0, {{961, 240}, {2881, 240}});
    auto pool = makePool();
    const auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    const auto& O = offline.audio.channels[0];
    REQUIRE(O.size() >= 96000);
    REQUIRE(peakOf(O, 72000, 24000) > 0.05);
    REQUIRE(peakOf(O, 48000, 24000) == 0.0);
    const std::vector<float> onTime(O.begin() + 72000, O.begin() + 96000);
    for (const bool off : {true, false})
        for (const std::uint32_t block : {37u, 32u})
            for (const EngineOptions& options :
                 {kInCallback, EngineOptions{.anticipative = false, .workers = 2}}) {
                INFO("off first " << off << ", block " << block << ", workers " << options.workers);
                constexpr std::int64_t kFrames = 72000 + 4 * 24000;
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, kFrames + 4096));
                Engine engine(rtPool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(session));
                REQUIRE(engine.awaitPlan());
                engine.setLoop(0.5, 1.0, true);
                engine.play();
                // The first wrap's look-ahead, after the take wrapped early.
                const std::int64_t edit = 47999 / block * block;
                REQUIRE(edit > 48000 - 64);
                backend.run(edit / block);
                std::int64_t played = edit;
                if (off) {
                    engine.setLoop(0.5, 1.0, false);
                    backend.run((60000 - played) / block);
                    played += (60000 - played) / block * block;
                }
                engine.setLoop(1.5, 2.0, true);
                backend.run((kFrames - played + block - 1) / block);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                CHECK(maxDiff(L, 0, O, static_cast<std::size_t>(edit)) == 0.0);
                // The note taken early, ended there, is past by a block.
                CHECK(peakOf(L, 48000 + 256, 72000 - 48000 - 256) == 0.0);
                for (int k = 0; k < 4; ++k) {
                    INFO("pass " << k);
                    CHECK(maxDiff(L, 72000 + static_cast<std::size_t>(k) * 24000, onTime, 24000) ==
                          0.0);
                }
            }
}
