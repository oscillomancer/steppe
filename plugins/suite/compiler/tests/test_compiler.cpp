// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The compiler end to end: scripts compile, run and compute what C++ would;
// what the subset refuses is refused with the diagnostic that teaches it.
#include <doctest/doctest.h>
#include <clocale>
#include <cstdlib>

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "runner.h"
#include "supple/script_module.h"

namespace supple::script::module_test {
#include "module_surface.h"
}

using namespace supple;
using scripttest::Audio;
using scripttest::JitRunner;

namespace {

scriptc::Result compileOk(const std::string& source, const scriptc::Options& options = {}) {
    auto r = scriptc::compile(source, options);
    INFO(scriptc::toText(r.diagnostics));
    REQUIRE(r.program);
    return r;
}

// The first error's code, or "" when the script compiles. Every error says
// how to fix it.
std::string firstError(const std::string& source) {
    auto r = scriptc::compile(source);
    std::string first;
    for (const auto& m : r.diagnostics) {
        CHECK_MESSAGE(!m.hint.empty(), m.code << ": " << m.message);
        if (m.error && first.empty()) first = m.code;
    }
    return first;
}

// Only the generated hook is an audio entry point; compilation and test
// buffers are prepared outside it. The interpreter is a test and diagnostic
// backend, not real-time safe: it allocates on its first call.
void callRealtime(scriptc::Program::ProcessHook hook, void* instance, void* self, void* block)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
    [[clang::nonblocking]]
#endif
{
    hook(instance, self, block);
}

const char* kGain = R"(#include <supple/script.h>

struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain"};
    Param gain = decibels("Gain", -24, 24, 0);
    double g = 1.0;

    void update() { g = dbToGain(gain); }
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= g;
    }
};
)";

}  // namespace

TEST_CASE("optional MIR listings contain a complete module with binary newlines") {
    const auto compiled = compileOk(kGain, {.listing = true});
    const auto listing = compiled.program->listing();
    CHECK(listing.find("module") != std::string_view::npos);
    CHECK(listing.find("endmod") != std::string_view::npos);
    CHECK(listing.find('\r') == std::string_view::npos);
    CHECK(compileOk(kGain).program->listing().empty());
}

TEST_CASE("factory patches are constant checked declarations with physical values") {
    const auto source = [](const std::string& declarations, const std::string& extra = "") {
        std::string s = kGain;
        s.insert(s.find("    void update()"),
                 extra + "    void factoryPatches(Patches& p) { " + declarations + " }\n");
        return s;
    };
    CHECK(firstError(source("p.add(\"A\"); p.add(\"A\");")) == "script.patch.name");
    CHECK(firstError(source("p.add(\"A\").set(gain, 25);")) == "script.patch.range");
    CHECK(firstError(source("p.add(\"A\").set(gain, 1).set(gain, 2);")) ==
          "script.patch.duplicate");
    CHECK(firstError(source("p.add(\"A\").set(unknown, 0);")) == "script.patch.param");
    CHECK(firstError(source("p.add(\"A\").set(p, 30);", "    Param p = percent(\"P\", 100);\n")) ==
          "script.patch.param");
    CHECK(firstError(source("p.add(\"A\").set(peak, 0);",
                            "    Param peak = decibels(\"Peak\", -60, 6, -60).meter();\n")) ==
          "script.patch.param");
    CHECK(firstError(
              source("p.add(\"A\").set(mode, \"wrong\");",
                     "    Param mode = choice(\"Mode\", {\"Low Pass\", \"HighPass\"}, 0);\n")) ==
          "script.patch.choice");
    CHECK(firstError(source("p.add(\"A\").set(gain, gain.value());")) ==
          "script.constant.required");
    CHECK(firstError(source("for (int i = 0; i < 2; ++i) p.add(\"A\");")) ==
          "script.patch.declaration");
    CHECK(firstError(source("p.add(\"A\").set(gain, 1e308 * 1e308);")) == "script.patch.range");
    auto good = source("p.add(\"Useful\").set(gain, -3).set(mix, 30).set(mode, \"highpass\");",
                       "    Param mix = percent(\"Mix\", 100);\n    Param mode = choice(\"Mode\", "
                       "{\"Low Pass\", \"HighPass\"}, 0);\n");
    good.replace(good.find("decibels(\"Gain\", -24, 24, 0)"),
                 std::string("decibels(\"Gain\", -24, 24, 0)").size(),
                 "decibels(\"Gain\", -24, 24, 0).withKey(\"level\")");
    auto compiled = compileOk(good);
    const auto& patch = compiled.program->factoryPatches().at(0);
    CHECK(patch.name == "Useful");
    REQUIRE(patch.values.size() == 3);
    CHECK(patch.values[0].first == script::detail::keyId("level"));
    CHECK(patch.values[1].second == 30.0);
    CHECK(patch.values[2].second == 1.0);
}

TEST_CASE("a file's default and a factory patch's file are the plugin's factory files") {
    const auto source = [](const std::string& spec, const std::string& declarations) {
        std::string s = kGain;
        s.insert(s.find("    void update()"),
                 "    AudioFile sample = audioFile(\"Sample\")" + spec +
                     ";\n    void factoryPatches(Patches& p) { " + declarations + " }\n");
        return s;
    };
    CHECK(firstError(source(".withDefault(\"/home/me/pad.flac\")", "")) == "script.file.default");
    CHECK(firstError(source(".withDefault(\"factory:\")", "")) == "script.file.default");
    CHECK(firstError(source(".withDefault(\"factory:a/pad.flac\")", "")) == "script.file.default");
    CHECK(firstError(source("", "p.add(\"A\").file(sample, \"/pad.flac\");")) ==
          "script.patch.file");
    CHECK(firstError(source("", "p.add(\"A\").file(gain, \"factory:pad.flac\");")) ==
          "script.patch.file");
    CHECK(firstError(source("",
                            "p.add(\"A\").file(sample, \"factory:a.flac\")"
                            ".file(sample, \"factory:b.flac\");")) == "script.patch.duplicate");
    auto compiled = compileOk(source(".withDefault(\"factory:pad.flac\")",
                                     "p.add(\"Keys\").file(sample, \"factory:keys.flac\");"));
    CHECK(compiled.program->files().at(0).def == "factory:pad.flac");
    const auto& patch = compiled.program->factoryPatches().at(0);
    REQUIRE(patch.files.size() == 1);
    CHECK(patch.files[0].first == script::detail::keyId("sample"));
    CHECK(patch.files[0].second == "factory:keys.flac");
}

TEST_CASE("a choice's options may have keys of their own") {
    const auto source = [](const std::string& slope, const std::string& patch = "") {
        std::string s = kGain;
        s.insert(s.find("    void update()"), "    Param slope = " + slope +
                                                  ";\n    void factoryPatches(Patches& p) { " +
                                                  patch + " }\n");
        return s;
    };
    const std::string slope = "choice(\"Slope\", {\"12 dB/oct\", \"24 dB/oct\"}, 0)";
    auto compiled = compileOk(
        source(slope + ".keys({\"db12\", \"db24\"})", "p.add(\"Steep\").set(slope, \"db24\");"));
    const auto& model = compiled.program->params().back();
    CHECK(model.choiceKeys == std::vector<std::string>{"db12", "db24"});
    CHECK(script::detail::choiceKey(model, 1) == "db24");
    CHECK(compiled.program->factoryPatches().at(0).values.at(0).second == 1.0);
    // Without .keys(), a key is its label in snake_case.
    CHECK(firstError(source(slope, "p.add(\"Steep\").set(slope, \"p24_db_oct\");")) == "");
    CHECK(firstError(source(slope + ".keys({\"db12\", \"db24\"})",
                            "p.add(\"Steep\").set(slope, \"p24_db_oct\");")) ==
          "script.patch.choice");
    CHECK(firstError(source(slope + ".keys({\"db12\"})")) == "script.param.range");
    CHECK(firstError(source(slope + ".keys({\"db12\", \"db12\"})")) == "script.param.range");
    CHECK(firstError(source(slope + ".keys({\"12db\", \"db24\"})")) == "script.param.range");
    CHECK(firstError(source("decibels(\"Slope\", 0, 1, 0).keys({\"a\"})")) == "script.param.range");
    CHECK(firstError(source(slope + ".keys(\"db12\")")) == "script.param.modifier");
}

