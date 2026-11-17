// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Scripts as the player runs them: through the framework's Engine, against
// the compiler's own runner, stopped when they spin, reset to what prepare
// left, and replaced while they play.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "Catalog.h"
#include "Check.h"
#include "Processor.h"
#include "core/Catalog.h"
#include "core/Engine.h"
#include "dsp/AudioFile.h"
#include "runner.h"
#include "supple/script_export.hpp"
#include "supple/script_native.h"
#include "supple/script_program.hpp"

// Example scripts compiled as C++, as a source export builds them.
namespace supple::script::native_synth {
#include "synth.cpp"
}  // namespace supple::script::native_synth
namespace supple::script::native_sampler {
#include "sampler.cpp"
}  // namespace supple::script::native_sampler
namespace supple::script::native_reverb {
#include "reverb.cpp"
}  // namespace supple::script::native_reverb
#ifdef SUPPLE_MARRAM_SCRIPT
namespace supple::script::native_marram {
#include SUPPLE_MARRAM_SCRIPT
}  // namespace supple::script::native_marram
#endif

using namespace supple;
using namespace supple::player;
using namespace supple::suite;

namespace {
int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

using Engine = BasicEngine<ScriptProcessor>;

std::shared_ptr<const ScriptDefinition> compileText(const std::string& source) {
    std::string problems;
    auto d = compileScript("test.cpp", source, &problems);
    CHECK_MSG(d != nullptr, "%s", problems.c_str());
    return d;
}

// Every test after a missing one would fail on its empty script, so this stops.
std::string libraryScript(const char* name) {
    std::string text;
    for (const auto& folder : libraryFolders())
        if (readScript(folder / name / (std::string(name) + ".cpp"), text)) return text;
    const char* named = std::getenv("SUPPLE_SCRIPT_LIBRARY");
    std::fprintf(stderr,
                 "no script %s in the library folders SUPPLE_SCRIPT_LIBRARY names (%s); "
                 "ctest sets it to the player's test/smoke and test/library\n",
                 name, named ? named : "unset");
    std::exit(1);
}

std::string exampleScript(const char* file) {
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/" + file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A mono WAV file of `frames` samples of make(n), in the temporary folder.
std::string writeWav(const char* name, int frames, double (*make)(int)) {
    dsp::AudioData a;
    a.sampleRate = 48000.0;
    a.channel.assign(1, std::vector<float>(static_cast<size_t>(frames)));
    for (int n = 0; n < frames; ++n)
        a.channel[0][static_cast<size_t>(n)] = static_cast<float>(make(n));
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("supple-processor-" + std::to_string(processId()) + "-" + name))
                                 .string();
    CHECK(dsp::saveWavFile(path, a));
    return path;
}

// A noise burst, rendered in blocks of `block` through an activated engine.
std::vector<double> render(Engine& engine, int frames, int block = 64) {
    std::vector<double> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    uint32_t seed = 1;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        left[static_cast<size_t>(i)] = right[static_cast<size_t>(i)] =
            (seed >> 9) / 8388608.0 - 1.0;
    }
    for (int start = 0; start < frames; start += block) {
        const int n = std::min(block, frames - start);
        double* io[] = {left.data() + start, right.data() + start};
        engine.process(io, io, n);
    }
    return left;
}

void setPlain(Engine& engine, uint32_t id, double plain) {
    const ParamTable& t = engine.definition()->params();
    const int i = t.indexOf(id);
    engine.store().set(static_cast<size_t>(i), toHost(t.at(static_cast<size_t>(i)), plain));
}

// Notes for one block: those of `all` (frame, key, on) within it.
struct BlockNotes {
    const std::vector<std::tuple<int, int, bool>>* all = nullptr;
    int start = 0, frames = 0;
    size_t k = 0;
    bool next(EngineEvent& e) {
        for (; k < all->size(); ++k) {
            const auto [frame, key, on] = (*all)[k];
            if (frame < start || frame >= start + frames) continue;
            e = EngineEvent{};
            e.type = EngineEvent::Type::Note;
            e.time = static_cast<uint32_t>(frame - start);
            e.note.key = static_cast<int16_t>(key);
            e.note.type = on ? NoteEvent::Type::On : NoteEvent::Type::Off;
            e.note.velocity = 0.8;
            ++k;
            return true;
        }
        return false;
    }
};

// An instrument playing `notes`, in blocks of 64: its left channel, and
// the keys of the notes it reported over.
std::vector<double> playNotes(const DefinitionPtr& d,
                              const std::vector<std::tuple<int, int, bool>>& notes, int frames,
                              std::vector<int>& ended, const std::string& property = {}) {
    Engine engine(d);
    engine.activate(48000.0, 64, 0, 2);
    if (!property.empty()) engine.setProperty(0, property, true);  // a file, or the routes
    std::vector<double> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    for (int start = 0; start < frames; start += 64) {
        const int n = std::min(64, frames - start);
        double* out[] = {left.data() + start, right.data() + start};
        BlockNotes events{&notes, start, n};
        engine.process<double>(nullptr, out, n, events);
        const NoteOutput& o = engine.noteOutput();
        for (int i = 0; i < o.count(); ++i)
            if (o[i].kind == NoteOutput::Kind::End) ended.push_back(o[i].note.key);
    }
    return left;
}

const char* const kSpinner = R"(#include <supple/script.h>
struct Spin : Effect {
    static constexpr Info info = {.name = "Spin", .id = "com.example.spin"};
    Param go = toggle("Go", false);
    int n = 0;
    void process(Block& io) {
        while (go.on()) ++n;
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= 0.5;
    }
};
)";

}  // namespace

TEST(native_container_bounds_report_runtime_status_without_a_source_line) {
    script::detail::Instance instance;
    script::Array<double, 1> array;
    script::Buffer<double> buffer;
    buffer.resize(1);
    double data[1] = {1.0}, spare = 0.0;
    script::Channel channel{data, 1, &spare};
    script::InputChannel input{data, 1};
    script::Block block;
    block.spare_ = &spare;
    const auto read = [&]() SUPPLE_REALTIME {
        script::detail::CurrentScope scope(&instance);
        const double result = array[-1] + buffer[1] + channel[1] + input[-1];
        (void)block[-1];
        (void)block.sidechain(-1);
        (void)block.output(-1, 0);
        return result;
    };
    CHECK(read() == 0.0);
    CHECK(instance.status.outOfRange.load() == 7);
    CHECK(instance.status.outOfRangeLine.load() == 0);
    CHECK(!instance.status.silent.load());
}

