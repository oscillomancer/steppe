// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include "../../../tests/file_platform.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include <supple/base/files.h>
#include <supple/base/paths.h>
#include <supple/sandbox/session.h>

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kGain = "com.suppleaudio.test.gain";
constexpr const char* kSine = "com.suppleaudio.test.sine";

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() / (std::string("supple-sandbox-") + tag + "-" +
                                            std::to_string(supple::test::processId()));
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::unique_ptr<Session> openSession(const TempDir& dir) {
    SessionOptions o;
    o.workdir = dir.path;
    o.pluginPath = {kFixture};
    auto s = Session::open(o);
    REQUIRE(s);
    return std::move(*s);
}

midi::Sequence twoNotes() {
    midi::Sequence p;
    p.ppq = 960;
    p.notes = {{0, 960, 0, 69, 127}, {960, 960, 0, 76, 64}};  // two quarter notes
    return p;
}

}  // namespace

TEST_CASE("sandbox retains scan diagnostics with its cached catalogue") {
    TempDir dir("scan-report");
    const auto bad = dir.path / "bad.clap";
    REQUIRE(writeFileAtomic(bad, "not a plugin"));
    SessionOptions options;
    options.workdir = dir.path;
    options.pluginPath = {kFixture, bad};
    auto opened = Session::open(options);
    REQUIRE(opened);
    auto& session = **opened;
    auto catalogue = session.plugins();
    REQUIRE(catalogue);
    CHECK_FALSE(catalogue->empty());
    REQUIRE(session.pluginScanReport().failures.size() == 1);
    CHECK(session.pluginScanReport().failures.front().module == fs::canonical(bad));
    CHECK(session.pluginScanReport().failures.front().reason.code == "host.load");
    // Other probes on this thread overwrite lastScanReport, not this catalogue's report.
    const std::vector<fs::path> empty;
    REQUIRE(host::scan(empty));
    CHECK(host::lastScanReport().failures.empty());
    REQUIRE(session.plugins());
    CHECK(session.pluginScanReport().failures.size() == 1);
}

TEST_CASE("evaluation MIDI sidecars retain compiler note data and follow render lifetime") {
    TempDir dir("eval-midi");
    SessionOptions options;
    options.workdir = dir.path;
    options.pluginPath = {kFixture};
    options.recordMidi = true;
    auto opened = Session::open(options);
    REQUIRE(opened);
    auto& session = **opened;
    REQUIRE(session.load(kSine));
    RenderInput input;
    input.notes = twoNotes();
    input.notes->length = 2880;
    input.meter = midi::Meter{3, 4};
    input.barMeters = {{3, 4}};
    input.tailSeconds = 0;
    auto render = session.render(input);
    REQUIRE(render);
    auto sidecar = (*render)->path;
    sidecar.replace_extension(".midi.json");
    auto text = readTextFile(sidecar);
    REQUIRE(text);
    auto parsed = json::parse(*text);
    REQUIRE(parsed);
    CHECK(parsed->find("schema")->asString() == "supple.render-midi/1.0");
    CHECK(parsed->find("ppq")->asInt() == 960);
    CHECK(parsed->find("length_ticks")->asInt() == 2880);
    CHECK(parsed->find("meter")->asString() == "3/4");
    CHECK(parsed->find("bar_meters")->at(0).asString() == "3/4");
    CHECK(parsed->find("notes")->size() == 2);
    CHECK(parsed->find("notes")->at(1).find("pitch")->asInt() == 76);
    CHECK(parsed->find("notes")->at(1).find("start")->asInt() == 960);
    REQUIRE(session.begin());
    auto rolledBack = session.render(input);
    REQUIRE(rolledBack);
    auto rolledSidecar = (*rolledBack)->path;
    rolledSidecar.replace_extension(".midi.json");
    REQUIRE(fs::exists(rolledSidecar));
    REQUIRE(session.rollback());
    CHECK_FALSE(fs::exists(rolledSidecar));
    CHECK(fs::exists(sidecar));
    session.reset();
    CHECK_FALSE(fs::exists(sidecar));

    options.recordMidi = false;
    auto ordinary = Session::open(options);
    REQUIRE(ordinary);
    REQUIRE((*ordinary)->load(kSine));
    auto unrecorded = (*ordinary)->render(input);
    REQUIRE(unrecorded);
    auto absent = (*unrecorded)->path;
    absent.replace_extension(".midi.json");
    CHECK_FALSE(fs::exists(absent));
}