TEST_CASE("a parameter applies on conditions that name a toggle or a choice above it") {
    const auto source = [](const std::string& declarations) {
        std::string s = kGain;
        s.insert(s.find("    double g = 1.0;"),
                 "    Param on = toggle(\"On\", true);\n"
                 "    Param shape = choice(\"Shape\", {\"Bell\", \"Low Cut\"}, 0);\n"
                 "    Param peak = decibels(\"Peak\", -60, 0, -60).meter();\n"
                 "    Param level = decibels(\"Level\", -60, 0, -12);\n" +
                     declarations + "\n");
        return s;
    };
    auto good = compileOk(
        source("    Param q = continuous(\"Q\", 0.1, 10, 1).activeWhen(on).activeUnless(shape, "
               "{\"low_cut\"});"));
    const auto& conditions = good.program->params().back().activeWhen;
    REQUIRE(conditions.size() == 2);
    CHECK((conditions[0].param == 1 && conditions[0].values == std::vector<int>{1}));
    CHECK((conditions[1].param == 2 && conditions[1].values == std::vector<int>{0}));
    auto refused = [&](const std::string& modifiers) {
        return firstError(source("    Param q = continuous(\"Q\", 0.1, 10, 1)" + modifiers + ";"));
    };
    CHECK(refused(".activeWhen(nothing)") == "script.param.active");
    CHECK(refused(".activeWhen(g)") == "script.param.active");
    CHECK(refused(".activeWhen(gain)") == "script.param.active");  // neither kind
    CHECK(refused(".activeWhen(peak)") == "script.param.active");  // a meter
    CHECK(refused(".activeWhen(q)") == "script.param.active");     // itself
    CHECK(refused(".activeWhen(on, {\"On\"})") == "script.param.active");
    CHECK(refused(".activeWhen(shape)") == "script.param.active");
    CHECK(refused(".activeWhen(shape, {\"Notch\"})") == "script.param.active");
    CHECK(refused(".activeUnless(shape, {\"Bell\", \"Low Cut\"})") == "script.param.active");
    CHECK(refused(".activeWhen(shape, {1})") == "script.param.active");
    CHECK(refused(".activeWhen(on).activeWhen(on).activeWhen(on).activeWhen(on).activeWhen(on)") ==
          "script.param.active");
    // A number's: above or below a value inside its range.
    CHECK(refused(".activeAbove(on, 0)") == "script.param.active");     // not a number
    CHECK(refused(".activeWhen(level)") == "script.param.active");      // not a toggle
    CHECK(refused(".activeAbove(level, 0)") == "script.param.active");  // never above 0
    CHECK(refused(".activeBelow(level, -60)") == "script.param.active");
    CHECK(refused(".activeAbove(level, \"x\")") == "script.param.active");
    CHECK(refused(".activeAbove(level)") == "script.param.active");
    auto ranged =
        compileOk(source("    Param q = continuous(\"Q\", 0.1, 10, 1).activeAbove(level, -30);"));
    CHECK(ranged.program->params().back().activeWhen.at(0).above == -30.0);
    // A condition names a parameter declared above.
    CHECK(firstError(source("    Param q = continuous(\"Q\", 0.1, 10, 1).activeWhen(later);\n"
                            "    Param later = toggle(\"Later\", true);")) ==
          "script.param.active");
}

TEST_CASE("a gain script compiles and applies its gain") {
    for (bool interpret : {false, true}) {
        CAPTURE(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = compileOk(kGain, options);
        const auto& program = *result.program;
        CHECK(program.info().name == "Gain");
        CHECK(program.info().id == "com.example.gain");
        CHECK(program.kind() == scriptc::PluginKind::Effect);
        REQUIRE(program.params().size() == 1);
        const auto& p = program.params()[0];
        CHECK(p.key == "gain");
        CHECK(p.name == "Gain");
        CHECK(p.unit == scriptc::ParamModel::Unit::Decibels);
        CHECK(p.min == -24.0);
        CHECK(p.max == 24.0);

        JitRunner run(result.program);
        run.prepare();
        run.set(0, -6.0);
        run.update();
        Audio audio(2, 100);
        for (int i = 0; i < 100; ++i)
            audio.data[0][static_cast<size_t>(i)] = audio.data[1][static_cast<size_t>(i)] = 0.5;
        run.process(audio, 32);
        const double expected = 0.5 * std::pow(10.0, -6.0 * 0.05);
        CHECK(audio.data[0][0] == expected);
        CHECK(audio.data[1][99] == expected);
    }
}

TEST_CASE("a local or a result may start from braces") {
    for (bool interpret : {false, true}) {
        CAPTURE(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = compileOk(R"(#include <supple/script.h>
struct Pair {
    double a = 0.0, b = 0.0;
};
double zero() { return {}; }
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        Array<double, 3> marks = {-1, 0.5, 3};
        const Pair p = {4, -2};
        for (int i = 0; i < io.frames; ++i)
            io[0][i] = marks[0] + marks[1] + marks[2] + p.a + p.b + zero();
    }
};
)",
                                options);
        JitRunner run(result.program);
        run.prepare();
        run.update();
        Audio audio(2, 4);
        run.process(audio, 4);
        CHECK(audio.data[0][3] == 4.5);
    }
}

TEST_CASE("deep nesting is refused before the compiler's walks could exhaust its stack") {
    auto nested = [](const std::string& open, const std::string& middle, const std::string& close,
                     int levels) {
        std::string text;
        for (int i = 0; i < levels; ++i) text += open;
        text += middle;
        for (int i = 0; i < levels; ++i) text += close;
        return text;
    };
    auto plugin = [](const std::string& body) {
        return "#include <supple/script.h>\nstruct T : Effect {\n"
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               body + "\n};\n";
    };
    auto chain = [](const std::string& link, int links) {
        std::string text = "1";
        for (int i = 0; i < links; ++i) text += link;
        return text;
    };
    const std::string process = " void process(Block& io) {}";
    CHECK(firstError(plugin("double x = " + nested("(", "1", ")", 100) + ";" + process)) == "");
    CHECK(firstError(plugin("double x = " + nested("(", "1", ")", 30000) + ";" + process)) ==
          "script.nesting");
    CHECK(firstError(plugin("double x = " + chain(" + 1", 30000) + ";" + process)) ==
          "script.nesting");
    CHECK(firstError(plugin("void process(Block& io) " + nested("{", "", "}", 30000))) ==
          "script.nesting");
}