// Review round 33: MIR's interpreter allocates on the audio thread, so only a
// Debug player honours SUPPLE_SCRIPT_INTERPRET; a shipped one compiles to
// machine code whatever the environment says.
TEST(only_a_debug_player_runs_scripts_in_the_interpreter) {
#if defined(_WIN32)
    _putenv_s("SUPPLE_SCRIPT_INTERPRET", "1");
#else
    setenv("SUPPLE_SCRIPT_INTERPRET", "1", 1);
#endif
    const auto d = compileText(libraryScript("tilt"));
#if defined(_WIN32)
    _putenv_s("SUPPLE_SCRIPT_INTERPRET", "");
#else
    unsetenv("SUPPLE_SCRIPT_INTERPRET");
#endif
    const auto* program = dynamic_cast<const scriptc::Program*>(d->code().get());
    CHECK(program != nullptr);
#ifdef NDEBUG
    CHECK(program && !program->interpreted());
#else
    CHECK(program && program->interpreted());
#endif
}

TEST(a_script_becomes_a_definition_with_its_keys_units_and_ids) {
    const auto d = compileText(libraryScript("tilt"));
    CHECK(d->info().id == "com.example.tilt" && d->info().name == "Tilt" &&
          d->info().vendor == "Example");
    CHECK(d->info().stateTag.size() == 4 && d->info().stateTag == stateTagFor("com.example.tilt"));
    CHECK(d->info().offers(2) && d->info().offers(1));
    const ParamTable& t = d->params();
    CHECK(t.count() == 3);
    const ParamDef& pivot = t.at(1);
    CHECK(pivot.unit == supple::Unit::Hertz && pivot.taper == Taper::Log && pivot.hostTapered());
    CHECK(t.at(0).hostTapered());  // linear, but normalized for semantic hosts
    CHECK(t.at(2).smoothing == 20.0);
    CHECK(d->meaning().params[1].key == "pivot");
    CHECK(d->meaning().patchType == "supple.tilt-patch/1.0");
    const auto percent = compileText(R"(#include <supple/script.h>
struct P : Effect {
    static constexpr Info info = {.name = "P", .id = "com.example.p"};
    Param mix = percent("Mix", 25);
    Param mode = choice("Mode", {"Soft Clip", "Hard"}, 1);
    Param slope = choice("Slope", {"12 dB/oct", "24 dB/oct"}, 0).keys({"db12", "db24"});
    void process(Block& io) { io[0][0] *= mix; }
};
)");
    CHECK(percent->params().at(0).max == 1.0 && percent->params().at(0).def == 0.25);
    CHECK(percent->meaning().params[1].choiceKeys ==
          (std::vector<std::string>{"soft_clip", "hard"}));
    CHECK(percent->meaning().params[2].choiceKeys == (std::vector<std::string>{"db12", "db24"}));
}

TEST(the_engine_plays_a_script_as_its_own_runner_does_bit_for_bit) {
    const auto d = compileText(libraryScript("tilt"));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    setPlain(engine, d->params().at(0).id, 4.5);
    setPlain(engine, d->params().at(1).id, 700.0);
    const std::vector<double> played = render(engine, 4096);

    // The runner gets what the engine hands the script: the host value's
    // plain value, which a log taper rounds to its own grid.
    scripttest::JitRunner runner(std::static_pointer_cast<const scriptc::Program>(d->code()),
                                 48000.0, 64, 2);
    const ParamTable& t = d->params();
    runner.setScript(0, toPlain(t.at(0), toHost(t.at(0), 4.5)));
    runner.setScript(1, toPlain(t.at(1), toHost(t.at(1), 700.0)));
    runner.prepare();
    runner.update();
    scripttest::Audio audio(2, 4096);
    uint32_t seed = 1;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u;
        audio.data[0][static_cast<size_t>(i)] = audio.data[1][static_cast<size_t>(i)] =
            (seed >> 9) / 8388608.0 - 1.0;
    }
    runner.process(audio, 64);
    size_t first = 0;
    while (first < played.size() && played[first] == audio.data[0][first]) ++first;
    CHECK_MSG(first == played.size(), "first difference at %zu: %.17g vs %.17g", first,
              first < played.size() ? played[first] : 0.0,
              first < played.size() ? audio.data[0][first] : 0.0);
}

TEST(spectral_whisper_is_identical_across_host_block_sizes) {
    const auto d = compileText(exampleScript("spectral.cpp"));
    Engine small(d), large(d);
    small.activate(48000.0, 1024, 2, 2);
    large.activate(48000.0, 1024, 2, 2);
    setPlain(small, d->params().at(0).id, 2.0);
    setPlain(large, d->params().at(0).id, 2.0);
    CHECK(render(small, 4096, 64) == render(large, 4096, 1024));
}

TEST(a_reset_returns_the_script_to_what_prepare_left) {
    const auto d = compileText(libraryScript("tilt"));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    setPlain(engine, d->params().at(0).id, -3.0);
    const std::vector<double> first = render(engine, 2048);
    engine.reset();
    CHECK(render(engine, 2048) == first);
}

TEST(a_script_that_spins_is_stopped_and_silent_and_says_where) {
    const auto d = compileText(kSpinner);
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(std::fabs(render(engine, 256).back()) > 0.0);
    setPlain(engine, d->params().at(0).id, 1.0);
    const std::vector<double> out = render(engine, 256);
    CHECK(out.back() == 0.0);
    const auto& status = engine.processor().instance().status;
    CHECK(status.budgetExceeded.load() >= 1);
    CHECK(status.budgetLine.load() == 7);
    // Off again, it stays stopped until it is prepared again.
    setPlain(engine, d->params().at(0).id, 0.0);
    CHECK(render(engine, 256).back() == 0.0);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(std::fabs(render(engine, 256).back()) > 0.0);
}