TEST_CASE("plugins: catalogue, lookup by id, case-insensitive id and path") {
    TempDir dir("plugins");
    auto s = openSession(dir);
    auto list = s->plugins();
    REQUIRE(list);
    CHECK(list->size() == 18);
    CHECK(s->findPlugin(kGain).value().name == "Test Gain");
    CHECK(s->findPlugin("COM.SUPPLEAUDIO.TEST.SINE").value().id == kSine);
    auto bad = s->findPlugin("com.suppleaudio.test.gian");
    REQUIRE(!bad);
    CHECK(bad.error().code == "sandbox.unknown_plugin");
    CHECK(bad.error().hint.find(kGain) != std::string::npos);
    auto byPath = s->findPlugin(kFixture.string());
    REQUIRE(!byPath);  // the fixture binary holds multiple plugins
    CHECK(byPath.error().code == "sandbox.ambiguous_plugin");
}

TEST_CASE("chain: instruments first, effects after, errors name the fix") {
    TempDir dir("chain");
    auto s = openSession(dir);
    auto g = s->load(kGain);
    REQUIRE(g);
    CHECK(*g == "in_1");
    auto i = s->load(kSine);  // an instrument goes first by default
    REQUIRE(i);
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_1"});
    auto second = s->load(kSine);
    REQUIRE(!second);
    CHECK(second.error().code == "sandbox.chain_order");
    auto front = s->load(kGain, 0);
    REQUIRE(!front);
    CHECK(front.error().path == "/position");
    auto past = s->load(kGain, 9);
    REQUIRE(!past);
    CHECK(past.error().code == "sandbox.position");
    auto g2 = s->load(kGain, 1);
    REQUIRE(g2);
    CHECK(*g2 == "in_3");  // the failed loads consumed no IDs
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_3", "in_1"});
    CHECK(s->move("in_1", 1));
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_1", "in_3"});
    CHECK(s->move("in_2", 2).error().code == "sandbox.chain_order");
    CHECK(s->remove("in_1"));
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_3"});
    auto gone = s->instance("in_1");
    REQUIRE(!gone);
    CHECK(gone.error().code == "sandbox.unknown_instance");
    CHECK(gone.error().hint.find("in_3 (Test Gain)") != std::string::npos);
}

TEST_CASE("params: semantic keys with unit strings; errors carry arg paths") {
    TempDir dir("params");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    json::Value values = json::Value::object();
    values["gain"] = "-6 dB";
    auto set = s->paramSet(g, values);
    REQUIRE(set);
    CHECK((*set)["gain"]["value"].asDouble() == doctest::Approx(-6.0));
    CHECK((*set)["gain"]["text"].asString().find("-6") != std::string::npos);
    auto all = s->paramGet(g);
    REQUIRE(all);
    CHECK(all->find("latency"));
    CHECK((*all)["latency"]["value"].asString() == "none");

    json::Value bad = json::Value::object();
    bad["gian"] = 1;
    auto e = s->paramSet(g, bad);
    REQUIRE(!e);
    CHECK(e.error().path == "/values/gian");
    json::Value range = json::Value::object();
    range["gain"] = "40 dB";
    auto r = s->paramSet(g, range);
    REQUIRE(!r);
    CHECK(r.error().path == "/values/gain");
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-6.0));

    const std::string keys[] = {"gian"};
    auto unknown = s->paramGet(g, keys);
    REQUIRE(!unknown);
    CHECK(unknown.error().code == "sandbox.unknown_param");
    CHECK(unknown.error().path == "/keys/0");
    CHECK(unknown.error().hint.find("gain") != std::string::npos);
}