TEST_CASE("the first scripts' mistakes are refused with a hint") {
    CHECK(firstError("struct X : Effect {};") == "script.include.missing");
    const std::string head = "#include <supple/script.h>\n";
    const std::string info =
        "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n";
    auto plugin = [&](const std::string& body) {
        return head + "struct T : Effect {\n" + info + body + "\n};\n";
    };
    CHECK(firstError(plugin("void process(Block& io) { float buf[16]; }")) ==
          "script.unsupported.c_array");
    CHECK(firstError(plugin("void process(Block& io) { double* p = nullptr; }")) ==
          "script.unsupported.pointer");
    CHECK(firstError(plugin(
              "void process(Block& io) { int n = io.frames; double x = 1.5; int k = x; }")) ==
          "script.type.float_to_int");
    CHECK(firstError(plugin("void process(Block& io) { int a = io.frames / io.channels; }")) ==
          "script.ub.division");
    CHECK(firstError(plugin("double phase; void process(Block& io) {}")) ==
          "script.member.initial");
    CHECK(firstError(plugin("void process(Block& io) { double x; }")) ==
          "script.local.uninitialized");
    CHECK(firstError(plugin("Buffer<float> b; void process(Block& io) { b.resize(10); }")) ==
          "script.rt.allocation");
    CHECK(firstError(plugin("DelayLine d; void process(Block& io) { d.prepare(100); }")) ==
          "script.rt.allocation");
    CHECK(firstError(plugin("void process(Block& io) { double y = sin(1); }")) == "");
    CHECK(firstError(plugin("Array<Oversampler, 2> o;\n"
                            "void prepare() { o[0].prepare(1); o[1].prepare(4, "
                            "Oversampler::Phase::Linear); }\n"
                            "void process(Block& io) {}")) == "");
    CHECK(firstError(plugin("void update() { setTail(4800); }\n"
                            "void process(Block& io) { setTail(-1); }")) == "");
    CHECK(firstError(plugin("void process(Block& io) {}\n"
                            "void draw(Canvas& g) { setTail(0); }")) == "script.editor.call");
    CHECK(firstError(plugin("void process(Block& io) { double y = min(1, 0.5); }")) ==
          "script.call.ambiguous");
    CHECK(firstError(plugin("void process(Block& io) { double y = M_PI; }")) ==
          "script.name.unknown");
    CHECK(firstError(plugin("void processBlock(Block& io) {}")) == "script.plugin.hook_name");
    CHECK(firstError(
              plugin("Random r; void process(Block& io) { double y = r.next() - r.next(); }")) ==
          "script.ub.order");
    for (const char* assignment : {"a[++i] = i;", "a[++i] += i;", "a[i] = i++;"})
        CHECK(firstError(
                  plugin(std::string("void process(Block& io) { Array<int, 4> a; int i = 0; ") +
                         assignment + " }")) == "script.ub.order");
    CHECK(firstError(plugin("void process(Block& io) { Array<int, 4> a; int i = 0; a[i++] = 1; "
                            "a[i] = a[i] + 1; }"))
              .empty());
    CHECK(firstError(plugin(
              "Random r; Random s; void process(Block& io) { double y = r.next() - s.next(); }")) ==
          "");
    CHECK(firstError(plugin(
              "int f(int n) { return n < 1 ? 0 : f(n - 1); } void process(Block& io) { f(3); }")) ==
          "script.call.recursion");
    CHECK(firstError(plugin("int f(int n) { if (n > 0) return 1; } void process(Block& io) {}")) ==
          "script.return.missing_path");
    CHECK(firstError(plugin("Param p = continuous(\"P\", 0, 1, 0, Unit::Furlongs);\n"
                            "void process(Block& io) {}")) == "script.param.unit");
    CHECK(firstError(plugin("Param p = decibels(\"P\", 0, 1, 0).curved(Unit::Hertz);\n"
                            "void process(Block& io) {}")) == "script.param.modifier");
    // Names as C++ looks them up: a member hides a factory of its name, a
    // local's name in its own initializer is that local, and the factories'
    // names are the runtime's.
    CHECK(firstError(plugin("Param ratio = ratio(\"Ratio\", 1, 20, 4);\n"
                            "void process(Block& io) {}")) == "script.name.hidden");
    CHECK(firstError(plugin("Param amount = ratio(\"Ratio\", 1, 20, 4).withKey(\"ratio\");\n"
                            "void process(Block& io) {}")) == "");
    CHECK(firstError(plugin("Param mix = percent(\"Mix\", 50);\n"
                            "void process(Block& io) { double mix = mix * 2.0; }")) ==
          "script.name.self");
    CHECK(firstError(plugin("void process(Block& io) { double sin = sin(0.5); }")) ==
          "script.name.self");
    CHECK(firstError(head + "double percent(double x) { return x * 0.01; }\n" +
                     "struct T : Effect {\n" + info + "void process(Block& io) {}\n};\n") ==
          "script.name.builtin");
    // Files: members of the plugin, declared with their kind, never copied
    // or assigned.
    CHECK(firstError(plugin("AudioFile s = audioFile(\"Sample\").help(\"A sound.\");\n"
                            "Convolver r = impulseResponse(\"Room\");\n"
                            "void process(Block& io) { double x = s.read(0, 1.5) + "
                            "r.process(0, 0.5); }")) == "");
    CHECK(firstError(plugin("AudioFile s;\nvoid process(Block& io) {}")) ==
          "script.file.declaration");
    CHECK(firstError(plugin("Convolver r = audioFile(\"Room\");\nvoid process(Block& io) {}")) ==
          "script.file.kind");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\").smoothed(5);\n"
                            "void process(Block& io) {}")) == "script.file.modifier");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\n"
                            "void process(Block& io) { auto t = s; }")) == "script.local.file");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\nAudioFile t = audioFile(\"T\");\n"
                            "void process(Block& io) { s = t; }")) == "script.file.read_only");
    CHECK(firstError(plugin("Array<AudioFile, 2> kit;\nvoid process(Block& io) {}")) ==
          "script.file.array");
    CHECK(firstError(plugin("AudioFile a = audioFile(\"S\");\n"
                            "AudioFile b = audioFile(\"T\").withKey(\"a\");\n"
                            "void process(Block& io) {}")) == "script.file.key");
    CHECK(firstError(head + "struct V { AudioFile s = audioFile(\"S\"); };\nstruct T : Effect {\n" +
                     info + "void process(Block& io) {}\n};\n") == "script.file.location");
    // Identities every plugin format and the framework accept.
    auto withInfo = [&](const std::string& fields) {
        return head + "struct T : Effect {\nstatic constexpr Info info = {.name = \"T\", " +
               fields + "};\nvoid process(Block& io) {}\n};\n";
    };
    CHECK(firstError(withInfo(".id = \"com.example.tilt_eq\"")) == "script.plugin.id");
    CHECK(firstError(withInfo(".id = \"com..tilt\"")) == "script.plugin.id");
    CHECK(firstError(withInfo(".id = \"com.example.tilt-eq\"")) == "");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .version = \"1.2\"")) ==
          "script.plugin.info");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .version = \"1.2.10\"")) == "");
    // CLAP features after the main category, which the kind gives.
    CHECK(firstError(withInfo(".id = \"com.example.t\", .features = {\"reverb\", \"stereo\"}")) ==
          "");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .features = {\"Reverb\"}")) ==
          "script.plugin.info");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .features = {\"audio-effect\"}")) ==
          "script.plugin.info");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .features = \"reverb\"")) ==
          "script.plugin.info");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .tail = 1, .features = {\"reverb\"}")) ==
          "script.type.designator_order");
    // Instruments: notes reach voices through the voice hooks, which only an
    // instrument has; scripts send no notes, so no note effects.
    auto instrument = [&](const std::string& body) {
        return head + "struct T : Instrument {\n" + info + body + "\n};\n";
    };
    CHECK(firstError(instrument("void process(Block& io) {}")) == "script.plugin.voices");
    CHECK(firstError(instrument("void startVoice(int v) {}\nvoid process(Block& io) {}")) ==
          "script.plugin.voices");
    CHECK(firstError(instrument("void startVoice(double v) {}\nvoid releaseVoice(int v) {}\n"
                                "void process(Block& io) {}")) == "script.plugin.hook");
    CHECK(firstError(plugin("void releaseVoice(int v) {}\nvoid process(Block& io) {}")) ==
          "script.plugin.hook");
    CHECK(firstError(plugin("void process(Block& io) { int k = voiceKey(0); }")) ==
          "script.plugin.instrument");
    CHECK(firstError(head + "struct T : NoteEffect {\n" + info + "};\n") == "script.plugin.kind");
    // Per note: expressions and modulation, an instrument's only.
    CHECK(firstError(
              instrument("Param cutoff = frequency(\"Cutoff\", 20, 20000, 1000).polyModulated();\n"
                         "void startVoice(int v) {}\nvoid releaseVoice(int v) {}\n"
                         "void process(Block& io) { double f = cutoff.forVoice(0) * "
                         "(1.0 + voiceBrightness(0)) * voiceVolume(0); }")) == "");
    CHECK(
        firstError(plugin("Param cutoff = frequency(\"Cutoff\", 20, 20000, 1000).polyModulated();\n"
                          "void process(Block& io) {}")) == "script.param.poly");
    CHECK(firstError(plugin("void process(Block& io) { double p = voicePressure(0); }")) ==
          "script.plugin.instrument");
}

TEST_CASE("sources are members of the plugin, and factory patches route them by key") {
    const std::string head = "#include <supple/script.h>\n";
    const std::string hooks =
        "void startVoice(int v) {}\nvoid releaseVoice(int v) {}\n"
        "void process(Block& io) {}\n";
    auto instrument = [&](const std::string& body) {
        return head + "struct T : Instrument {\n" +
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               "Param cutoff = frequency(\"Cutoff\", 20, 20000, 1000).polyModulated();\n" + body +
               "\n" + hooks + "};\n";
    };
    auto effect = [&](const std::string& body) {
        return head + "struct T : Effect {\n" +
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               "Param mix = percent(\"Mix\", 50).modulated();\n" + body +
               "\nvoid process(Block& io) {}\n};\n";
    };
    CHECK(firstError(
              instrument("Source lfo = voiceSource(\"LFO\");\n"
                         "void factoryPatches(Patches& p) { p.add(\"A\").route(\"lfo>cutoff\", "
                         "2).route(\"velocity>cutoff\", 1, \"modwheel\"); }")) == "");
    CHECK(firstError(effect("Source macro = source(\"Macro\").unipolar();")) == "");
    CHECK(firstError(effect("Source lfo = voiceSource(\"LFO\");")) == "script.source.voice");
    CHECK(firstError(instrument("Source velocity = voiceSource(\"Velocity\");")) ==
          "script.source.key");
    CHECK(firstError(instrument("Source a = source(\"A\").withKey(\"x\");\n"
                                "Source b = source(\"B\").withKey(\"x\");")) ==
          "script.source.key");
    CHECK(firstError(instrument("Array<Source, 2> lfos;")) == "script.source.array");
    CHECK(firstError(instrument("Source lfo;")) == "script.source.declaration");
    CHECK(firstError(instrument("Source lfo = voiceSource(\"LFO\").smoothed(2);")) ==
          "script.source.modifier");
    CHECK(firstError(instrument("Source lfo = voiceSource(\"LFO\");\n"
                                "void f() { auto copy = lfo; }")) == "script.local.source");
    CHECK(firstError(instrument("Param fine = cents(\"Fine\", -100, 100, 0).routeRange(5, 1);")) ==
          "script.param.modifier");
    CHECK(firstError(
              instrument("void factoryPatches(Patches& p) { p.add(\"A\").route(\"nothing>cutoff\", "
                         "2); }")) == "script.patch.route");
    CHECK(firstError(effect("Source m = source(\"M\");\n"
                            "void factoryPatches(Patches& p) { p.add(\"A\").route(\"m>gain\", 2); "
                            "}")) == "script.patch.route");
    // An effect has no sources of its own.
    CHECK(
        firstError(effect("void factoryPatches(Patches& p) { p.add(\"A\").route(\"modwheel>mix\", "
                          "2); }")) == "script.patch.route");
}