TEST(a_script_whose_prepare_asks_for_too_much_memory_is_stopped_and_not_swapped_in) {
    const std::string fits = R"(#include <supple/script.h>
struct Big : Effect {
    static constexpr Info info = {.name = "Big", .id = "com.example.big"};
    Buffer<double> table;
    DelayLine line;
    void prepare() {
        table.resize(1 << 20);
        line.prepare(1000);
    }
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= 0.5;
    }
};
)";
    // A line of 2^27 samples is the limit on its own; after the table it
    // passes it, and is refused before anything is allocated.
    std::string tooBig = fits;
    tooBig.replace(tooBig.find("line.prepare(1000)"), 18, "line.prepare((1 << 27) - 4)");
    const auto good = compileText(fits), bad = compileText(tooBig);
    Engine stopped(bad);
    stopped.activate(48000.0, 64, 2, 2);
    const auto& status = stopped.processor().instance().status;
    CHECK(status.memoryRefused.load());
    CHECK(status.silent.load());
    CHECK(render(stopped, 256).back() == 0.0);
    stopped.reset();
    CHECK(render(stopped, 256).back() == 0.0);  // until it is prepared again

    Engine running(good);
    running.activate(48000.0, 64, 2, 2);
    CHECK(!running.processor().instance().status.memoryRefused.load());
    CHECK(std::fabs(render(running, 256).back()) > 0.0);
    CHECK(!running.processor().swap(bad->code(), ParamValues(good->params())));
    CHECK(running.processor().swapRefusedMemory());
    CHECK(!running.processor().instance().status.memoryRefused.load());
    CHECK(std::fabs(render(running, 256).back()) > 0.0);
}

TEST(a_new_compile_replaces_the_running_one_with_a_crossfade) {
    // Stateless, so the new code's output is the reference's exactly.
    const std::string level = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    Param level = decibels("Level", -12, 12, 0);
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(level);
    }
};
)";
    std::string louder = level;
    louder.replace(louder.find("*= dbToGain"), 11, "*= 2.0 * dbToGain");
    const auto d = compileText(level);
    const auto next = compileText(louder);
    CHECK(sameShape(*next, *d));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    const std::vector<double> before = render(engine, 512);
    ParamValues values(d->params());
    engine.processor().swap(next->code(), values);
    // During the fade the output lies between the two; past it, the new code's.
    const std::vector<double> fading = render(engine, 64);
    CHECK(std::fabs(fading[32]) > std::fabs(before[32]) &&
          std::fabs(fading[32]) < 2.0 * std::fabs(before[32]));
    render(engine, 512);
    Engine reference(next);
    reference.activate(48000.0, 64, 2, 2);
    CHECK(render(engine, 512) == render(reference, 512));
    engine.processor().collect();
}

TEST(replacement_crossfades_each_output_bus_independently) {
    const std::string source = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level",
                                  .outputs = {"First", "Second"}};
    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) {
            io[0][i] = 1.0;
            io.output(0, 0)[i] += 1.0;
            io.output(1, 0)[i] += 2.0;
        }
    }
};
)";
    std::string louder = source;
    for (size_t at = 0; (at = louder.find("1.0", at)) != std::string::npos; at += 3)
        louder.replace(at, 3, "3.0");
    louder.replace(louder.find("+= 2.0"), 6, "+= 6.0");
    const auto d = compileText(source), next = compileText(louder);
    ScriptProcessor processor(d);
    ParamValues values(d->params());
    processor.prepare({.sampleRate = 1000.0, .maxBlockSize = 16, .channels = 1}, values);
    processor.swap(next->code(), values);
    double main[16]{}, first[16]{}, second[16]{};
    double* mainBus[] = {main};
    double* firstBus[] = {first};
    double* secondBus[] = {second};
    double* const* auxiliary[] = {firstBus, secondBus};
    supple::Block block;
    block.data = mainBus;
    block.channels = 1;
    block.frames = 16;
    block.auxOutputs = 2;
    block.auxData = auxiliary;
    const auto process = [](ScriptProcessor& p, supple::Block& b) SUPPLE_REALTIME { p.process(b); };
    process(processor, block);
    for (int i = 0; i < 16; ++i) {
        const double t = std::min(1.0, i / 10.0);
        CHECK_NEAR(main[i], 1.0 + 2.0 * t, 1e-12);
        CHECK_NEAR(first[i], main[i], 1e-12);
        CHECK_NEAR(second[i], 2.0 * main[i], 1e-12);
    }
    processor.collect();
}

TEST(an_edit_to_what_describes_the_plugin_redefines_it) {
    const std::string base = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    Param level = decibels("Level", -12, 12, 0);
    void process(Block& io) {}
};
)";
    const auto d = compileText(base);
    auto edited = [&](const char* from, const char* to) {
        std::string text = base;
        text.replace(text.find(from), std::string(from).size(), to);
        return compileText(text);
    };
    CHECK(sameShape(*edited("void process(Block& io) {}", "void process(Block& io) { }"), *d));
    CHECK(!sameShape(*edited("12, 0)", "12, 0).withKey(\"gain\")"), *d));
    CHECK(!sameShape(*edited("12, 0)", "12, 0).help(\"How loud.\")"), *d));
    CHECK(!sameShape(*edited("\"com.example.level\"", "\"com.example.level\", .latency = 4"), *d));
    CHECK(!sameShape(*edited("\"com.example.level\"", "\"com.example.level\", .tail = 100"), *d));
}

TEST(a_replacement_reporting_another_latency_is_not_swapped) {
    const std::string source = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    void prepare() { setLatency(4); }
    void process(Block& io) {}
};
)";
    std::string later = source;
    later.replace(later.find("setLatency(4)"), 13, "setLatency(8)");
    const auto d = compileText(source), same = compileText(source), next = compileText(later);
    ScriptProcessor processor(d);
    ParamValues values(d->params());
    processor.prepare({.sampleRate = 48000.0, .maxBlockSize = 16, .channels = 1}, values);
    CHECK(processor.latencySamples() == 4);
    CHECK(!processor.swap(next->code(), values));
    CHECK(processor.swap(same->code(), values));
    processor.collect();
}

TEST(a_replacement_prepares_with_the_running_voice_count_and_offline_mode) {
    const std::string source = R"(#include <supple/script.h>
struct I : Instrument {
    static constexpr Info info = {.name = "I", .id = "com.example.count", .voices = 4};
    double counted = 0;
    void prepare() { counted = voiceCount() + (offline() ? 10 : 0); }
    void startVoice(int v) {}
    void releaseVoice(int v) { voiceFinished(v); }
    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) io[0][i] = counted;
    }
};
)";
    std::string next = source;
    next.replace(next.find("io[0][i] = counted"), 18, "io[0][i] = counted + 0.0");
    const auto d = compileText(source), replacement = compileText(next);
    Engine engine(d);
    engine.setOffline(true);
    engine.activate(48000.0, 64, 0, 2);
    CHECK(render(engine, 64).back() == 14.0);
    ParamValues values(d->params());
    CHECK(engine.processor().swap(replacement->code(), values));
    render(engine, 1024);  // past the fade
    CHECK(render(engine, 64).back() == 14.0);
    engine.processor().collect();
}