TEST_CASE("patches: merge reports the effective change; invalid patches change nothing") {
    TempDir dir("patches");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    auto merge = json::parse(R"({"params": {"gain": -12}})");
    auto m = s->patchMerge(g, *merge);
    REQUIRE(m);
    REQUIRE(m->changed.find("params"));
    CHECK(m->changed.find("params")->size() == 1);
    CHECK((*m->changed.find("params")->find("gain")).asDouble() == -12.0);
    CHECK(!m->description.empty());
    auto again = s->patchMerge(g, *merge);
    REQUIRE(again);
    CHECK(json::writeCompact(again->changed) == "{}");  // idempotent

    const auto before = s->patchGet(g, false).value();
    auto bad = s->patchMerge(g, *json::parse(R"({"params": {"nope": 1}})"));
    REQUIRE(!bad);
    CHECK(bad.error().path == "/patch/params/nope");
    CHECK(s->patchGet(g, false).value() == before);

    auto full = s->patchSet(g, before);
    CHECK(full);
    const std::string sine = s->load(kSine).value();
    auto ns = s->patchGet(sine, false);
    REQUIRE(!ns);
    CHECK(ns.error().code == "sandbox.not_semantic");
}

TEST_CASE("presets: save, list, load restores; names and plugins are checked") {
    TempDir dir("presets");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -3}})")));
    const std::string tags[] = {"quiet"};
    auto path = s->presetSave(g, "Down 3", tags, "a little quieter");
    REQUIRE(path);
    CHECK(fs::exists(*path));
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": 6}})")));
    auto loaded = s->presetLoad(g, "Down 3");
    REQUIRE(loaded);
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-3.0));
    auto list = s->presetList();
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    CHECK(list->front().plugin == kGain);
    CHECK(list->front().tags == std::vector<std::string>{"quiet"});
    CHECK(s->presetList("com.other").value().empty());

    CHECK(s->presetSave(g, "../escape").error().code == "sandbox.preset_name");
    CHECK(s->presetSave(g, ".hidden").error().code == "sandbox.preset_name");
    auto missing = s->presetLoad(g, "Down 4");
    REQUIRE(!missing);
    CHECK(missing.error().code == "sandbox.unknown_preset");
    CHECK(missing.error().hint.find("Down 3") != std::string::npos);
    // The sine fixture has no state extension: saving says so.
    const std::string sine = s->load(kSine).value();
    REQUIRE(s->presetSave(sine, "x"));
    REQUIRE(s->presetLoad(sine, "x"));
}

TEST_CASE("presets: older folders still load; another plugin's there never does") {
    TempDir dir("presets-legacy");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    auto saved = s->presetSave(g, "Old");
    REQUIRE(saved);
    CHECK(saved->parent_path().filename() == paths::idFolder(kGain));
    const fs::path older = saved->parent_path().parent_path() / paths::legacyIdFolder(kGain);
    fs::create_directories(older);
    fs::rename(*saved, older / "Old.json");
    // A file there that is another plugin's: IDs such as com.example/a and
    // com.example_a shared one.
    auto text = readTextFile(older / "Old.json").value();
    const std::string plugin = std::string("\"") + kGain + "\"";
    text.replace(text.find(plugin), plugin.size(), "\"com.suppleaudio.test/gain\"");
    REQUIRE(writeFileAtomic(older / "Theirs.json", text));
    auto list = s->presetList(kGain);
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    CHECK(list->front().name == "Old");
    REQUIRE(s->presetLoad(g, "Old"));
    CHECK(s->presetLoad(g, "Theirs").error().code == "sandbox.unknown_preset");
    REQUIRE(s->presetSave(g, "Old"));  // moves it to the plugin's own folder
    CHECK(fs::exists(*saved));
    CHECK(!fs::exists(older / "Old.json"));
    CHECK(s->presetList(kGain).value().size() == 1);
}