TEST_CASE("editor code reads parameters and Shared members, and keeps the rest apart") {
    const std::string head = "#include <supple/script.h>\n";
    auto plugin = [&](const std::string& body) {
        return head + "struct T : Effect {\n" +
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               "Param gain = decibels(\"Gain\", -24, 24, 0);\n" +
               "Param out = decibels(\"Out\", -60, 0, 0).meter();\n" + body + "\n};\n";
    };
    const std::string process = "void process(Block& io) {}\n";
    // What editor code may do: read parameters and Shared members, keep
    // members of its own, format text, place controls.
    CHECK(firstError(
              plugin("Shared<double> level;\nSharedArray<float, 8> scope;\n" + process +
                     "void draw(Canvas& g) {\n"
                     "  const char* text = format(\"%.1f dB\", level.get() + gain + scope[1]);\n"
                     "  g.fillText(text, 10, 20);\n"
                     "  g.fillStyle(Theme::accent);\n"
                     "  g.textAlign(TextAlign::Center);\n"
                     "  g.fillText(gain.text(), 10, 40);\n"
                     "}")) == "");
    CHECK(firstError(plugin("double zoom = 1.0;\nAudioFile s = audioFile(\"S\");\n" + process +
                            "void editor(Editor& e) {\n"
                            "  e.size(300, 200);\n"
                            "  e.knob(gain, 10, 10, 80, 90);\n"
                            "  e.readout(out, 100, 10, 120, 30);\n"
                            "  e.file(s, 10, 110, 200, 40);\n"
                            "  zoom = 2.0;\n"
                            "}\n"
                            "void draw(Canvas& g) { g.scale(zoom, zoom); }")) == "");
    // A member both sides use, and one only prepare() fills.
    CHECK(firstError(plugin("double peak = 0.0;\nvoid process(Block& io) { peak = 1.0; }\n"
                            "void draw(Canvas& g) { g.fillRect(0, 0, peak, 1); }")) ==
          "script.editor.member");
    CHECK(firstError(plugin("double table = 0.0;\nvoid prepare() { table = 2.0; }\n" + process +
                            "void draw(Canvas& g) { g.fillRect(0, 0, table, 1); }")) ==
          "script.editor.member");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\n" + process +
                            "void draw(Canvas& g) { double n = s.frames(); }")) ==
          "script.editor.file");
    // What editor code may not change.
    CHECK(firstError(plugin("Shared<double> level;\n" + process +
                            "void draw(Canvas& g) { level.set(0.0); }")) ==
          "script.editor.shared_write");
    CHECK(firstError(plugin("SharedArray<float, 4> s;\n" + process +
                            "void draw(Canvas& g) { s[0] = 1; }")) == "script.editor.shared_write");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { out.set(1.0); }")) ==
          "script.editor.param_write");
    // Each side's built-ins.
    CHECK(firstError(plugin("void process(Block& io) { const char* t = format(\"%d\", 1); }")) ==
          "script.editor.call");
    CHECK(firstError(plugin("DelayLine d;\n" + process +
                            "void draw(Canvas& g) { d.prepare(10); }")) == "script.editor.call");
    // Text is made where it is drawn: no member holds it, in an Array either.
    CHECK(firstError(plugin("Array<const char*, 2> names = {\"A\", \"B\"};\n" + process)) ==
          "script.text.member");
    CHECK(firstError(plugin("struct Row { Array<Array<const char*, 2>, 2> words; };\nRow row;\n" +
                            process)) == "script.text.member");
    // format()'s pattern is checked as it is written.
    CHECK(firstError(plugin(process +
                            "void draw(Canvas& g) { g.fillText(format(\"%s\", 1.0), 0, 0); }")) ==
          "script.format.conversion");
    CHECK(
        firstError(plugin(
            process + "void draw(Canvas& g) { g.fillText(format(\"%d and %d\", 1.0), 0, 0); }")) ==
        "script.format.count");
    // trace() is the audio code's, under a name written out.
    CHECK(firstError(plugin("void process(Block& io) { trace(\"x\", 1.0); trace(\"y\", 2.0, 3); "
                            "}")) == "");
    CHECK(firstError(plugin("void process(Block& io) { const char* name = \"x\"; "
                            "trace(name, 1.0); }")) == "script.trace.name");
    CHECK(firstError(plugin("void process(Block& io) { trace(\"" + std::string(48, 'a') +
                            "\", 1.0); }")) == "script.trace.name");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { trace(\"x\", 1.0); }")) ==
          "script.editor.call");
    // As many names as a trace keeps (a name given twice is one), not more.
    std::string names;
    for (int k = 0; k < 16; ++k) names += "trace(\"n" + std::to_string(k) + "\", 1.0); ";
    CHECK(firstError(plugin("void process(Block& io) { " + names + "trace(\"n0\", 2.0); }")) == "");
    CHECK(firstError(plugin("void process(Block& io) { " + names + "trace(\"n16\", 2.0); }")) ==
          "script.trace.names");
    // Shared state belongs to the plugin struct, starts at zero, holds numbers.
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { Shared<double> x; }")) ==
          "script.shared.where");
    CHECK(firstError(plugin("Shared<double> level = 1;\n" + process)) == "script.shared.initial");
    CHECK(firstError(plugin("Shared<Biquad> f;\n" + process)) == "script.shared.type");
    CHECK(firstError(head + "struct V { Shared<double> x; };\nstruct T : Effect {\n" +
                     "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
                     process + "};\n") == "script.shared.where");
    // The hooks, as the runtime calls them.
    CHECK(firstError(plugin(process + "void draw() {}")) == "script.plugin.hook");
    CHECK(firstError(plugin(process + "void paint(Canvas& g) {}")) == "script.plugin.hook_name");
    // pointer() edits parameters, as a user would; nothing else in the
    // editor does, and it needs a draw() to act on.
    const std::string draw = "void draw(Canvas& g) {}\n";
    CHECK(firstError(plugin("bool held = false;\n" + process + draw +
                            "void pointer(const Pointer& p) {\n"
                            "  if (p.action() == PointerAction::Drag && held)\n"
                            "    move(xToHz(p.x(), 20, 20000, 0, 100));\n"
                            "  if (p.doubleClick()) gain.reset();\n"
                            "  held = p.within(1, 2, 3) || p.over(0, 0, 4, 4);\n"
                            "}\n"
                            "void move(double hz) { gain.editNormalized(hz / 20000); }")) == "");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { gain.edit(0); }")) ==
          "script.editor.edit");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { nudge(); }\n"
                                      "void nudge() { gain.reset(); }")) == "script.editor.edit");
    CHECK(firstError(plugin("void process(Block& io) { gain.edit(0); }\n" + draw)) ==
          "script.editor.call");
    // An instrument's pointer() plays it, as its keyboard does; nothing else does.
    auto synth = [&](const std::string& body) {
        return head + "struct T : Instrument {\n" +
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               "void startVoice(int v) {}\nvoid releaseVoice(int v) {}\n" + process + draw + body +
               "\n};\n";
    };
    CHECK(firstError(synth("void pointer(const Pointer& p) {\n"
                           "  if (p.action() == PointerAction::Press) playNote(36, 0.8, true);\n"
                           "}")) == "");
    CHECK(firstError(synth("void editor(Editor& e) { playNote(36, 1, true); }")) ==
          "script.editor.edit");
    CHECK(firstError(head + "struct T : Instrument {\n" +
                     "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
                     "void startVoice(int v) { playNote(36, 1, true); }\n"
                     "void releaseVoice(int v) {}\n" +
                     process + "};\n") == "script.editor.call");
    CHECK(firstError(plugin(process + draw +
                            "void pointer(const Pointer& p) { playNote(36, 1, true); }")) ==
          "script.plugin.instrument");
    CHECK(firstError(plugin(process + "void pointer(const Pointer& p) {}")) ==
          "script.editor.pointer");
    CHECK(firstError(plugin(process + draw + "int pointer(const Pointer& p) { return 0; }")) ==
          "script.plugin.hook");
    CHECK(firstError(plugin(process + draw + "void onMouseDown(const Pointer& p) {}")) ==
          "script.plugin.hook_name");
}

TEST_CASE("continuous() takes a unit, for the units without a factory") {
    const std::string source = R"(#include <supple/script.h>

struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    Param tempo = continuous("Tempo", 40, 240, 120, Unit::Bpm);
    Param plain = continuous("Amount", 0, 1, 0.5);
    Param across = continuous("Width", 1, 60, 18, Unit::Meters);
    void process(Block& io) {}
};
)";
    auto r = compileOk(source);
    REQUIRE(r.program->params().size() == 3);
    CHECK(r.program->params()[0].unit == scriptc::ParamModel::Unit::Bpm);
    CHECK(r.program->params()[1].unit == scriptc::ParamModel::Unit::None);
    CHECK(r.program->params()[2].unit == scriptc::ParamModel::Unit::Meters);
}