TEST(offline_reports_the_host_render_mode_as_it_changes) {
    const auto d = compileText(R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.offline"};
    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) io[0][i] = offline() ? 1.0 : 0.0;
    }
};
)");
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(render(engine, 64).back() == 0.0);
    engine.setOffline(true);
    CHECK(render(engine, 64).back() == 1.0);
}

TEST(replacements_wait_for_retirement_room_without_destroying_audio_state) {
    const auto d = compileText(R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    Shared<double> level;
    void process(Block& io) { level.set(1.0); }
};
)");
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    std::vector<std::weak_ptr<SharedChannel>> channels;
    channels.push_back(engine.processor().editorChannel());
    for (int k = 0; k < 12; ++k) {
        engine.processor().swap(d->code(), ParamValues(d->params()));
        render(engine, 64);
        // The queue holds eight retired generations. Once it is full,
        // another fade must wait; only collect() may destroy audio state.
        if (k < 9) channels.push_back(engine.processor().editorChannel());
        for (const auto& channel : channels) CHECK(!channel.expired());
    }
    engine.processor().collect();
    render(engine, 512);
    engine.processor().collect();
    for (const auto& channel : channels) CHECK(channel.expired());
}

TEST(code_that_indexed_out_of_range_still_crossfades_and_its_faults_stay_its_own) {
    // The old code reads past its block every time; the replacement does not.
    const std::string level = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= 1.0 + io[c][io.frames];
    }
};
)";
    std::string louder = level;
    louder.replace(louder.find("1.0 + io[c][io.frames]"), 22, "2.0");
    const auto d = compileText(level);
    const auto next = compileText(louder);
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    const std::vector<double> before = render(engine, 512);
    const auto& status = engine.processor().instance().status;
    CHECK(status.outOfRange.load() >= 1);
    engine.processor().swap(next->code(), ParamValues(d->params()));
    const std::vector<double> fading = render(engine, 64);
    CHECK(std::fabs(fading[32]) > std::fabs(before[32]) &&
          std::fabs(fading[32]) < 2.0 * std::fabs(before[32]));
    CHECK(status.outOfRange.load() == 0);
    render(engine, 512);
    CHECK(status.outOfRange.load() == 0);
    CHECK(!status.silent.load());
    engine.processor().collect();
}

TEST(a_new_compile_is_prepared_beside_the_one_playing) {
    // swap() runs the new code's prepare() on this thread while another
    // plays the old: they must share nothing a hook writes (TSan says if
    // they do), such as the loop budget.
    const auto d = compileText(R"(#include <supple/script.h>
struct W : Effect {
    static constexpr Info info = {.name = "W", .id = "com.example.work"};
    double sum = 0.0;
    void prepare() {
        for (int k = 0; k < 1000; ++k) sum += 1.0;
    }
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= 0.5;
    }
};
)");
    if (!d) return;
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    std::atomic<bool> done{false};
    std::thread audio([&] {
        while (!done.load()) render(engine, 256);
    });
    const ParamValues values(d->params());
    for (int k = 0; k < 20; ++k) {
        engine.processor().swap(d->code(), values);
        engine.processor().collect();
    }
    done = true;
    audio.join();
    engine.processor().collect();
    CHECK(engine.processor().instance().status.budgetExceeded.load() == 0);
}

TEST(script_reads_are_bounded_and_reject_io_errors) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("supple-script-read-" + std::to_string(processId()) + ".cpp");
    std::string text;
    CHECK(!readScript(std::filesystem::temp_directory_path(), text));
    {
        std::ofstream out(path, std::ios::binary);
        out << std::string(kMaxScriptBytes, 'x');
    }
    CHECK(readScript(path, text));
    CHECK(text.size() == kMaxScriptBytes);
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << std::string(kMaxScriptBytes, 'y');
    }
    CHECK(!readScript(path, text));
    CHECK(text.size() <= kMaxScriptBytes + 1);
    std::filesystem::remove(path);
}

TEST(library_script_names_preserve_native_unicode_paths) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("supple-script-names-" + std::to_string(processId()));
    const auto name = files::fromUtf8("\xe9\x9f\xb3\xe6\xa5\xbd-\xf0\x9f\x8e\xb5");
    auto filename = name;
    filename += ".cpp";
    const auto script = root / name / filename;
    std::filesystem::create_directories(script.parent_path());
    {
        std::ofstream out(script, std::ios::binary);
    }
    const auto found = libraryScripts(root);
    CHECK(found.size() == 1);
    if (found.size() == 1) CHECK(found[0] == script);
    std::filesystem::remove_all(root);
}

TEST(the_library_lists_a_plugin_per_script_that_compiles) {
    const std::vector<DefinitionPtr> plugins = catalog().list();
    CHECK(plugins.size() == 3);
    CHECK(catalog().find("com.example.tilt") != nullptr);
    const DefinitionPtr pluck = catalog().find("com.example.pluck");
    CHECK(pluck != nullptr && pluck->info().instrument() && pluck->info().voices == 4);
    CHECK(catalog().find("com.suppleaudio.script-player") != nullptr);
    CHECK(catalog().list()[0] == plugins[0]);  // unchanged, the same plugin
}