TEST_CASE("render: notes through instrument + effect, WAV written, deterministic" *
          doctest::description("rt")) {
    TempDir dir("render");
    auto s = openSession(dir);
    REQUIRE(s->load(kSine));
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -6}})")));
    RenderInput in;
    in.notes = twoNotes();
    in.name = "two notes";
    auto r = s->render(in);
    REQUIRE(r);
    const RenderRecord& rec = **r;
    CHECK(rec.id == "rd_1");
    CHECK(fs::exists(rec.path));
    // Two quarter notes at 120 BPM = 1 s, then a short silent tail.
    CHECK(rec.seconds >= 1.0);
    CHECK(rec.seconds < 1.3);
    CHECK_FALSE(rec.tailCapped);
    // Loudest note: velocity 127 → 0.25 amplitude, -6 dB.
    CHECK(rec.peakDbfs == doctest::Approx(20 * std::log10(0.25) - 6).epsilon(0.002));
    auto wav = audio::readAudio(rec.path);
    REQUIRE(wav);
    CHECK(static_cast<std::int64_t>(wav->frames()) == rec.frames);

    auto r2 = s->render(in);
    REQUIRE(r2);
    CHECK((*r2)->id == "rd_2");
    CHECK(readFile(rec.path).value() == readFile((*r2)->path).value());

    RenderInput tempo = in;
    tempo.tempo = 60.0;
    tempo.tailSeconds = 0.0;
    auto slow = s->render(tempo);
    REQUIRE(slow);
    CHECK((*slow)->frames == 96000);  // 2 s at 60 BPM, no tail

    RenderInput audioIn;
    audioIn.audio = audio::AudioData{};
    auto noInput = s->render(audioIn);
    REQUIRE(!noInput);
    CHECK(noInput.error().code == "render.no_input");
}

TEST_CASE("render: effect-only chain processes audio input") {
    TempDir dir("render-fx");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -20}})")));
    RenderInput in;
    audio::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign(2, std::vector<float>(4800, 0.5f));
    in.audio = a;
    in.tailSeconds = 0.0;
    auto r = s->render(in);
    REQUIRE(r);
    CHECK((*r)->frames == 4800);
    CHECK((*r)->peakDbfs == doctest::Approx(20 * std::log10(0.5) - 20).epsilon(0.001));
    RenderInput notes;
    notes.notes = twoNotes();
    CHECK(s->render(notes).error().code == "render.not_instrument");
    a.sampleRate = 44100;
    in.audio = a;
    CHECK(s->render(in).error().code == "render.input_rate");
}

TEST_CASE("transactions: rollback restores chain, state and renders; IDs stay unique") {
    TempDir dir("txn");
    auto s = openSession(dir);
    const std::string sine = s->load(kSine).value();
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -3}})")));
    RenderInput in;
    in.notes = twoNotes();
    REQUIRE(s->render(in));

    REQUIRE(s->begin());
    CHECK(s->begin().error().code == "sandbox.transaction_nested");
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -30}})")));
    const std::string g2 = s->load(kGain).value();
    REQUIRE(s->remove(sine));
    auto r = s->render([] {
        RenderInput a;
        a.seconds = 0.1;
        return a;
    }());
    REQUIRE(r);
    const fs::path madeInTxn = (*r)->path;
    SessionSettings st = s->settings();
    st.tempo = 90;
    REQUIRE(s->setSettings(st));
    REQUIRE(s->rollback());

    CHECK(s->chain() == std::vector<std::string>{sine, g});
    CHECK(!s->instance(g2).ok());
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-3.0));
    CHECK(s->renders().size() == 1);
    CHECK(!fs::exists(madeInTxn));
    CHECK(s->settings().tempo == 120.0);
    // The restored instrument still works.
    auto after = s->render(in);
    REQUIRE(after);
    CHECK((*after)->id == "rd_3");  // rd_2 was rolled back, never reused
    CHECK(s->load(kGain).value() == "in_4");

    REQUIRE(s->begin());
    REQUIRE(s->remove(g));
    s->commit();
    CHECK(!s->instance(g).ok());
}

TEST_CASE("settings: validation paths, summary, reset") {
    TempDir dir("settings");
    auto s = openSession(dir);
    SessionSettings bad;
    bad.sampleRate = 1000;
    bad.denominator = 3;
    auto e = s->setSettings(bad);
    REQUIRE(!e);
    REQUIRE(e.error().details.size() == 2);
    CHECK(e.error().details[0].path == "/sample_rate");
    CHECK(e.error().details[1].path == "/meter");
    REQUIRE(s->load(kSine));
    RenderInput in;
    in.notes = twoNotes();
    REQUIRE(s->render(in));
    SessionSettings hi;
    hi.sampleRate = 96000;
    REQUIRE(s->setSettings(hi));
    auto r = s->render(in);
    REQUIRE(r);
    CHECK((*r)->sampleRate == 96000);
    const auto sum = s->summary();
    CHECK(sum.find("settings")->find("meter")->asString() == "4/4");
    CHECK(sum.find("instances")->find("in_1"));
    CHECK(sum.find("renders")->size() == 2);
    const fs::path file = (*r)->path;
    s->reset();
    CHECK(s->chain().empty());
    CHECK(s->renders().empty());
    CHECK(!fs::exists(file));
    CHECK(s->settings().sampleRate == 48000);
}