TEST_CASE("an instrument takes its notes through voice hooks") {
    const std::string source = R"(#include <supple/script.h>

struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 4};
    Param glide = milliseconds("Glide", 0, 500, 0);

    struct Voice {
        Oscillator osc;
        Adsr env;
    };
    Array<Voice, 4> voice;

    void update() {
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
    }
    void startVoice(int v, bool retrigger) {
        if (retrigger) voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }
    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            for (int i = 0; i < io.frames; ++i) {
                voice[v].osc.setFrequency(voiceHz(v, i));
                const double tone = voice[v].osc.next();
                const double x = tone * voice[v].env.next() * voiceVelocity(v);
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!voice[v].env.active()) voiceFinished(v);
        }
    }
};
)";
    auto r = compileOk(source);
    CHECK(r.program->kind() == scriptc::PluginKind::Instrument);
    CHECK(r.program->info().voices == 4);
    CHECK(r.program->startVoiceRetrigger() != nullptr);
    CHECK(r.program->startVoice() == nullptr);
    CHECK(r.program->releaseVoice() != nullptr);
    CHECK(r.program->stopVoice() != nullptr);
}

TEST_CASE("every whole script in the guides compiles") {
    // A guide's ```cpp blocks that are scripts (they include the runtime):
    // what an agent copies first must be right.
    int scripts = 0;
    for (const char* guide : {"dsp-language.md", "dsp-workflow.md", "modules.md"}) {
        std::ifstream in(std::string(SUPPLE_SKILL_GUIDES) + "/" + guide);
        REQUIRE_MESSAGE(in, guide);
        std::stringstream all;
        all << in.rdbuf();
        const std::string text = all.str();
        for (size_t at = text.find("```cpp\n"); at != std::string::npos;
             at = text.find("```cpp\n", at + 1)) {
            const size_t begin = at + 7, end = text.find("```", begin);
            REQUIRE(end != std::string::npos);
            const std::string block = text.substr(begin, end - begin);
            if (block.find("#include <supple/script.h>") == std::string::npos) continue;
            ++scripts;
            auto r = scriptc::compile(block);
            INFO(guide << ":\n" << block);
            CHECK_MESSAGE(r.program != nullptr, scriptc::toText(r.diagnostics));
        }
    }
    CHECK(scripts >= 3);
}

TEST_CASE("invalid constant division reports an error without executing it") {
    for (const char* op : {"/", "%"}) {
        const std::string source =
            "#include <supple/script.h>\n"
            "struct T : Effect {\n"
            "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
            "void process(Block& io) { int x = (-2147483647 - 1) " +
            std::string(op) + " -1; }\n};\n";
        CHECK(firstError(source) == "script.ub.division");
    }
}

TEST_CASE("literals must have the same spelling and type as C++") {
    auto script = [](const std::string& expression) {
        return "#include <supple/script.h>\n"
               "struct T : Effect {\n"
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
               "void process(Block& io) { double x = " +
               expression + "; }\n};\n";
    };
    for (const char* literal : {"1e", "1e+", "1.2.3", "1'e3", "1.0'f", "0x'1"}) {
        INFO(literal);
        CHECK(firstError(script(literal)) == "script.lex.number");
    }
    for (const char* literal : {"0x1u", "0x1L", "0x1ULL"}) {
        INFO(literal);
        CHECK(firstError(script(literal)) == "script.lex.int_suffix");
    }
    for (const char* literal : {"1'000", "1.25e+2", "1.0e1'0", "0xA'B", "0b10'01"}) {
        INFO(literal);
        CHECK(firstError(script(literal)).empty());
    }
    CHECK(firstError(script("1; // comment \\\n io[0][0] = 2")) == "script.lex.continuation");
    CHECK(firstError(script("1; const char* text = \"\\012\"")) == "script.lex.escape");
}

TEST_CASE("floating literals retain direct rounding and underflow behavior") {
    const auto result = compileOk(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        io[0][0] = 1e-400;
        io[0][1] = 1e-50f;
        io[0][2] = 1e-45f;
        io[0][3] = 5e-324;
        io[0][4] = 1.0000000596046447753906250000000000000000000000000000000001f;
    }
};)");
    JitRunner runner(result.program);
    Audio audio(1, 5);
    runner.process(audio);
    CHECK(audio.data[0] ==
          std::vector<double>{0, 0, static_cast<double>(std::numeric_limits<float>::denorm_min()),
                              std::numeric_limits<double>::denorm_min(),
                              static_cast<double>(std::nextafter(1.0f, 2.0f))});
    for (const char* literal : {"1e400", "1e50f"})
        CHECK(firstError("#include <supple/script.h>\nstruct T : Effect { double x = " +
                         std::string(literal) + "; void process(Block& io) {} };") ==
              "script.lex.range");
}

TEST_CASE("numeric literals ignore the host decimal separator") {
    const std::string original = std::setlocale(LC_NUMERIC, nullptr);
    struct Restore {
        const std::string& locale;
        ~Restore() { std::setlocale(LC_NUMERIC, locale.c_str()); }
    } restore{original};
    bool available = false;
    for (const char* locale :
         {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de-DE", "German_Germany.1252"})
        if (std::setlocale(LC_NUMERIC, locale)) {
            available = true;
            break;
        }
    if (!available) {
        // CI installs one and sets this, so the check cannot pass there unexercised.
        CHECK_MESSAGE(!std::getenv("SUPPLE_REQUIRE_COMMA_LOCALE"),
                      "no comma-decimal locale is installed");
        MESSAGE("No comma-decimal locale installed; locale-specific check skipped");
        return;
    }
    const auto result = compileOk(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        io[0][0] = 1.25;
        io[0][1] = 1.25f;
    }
};)");
    JitRunner runner(result.program);
    Audio audio(1, 2);
    runner.process(audio);
    CHECK(audio.data[0] == std::vector<double>{1.25, 1.25});
}

TEST_CASE("sidechain views return zero outside their bounds") {
    const std::string source = R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t", .sidechain = true};
    void process(Block& io) {
        auto key = io.sidechain(0);
        io[-1][0] = 42;
        io[0][0] = key[-1];
        io[0][1] = key[io.frames];
        io[0][2] = key[0];
        io[-1][0] = 42;
        io[0][3] = io.sidechain(-1)[0];
    }
};
)";
    for (bool interpret : {false, true}) {
        scriptc::Options options;
        options.interpret = interpret;
        auto result = compileOk(source, options);
        for (bool connected : {false, true}) {
            JitRunner run(result.program);
            Audio key(2, 4);
            key.data[0][0] = 0.5;
            run.key = connected ? &key : nullptr;
            Audio audio(2, 4);
            run.process(audio);
            CHECK(audio.data[0] == std::vector<double>{0, 0, connected ? 0.5 : 0.0, 0});
        }
    }
}

TEST_CASE("invalid metadata always has an actionable hint") {
    auto plugin = [](const std::string& declaration) {
        return "#include <supple/script.h>\n"
               "struct T : Effect {\n"
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               declaration + "\nvoid process(Block& io) {}\n};\n";
    };
    for (const std::string factory :
         {"decibels(\"\", -12, 12, 0)", "decibels(\"P\", 1, 0, 0)", "decibels(\"P\", -12, 12, 20)",
          "frequency(\"P\", 0, 1000, 500)",
          "continuous(\"P\", 0, 1, 0).curved(Curve::Pow, 1.0 / 0.0)",
          "continuous(\"P\", 0, 1, 0).smoothed(0.0 / 0.0)", "integer(\"P\", 0, 1e20, 0)",
          "choice(\"P\", {\"A\", \"A\"}, 0)", "choice(\"P\", {\"A\", \"B\"}, 0).offAtMin()",
          "decibels(\"P\", -12, 12, 0).meter().smoothed(20)",
          "decibels(\"P\", -12, 12, 0).meter().modulated()"}) {
        INFO(factory);
        CHECK(firstError(plugin("Param p = " + factory + ";")) == "script.param.range");
    }
    for (const std::string modifier : {"meter(1)", "help(\"a\", \"b\")", "withId(1, 2)",
                                       "smoothed()", "curved(Curve::Pow, 2, 3)"}) {
        INFO(modifier);
        CHECK(firstError(plugin("Param p = decibels(\"P\", -12, 12, 0)." + modifier + ";")) ==
              "script.param.modifier");
    }
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\").withKey(\"a.b\");")) ==
          "script.file.key");
}

TEST_CASE("a mutable reference cannot default to a temporary") {
    CHECK(firstError(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    double read(int& value = 1) { return value; }
    void process(Block& io) {}
};
)") == "script.type.reference");
}

TEST_CASE("array size does not make its subscript a constant expression") {
    CHECK(firstError(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        Array<Array<int, 2>, 2> a;
        constexpr int width = a[0].size();
    }
};
)") == "script.constant.required");
}

TEST_CASE("the local storage limit covers a function's complete frame") {
    auto plugin = [](const std::string& body) {
        return "#include <supple/script.h>\n"
               "struct T : Effect {\n"
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
               "void process(Block& io) { " +
               body + " }\n};\n";
    };
    CHECK(firstError(plugin("Array<double, 8000> a;")).empty());
    CHECK(firstError(plugin("Array<double, 8000> a; Array<double, 8000> b;")) ==
          "script.local.size");
    CHECK(firstError(plugin("Array<double, 8000> a; auto b = a;")) == "script.local.size");
}