TEST(an_instrument_plays_its_notes_the_same_compiled_either_way_and_ends_each) {
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/synth.cpp");
    std::stringstream text;
    text << in.rdbuf();
    const auto jit = compileText(text.str());
    using Native = supple::script::native_synth::Synth;
    const auto native =
        makeScriptDefinition(std::make_shared<const supple::script::detail::NativeCode<Native>>(
            std::vector<supple::script::detail::NativeCode<Native>::Key>{
                {&Native::attack, "attack"},
                {&Native::release, "release"},
                {&Native::cutoff, "cutoff"},
                {&Native::rate, "rate"},
                {&Native::voices, "voices"},
                {&Native::glide, "glide"},
                {&Native::level, "level"}},
            std::vector<supple::script::detail::NativeCode<Native>::FileKey>{
                {&Native::lfo, "lfo"}}));
    CHECK(jit != nullptr && native != nullptr);
    if (!jit || !native) return;
    CHECK(jit->info().instrument() && jit->info().takesNotes() && jit->info().voices == 8);

    // A chord, a note stolen from it (nine notes on eight voices), released.
    std::vector<std::tuple<int, int, bool>> notes = {{10, 60, true},    {100, 64, true},
                                                     {300, 67, true},   {2000, 60, false},
                                                     {2500, 64, false}, {3000, 67, false}};
    for (int k = 0; k < 6; ++k) {
        notes.push_back({400 + k, 70 + k, true});
        notes.push_back({3100 + k, 70 + k, false});
    }
    std::vector<int> jitEnded, nativeEnded;
    // The LFO and the velocity moving the cutoff, as the Wah factory patch does.
    const std::string wah = "lfo>cutoff 1.5\nvelocity>cutoff 1.0\n";
    const auto a = playNotes(jit, notes, 24000, jitEnded, wah);
    const auto b = playNotes(native, notes, 24000, nativeEnded, wah);
    CHECK(a == b);
    CHECK(jitEnded == nativeEnded);
    std::vector<int> unrouted;
    CHECK(playNotes(jit, notes, 24000, unrouted) != a);  // the routes are heard
    CHECK(jit->presets().size() == 1 && jit->presets()[0].texts.size() == 1 &&
          jit->presets()[0].texts[0].second == wah);
    double peak = 0.0;
    for (int i = 400; i < 2000; ++i) peak = std::max(peak, std::abs(a[static_cast<size_t>(i)]));
    CHECK(peak > 0.01);
    // Every note ended once (the first one stolen), and then silence.
    std::vector<int> sorted = jitEnded;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == (std::vector<int>{60, 64, 67, 70, 71, 72, 73, 74, 75}));
    for (int i = 23000; i < 24000; ++i) CHECK(a[static_cast<size_t>(i)] == 0.0);
}

TEST(a_sampler_plays_its_file_the_same_compiled_either_way_and_ends_with_it) {
    const auto jit = compileText(exampleScript("sampler.cpp"));
    using Native = supple::script::native_sampler::Sampler;
    using Code = supple::script::detail::NativeCode<Native>;
    const auto native = makeScriptDefinition(
        std::make_shared<const Code>(std::vector<Code::Key>{{&Native::root, "root"},
                                                            {&Native::attack, "attack"},
                                                            {&Native::release, "release"},
                                                            {&Native::level, "level"}},
                                     std::vector<Code::FileKey>{{&Native::sample, "sample"}}));
    CHECK(jit != nullptr && native != nullptr);
    if (!jit || !native) return;
    CHECK(jit->properties().count() == 1 && jit->properties().at(0).isFile());
    CHECK(jit->meaning().properties.at(0).key == "sample");
    // A tenth of a second of a fading 440 Hz tone.
    const std::string tone = writeWav("tone.wav", 4800, [](int n) {
        return 0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * n / 48000.0) *
               std::exp(-n / 2000.0);
    });
    // The root key, and an octave up (twice as fast), both held long.
    const std::vector<std::tuple<int, int, bool>> notes = {
        {10, 60, true}, {500, 72, true}, {20000, 60, false}, {20000, 72, false}};
    std::vector<int> jitEnded, nativeEnded;
    const auto a = playNotes(jit, notes, 24000, jitEnded, tone);
    const auto b = playNotes(native, notes, 24000, nativeEnded, tone);
    std::filesystem::remove(tone);
    CHECK(a == b);
    double peak = 0.0;
    for (double x : a) peak = std::max(peak, std::abs(x));
    CHECK(peak > 0.05);
    // Each note ended with the sample, its key still down, and then silence.
    std::sort(jitEnded.begin(), jitEnded.end());
    CHECK(jitEnded == (std::vector<int>{60, 72}));
    for (int i = 5000; i < 24000; ++i) CHECK(a[static_cast<size_t>(i)] == 0.0);
}

TEST(a_files_peak_and_rms_over_any_range_are_what_its_samples_say) {
    // Over three levels of blocks (64, 4096, 262144 frames): each frame of
    // the output is a range's peak (left) and RMS (right), of ranges that
    // start and end anywhere, run past the file's end or hold nothing.
    const auto d = compileText(R"(#include <supple/script.h>
struct Levels : Effect {
    static constexpr Info info = {.name = "Levels", .id = "com.example.levels"};
    AudioFile s = audioFile("Sample");
    int n = 0;
    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) {
            const int from = n * 1171 - 300, to = from + 200000 - n * 791;
            io[0][i] = s.peak(0, from, to);
            io[1][i] = s.rms(0, from, to);
            ++n;
        }
    }
};
)");
    CHECK(d != nullptr);
    if (!d) return;
    constexpr int kFrames = 300000;
    auto shape = [](int n) {
        return (n == 123457 ? 0.95 : 0.0) + 0.5 * std::sin(n * 0.001) * std::sin(n * 0.37);
    };
    const std::string path = writeWav("levels.wav", kFrames, shape);
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    engine.setProperty(0, path, true);
    constexpr int kRanges = 384;
    std::vector<double> left(kRanges, 0.0), right(kRanges, 0.0);
    for (int start = 0; start < kRanges; start += 64) {
        double* io[] = {left.data() + start, right.data() + start};
        engine.process(io, io, 64);
    }
    std::filesystem::remove(path);
    for (int k = 0; k < kRanges; ++k) {
        const int from = k * 1171 - 300, to = from + 200000 - k * 791;
        double most = 0.0, sum = 0.0;
        for (int i = std::max(from, 0); i < std::min(to, kFrames); ++i) {
            const double x = static_cast<float>(shape(i));
            most = std::max(most, std::abs(x));
            sum += x * x;
        }
        CHECK(left[static_cast<size_t>(k)] == most);
        CHECK_NEAR(right[static_cast<size_t>(k)], to > from ? std::sqrt(sum / (to - from)) : 0.0,
                   1e-9);
    }
}

TEST(a_files_default_and_a_patchs_file_are_factory_files) {
    const auto d = compileText(R"(#include <supple/script.h>
struct Kit : Instrument {
    static constexpr Info info = {.name = "Kit", .id = "com.example.kit", .voices = 4};
    AudioFile sample = audioFile("Sample").withDefault("factory:pad.flac");
    void factoryPatches(Patches& p) { p.add("Keys").file(sample, "factory:keys.flac"); }
    void startVoice(int v) {}
    void releaseVoice(int v) {}
    void process(Block& io) {}
};
)");
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(std::string(d->properties().at(0).def) == "factory:pad.flac");
    CHECK(d->presets().size() == 1);
    if (d->presets().empty()) return;
    const auto& texts = d->presets()[0].texts;
    CHECK(std::find(texts.begin(), texts.end(),
                    std::pair<uint32_t, std::string>{d->properties().at(0).id,
                                                     "factory:keys.flac"}) != texts.end());
}