TEST_CASE("sequence events: tick timing, offs before controls before ons, zero-length notes") {
    midi::Sequence p;
    p.ppq = 480;
    p.notes = {{0, 480, 0, 60, 100}, {480, 480, 0, 60, 100}, {960, 0, 9, 36, 127}};
    p.controls = {{480, midi::ControlKind::CC, 0, 74, 90},
                  {960, midi::ControlKind::PitchBend, 0, 0, -8192}};
    const auto ev = sequenceEvents(p, 120.0, 48000.0);
    REQUIRE(ev.size() == 8);
    CHECK(ev[0].sample == 0);
    CHECK(ev[1].sample == 24000);
    CHECK(ev[1].event.kind == host::Event::Kind::NoteOff);  // first note ends,
    CHECK(ev[2].event.kind == host::Event::Kind::Midi);     // the controller moves,
    CHECK(ev[2].event.midi[0] == 0xb0);
    CHECK(ev[2].event.midi[1] == 74);
    CHECK(ev[2].event.midi[2] == 90);
    CHECK(ev[3].event.kind == host::Event::Kind::NoteOn);  // then the next note starts
    CHECK(ev[3].event.noteId == 1);
    CHECK(ev[4].sample == 48000);
    CHECK(ev[4].event.kind == host::Event::Kind::NoteOff);
    CHECK(ev[5].event.midi[0] == 0xe0);  // pitch bend -8192 -> 0x00 0x00
    CHECK(ev[5].event.midi[1] == 0);
    CHECK(ev[5].event.midi[2] == 0);
    CHECK(ev[6].sample == 48000);
    CHECK(ev[6].event.kind == host::Event::Kind::NoteOn);
    CHECK(ev[7].sample == 48001);  // zero-length note lasts one sample
    CHECK(ev[7].event.kind == host::Event::Kind::NoteOff);
    CHECK(ev[7].event.channel == 9);
    CHECK(ticksToSamples(1, 960, 120.0, 48000.0) == 25);  // 25.0 exactly
    CHECK(ticksToSamples(7, 960, 97.0, 44100.0) == 199);  // 198.9…
}

TEST_CASE("the session tick adopts background work and drops finished job threads") {
    TempDir dir("tick");
    auto s = openSession(dir);
    std::mutex mutex;
    std::condition_variable cv;
    bool running = false, finish = false;
    s->startJob([&](supple::StopToken) {
        {
            std::lock_guard lock(mutex);
            running = true;
            cv.notify_all();
        }
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return finish; });
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return running; });
    }
    CHECK(s->jobThreadCountForTests() == 1);
    s->tick();  // still running: it stays
    CHECK(s->jobThreadCountForTests() == 1);
    {
        std::lock_guard lock(mutex);
        finish = true;
    }
    cv.notify_all();
    while (s->activeJobThreadCountForTests() != 0) std::this_thread::yield();
    REQUIRE(s->activeJobThreadCountForTests() == 0);
    s->tick();
    CHECK(s->jobThreadCountForTests() == 0);
}

TEST_CASE("reset joins jobs and discards late background render records") {
    TempDir dir("reset-jobs");
    auto session = openSession(dir);
    auto record = session->reserveRender("late");
    session->startJob([&](supple::StopToken stop) {
        while (!stop.stop_requested()) std::this_thread::yield();
        session->submitRender(std::move(record));
    });
    session->reset();
    session->tick();
    CHECK(session->renders().empty());
}

TEST_CASE("render rejects overflowing and negative MIDI times before converting samples") {
    TempDir dir("invalid-times");
    auto session = openSession(dir);
    REQUIRE(session->load(kSine));
    RenderInput input;
    input.notes.emplace();
    for (const midi::Note note : {midi::Note{INT64_MAX, 1}, midi::Note{1, INT64_MAX},
                                  midi::Note{-1, 1}, midi::Note{0, -1}}) {
        input.notes->notes = {note};
        CHECK_FALSE(session->render(input));
    }
    input.notes->notes.clear();
    input.notes->length = INT64_MAX;
    CHECK_FALSE(session->render(input));
    input.notes->length = 0;
    input.notes->controls = {{INT64_MAX, midi::ControlKind::CC, 0, 1, 0}};
    CHECK_FALSE(session->render(input));
    auto instance = session->instance(session->chain().front());
    REQUIRE(instance);
    host::Instance* chain[] = {*instance};
    SessionSettings settings;
    settings.sampleRate = 0;
    CHECK_FALSE(Session::renderThrough(chain, input, settings));
}