TEST_CASE("what a C++ compiler would refuse is refused, so a source export builds") {
    const std::string head =
        "#include <supple/script.h>\n"
        "constexpr int kOne = 1;\n";
    const std::string plugin =
        "struct T : Effect {\n"
        "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n";
    CHECK(firstError(head + plugin + "void process(Block& io) noexcept {}\n};\n").empty());
    CHECK(firstError(head + plugin + "void process(Block& io) noexcept(unknownName) {}\n};\n") ==
          "script.unsupported.noexcept");
    CHECK(firstError(head + "double later();\n").size() > 0);  // separate declarations: refused
    CHECK(firstError(head +
                     "double earlier() { return later(); }\n"
                     "double later() { return 1.0; }\n" +
                     plugin + "void process(Block& io) { io[0][0] = earlier(); }\n};\n") ==
          "script.name.order");
    CHECK(firstError(head + plugin +
                     "void process(Block& io) { io[0][0] = kTwo; }\n};\n"
                     "constexpr double kTwo = 2.0;\n") == "script.name.order");
    CHECK(firstError(head + "constexpr int bad = 2147483647 + kOne;\n" + plugin +
                     "void process(Block& io) {}\n};\n") == "script.constant.overflow");
    CHECK(firstError(head + "constexpr int fine = 2147483646 + kOne;\n" + plugin +
                     "void process(Block& io) {}\n};\n")
              .empty());
}

TEST_CASE("the frames along a chain of calls are limited together") {
    // Each function holds 60 KiB; a chain of n of them needs n times that.
    auto chain = [](int n) {
        std::string text =
            "#include <supple/script.h>\n"
            "struct T : Effect {\n"
            "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
            "double f0() { Array<double, 7500> a; a[0] = 1.0; return a[0]; }\n";
        for (int i = 1; i < n; ++i)
            text += "double f" + std::to_string(i) + "() { Array<double, 7500> a; a[0] = f" +
                    std::to_string(i - 1) + "(); return a[0]; }\n";
        return text + "void process(Block& io) { io[0][0] = f" + std::to_string(n - 1) +
               "(); }\n};\n";
    };
    CHECK(firstError(chain(4)).empty());
    CHECK(firstError(chain(5)) == "script.stack");
}

TEST_CASE("calls spend the loop budget, so a tree of calls without loops stops") {
    // f16 calls f15 twice, ... down to f0: 65536 calls of f0 in all.
    std::string source =
        "#include <supple/script.h>\n"
        "struct T : Effect {\n"
        "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
        "double count = 0;\n"
        "void f0() { count += 1; }\n";
    for (int i = 1; i <= 16; ++i)
        source += "void f" + std::to_string(i) + "() { f" + std::to_string(i - 1) + "(); f" +
                  std::to_string(i - 1) + "(); }\n";
    source += "void process(Block& io) { f16(); io[0][0] = count; }\n};\n";
    auto result = compileOk(source);
    JitRunner run(result.program);
    Audio audio(2, 1);
    script::detail::CurrentScope current(&run.instance);
    run.instance.budget = 10000;
    run.blocks(audio, 1, [&](script::Block& block) {
        callRealtime(result.program->process(), &run.instance, run.self(), &block);
    });
    CHECK(run.instance.status.budgetExceeded.load() == 1);
    CHECK(audio.data[0][0] == 0.0);
}

TEST_CASE("loop budgets stop all back edges and unwind script calls") {
    for (const std::string loop :
         {"while (true) { continue; }", "for (;;) { continue; }", "do { continue; } while (true);",
          "Array<int, 8> a; for (int x : a) { continue; }"}) {
        const std::string source =
            "#include <supple/script.h>\n"
            "struct T : Effect {\n"
            "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n"
            "double work() { " +
            loop +
            " return 1.0; }\n"
            "void process(Block& io) { io[0][0] = work(); io[0][1] = 2.0; }\n};\n";
        for (bool interpret : {false, true}) {
            INFO(loop << ", interpret=" << interpret);
            scriptc::Options options;
            options.interpret = interpret;
            auto result = compileOk(source, options);
            JitRunner run(result.program);
            Audio audio(2, 2);
            script::detail::CurrentScope current(&run.instance);
            run.instance.budget = 3;
            run.blocks(audio, 2, [&](script::Block& block) {
                if (interpret)
                    result.program->process()(&run.instance, run.self(), &block);
                else
                    callRealtime(result.program->process(), &run.instance, run.self(), &block);
            });
            CHECK(run.instance.status.budgetExceeded.load() == 1);
            CHECK(run.instance.status.budgetLine.load() == 4);
            CHECK(run.instance.budget < 0);
            CHECK(audio.data[0] == std::vector<double>{0, 0});
        }
    }
}

TEST_CASE("NaN comparisons and bounded sidechain reads run in a JIT audio hook") {
    auto result = compileOk(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        double x = sqrt(-1.0);
        io[0][0] = x != 0.0;
        io[0][1] = x < 0.0;
        io[0][2] = x == x;
        io[0][3] = bool(x);
        auto key = io.sidechain(0);
        io[0][4] = key[-1];
    }
};
)");
    JitRunner run(result.program);
    Audio audio(2, 5);
    script::detail::CurrentScope current(&run.instance);
    run.instance.budget = 100;
    run.blocks(audio, 5, [&](script::Block& block) {
        callRealtime(result.program->process(), &run.instance, run.self(), &block);
    });
    CHECK(audio.data[0] == std::vector<double>{1, 0, 0, 1, 0});
}

TEST_CASE("a value squared where registers spill compiles (MIR's allocator, patched)") {
    // MIR 1.0.0 asserted on an instruction naming one spilled register as its
    // result and both operands (e = e * e after coalescing), aborting the host.
    compileOk(R"(#include <supple/script.h>
struct Sec { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };
void poles(Sec& s, double wn, double zeta) {
    const double e = exp(-zeta * wn);
    s.a2 = e * e;
    if (zeta < 1.0)
        s.a1 = -2.0 * e * cos(wn * sqrt(1.0 - zeta * zeta));
    else
        s.a1 = -2.0 * e * cosh(wn * sqrt(zeta * zeta - 1.0));
}
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    Param zeta = continuous("Zeta", 0.1, 4, 0.7);
    Sec s;
    void update() { poles(s, 0.3, zeta); }
    void process(Block& io) {}
};
)");
}

TEST_CASE("float equality where registers run past xmm7 compiles and runs (MIR, patched)") {
    // MIR 1.0.0's pattern for a float == or != left out the REX prefix its
    // xmm8-15 operands need, so its size estimate fell a byte short: a debug
    // build asserted, aborting the host, and a release build could choose a
    // short jump that does not reach.
    auto result = compileOk(R"(#include <supple/script.h>
struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    void process(Block& io) {
        Array<float, 12> f;
        for (int k = 0; k < 12; ++k) f[k] = static_cast<float>(io[0][k]);
        float a = f[0], b = f[1], c = f[2], d = f[3], e = f[4], g = f[5];
        float h = f[6], i = f[7], j = f[8], l = f[9], m = f[10], n = f[11];
        float nan = static_cast<float>(sqrt(-1.0));
        int same = 0, differ = 0;
        if (a == b) same += 1;
        if (c != d) differ += 1;
        if (e == g) same += 1;
        if (h != i) differ += 1;
        if (j == l) same += 1;
        if (m != n) differ += 1;
        if (nan == nan) same += 100;
        if (nan != nan) differ += 10;
        io[0][0] = same;
        io[0][1] = differ;
        io[0][2] = a + b + c + d + e + g + h + i + j + l + m + n;
    }
};
)");
    JitRunner run(result.program);
    Audio audio(1, 12);
    audio.data[0] = {1, 1, 2, 3, 4, 4, 5, 5, 6, 6, 7, 8};
    script::detail::CurrentScope current(&run.instance);
    run.instance.budget = 1000;
    run.blocks(audio, 12, [&](script::Block& block) {
        callRealtime(result.program->process(), &run.instance, run.self(), &block);
    });
    CHECK(audio.data[0][0] == 3);   // a == b, e == g, j == l; NaN equals nothing
    CHECK(audio.data[0][1] == 12);  // c != d, m != n, and NaN differs from itself
    CHECK(audio.data[0][2] == 52);
}

namespace {
std::string moduleScript(const std::string& members,
                         const std::string& hooks = "void process(int frames) {}") {
    return "#include <supple/script.h>\nstruct TestModule : Module {\n"
           "static constexpr ModuleInfo info = {.name = \"Test\", .id = \"com.example.test\"};\n" +
           members + "\n" + hooks + "\n};";
}
}  // namespace