TEST(a_convolution_reverb_plays_its_room_the_same_compiled_either_way) {
    const auto jit = compileText(exampleScript("reverb.cpp"));
    using Native = supple::script::native_reverb::Reverb;
    using Code = supple::script::detail::NativeCode<Native>;
    const auto native = makeScriptDefinition(std::make_shared<const Code>(
        std::vector<Code::Key>{
            {&Native::predelay, "predelay"}, {&Native::tone, "tone"}, {&Native::mix, "mix"}},
        std::vector<Code::FileKey>{{&Native::room, "room"}}));
    CHECK(jit != nullptr && native != nullptr);
    if (!jit || !native) return;
    // A quarter second of fading noise: a small room.
    const std::string room = writeWav("room.wav", 12000, [](int n) {
        const uint32_t h = static_cast<uint32_t>(n) * 2654435761u;
        return ((h >> 8) / 8388608.0 - 1.0) * std::exp(-n / 2500.0);
    });
    auto play = [&](const DefinitionPtr& d, bool withRoom) {
        Engine engine(d);
        engine.activate(48000.0, 64, 2, 2);
        if (withRoom) engine.setProperty(0, room, true);
        setPlain(engine, d->params().at(2).id, 100.0);  // all reverb
        std::vector<double> first = render(engine, 4096);
        // A reset clears the room's tail too.
        engine.reset();
        CHECK(render(engine, 4096) == first);
        return first;
    };
    const auto a = play(jit, true);
    const auto b = play(native, true);
    const auto dry = play(jit, false);
    std::filesystem::remove(room);
    CHECK(a == b);
    double energy = 0.0;
    for (double x : a) energy += x * x;
    CHECK(energy > 1.0);
    for (double x : dry) CHECK(x == 0.0);  // no room: no reverb
}

// Events at given frames, for one block at a time.
struct BlockEvents {
    const std::vector<std::pair<int, EngineEvent>>* all = nullptr;
    int start = 0, frames = 0;
    size_t k = 0;
    bool next(EngineEvent& e) {
        for (; k < all->size(); ++k) {
            const auto& [frame, event] = (*all)[k];
            if (frame < start || frame >= start + frames) continue;
            e = event;
            e.time = static_cast<uint32_t>(frame - start);
            ++k;
            return true;
        }
        return false;
    }
};

TEST(note_expressions_and_per_note_modulation_reach_the_voice_they_address) {
    const auto d = compileText(R"(#include <supple/script.h>
struct E : Instrument {
    static constexpr Info info = {.name = "E", .id = "com.example.expressive", .voices = 4};
    Param level = decibels("Level", -24, 0, 0).polyModulated();
    Array<double, 4> phase;
    void startVoice(int v) { phase[v] = 0.0; }
    void releaseVoice(int v) { voiceFinished(v); }
    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            const double gain = 0.1 * dbToGain(level.forVoice(v)) * (1.0 + voiceBrightness(v));
            for (int i = 0; i < io.frames; ++i) {
                const double x = sin(twoPi * phase[v]) * gain;
                phase[v] = fmod(phase[v] + voiceHz(v, i) / sampleRate(), 1.0);
                io[0][i] += x * (1.0 - voicePan(v));
                io[1][i] += x * voicePan(v);
            }
        }
    }
};
)");
    if (!d) return;
    CHECK(d->info().noteExpressions == 0x7F);
    CHECK(d->params().at(0).is(pflag::kPolyMod));
    // Keys 60 and 72 together; then, for 72 alone: full brightness, pan
    // right, and its level modulated down by a quarter of its range (6 dB).
    auto event = [](EngineEvent::Type type) {
        EngineEvent e;
        e.type = type;
        return e;
    };
    std::vector<std::pair<int, EngineEvent>> events;
    for (int key : {60, 72}) {
        EngineEvent on = event(EngineEvent::Type::Note);
        on.note.key = static_cast<int16_t>(key);
        on.note.velocity = 1.0;
        events.push_back({0, on});
    }
    auto expression = [&](int frame, NoteExpression::Type which, double value) {
        EngineEvent x = event(EngineEvent::Type::Expression);
        x.address.key = 72;
        x.expression = which;
        x.value = value;
        events.push_back({frame, x});
    };
    expression(4800, NoteExpression::Type::Brightness, 1.0);
    expression(9600, NoteExpression::Type::Pan, 1.0);
    EngineEvent mod = event(EngineEvent::Type::NoteModulation);
    mod.index = 0;
    mod.address.key = 72;
    mod.value = -0.25;
    events.push_back({14400, mod});

    Engine engine(d);
    engine.activate(48000.0, 64, 0, 2);
    const int frames = 19200;
    std::vector<double> left(frames), right(frames);
    for (int start = 0; start < frames; start += 64) {
        double* out[] = {left.data() + start, right.data() + start};
        BlockEvents block{&events, start, 64};
        engine.process<double>(nullptr, out, 64, block);
    }
    auto rms = [](const std::vector<double>& x, int from) {
        double sum = 0.0;
        for (int i = from; i < from + 2400; ++i)
            sum += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i)];
        return std::sqrt(sum / 2400.0);
    };
    const double sine = 0.1 * 0.5 / std::sqrt(2.0);  // one centred voice, per channel
    // Both centred; then 72 twice as loud; then 72 all right; then 72 at -6 dB.
    auto near = [](double got, double want) { CHECK_NEAR(got, want, 0.02 * want); };
    near(rms(left, 2000), sine * std::sqrt(2.0));
    near(rms(left, 7000), sine * std::sqrt(5.0));
    near(rms(left, 12000), sine);
    near(rms(right, 12000), sine * std::sqrt(17.0));
    const double down = std::pow(10.0, -6.0 / 20.0);
    near(rms(right, 16800), sine * std::sqrt(1.0 + 16.0 * down * down));
}