TEST_CASE("transactions refuse unsnapshotable instances and report failed restores") {
    TempDir dir("transaction-errors");
    auto session = openSession(dir);
    auto id = session->load("com.suppleaudio.test.edge");
    REQUIRE(id);
    auto instance = session->instance(*id);
    REQUIRE(instance);
    const std::array<std::uint8_t, 4> cannotSave{7, 0, 0, 0};
    REQUIRE((*instance)->loadState(cannotSave));
    auto begin = session->begin();
    REQUIRE_FALSE(begin);
    CHECK(begin.error().code == "sandbox.snapshot_failed");
    CHECK(begin.error().path == "/instances/" + *id);
    REQUIRE(begin.error().details.size() == 1);
    const std::array<std::uint8_t, 4> good{};
    REQUIRE((*instance)->loadState(good));
    REQUIRE(session->begin());
    const std::array<std::uint8_t, 4> cannotRestore{8, 0, 0, 0};
    REQUIRE((*instance)->loadState(cannotRestore));
    auto rollback = session->rollback();
    REQUIRE_FALSE(rollback);
    CHECK(rollback.error().code == "sandbox.rollback_failed");
    REQUIRE(rollback.error().details.size() == 1);
    CHECK(rollback.error().details[0].path == "/instances/" + *id);
}

TEST_CASE("malformed semantic schemas load as plain parameter plugins") {
    TempDir dir("schema-fallback");
    auto session = openSession(dir);
    auto id = session->load("com.suppleaudio.test.malformed-schema");
    REQUIRE(id);
    CHECK(session->schema(*id).isNull());
    auto values = json::parse(R"({"0": 0.75})");
    REQUIRE(values);
    REQUIRE(session->paramSet(*id, *values));
}

TEST_CASE("preset schemas and versions reject future formats but allow legacy files") {
    TempDir dir("preset-version");
    auto session = openSession(dir);
    auto id = session->load(kGain);
    REQUIRE(id);
    auto path = session->presetSave(*id, "Versioned");
    REQUIRE(path);
    auto text = readTextFile(*path);
    REQUIRE(text);
    auto document = json::parse(*text);
    REQUIRE(document);
    (*document)["schema"] = "supple.preset/2.0";
    REQUIRE(writeFileAtomic(*path, json::writeCompact(*document)));
    auto future = session->presetLoad(*id, "Versioned");
    REQUIRE_FALSE(future);
    CHECK(future.error().code == "sandbox.preset_schema");
    (*document)["schema"] = "supple.preset/1.0";
    (*document)["version"] = 2;
    REQUIRE(writeFileAtomic(*path, json::writeCompact(*document)));
    CHECK(session->presetLoad(*id, "Versioned").error().code == "sandbox.preset_version");
    json::Value legacy = json::Value::object();
    legacy["plugin"] = (*document)["plugin"];
    legacy["patch"] = (*document)["patch"];
    REQUIRE(writeFileAtomic(*path, json::writeCompact(legacy)));
    REQUIRE(session->presetLoad(*id, "Versioned"));
}

TEST_CASE("invocation savepoints restore changes without closing the enclosing transaction") {
    TempDir dir("savepoint");
    auto session = openSession(dir);
    auto original = session->load(kGain);
    REQUIRE(original);
    REQUIRE(session->begin());
    auto added = session->load(kGain);
    REQUIRE(added);
    auto checkpoint = session->invocationSavepoint();
    REQUIRE(checkpoint);
    REQUIRE(session->remove(*original));
    REQUIRE(session->remove(*added));
    REQUIRE(session->load(kGain));
    REQUIRE((*checkpoint)());
    CHECK(session->inTransaction());
    CHECK(session->chain() == std::vector<std::string>{*original, *added});
    REQUIRE(session->rollback());
    CHECK(session->chain() == std::vector<std::string>{*original});
}