TEST_CASE("modules describe ports, identity, parameters, presets and face hooks") {
    const auto r = compileOk(R"cpp(
#include <supple/script.h>
        struct TestModule : Module {
            static constexpr ModuleInfo info = {.name = "Test",
                                                .id = "com.example.test",
                                                .vendor = "Example",
                                                .version = "2.3.4",
                                                .description = "A test.",
                                                .url = "https://example.com",
                                                .tags = {"utility", "poly"},
                                                .width = 120,
                                                .height = 300};
            Input audioInlet = audioIn("In").help("Audio.").in("Inputs").normal(0.25);
            Input pitch = pitchIn("Pitch");
            Input gate = gateIn("Gate");
            Input cv = cvIn("CV");
            Input uni = unipolarIn("Uni");
            Input phase = phaseIn("Phase");
            Input generic = portSpec("Generic", SignalKind::Cv, false);
            Output out = audioOut("Out").withKey("signal_out");
            Output pitchOutJack = pitchOut("Pitch");
            Output gateOutJack = gateOut("Gate");
            Output cvOutJack = cvOut("CV");
            Output uniOutJack = unipolarOut("Uni");
            Output phaseOutJack = phaseOut("Phase");
            Param level = percent("Level", 25).smoothed(10).help("Gain.");
            Param mode = choice("Mode", {"A", "B"}, 0);
            Param enabled = toggle("Enabled", true);
            Param reading = continuous("Reading", 0, 1, 0).meter();
            Shared<double> seen;
            Random random;
            void prepare() { random.seed(seed()); }
            void update() { seen.set(level.value()); }
            void process(int frames) {
                auto t = transport();
                out.setChannels(channels());
                for (int c = 0; c < out.channels(); ++c)
                    for (int i = 0; i < frames; ++i)
                        out[c][i] = audioInlet[c][i] * level.at(i, c) + t.tempo * 0.0;
            }
            void panel(Panel& p) {
                p.size(120, 300);
                p.knob(level, 30, 50);
                p.knob(level, 80, 50, 24);
                p.slider(level, 10, 80, 100, 20);
                p.choice(mode, 10, 110, 100, 20);
                p.toggle(enabled, 10, 140, 100, 20);
                p.readout(reading, 10, 170, 100, 20);
                p.input(audioInlet, 30, 220);
                p.output(out, 90, 220);
                p.display(10, 240, 100, 30);
            }
            void draw(Canvas& g) { g.fillText(format("%.2f", seen.get()), 10, 260); }
            void pointer(const Pointer& p) {
                if (p.action() == PointerAction::Press) level.edit(0.5);
            }
            void factoryPatches(Patches& p) { p.add("Half").set(level, 50).set(mode, "b"); }
        };
    )cpp");
    CHECK(r.program->kind() == scriptc::PluginKind::Module);
    CHECK(r.program->process() == nullptr);
    CHECK(r.program->editor() == nullptr);
    auto module = scriptc::moduleCode(r.program);
    REQUIRE(module);
    CHECK(module->info().tags == std::vector<std::string>{"utility", "poly"});
    CHECK(module->info().width == 120);
    CHECK(module->info().height == 300);
    REQUIRE(module->ports().size() == 13);
    CHECK(module->ports()[0].member == "audioInlet");
    CHECK(module->ports()[0].key == "audio_inlet");
    CHECK(module->ports()[0].normal == 0.25);
    CHECK(module->ports()[0].group == "Inputs");
    CHECK(module->ports()[0].description == "Audio.");
    CHECK(module->ports()[6].signal == script::detail::PortModel::Signal::Cv);
    CHECK(module->ports()[7].key == "signal_out");
    CHECK(module->ports()[7].index == 0);
    CHECK(module->ports()[7].output);
    CHECK(module->shared().size() == 1);
    CHECK(module->factoryPatches().size() == 1);
    CHECK(module->prepare());
    CHECK(module->update());
    CHECK(module->process());
    CHECK(module->panel());
    CHECK(module->draw());
    CHECK(module->pointer());
    CHECK_FALSE(scriptc::moduleCode(nullptr));
    auto plugin = compileOk(kGain);
    CHECK_FALSE(scriptc::moduleCode(plugin.program));
}

TEST_CASE("modules reject plugin-only declarations with module diagnostics") {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {moduleScript("Source lfo = source(\"LFO\");"), "script.module.source"},
        {moduleScript("AudioFile file = audioFile(\"File\");"), "script.module.file"},
        {moduleScript("Convolver room = impulseResponse(\"Room\");"), "script.module.file"},
        {moduleScript("", "void process(Block& io) {}"), "script.module.process"},
        {moduleScript("", "void process(int frames) {} void editor(Editor& e) {}"),
         "script.module.editor"},
        {moduleScript("", "void process(int frames) {} void startVoice(int v) {}"),
         "script.module.voice"},
        {moduleScript("", "void process(int frames) {} void releaseVoice(int v) {}"),
         "script.module.voice"},
        {moduleScript("", "void process(int frames) {} void stopVoice(int v) {}"),
         "script.module.voice"},
        {moduleScript("", "void process(int frames) { int v = voiceCount(); }"),
         "script.module.voice"},
        {moduleScript("", "void process(int frames) {} void prepare() { setLatency(0); }"),
         "script.module.latency"},
        {moduleScript("", "void process(int frames) { setTail(0); }"), "script.module.tail"},
        {moduleScript("Param gain = percent(\"Gain\", 50);",
                      "void process(int frames) {} void factoryPatches(Patches& p) { "
                      "p.add(\"P\").route(\"lfo>gain\", 1); }"),
         "script.module.route"},
        {moduleScript("Input a = audioIn(\"A\"), b = audioIn(\"B\");"),
         "script.module.port_declaration"},
        {moduleScript("Array<Input, 2> in;"), "script.module.port_location"},
        {moduleScript("Input in = audioOut(\"In\");"), "script.module.port_direction"},
        {moduleScript("Output out = audioIn(\"Out\");"), "script.module.port_direction"},
        {moduleScript("Input in;"), "script.module.port_declaration"},
        {moduleScript("Input in = audioIn(\"\");"), "script.module.port_name"},
        {moduleScript("Input in = audioIn(\"In\").withKey(\"BAD\");"), "script.module.key"},
        {moduleScript("Input in = audioIn(\"In\").smoothed(10);"), "script.module.port_modifier"},
        {moduleScript("", "void process(int frames) { Input in = audioIn(\"In\"); }"),
         "script.module.port_location"},
    };
    for (const auto& [source, code] : cases) {
        CAPTURE(source);
        CHECK(firstError(source) == code);
    }
}

TEST_CASE("module ports and parameters share keys and each direction has a limit") {
    CHECK(
        firstError(moduleScript(
            "Param level = percent(\"Level\", 50); Input in = cvIn(\"In\").withKey(\"level\");")) ==
        "script.module.key");
    CHECK(firstError(moduleScript(
              "Input in = cvIn(\"In\"); Output out = cvOut(\"Out\").withKey(\"in\");")) ==
          "script.module.key");
    for (const char* direction : {"In", "Out"}) {
        std::string members;
        for (int i = 0; i < 32; ++i)
            members += std::string(direction == std::string("In") ? "Input" : "Output") + " p" +
                       std::to_string(i) + " = audio" + direction + "(\"Port\");\n";
        compileOk(moduleScript(members));
        members += std::string(direction == std::string("In") ? "Input" : "Output") +
                   " extra = audio" + direction + "(\"Extra\");";
        CHECK(firstError(moduleScript(members)) == "script.module.port_limit");
    }
}

TEST_CASE("plugins refuse the module-only surface") {
    const auto plugin = [](const std::string& members, const std::string& body) {
        return "#include <supple/script.h>\nstruct P : Effect { static constexpr Info info = "
               "{.name = \"P\", .id = \"com.example.p\"};" +
               members + "void process(Block& io) {" + body + "}};";
    };
    CHECK(firstError(plugin("Input in = audioIn(\"In\");", "")) == "script.module.port_location");
    CHECK(firstError(plugin("Output out = audioOut(\"Out\");", "")) ==
          "script.module.port_location");
    CHECK(firstError(plugin("Param p = percent(\"P\", 50);", "double x = p.at(0, 0);")) ==
          "script.plugin.param_channel");
    CHECK(firstError(plugin("", "int x = seed();")) == "script.plugin.module");
    CHECK(firstError(plugin("", "auto t = transport();")) == "script.plugin.module");
    CHECK(firstError(plugin("void panel(Panel& p) {}", "")) == "script.plugin.panel");
    // Existing plugins (Marram) use panel as an ordinary drawing helper.
    compileOk(
        plugin("void panel(Canvas& g, double x) { g.fillRect(x, 0, 10, 10); } "
               "void draw(Canvas& g) { panel(g, 20); }",
               ""));
}

namespace {
namespace sd = script::detail;
namespace ss = script;

void callRealtimeModule(sd::ModuleCode::ProcessHook hook, void* instance, void* self,
                        std::int64_t frames)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
    [[clang::nonblocking]]
#endif
{
    hook(instance, self, frames);
}

struct ModuleRun {
    const sd::ModuleCode& code;
    sd::Instance record;
    void* self;
    std::array<double, 64> input{}, output{};
    std::array<double, 16> modulation{};
    std::array<double, 4> zero{}, glide{0.4, 0.3, 0.2, 0.1};
    double spare = 0;