TEST(routes_move_a_parameter_by_their_sources_values_in_its_unit) {
    // Each voice a constant at Level (dB): what the routes do to the level
    // is what comes out.
    const auto d = compileText(R"(#include <supple/script.h>
struct R : Instrument {
    static constexpr Info info = {.name = "R", .id = "com.example.routes", .voices = 4};
    Param level = decibels("Level", -48, 6, -12).polyModulated();
    Param voices = integer("Voices", 1, 4, 4);
    Source half = voiceSource("Half");
    void update() { setVoiceLimit(voices.index()); }
    void startVoice(int v) { half.set(v, 0.5); }
    void releaseVoice(int v) { voiceFinished(v); }
    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            const double x = dbToGain(level.forVoice(v));
            for (int i = 0; i < io.frames; ++i) io[0][i] += x;
        }
    }
};
)");
    if (!d) return;
    const Meaning& m = d->meaning();
    CHECK(m.routes == 0);
    if (m.routes != 0) return;
    CHECK(d->properties().at(0).isHidden());  // the automatic editor's to leave out
    CHECK(m.sources.front().key == "half");
    CHECK(m.sources[1].key == "velocity");  // an instrument's own, after the script's
    CHECK(m.params[0].routable == Routable::Voice);
    auto play = [&](const char* routes, int notes, int limit = 4) {
        Engine engine(d);
        engine.setProperty(0, routes);
        setPlain(engine, d->params().at(1).id, limit);
        engine.activate(48000.0, 64, 0, 2);
        std::vector<std::pair<int, EngineEvent>> events;
        for (int k = 0; k < notes; ++k) {
            EngineEvent on;
            on.type = EngineEvent::Type::Note;
            on.note.key = static_cast<int16_t>(60 + k);
            on.note.velocity = 0.5;
            events.push_back({0, on});
        }
        std::vector<double> left(256), right(256);
        for (int start = 0; start < 256; start += 64) {
            double* out[] = {left.data() + start, right.data() + start};
            BlockEvents block{&events, start, 64};
            engine.process<double>(nullptr, out, 64, block);
        }
        return 20.0 * std::log10(left[200]);
    };
    CHECK_NEAR(play("", 1), -12.0, 1e-9);
    CHECK_NEAR(play("half>level 12.0\n", 1), -6.0, 1e-9);                      // 12 dB at 0.5
    CHECK_NEAR(play("half>level 12.0\nvelocity>level 2.0\n", 1), -5.0, 1e-9);  // they add
    CHECK_NEAR(play("half>level 12.0 velocity\n", 1), -9.0, 1e-9);             // through a via
    CHECK_NEAR(play("half>level 40.0\n", 1), 6.0, 1e-9);                       // kept in range
    // Four notes on two voices: the limit steals the rest.
    CHECK_NEAR(play("", 4), -12.0 + 20.0 * std::log10(4.0), 1e-9);
    CHECK_NEAR(play("", 4, 2), -12.0 + 20.0 * std::log10(2.0), 1e-9);
}

TEST(an_effect_reads_its_global_routes_at_each_frame) {
    const auto d = compileText(R"(#include <supple/script.h>
struct G : Effect {
    static constexpr Info info = {.name = "G", .id = "com.example.global-routes"};
    Param mix = percent("Mix", 100).modulated();
    Source ramp = source("Ramp").unipolar();
    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) {
            ramp.set(i / 63.0);
            for (int c = 0; c < io.channels; ++c) io[c][i] = mix.routed(i);
        }
    }
};
)");
    if (!d) return;
    Engine engine(d);
    engine.setProperty(0, "ramp>mix -50.0\n");
    engine.activate(48000.0, 64, 0, 2);
    std::vector<double> left(64, 0.0), right(64, 0.0);
    double* io[] = {left.data(), right.data()};
    engine.process(io, io, 64);
    CHECK_NEAR(left[0], 1.0, 1e-12);
    CHECK_NEAR(left[63], 0.5, 1e-12);
    CHECK_NEAR(left[21], 1.0 - 0.5 * 21.0 / 63.0, 1e-12);
}

TEST(a_script_reports_the_latency_it_sets_in_prepare) {
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/drive.cpp");
    std::stringstream text;
    text << in.rdbuf();
    const auto d = compileText(text.str());
    if (!d) return;
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    // The oversampler's delay, which Info does not give.
    CHECK(engine.latency() > 0 && engine.latency() < 16);
}

TEST(a_script_reports_the_tail_it_sets_as_it_plays) {
    const auto d = compileText(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.tail", .tail = 100};
    Param seconds = continuous("Seconds", 0, 10, 0);
    void update() { setTail(roundToInt(seconds * sampleRate())); }
    void process(Block& io) {
        if (seconds > 9) setTail(-1);
    }
};
)");
    if (!d) return;
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(engine.tail() == 0);
    const uint32_t id = d->params().at(0).id;
    double left[64]{}, right[64]{};
    double* io[] = {left, right};
    setPlain(engine, id, 2.0);
    engine.process<double>(io, io, 64);
    CHECK(engine.tail() == 96000);
    setPlain(engine, id, 9.5);
    engine.process<double>(io, io, 64);
    CHECK(engine.tail() == UINT32_MAX);  // set in process(): forever
}

TEST(a_prepared_tail_survives_reset_and_replacement) {
    const std::string source = R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.tail", .tail = 100};
    void prepare() { setTail(200); }
    void process(Block& io) { if (io[0][0] > 0) setTail(-1); }
};
)";
    const auto d = compileText(source);
    if (!d) return;
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(engine.tail() == 200);
    double left[64]{1.0}, right[64]{};
    double* io[] = {left, right};
    engine.process<double>(io, io, 64);
    CHECK(engine.tail() == UINT32_MAX);
    engine.reset();
    CHECK(engine.processor().tailSamples() == 200);
    left[0] = 0.0;
    engine.process<double>(io, io, 64);
    CHECK(engine.tail() == 200);

    std::string changed = source;
    changed.replace(changed.find("setTail(200)"), 12, "setTail(300)");
    const auto next = compileText(changed);
    if (!next) return;
    ParamValues values(d->params());
    CHECK(engine.processor().swap(next->code(), values));
    left[0] = 0.0;
    engine.process<double>(io, io, 64);
    CHECK(engine.tail() == 300);
    engine.reset();
    CHECK(engine.tail() == 300);
    engine.processor().collect();
}