    ModuleRun(const sd::ModuleCode& codeIn, int inputChannels, int modChannels)
        : code(codeIn), self(code.create()) {
        record.channels = 4;
        record.maxBlockSize = 32;
        record.seed = 1741;
        record.transport.tempo = 137.5;
        record.offline = true;
        sd::Instance::bind(*static_cast<ss::Plugin*>(self), &record);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = inputChannels ? double(i) * 0.03125 : 0.25;
        for (std::size_t i = 0; i < modulation.size(); ++i) modulation[i] = double(i) * 0.015625;
        auto* base = static_cast<unsigned char*>(self);
        auto& in = *reinterpret_cast<ss::Input*>(base + code.ports()[0].offset);
        auto& out = *reinterpret_cast<ss::Output*>(base + code.ports()[1].offset);
        sd::PortBinding::setSlot(in, 0);
        sd::PortBinding::setSlot(out, 0);
        sd::PortBinding::bindInput(in, input.data(), zero.data(), inputChannels <= 1 ? 0 : 4,
                                   inputChannels, 4);
        sd::PortBinding::bindOutput(out, output.data(), &spare, 4, 4, 4);
        auto& p = *reinterpret_cast<ss::Param*>(base + code.params()[0].offset);
        sd::ParamBinding::setSlot(p, 0);
        sd::ParamBinding::setValue(p, 0.5, true);
        sd::ParamBinding::setGlide(p, glide.data(), 4);
        sd::ParamBinding::setModulation(p, modulation.data(), 4, modChannels);
        sd::CurrentScope scope(&record);
        if (code.prepare()) code.prepare()(&record, self);
        if (code.update()) code.update()(&record, self);
    }
    ~ModuleRun() { code.destroy(self); }
    void run(bool interpret = false) {
        sd::CurrentScope scope(&record);
        record.budget = 65536 * 4;
        if (interpret)
            code.process()(&record, self, 4);
        else
            callRealtimeModule(code.process(), &record, self, 4);
    }
};

struct PanelCalls final : sd::PanelBuilder {
    std::vector<double> calls;
    void size(double w, double h) override { calls.insert(calls.end(), {1, w, h}); }
    void control(Control kind, int slot, double x, double y, double w, double h) override {
        calls.insert(calls.end(), {2, double(kind), double(slot), x, y, w, h});
    }
    void jack(bool output, int index, double x, double y) override {
        calls.insert(calls.end(), {3, double(output), double(index), x, y});
    }
    void display(double x, double y, double w, double h) override {
        calls.insert(calls.end(), {4, x, y, w, h});
    }
};
}  // namespace

TEST_CASE("module bounds, modulation fallbacks, transport and panel match native C++") {
    const std::string file = __FILE__;
    std::ifstream in(file.substr(0, file.find_last_of("/\\") + 1) + "module_surface.h");
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    const sd::NativeModuleCode<script::module_test::ModuleSurface> native;
    for (bool interpret : {false, true}) {
        auto r = compileOk(text.str(), {.interpret = interpret});
        auto compiled = scriptc::moduleCode(std::move(r.program));
        REQUIRE(compiled);
        // Only the adapter retains this Program now, including string literals and thunks.
        for (int inputChannels : {0, 1, 2, 4}) {
            for (int modChannels : {0, 1, 2, 4}) {
                CAPTURE(interpret);
                CAPTURE(inputChannels);
                CAPTURE(modChannels);
                ModuleRun a(native, inputChannels, modChannels),
                    b(*compiled, inputChannels, modChannels);
                a.run();
                b.run(interpret);
                for (std::size_t i = 0; i < a.output.size(); ++i)
                    CHECK(std::bit_cast<std::uint64_t>(a.output[i]) ==
                          std::bit_cast<std::uint64_t>(b.output[i]));
                CHECK(a.record.status.outOfRange.load() == b.record.status.outOfRange.load());
                CHECK(a.record.status.outOfRange.load() == 12);
                CHECK(b.record.status.budgetExceeded.load() == 0);
                CHECK(a.output[14 * 4 + 1] == 0.5);
                CHECK(a.output[14 * 4 + 2] == 0.5);
                CHECK(a.output[14 * 4 + 3] == (modChannels == 1 ? 0.0 : 0.4));
                CHECK(a.output[15 * 4 + 1] == 137.5);
                ss::Panel pa, pb;
                PanelCalls ca, cb;
                sd::Layout::bind(pa, &ca);
                sd::Layout::bind(pb, &cb);
                native.panel()(&a.record, a.self, &pa);
                compiled->panel()(&b.record, b.self, &pb);
                CHECK(ca.calls == cb.calls);
                CHECK_FALSE(ca.calls.empty());
                // Copy all runtime objects and bindings as native assignment does.
                void* copy = compiled->create();
                compiled->copy(copy, b.self);
                b.run(interpret);
                const auto next = b.output;
                {
                    sd::CurrentScope scope(&b.record);
                    b.record.budget = 65536 * 4;
                    if (interpret)
                        compiled->process()(&b.record, copy, 4);
                    else
                        callRealtimeModule(compiled->process(), &b.record, copy, 4);
                }
                for (std::size_t i = 0; i < next.size(); ++i)
                    CHECK(std::bit_cast<std::uint64_t>(next[i]) ==
                          std::bit_cast<std::uint64_t>(b.output[i]));
                compiled->destroy(copy);
            }
        }
    }
}

TEST_CASE("a module process hook receives its frame count and respects its budget") {
    const auto source = moduleScript(
        "Output out = audioOut(\"Out\");",
        "void process(int frames) { for (int i = 0; i < frames * 70000; ++i) out[0][0] = i; }");
    for (bool interpret : {false, true}) {
        auto r = compileOk(source, {.interpret = interpret});
        auto module = scriptc::moduleCode(r.program);
        void* self = module->create();
        sd::Instance instance;
        sd::Instance::bind(*static_cast<ss::Plugin*>(self), &instance);
        double data[4] = {}, spare = 0;
        auto& out = *reinterpret_cast<ss::Output*>(static_cast<unsigned char*>(self) +
                                                   module->ports()[0].offset);
        sd::PortBinding::bindOutput(out, data, &spare, 4, 1, 4);
        for (int frames : {1, 4}) {
            instance.status.clear();
            instance.budget = 65536 * frames;
            sd::CurrentScope scope(&instance);
            module->process()(&instance, self, frames);
            CHECK(instance.status.budgetExceeded.load() == 1);
            CHECK(instance.budget < 0);
            CHECK(data[0] >= 65000 * frames);
        }
        module->destroy(self);
    }
}

TEST_CASE("module identity and context checks preserve the script subset") {
    auto identity = [](const std::string& body) {
        return "#include <supple/script.h>\nstruct T : Module { static constexpr ModuleInfo info = "
               "{" +
               body + "}; void process(int frames) {} };";
    };
    CHECK(firstError(identity(".name = \"T\"")) == "script.module.id");
    CHECK(firstError(identity(".id = \"com.example.t\"")) == "script.module.name");
    CHECK(firstError(identity(".name = \"T\", .id = \"bad_id\"")) == "script.module.id");
    CHECK(firstError(identity(".name = \"T\", .id = \"com.example.t\", .tags = {\"UPPER\"}")) ==
          "script.module.info");
    CHECK(firstError(identity(".name = \"T\", .id = \"com.example.t\", .tags = {\"a\", \"b\", "
                              "\"c\", \"d\", \"e\"}")) == "script.module.info");
    CHECK(firstError(identity(".name = \"T\", .id = \"com.example.t\", .width = -1")) ==
          "script.module.info");
    CHECK(firstError(identity(".name = \"T\", .id = \"com.example.t\", .name = \"U\"")) ==
          "script.module.info");
    CHECK(firstError(moduleScript(
              "Param a = percent(\"A\", 50); Param b = percent(\"B\", 25).withKey(\"a\");")) ==
          "script.module.key");
    CHECK(firstError(moduleScript("Input in = audioIn(\"In\");",
                                  "void process(int frames) { auto copy = in; }")) ==
          "script.module.port_location");
    CHECK(firstError(
              moduleScript("", "void process(int frames) { panel(); } void panel(Panel& p) {}")) ==
          "script.call.arguments");
    CHECK(firstError(moduleScript(
              "Input in = audioIn(\"In\");",
              "void process(int frames) {} void draw(Canvas& g) { double x = in[0][0]; }")) ==
          "script.editor.port");
    CHECK(firstError(moduleScript(
              "Output out = audioOut(\"Out\");",
              "void process(int frames) {} void panel(Panel& p) { out.setChannels(2); }")) ==
          "script.editor.call");
    CHECK(firstError(moduleScript(
              "Param p = percent(\"P\", 50);",
              "void process(int frames) {} void panel(Panel& face) { p.edit(0.5); }")) ==
          "script.editor.edit");
    CHECK(firstError(moduleScript("Buffer<double> data;",
                                  "void process(int frames) { data.resize(16); }")) ==
          "script.rt.allocation");
    CHECK(firstError(
              moduleScript("Input in = portSpec(\"In\", static_cast<SignalKind>(99), false);")) ==
          "script.module.port_declaration");
    compileOk(moduleScript("Input in = portSpec(\"In\", SignalKind::Audio, 0);"));
    CHECK(firstError(moduleScript("", "void process(int frames) { this->setTail(0); }")) ==
          "script.module.tail");
    CHECK(
        firstError(moduleScript("Param p = frequency(\"P\", 20, 20000, 1000).polyModulated();")) ==
        "script.module.modulation");
}