TEST(a_parameter_that_restarts_sets_the_latency_of_the_next_activation) {
    const auto d = compileText(R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.ahead"};
    Param ahead = choice("Lookahead", {"Off", "On"}, 0).restarts();
    void prepare() { setLatency(ahead.index() * 32); }
    void process(Block& io) {}
};
)");
    if (!d) return;
    const ParamDef& p = d->params().at(0);
    CHECK(p.is(pflag::kRestart));
    CHECK(!p.is(pflag::kAutomatable));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(engine.latency() == 0);
    setPlain(engine, p.id, 1.0);
    engine.deactivate();
    engine.activate(48000.0, 64, 2, 2);
    CHECK(engine.latency() == 32);
}

TEST(embedded_payloads_are_bounded_and_validate_the_schema) {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("supple-player-payload-" + std::to_string(processId()));
    std::filesystem::remove_all(folder);
    const auto module = folder / "Reader.clap/Contents/MacOS/Reader";
#if defined(__APPLE__)
    const auto storage =
        module.parent_path().parent_path() / "Resources" / scriptc::kExportResource;
#else
    const auto storage = module;
#endif
    std::filesystem::create_directories(storage.parent_path());
    auto write = [&](const std::string& payload) {
        std::ofstream out(storage, std::ios::binary | std::ios::trunc);
#if defined(__APPLE__)
        out << payload;
#else
        out << "player prefix" << scriptc::exportTrailer(payload);
#endif
    };
    write(R"({"schema":"supple.script-export/1.0","name":"reader","source":"test source"})");
    const auto read = embeddedScript(module);
    CHECK(read.has_value());
    if (read) {
        CHECK(read->name == "reader");
        CHECK(read->source == "test source");
    }
    write(R"({"schema":"wrong","name":"reader","source":"test source"})");
    CHECK(!embeddedScript(module));
#if defined(__APPLE__)
    {
        std::ofstream out(storage, std::ios::binary | std::ios::trunc);
        out.seekp(static_cast<std::streamoff>(scriptc::kMaxExportPayload));
        out.put('x');
    }
    CHECK(!embeddedScript(module));
    std::filesystem::remove(storage);
    std::filesystem::create_directory(storage);
    CHECK(!embeddedScript(module));
#endif
    CHECK(std::filesystem::is_regular_file(ownModule()));
    std::filesystem::remove_all(folder);
}

#ifdef SUPPLE_MARRAM_SCRIPT
TEST(marram_synced_factory_patch_is_identical_across_transport_block_splits) {
    std::ifstream in(SUPPLE_MARRAM_SCRIPT);
    std::stringstream text;
    text << in.rdbuf();
    const auto jit = compileText(text.str());
    using Native = supple::script::native_marram::Marram;
    using Code = supple::script::detail::NativeCode<Native>;
    const auto native = makeScriptDefinition(std::make_shared<const Code>(
        std::vector<Code::Key>{}, std::vector<Code::FileKey>{{&Native::lfo1, "lfo1"},
                                                             {&Native::lfo2, "lfo2"},
                                                             {&Native::lfo3, "lfo3"},
                                                             {&Native::macro1, "macro1"},
                                                             {&Native::macro2, "macro2"},
                                                             {&Native::macro3, "macro3"},
                                                             {&Native::macro4, "macro4"}}));
    CHECK(jit && native);
    if (!jit || !native) return;
    const auto play = [&](const std::shared_ptr<const ScriptDefinition>& d, int partition,
                          bool changes) {
        Engine engine(d);
        bool found = false;
        for (const auto& patch : d->presets()) {
            if (patch.name != "Wobble Bass") continue;
            found = true;
            for (const auto& [id, value] : patch.values) setPlain(engine, id, value);
            for (const auto& [id, value] : patch.texts)
                engine.setProperty(d->properties().indexOf(id), value);
        }
        CHECK(found);
        engine.activate(48000.0, 512, 0, 2);
        constexpr int segmentFrames = 2048;
        constexpr int segments = 7;
        std::vector<double> left(segmentFrames * segments), right(left.size());
        std::vector<std::pair<int, EngineEvent>> notes;
        for (auto [at, key] : {std::pair{100, 48}, std::pair{900, 55}}) {
            EngineEvent event;
            event.type = EngineEvent::Type::Note;
            event.note.key = static_cast<int16_t>(key);
            event.note.velocity = 0.8;
            notes.emplace_back(at, event);
        }
        if (changes) {
            EngineEvent event;
            event.type = EngineEvent::Type::Value;
            event.index = d->params().indexOf(supple::script::detail::keyId("lfo1.division"));
            event.value = toHost(d->params().at(static_cast<size_t>(event.index)), 6);
            notes.emplace_back(6 * segmentFrames + 333, event);
        }
        double beat = 0.125;
        double bpm = 120.0;
        for (int segment = 0; segment < segments; ++segment) {
            bool playing = true;
            if (changes) {
                if (segment == 1) bpm = 180.0;
                if (segment == 2) beat = 4.2;  // seek
                if (segment == 3) beat = 0.1;  // loop
                if (segment == 4) playing = false;
                if (segment == 5) beat = 0.35;  // restart
            }
            for (int at = 0; at < segmentFrames; at += partition) {
                const int n = std::min(partition, segmentFrames - at);
                const int pos = segment * segmentFrames + at;
                double* out[] = {left.data() + pos, right.data() + pos};
                BlockEvents events{&notes, pos, n};
                supple::Transport transport;
                transport.playing = playing;
                transport.hasTempo = transport.hasBeats = true;
                transport.tempo = bpm;
                transport.beats = beat + (playing ? at * bpm / 60.0 / 48000.0 : 0.0);
                engine.process<double>(nullptr, out, n, events, &transport);
            }
            if (playing) beat += segmentFrames * bpm / 60.0 / 48000.0;
        }
        CHECK(engine.processor().instance().status.outOfRange.load() == 0);
        CHECK(engine.processor().instance().status.budgetExceeded.load() == 0);
        CHECK(std::any_of(left.begin(), left.end(), [](double x) { return std::abs(x) > 0.01; }));
        left.insert(left.end(), right.begin(), right.end());
        CHECK(std::all_of(left.begin(), left.end(), [](double x) { return std::isfinite(x); }));
        return left;
    };
    for (bool changes : {false, true}) {
        const auto a = play(native, 128, changes);
        CHECK(a == play(native, 511, changes));
        const auto b = play(jit, 128, changes);
        CHECK(b == play(jit, 511, changes));
        CHECK(a == b);
    }
}
#endif

SUPPLE_TEST_MAIN
