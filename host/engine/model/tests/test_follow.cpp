// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A take's context and follow (schema 2.21, ADR-0098 "Amendment: following is
// playback", session-model "Take context and follow"): the fields, the file, the
// invariants, a note's place, and what a note sounds as.
#include <doctest/doctest.h>
#include <supple/harmony/harmony.h>
#include <supple/json/parse.h>
#include <supple/json/write.h>
#include <supple/model/context.h>
#include <supple/model/follow.h>
#include <supple/model/launcher.h>
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>

#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace supple;
using namespace supple::model;

namespace {

ContextPoint keyPt(double beat, const char* tonic, const char* scale) {
    ContextPoint p;
    p.beat = beat;
    p.type = ContextType::Key;
    p.tonic = tonic;
    p.scale = scale;
    return p;
}
ContextPoint chordPt(double beat, const char* symbol) {
    ContextPoint p;
    p.beat = beat;
    p.type = ContextType::Chord;
    p.symbol = symbol;
    return p;
}

midi::Note note(std::int64_t startBeat, std::int64_t lengthBeats, int pitch = 60, int channel = 0) {
    midi::Note n;
    n.start = startBeat * 960;
    n.length = lengthBeats * 960;
    n.pitch = static_cast<std::uint8_t>(pitch);
    n.channel = static_cast<std::uint8_t>(channel);
    return n;
}

// A channel with one MIDI item holding one take of one source.
struct Rig {
    Session s;
    Id track, midi, take, item;
};

struct RigSpec {
    std::vector<midi::Note> notes;
    std::int64_t lengthBeats = 8;  // the source's length
    double position = 0, length = 8, startOffset = 0, playrate = 1;
    bool loop = false;
    std::vector<ContextPoint> context;  // the take's
    std::optional<double> period;       // its context_period
    double phase = 0;                   // its context_phase
    ClipFollow follow = ClipFollow::None;
    std::vector<ContextPoint> project;  // the project's context track
};

Rig rig(const RigSpec& spec) {
    Rig r;
    EditTxn t(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    r.track = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(r.track); });
    MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = spec.lengthBeats * 960;
    src.sequence.notes = spec.notes;
    r.midi = t.create(src);
    Take take;
    take.source = r.midi;
    take.startOffset = spec.startOffset;
    take.playrate = spec.playrate;
    take.context = spec.context;
    take.contextPeriod = spec.period;
    take.contextPhase = spec.phase;
    take.follow = spec.follow;
    r.take = t.create(take);
    Item item;
    item.track = r.track;
    item.position = spec.position;
    item.length = spec.length;
    item.loop = spec.loop;
    item.takes = {r.take};
    item.activeTake = r.take;
    r.item = t.create(item);
    for (const auto& p : spec.project) t.create(p);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    r.s = *s;
    return r;
}

template <class T>
Result<Session> edit(const Session& s, Id id, std::function<void(T&)> f) {
    EditTxn t(s, s.version + 1);
    t.modify<T>(id, std::move(f));
    return t.commit();
}

std::string messages(const Error& e) {
    std::string out = e.path + " " + e.message + "\n";
    for (const auto& d : e.details) out += d.path + " " + d.message + "\n";
    return out;
}
bool says(const Result<Session>& r, const std::string& text) {
    return !r && messages(r.error()).find(text) != std::string::npos;
}

harmony::Key key(const char* tonic, harmony::Scale scale) {
    return harmony::Key{*harmony::parsePitchClass(tonic), scale};
}
harmony::Chord chord(const char* symbol) { return harmony::parseChord(symbol).value(); }

bool hasCode(const std::vector<schema::Warning>& warnings, const std::string& code) {
    for (const auto& w : warnings)
        if (w.code == code) return true;
    return false;
}

// What an item sounds, as (note, song beat in 1/1e6, context beat in 1/1e6, pitch):
// two occurrences are the same note in the same place when these agree, whatever
// pass numbering the item gives them.
using Heard = std::tuple<std::uint32_t, long, long, int>;
std::set<Heard> heard(const Session& s, Id item, double from = -1e6, double to = 1e6) {
    std::set<Heard> out;
    for (const SoundingNote& n : soundingNotes(s, item, from, to))
        out.insert({n.note, std::lround(n.songBeat * 1e6), std::lround(n.contextBeat * 1e6),
                    n.sounds.pitch});
    return out;
}
bool subset(const std::set<Heard>& small, const std::set<Heard>& big) {
    for (const Heard& h : small)
        if (!big.contains(h)) return false;
    return true;
}

// The project reads F major from the start, then A minor from beat 8, and the take
// reads C major, then Eb major from its beat 4 (a context of period 8).
RigSpec varied() {
    RigSpec spec;
    spec.notes = {note(0, 2, 60), note(2, 2, 64), note(4, 2, 67), note(6, 1, 72)};
    spec.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major")};
    spec.period = 8;
    spec.follow = ClipFollow::Key;
    spec.project = {keyPt(0, "F", "major"), keyPt(8, "A", "minor")};
    return spec;
}

}  // namespace

// --- the file ---------------------------------------------------------------------------

TEST_CASE("take context: a plain take writes none of the four members") {
    const Rig r = rig({});
    const std::string bytes = serializeProject(r.s);
    CHECK(bytes.find("\"supple.project/2.21\"") != std::string::npos);
    CHECK(bytes.find("context_period") == std::string::npos);
    CHECK(bytes.find("context_phase") == std::string::npos);
    CHECK(bytes.find("\"follow\"") == std::string::npos);
}

TEST_CASE("take context: the four members save, reopen and serialize the same") {
    RigSpec spec = varied();
    spec.phase = 1.5;
    spec.context.push_back(chordPt(0, "C"));
    spec.context.push_back(chordPt(2, "F"));
    std::stable_sort(spec.context.begin(), spec.context.end(), [](const auto& a, const auto& b) {
        return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
    });
    const Rig r = rig(spec);
    const std::string bytes = serializeProject(r.s);
    CHECK(bytes.find("\"context_period\": 8") != std::string::npos);
    CHECK(bytes.find("\"context_phase\": 1.5") != std::string::npos);
    CHECK(bytes.find("\"follow\": \"key\"") != std::string::npos);
    std::vector<schema::Warning> warnings;
    auto again = parseProject(bytes, &warnings);
    REQUIRE_MESSAGE(again, (again ? "" : again.error().toString()));
    CHECK(warnings.empty());
    const Take& was = *r.s.takes.find(r.take);
    const Take& now = *again->takes.find(r.take);
    CHECK(now.context == was.context);
    CHECK(now.contextPeriod == std::optional<double>(8.0));
    CHECK(now.contextPhase == 1.5);
    CHECK(now.follow == ClipFollow::Key);
    CHECK(serializeProject(*again) == bytes);
}

TEST_CASE("take context: a 2.20 file opens unchanged") {
    const Rig r = rig({});
    std::string bytes = serializeProject(r.s);
    const auto at = bytes.find("supple.project/2.21");
    REQUIRE(at != std::string::npos);
    bytes.replace(at, 19, "supple.project/2.20");
    std::vector<schema::Warning> warnings;
    auto old = parseProject(bytes, &warnings);
    REQUIRE_MESSAGE(old, (old ? "" : old.error().toString()));
    CHECK(warnings.empty());
    const Take& take = *old->takes.find(r.take);
    CHECK(take.context.empty());
    CHECK_FALSE(take.contextPeriod);
    CHECK(take.contextPhase == 0);
    CHECK(take.follow == ClipFollow::None);
}

namespace {
// The file with the take's JSON changed by `f`, opened with its warnings.
Result<Session> openChanged(const Session& s, Id take, const std::function<void(json::Value&)>& f,
                            std::vector<schema::Warning>& warnings) {
    auto doc = json::parse(serializeProject(s)).value();
    f(doc["takes"][take.str()]);
    return parseProject(json::writeCompact(doc), &warnings);
}
json::Value pointJson(const ContextPoint& p) { return toJson(p); }
}  // namespace

TEST_CASE("take context: the open never fails, and says what it dropped") {
    RigSpec spec = varied();
    const Rig r = rig(spec);
    SUBCASE("points against the rules") {
        std::vector<schema::Warning> warnings;
        auto s = openChanged(
            r.s, r.take,
            [&](json::Value& tk) {
                auto points = json::Value::array();
                points.push_back(pointJson(keyPt(0, "C", "major")));
                points.push_back(pointJson(keyPt(0, "D", "major")));   // a second at beat 0
                points.push_back(pointJson(keyPt(-1, "G", "major")));  // before the start
                points.push_back(pointJson(keyPt(8, "G", "major")));   // at the period
                points.push_back(pointJson(keyPt(4, "Eb", "major")));
                tk["context"] = std::move(points);
            },
            warnings);
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        const Take& take = *s->takes.find(r.take);
        CHECK(take.context ==
              std::vector<ContextPoint>{keyPt(0, "C", "major"), keyPt(4, "Eb", "major")});
        CHECK(hasCode(warnings, "take.context_dropped"));
        CHECK(take.follow == ClipFollow::Key);  // still has a readable key
    }
    SUBCASE("a period that is not positive reads as no period") {
        std::vector<schema::Warning> warnings;
        auto s =
            openChanged(r.s, r.take, [](json::Value& tk) { tk["context_period"] = 0; }, warnings);
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        CHECK_FALSE(s->takes.find(r.take)->contextPeriod);
        CHECK(s->takes.find(r.take)->context.size() == 2);
        CHECK(hasCode(warnings, "take.context_dropped"));
    }
    SUBCASE("a follow this build does not know reads as none") {
        std::vector<schema::Warning> warnings;
        auto s =
            openChanged(r.s, r.take, [](json::Value& tk) { tk["follow"] = "arpeggio"; }, warnings);
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        CHECK(s->takes.find(r.take)->follow == ClipFollow::None);
        CHECK(hasCode(warnings, "take.follow_reset"));
    }
    SUBCASE("follow without a readable key reads as none") {
        std::vector<schema::Warning> warnings;
        auto s = openChanged(
            r.s, r.take,
            [&](json::Value& tk) {
                auto points = json::Value::array();
                points.push_back(pointJson(chordPt(0, "C")));
                tk["context"] = std::move(points);
            },
            warnings);
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        CHECK(s->takes.find(r.take)->follow == ClipFollow::None);
        CHECK(hasCode(warnings, "take.follow_reset"));
        warnings.clear();
        auto unreadable = openChanged(
            r.s, r.take,
            [&](json::Value& tk) {
                auto points = json::Value::array();
                points.push_back(pointJson(keyPt(0, "C", "lydian_dominant_9000")));
                tk["context"] = std::move(points);
            },
            warnings);
        REQUIRE_MESSAGE(unreadable, (unreadable ? "" : unreadable.error().toString()));
        CHECK(unreadable->takes.find(r.take)->context.size() == 1);  // kept as data
        CHECK(unreadable->takes.find(r.take)->follow == ClipFollow::None);
    }
}

TEST_CASE("take context: an audio take holds none of the four") {
    EditTxn t(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(track); });
    const Id media = t.create(MediaSource{"a.wav", "", "wav", 48000, 1, 48000});
    Take take;
    take.source = media;
    const Id tk = t.create(take);
    Item item;
    item.track = track;
    item.length = 4;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const Session s = t.commit().value();
    CHECK(says(edit<Take>(s, tk, [](Take& x) { x.context = {keyPt(0, "C", "major")}; }),
               "holds no context"));
    CHECK(says(edit<Take>(s, tk, [](Take& x) { x.follow = ClipFollow::Key; }), "holds no context"));
    CHECK(says(edit<Take>(s, tk, [](Take& x) { x.contextPhase = 1; }), "holds no context"));
    CHECK(says(edit<Take>(s, tk, [](Take& x) { x.contextPeriod = 4; }), "holds no context"));
    // The file: an audio take's members are dropped with a note each, and it opens.
    std::vector<schema::Warning> warnings;
    auto doc = json::parse(serializeProject(s)).value();
    auto points = json::Value::array();
    points.push_back(pointJson(keyPt(0, "C", "major")));
    doc["takes"][tk.str()]["context"] = std::move(points);
    doc["takes"][tk.str()]["follow"] = "key";
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(opened->takes.find(tk)->context.empty());
    CHECK(opened->takes.find(tk)->follow == ClipFollow::None);
    CHECK(hasCode(warnings, "take.context_dropped"));
    CHECK(hasCode(warnings, "take.follow_reset"));
}

TEST_CASE("take context: a clip's takes hold none of the four") {
    EditTxn t(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(track); });
    const Id scene = t.create(Scene{"Verse", "", 0});
    const Id src = t.create(MidiSource{});
    Take take;
    take.source = src;
    const Id tk = t.create(take);
    Clip clip;
    clip.track = track;
    clip.scene = scene;
    clip.takes = {tk};
    clip.activeTake = tk;
    clip.length = 8;
    clip.context = {keyPt(0, "C", "major")};
    clip.follow = ClipFollow::Key;
    t.create(clip);
    const Session s = t.commit().value();
    const auto held = [&](std::function<void(Take&)> f) { return edit<Take>(s, tk, std::move(f)); };
    CHECK(says(held([](Take& x) { x.context = {keyPt(0, "C", "major")}; }), "a clip's own"));
    CHECK(says(held([](Take& x) { x.contextPeriod = 8; }), "a clip's own"));
    CHECK(says(held([](Take& x) { x.contextPhase = 1; }), "a clip's own"));
    CHECK(says(held([](Take& x) {
                   x.context = {keyPt(0, "C", "major")};
                   x.follow = ClipFollow::Key;
               }),
               "a clip's own"));
    // The file: the members are dropped, the clip's own stay.
    std::vector<schema::Warning> warnings;
    auto doc = json::parse(serializeProject(s)).value();
    auto points = json::Value::array();
    points.push_back(pointJson(keyPt(0, "D", "minor")));
    doc["takes"][tk.str()]["context"] = std::move(points);
    doc["takes"][tk.str()]["follow"] = "key";
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(opened->takes.find(tk)->context.empty());
    CHECK(opened->takes.find(tk)->follow == ClipFollow::None);
    CHECK(opened->clips.begin()->value->follow == ClipFollow::Key);
    CHECK(opened->clips.begin()->value->context.size() == 1);
    CHECK(hasCode(warnings, "take.context_dropped"));
    CHECK(hasCode(warnings, "take.follow_reset"));
}

// --- the invariants, at commit ------------------------------------------------------------

TEST_CASE("take context: points are sorted, unique and inside the period") {
    const Rig r = rig(varied());
    const auto set = [&](std::vector<ContextPoint> points, std::optional<double> period = 8.0,
                         double phase = 0, ClipFollow follow = ClipFollow::None) {
        return edit<Take>(r.s, r.take, [&](Take& t) {
            t.context = std::move(points);
            t.contextPeriod = period;
            t.contextPhase = phase;
            t.follow = follow;
        });
    };
    CHECK(set({keyPt(0, "C", "major"), chordPt(0, "C"), chordPt(7.5, "G")}));
    CHECK(says(set({chordPt(4, "A7"), chordPt(0, "Dm")}), "not sorted"));
    CHECK(says(set({chordPt(0, "Dm"), chordPt(0, "Am")}), "two points of one type"));
    CHECK(says(set({keyPt(8, "D", "minor")}), "at or past the context's period"));
    CHECK(says(set({keyPt(-1, "D", "minor")}), "beat 0 or later"));
    // Without a period the points may lie anywhere from 0.
    CHECK(set({keyPt(0, "C", "major"), keyPt(100, "D", "minor")}, std::nullopt));
    CHECK(says(set({}, 0.0), "period"));
    CHECK(says(set({}, -4.0), "period"));
    CHECK(says(set({}, std::numeric_limits<double>::infinity()), "period"));
    CHECK(says(set({}, 8.0, std::numeric_limits<double>::quiet_NaN()), "phase"));
    CHECK(set({}, 8.0, -3.5));  // a phase may be negative
}

TEST_CASE("take context: at most 4096 points of a type") {
    const Rig r = rig({});
    const auto big = edit<Take>(r.s, r.take, [&](Take& t) {
        for (std::size_t i = 0; i < kMaxContextPoints; ++i)
            t.context.push_back(chordPt(static_cast<double>(i), "C"));
    });
    REQUIRE_MESSAGE(big, (big ? "" : big.error().toString()));
    CHECK(says(edit<Take>(*big, r.take, [](Take& t) { t.context.push_back(chordPt(5000.0, "C")); }),
               "context points of one type"));
}

TEST_CASE("take context: follow needs a readable key in the context") {
    const Rig r = rig(varied());
    const auto follow = [&](std::vector<ContextPoint> points) {
        return edit<Take>(r.s, r.take, [&](Take& t) {
            t.context = std::move(points);
            t.follow = ClipFollow::Chords;
        });
    };
    CHECK(follow({keyPt(0, "C", "major")}));
    CHECK(follow({keyPt(4, "C", "major")}));  // its first key holds before it: no key at 0 needed
    CHECK(says(follow({}), "follow needs a readable key"));
    CHECK(says(follow({chordPt(0, "C")}), "follow needs a readable key"));
    CHECK(says(follow({keyPt(0, "C", "nonesuch")}), "follow needs a readable key"));
}

// --- fitting a context made by an edit ---------------------------------------------------------

TEST_CASE("take context: fitTakeContext sorts, drops, and resets follow") {
    Take take;
    take.source = Id{Kind::Midi, 1};
    take.context = {chordPt(2.0004, "F"), keyPt(0, "C", "major"), chordPt(2, "G"), chordPt(9, "C"),
                    keyPt(-1, "D", "minor")};
    take.contextPeriod = 8;
    take.follow = ClipFollow::Chords;
    const auto notes = fitTakeContext(take, TakeHome::Item);
    // 2.0004 rounds to the grid (2.000, 1/960 = 0.00104); the second chord at 2 is dropped as a
    // duplicate.
    CHECK(take.context.size() == 2);
    CHECK(take.context[0].type == ContextType::Key);
    CHECK(take.context[1].symbol == "F");
    int dropped = 0;
    for (const auto& n : notes) dropped += n.code == "take.context_dropped";
    CHECK(dropped == 3);
    CHECK(take.follow == ClipFollow::Chords);
    // An edit that leaves no readable key resets follow, with its note.
    take.context = {chordPt(0, "C")};
    const auto again = fitTakeContext(take, TakeHome::Item);
    CHECK(take.follow == ClipFollow::None);
    REQUIRE(again.size() == 1);
    CHECK(again[0].code == "take.follow_reset");
    CHECK(fitTakeContext(take, TakeHome::Item).empty());  // idempotent
    // A phase that is not finite reads as 0, and a period that is not positive goes.
    take.contextPhase = std::numeric_limits<double>::quiet_NaN();
    take.contextPeriod = -1;
    const auto bad = fitTakeContext(take, TakeHome::Item);
    CHECK(take.contextPhase == 0);
    CHECK_FALSE(take.contextPeriod);
    CHECK(bad.size() == 2);
    // A clip's take, and a take with no source or a media source, hold nothing.
    take.context = {keyPt(0, "C", "major")};
    take.follow = ClipFollow::Key;
    take.contextPhase = 2;
    fitTakeContext(take, TakeHome::Clip);
    CHECK(take.context.empty());
    CHECK(take.follow == ClipFollow::None);
    CHECK(take.contextPhase == 0);
}

// --- reading a take's context -------------------------------------------------------------

TEST_CASE("take context: the first point of a type holds before it") {
    const std::vector<ContextPoint> points = {keyPt(4, "D", "minor"), chordPt(4, "Dm"),
                                              keyPt(8, "G", "major")};
    CHECK(contextKeyPointAt(points, 0)->tonic == "D");
    CHECK(contextKeyPointAt(points, 3.9)->tonic == "D");
    CHECK(contextKeyPointAt(points, 8)->tonic == "G");
    CHECK(contextChordPointAt(points, 0)->symbol == "Dm");  // before the first chord, the first
    CHECK(contextKeyPointAt({}, 0) == nullptr);
    CHECK(contextChordPointAt({}, 5) == nullptr);
    // Within a millionth of a beat below a point, the point holds (float noise at a boundary).
    CHECK(contextKeyPointAt(points, 8 - 1e-9)->tonic == "G");
    CHECK(contextKeyPointAt(points, 8 - 1e-3)->tonic == "D");
}

TEST_CASE(
    "take context: the context beat is the unwrapped source beat plus the phase, modulo the "
    "period") {
    Take take;
    CHECK(contextBeatOf(take, 9) == 9);  // no period: unwrapped
    CHECK(contextBeatOf(take, -3) == -3);
    take.contextPeriod = 8;
    CHECK(contextBeatOf(take, 9) == 1);
    CHECK(contextBeatOf(take, -1) == 7);
    CHECK(contextBeatOf(take, 16) == 0);
    CHECK(contextBeatOf(take, 8 - 1e-9) == 0);  // float noise below the period reads as its start
    take.contextPhase = 2;
    CHECK(contextBeatOf(take, 3) == 5);
    CHECK(contextBeatOf(take, 7) == 1);
}

// --- what a note sounds as ----------------------------------------------------------------

TEST_CASE("soundingKey agrees with harmony::follow over the whole key range") {
    const Rig r = rig(varied());
    const ContextView& project = *r.s.contextView;
    std::vector<ContextPoint> context = {keyPt(0, "D", "minor"), chordPt(0, "Dm"),
                                         chordPt(4, "A7")};
    harmony::Place from0{key("D", harmony::Scale::Minor), chord("Dm")};
    harmony::Place from5{key("D", harmony::Scale::Minor), chord("A7")};
    harmony::Place to0{key("F", harmony::Scale::Major), {}};
    harmony::Place to9{key("A", harmony::Scale::Minor), {}};
    for (int pitch = 0; pitch < 128; ++pitch) {
        for (int channel : {0, 9}) {
            for (auto [cb, songBeat, from, to] :
                 {std::tuple{0.0, 0.0, from0, to0}, std::tuple{5.0, 9.0, from5, to9}}) {
                const auto got =
                    soundingKey(context, ClipFollow::Key, project, cb, songBeat, channel, pitch);
                if (channel == 9) {
                    CHECK(got.pitch == pitch);
                    CHECK(got.by == harmony::FollowedBy::Copy);
                    continue;
                }
                const auto want = harmony::follow(pitch, from, to, harmony::FollowMode::Key);
                CHECK(got.pitch == want.pitch);
                CHECK(got.by == want.by);
                CHECK(got.folded == want.folded);
            }
        }
    }
}

TEST_CASE("soundingKey in chords mode, and with a side missing") {
    RigSpec spec = varied();
    spec.project = {keyPt(0, "F", "major"), keyPt(8, "A", "minor"), chordPt(0, "F"),
                    chordPt(8, "Am")};
    const Rig r = rig(spec);
    const ContextView& project = *r.s.contextView;
    const std::vector<ContextPoint> context = {keyPt(0, "C", "major"), chordPt(0, "C"),
                                               chordPt(4, "G7")};
    const harmony::Place from{key("C", harmony::Scale::Major), chord("G7")};
    const harmony::Place to{key("A", harmony::Scale::Minor), chord("Am")};
    for (int pitch = 0; pitch < 128; ++pitch) {
        const auto want = harmony::follow(pitch, from, to, harmony::FollowMode::Chords);
        CHECK(soundingKey(context, ClipFollow::Chords, project, 5, 9, 0, pitch).pitch ==
              want.pitch);
    }
    // None never maps; a project with no key where the note starts leaves it as written.
    CHECK(soundingKey(context, ClipFollow::None, project, 0, 0, 0, 61).pitch == 61);
    RigSpec later = varied();
    later.project = {keyPt(4, "F", "major")};
    const Rig bare = rig(later);
    const auto early = soundingKey(context, ClipFollow::Key, *bare.s.contextView, 0, 3, 0, 61);
    CHECK(early.pitch == 61);
    CHECK(early.by == harmony::FollowedBy::Copy);
    // A key this build cannot read on the take's side: as written.
    const std::vector<ContextPoint> unreadable = {keyPt(0, "C", "nonesuch")};
    CHECK(soundingKey(unreadable, ClipFollow::Key, project, 0, 0, 0, 61).pitch == 61);
    // Before the first chord point of a chords take, the first chord holds.
    const std::vector<ContextPoint> late = {keyPt(0, "C", "major"), chordPt(4, "G7")};
    CHECK(soundingKey(late, ClipFollow::Chords, project, 0, 9, 0, 62).pitch ==
          harmony::follow(62, from, to, harmony::FollowMode::Chords).pitch);
}

TEST_CASE("soundingNotes: a plain item, in the item's place") {
    RigSpec spec = varied();
    spec.position = 4;
    spec.length = 8;
    spec.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major")};
    const Rig r = rig(spec);
    const auto notes = soundingNotes(r.s, r.item, 0, 100);
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].note == 0);
    CHECK(notes[0].songBeat == 4);
    CHECK(notes[0].contextBeat == 0);
    CHECK(notes[0].pass == 0);
    CHECK(notes[1].songBeat == 6);
    CHECK(notes[2].songBeat == 8);
    CHECK(notes[2].contextBeat == 4);
    CHECK(notes[0].written == 60);
    CHECK(notes[0].channel == 0);
    // Song beat 4 reads the project's F major; note 2 sits at song beat 8: A minor.
    CHECK(notes[0].sounds.pitch == harmony::follow(60, {key("C", harmony::Scale::Major), {}},
                                                   {key("F", harmony::Scale::Major), {}},
                                                   harmony::FollowMode::Key)
                                       .pitch);
    CHECK(notes[2].sounds.pitch == harmony::follow(67, {key("Eb", harmony::Scale::Major), {}},
                                                   {key("A", harmony::Scale::Minor), {}},
                                                   harmony::FollowMode::Key)
                                       .pitch);
    // A range keeps the occurrences whose heard start falls in it.
    CHECK(soundingNotes(r.s, r.item, 6, 8).size() == 1);
    CHECK(soundingNotes(r.s, r.item, 0, 4).empty());
    // A take that does not follow sounds as written, in the same places.
    const Rig plain = rig([&] {
        RigSpec p = spec;
        p.follow = ClipFollow::None;
        return p;
    }());
    for (const auto& n : soundingNotes(plain.s, plain.item, 0, 100)) {
        CHECK(n.sounds.pitch == n.written);
        CHECK(n.sounds.by == harmony::FollowedBy::Copy);
    }
}

TEST_CASE(
    "soundingNotes: a context of period 8 over 4 beats of looping content reads the second half in "
    "the second pass") {
    RigSpec spec;
    spec.notes = {note(0, 1, 60), note(2, 1, 64)};
    spec.lengthBeats = 4;
    spec.length = 8;
    spec.loop = true;
    spec.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major")};
    spec.period = 8;
    spec.follow = ClipFollow::Key;
    spec.project = {keyPt(0, "F", "major")};
    const Rig r = rig(spec);
    const auto notes = soundingNotes(r.s, r.item, 0, 100);
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].pass == 0);
    CHECK(notes[2].pass == 1);
    CHECK(notes[0].contextBeat == 0);
    CHECK(notes[1].contextBeat == 2);
    CHECK(notes[2].contextBeat == 4);
    CHECK(notes[3].contextBeat == 6);
    CHECK(notes[2].songBeat == 4);
    const harmony::Place to{key("F", harmony::Scale::Major), {}};
    CHECK(notes[0].sounds.pitch ==
          harmony::follow(60, {key("C", harmony::Scale::Major), {}}, to, harmony::FollowMode::Key)
              .pitch);
    CHECK(notes[2].sounds.pitch ==
          harmony::follow(60, {key("Eb", harmony::Scale::Major), {}}, to, harmony::FollowMode::Key)
              .pitch);
    CHECK(notes[0].sounds.pitch != notes[2].sounds.pitch);
    // The same at a take playrate of 2: 8 content beats in 4 item beats, and the
    // context beats run at the rate, so the context beat of a note is the same.
    RigSpec fast = spec;
    fast.playrate = 2;
    fast.length = 4;
    const Rig q = rig(fast);
    const auto quick = soundingNotes(q.s, q.item, 0, 100);
    REQUIRE(quick.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(quick[i].contextBeat == notes[i].contextBeat);
        CHECK(quick[i].songBeat == notes[i].songBeat / 2);
    }
}

TEST_CASE("soundingNotes: the context beat is a function of the source beat, so a slip keeps it") {
    RigSpec spec;
    spec.notes = {note(2, 1, 60), note(3, 1, 62), note(6, 1, 64)};
    spec.length = 4;
    spec.position = 4;
    spec.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major")};
    spec.follow = ClipFollow::Key;
    spec.project = {keyPt(0, "F", "major")};
    const Rig r = rig(spec);
    const auto before = soundingNotes(r.s, r.item, 0, 100);
    REQUIRE(before.size() == 2);  // the item plays source beats 0..4
    CHECK(before[0].contextBeat == 2);
    CHECK(before[0].songBeat == 6);
    // A slip of 1: the same notes, one beat earlier, the same place in the context.
    const Session slipped = edit<Take>(r.s, r.take, [](Take& t) { t.startOffset = 1; }).value();
    const auto after = soundingNotes(slipped, r.item, 0, 100);
    REQUIRE(after.size() == 2);
    CHECK(after[0].note == before[0].note);
    CHECK(after[0].contextBeat == before[0].contextBeat);
    CHECK(after[0].songBeat == before[0].songBeat - 1);
    // A phase moves the context under the notes.
    const Session phased = edit<Take>(r.s, r.take, [](Take& t) { t.contextPhase = 4; }).value();
    CHECK(soundingNotes(phased, r.item, 0, 100)[0].contextBeat == 6);
    // A looped item whose offset has passed a whole cycle: the unwrapped source beat counts it.
    RigSpec looped = spec;
    looped.lengthBeats = 4;
    looped.loop = true;
    looped.startOffset = 5;  // one cycle and a beat in
    looped.notes = {note(1, 1, 60)};
    looped.length = 3;
    const Rig l = rig(looped);
    const auto ln = soundingNotes(l.s, l.item, 0, 100);
    REQUIRE(ln.size() >= 1);
    // The note at source beat 1 first sounds at unwrapped beat 5 (offset 5): item beat 0.
    CHECK(ln[0].contextBeat == 5);
    CHECK(ln[0].songBeat == 4);
}

TEST_CASE("soundingNotes: a chased note, a note before song beat 0, a crossing note") {
    SUBCASE("chased: the item starts inside a note and the note maps by its own start") {
        RigSpec spec;
        spec.notes = {note(0, 4, 60)};
        spec.position = 2;
        spec.length = 4;
        spec.startOffset = 1;  // the note began one beat before the item's edge
        spec.context = {keyPt(0, "C", "major"), keyPt(1, "Eb", "major")};
        spec.follow = ClipFollow::Key;
        spec.project = {keyPt(0, "F", "major"), keyPt(2, "A", "minor")};
        const Rig r = rig(spec);
        const auto notes = soundingNotes(r.s, r.item, 0, 100);
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].songBeat == 1);     // where it would have sounded
        CHECK(notes[0].contextBeat == 0);  // by its own start, not by the item's edge
        CHECK(notes[0].sounds.pitch == harmony::follow(60, {key("C", harmony::Scale::Major), {}},
                                                       {key("F", harmony::Scale::Major), {}},
                                                       harmony::FollowMode::Key)
                                           .pitch);
        // Its heard start is the item's start: the range selects it there.
        CHECK(soundingNotes(r.s, r.item, 2, 3).size() == 1);
        CHECK(soundingNotes(r.s, r.item, 0, 2).empty());
    }
    SUBCASE("a note that would have sounded before song beat 0 sounds as written") {
        RigSpec spec;
        spec.notes = {note(0, 4, 60)};
        spec.position = 0;
        spec.length = 4;
        spec.startOffset = 1;
        spec.context = {keyPt(0, "C", "major")};
        spec.period = 8;
        spec.follow = ClipFollow::Key;
        spec.project = {keyPt(0, "F", "major")};
        const Rig r = rig(spec);
        const auto notes = soundingNotes(r.s, r.item, -10, 100);
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].songBeat == -1);
        CHECK(notes[0].contextBeat == 0);  // wraps: (phase 0 + source beat 0) mod 8
        CHECK(notes[0].sounds.pitch == 60);
        CHECK(notes[0].sounds.by == harmony::FollowedBy::Copy);
    }
    SUBCASE("a crossing note from the loop's previous pass maps by its own start") {
        RigSpec spec;
        spec.notes = {note(3, 2, 60)};  // beats 3 to 5 of a 4-beat source
        spec.lengthBeats = 4;
        spec.loop = true;
        spec.position = 4;
        spec.length = 4;
        spec.context = {keyPt(0, "C", "major")};
        spec.follow = ClipFollow::Key;
        spec.project = {keyPt(0, "F", "major"), keyPt(4, "A", "minor")};
        const Rig r = rig(spec);
        const auto notes = soundingNotes(r.s, r.item, 0, 100);
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].pass == -1);
        CHECK(notes[0].songBeat == 3);  // the preceding cycle's note, where it began
        CHECK(notes[0].contextBeat ==
              -1);  // unwrapped source beat 3 - 4, no period: the first key holds
        CHECK(notes[1].pass == 0);
        CHECK(notes[1].songBeat == 7);
        // The crossing fragment maps by project beat 3 (F major), not by the item's start.
        CHECK(notes[0].sounds.pitch == harmony::follow(60, {key("C", harmony::Scale::Major), {}},
                                                       {key("F", harmony::Scale::Major), {}},
                                                       harmony::FollowMode::Key)
                                           .pitch);
        CHECK(notes[1].sounds.pitch == harmony::follow(60, {key("C", harmony::Scale::Major), {}},
                                                       {key("A", harmony::Scale::Minor), {}},
                                                       harmony::FollowMode::Key)
                                           .pitch);
    }
}

// --- invariance: edits that do not mean to change a note's place ---------------------------------

namespace {

// What the commands do to the item and its take, as the contract says.
Session splitAt(const Session& s, Id item, Id take, double at, Id* rightItem, bool wrap = false) {
    EditTxn t(s, s.version + 1);
    const Item old = *s.items.find(item);
    Take right = *s.takes.find(take);
    const double delta = (at - old.position) * right.playrate;
    right.startOffset += delta;
    if (wrap && old.loop) {  // a looped item only: an unlooped one plays its source once
        const MidiSource& src = *s.midi.find(*right.source);
        wrapStartOffset(right, static_cast<double>(src.sequence.length) / src.sequence.ppq);
    }
    const Id rightTake = t.create(right);
    Item left = old, rightI = old;
    left.length = at - old.position;
    rightI.position = at;
    rightI.length = old.position + old.length - at;
    rightI.takes = {rightTake};
    rightI.activeTake = rightTake;
    t.put<Item>(item, left);
    *rightItem = t.create(rightI);
    return t.commit().value();
}

}  // namespace

TEST_CASE("invariance: split keeps every note's place and what it sounds as") {
    for (const bool loop : {false, true}) {
        for (const double rate : {1.0, 2.0}) {
            for (const bool wrap : {false, true}) {
                RigSpec spec = varied();
                spec.loop = loop;
                spec.playrate = rate;
                spec.length = loop ? 12 : 8;
                spec.lengthBeats = loop ? 4 : 8;
                if (loop)
                    spec.notes = {note(0, 2, 60), note(1, 2, 64), note(3, 3, 67)};  // one crosses
                spec.phase = 1;
                spec.startOffset = loop ? 1 : 0;
                spec.position = 4;
                const Rig r = rig(spec);
                const auto before = heard(r.s, r.item);
                REQUIRE_FALSE(before.empty());
                for (const double at :
                     {5.0, 6.5, 8.0, 10.5}) {  // inside a pass, on a boundary, past the first pass
                    Id right;
                    const Session s = splitAt(r.s, r.item, r.take, at, &right, wrap);
                    auto both = heard(s, r.item);
                    const auto rightHeard = heard(s, right);
                    both.insert(rightHeard.begin(), rightHeard.end());
                    INFO("loop " << loop << " rate " << rate << " wrap " << wrap << " at " << at);
                    CHECK(subset(heard(s, r.item), before));
                    CHECK(subset(rightHeard, before));
                    CHECK(both == before);
                }
            }
        }
    }
}

TEST_CASE("invariance: both halves of a note cut by a split sound the pitch the whole note did") {
    RigSpec spec;
    spec.notes = {note(2, 4, 60)};  // 2 to 6
    spec.length = 8;
    spec.context = {keyPt(0, "C", "major"), keyPt(3, "Eb", "major")};
    spec.follow = ClipFollow::Key;
    spec.project = {keyPt(0, "F", "major"), keyPt(4, "A", "minor")};
    const Rig r = rig(spec);
    const int whole = soundingNotes(r.s, r.item, 0, 100)[0].sounds.pitch;
    Id right;
    const Session s = splitAt(r.s, r.item, r.take, 4, &right);
    const auto leftHalf = soundingNotes(s, r.item, 0, 100);
    const auto rightHalf = soundingNotes(s, right, 0, 100);
    REQUIRE(leftHalf.size() == 1);
    REQUIRE(rightHalf.size() == 1);  // chased at the right part's start
    CHECK(leftHalf[0].sounds.pitch == whole);
    CHECK(rightHalf[0].sounds.pitch == whole);
    CHECK(rightHalf[0].songBeat == 2);  // its own start, before the right part's edge
    CHECK(rightHalf[0].contextBeat == 2);
}

TEST_CASE("invariance: a wrapped start_offset adds the wrapped amount to the phase") {
    RigSpec spec = varied();
    spec.loop = true;
    spec.lengthBeats = 4;
    spec.length = 16;
    spec.startOffset = 3;
    spec.phase = 0.5;
    const Rig r = rig(spec);
    Take take = *r.s.takes.find(r.take);
    take.startOffset = 9.5;  // two cycles and 1.5
    take.contextPhase = 0.5;
    wrapStartOffset(take, 4);
    CHECK(take.startOffset == 1.5);
    CHECK(take.contextPhase == 0.5 + 8);
    // A negative offset wraps up into [0, 4).
    take.startOffset = -1;
    take.contextPhase = 0;
    wrapStartOffset(take, 4);
    CHECK(take.startOffset == 3);
    CHECK(take.contextPhase == -4);
    // A source with no length, and an offset already inside, are left alone.
    take.startOffset = 2;
    take.contextPhase = 7;
    wrapStartOffset(take, 4);
    CHECK(take.startOffset == 2);
    CHECK(take.contextPhase == 7);
    wrapStartOffset(take, 0);
    CHECK(take.startOffset == 2);
}

TEST_CASE("invariance: trims from either side keep every surviving note's place") {
    for (const bool loop : {false, true}) {
        for (const double rate : {1.0, 2.0}) {
            RigSpec spec = varied();
            spec.loop = loop;
            spec.playrate = rate;
            spec.position = 4;
            spec.length = 8;
            if (loop) {
                spec.lengthBeats = 4;
                spec.length = 12;
                spec.notes = {note(0, 2, 60), note(1, 2, 64), note(3, 3, 67)};
            }
            const Rig r = rig(spec);
            const auto before = heard(r.s, r.item);
            INFO("loop " << loop << " rate " << rate);
            // The right edge moves in: nothing else changes.
            const Session right = edit<Item>(r.s, r.item, [](Item& i) { i.length -= 3; }).value();
            CHECK(subset(heard(right, r.item), before));
            CHECK_FALSE(heard(right, r.item).empty());
            // The left edge moves in: the item starts later and its content offset follows, as
            // item.set {position, length} with take.set {start_offset} does. No phase changes.
            EditTxn t(r.s, r.s.version + 1);
            t.modify<Item>(r.item, [](Item& i) {
                i.position += 2;
                i.length -= 2;
            });
            t.modify<Take>(r.take, [&](Take& tk) { tk.startOffset += 2 * rate; });
            const Session left = t.commit().value();
            CHECK(subset(heard(left, r.item), before));
            CHECK_FALSE(heard(left, r.item).empty());
            // The same trim as two separate calls, the item first and then the take (or the
            // other way round): no phase is touched, so each order ends in the same place.
            const Session twoCalls =
                edit<Take>(edit<Item>(r.s, r.item,
                                      [](Item& i) {
                                          i.position += 2;
                                          i.length -= 2;
                                      })
                               .value(),
                           r.take, [&](Take& tk) { tk.startOffset += 2 * rate; })
                    .value();
            CHECK(heard(twoCalls, r.item) == heard(left, r.item));
            const Session otherOrder =
                edit<Item>(
                    edit<Take>(r.s, r.take, [&](Take& tk) { tk.startOffset += 2 * rate; }).value(),
                    r.item,
                    [](Item& i) {
                        i.position += 2;
                        i.length -= 2;
                    })
                    .value();
            CHECK(heard(otherOrder, r.item) == heard(left, r.item));
            // The left edge moves out (a negative trim): the same.
            EditTxn u(r.s, r.s.version + 1);
            u.modify<Item>(r.item, [](Item& i) {
                i.position -= 2;
                i.length += 2;
            });
            u.modify<Take>(r.take, [&](Take& tk) { tk.startOffset -= 2 * rate; });
            const Session out = u.commit().value();
            CHECK(subset(before, heard(out, r.item)));
        }
    }
}

TEST_CASE("invariance: loop on and off, and a playrate change, keep each note's context beat") {
    RigSpec spec = varied();
    spec.lengthBeats = 4;
    spec.loop = true;
    spec.length = 8;
    spec.notes = {note(0, 1, 60), note(2, 1, 64)};
    const Rig r = rig(spec);
    const auto looped = heard(r.s, r.item);
    // Turning the loop off keeps the first pass: those occurrences are the same.
    const Session off = edit<Item>(r.s, r.item, [](Item& i) { i.loop = false; }).value();
    const auto once = heard(off, r.item);
    REQUIRE_FALSE(once.empty());
    CHECK(subset(once, looped));
    CHECK(once.size() == 2);
    // And on again: everything the unlooped item played is still played, in place.
    const Session on = edit<Item>(off, r.item, [](Item& i) { i.loop = true; }).value();
    CHECK(subset(once, heard(on, r.item)));
    CHECK(heard(on, r.item) == looped);
    // A playrate change moves the notes in song time, never in the context: compare by
    // note and pass.
    const auto contexts = [&](const Session& s) {
        std::set<std::tuple<std::uint32_t, std::int32_t, long>> out;
        for (const SoundingNote& n : soundingNotes(s, r.item, -100, 100))
            out.insert({n.note, n.pass, std::lround(n.contextBeat * 1e6)});
        return out;
    };
    const auto at1 = contexts(r.s);
    const Session faster = edit<Take>(r.s, r.take, [](Take& t) { t.playrate = 2; }).value();
    const auto at2 = contexts(faster);
    for (const auto& c : at1) CHECK(at2.contains(c));  // the faster item plays more passes
    CHECK(at2.size() >= at1.size());
}

// --- contextOver -----------------------------------------------------------------------------

TEST_CASE(
    "contextOver: the span, repeats unrolled and re-based, with the key and chord carried in") {
    Take take;
    take.context = {keyPt(0, "C", "major"), chordPt(0, "C"), chordPt(2, "F"),
                    keyPt(4, "Eb", "major"), chordPt(6, "")};
    take.contextPeriod = 8;
    SUBCASE("across a repeat") {
        const auto got = contextOver(take, 4, 12);
        REQUIRE(got.size() == 6);
        // Eb (at u0 itself) and F (in force) carried to 0; the rest re-based.
        CHECK(got[0].type == ContextType::Key);
        CHECK(got[0].beat == 0);
        CHECK(got[0].tonic == "Eb");
        CHECK(got[1].type == ContextType::Chord);
        CHECK(got[1].beat == 0);
        CHECK(got[1].symbol == "F");
        CHECK(got[2].beat == 2);
        CHECK(got[2].symbol.empty());  // "no chord from here" at u 6
        CHECK(got[3].beat == 4);
        CHECK(got[3].tonic == "C");
        CHECK(got[4].beat == 4);
        CHECK(got[4].type == ContextType::Chord);
        CHECK(got[4].symbol == "C");
        CHECK(got[5].beat == 6);
        CHECK(got[5].symbol == "F");
    }
    SUBCASE("within one period, a point at the start is the carried one") {
        const auto got = contextOver(take, 2, 6);
        REQUIRE(got.size() == 3);
        CHECK(got[0].tonic == "C");   // the key in force at 2
        CHECK(got[1].symbol == "F");  // the chord at 2
        CHECK(got[2].beat == 2);
        CHECK(got[2].tonic == "Eb");
    }
    SUBCASE("a span starting at a negative context beat wraps") {
        const auto got = contextOver(take, -2, 2);
        REQUIRE(got.size() >= 2);
        CHECK(got[0].tonic == "Eb");  // u = -2 reads 6 of the previous repeat
        CHECK(got[1].symbol.empty());
    }
}

TEST_CASE("contextOver: before the first point of a type, the first holds") {
    Take take;
    take.context = {keyPt(4, "D", "minor"), chordPt(6, "Dm")};
    const auto got = contextOver(take, 0, 8);
    REQUIRE(got.size() == 4);
    CHECK(got[0].type == ContextType::Key);
    CHECK(got[0].beat == 0);
    CHECK(got[0].tonic == "D");
    CHECK(got[1].type == ContextType::Chord);
    CHECK(got[1].beat == 0);
    CHECK(got[1].symbol == "Dm");
    CHECK(got[2].beat == 4);
    CHECK(got[3].beat == 6);
    CHECK(contextOver(Take{}, 0, 8).empty());
}

TEST_CASE("contextOver: an unreadable point is data, carried as it is") {
    Take take;
    take.context = {keyPt(0, "C", "nonesuch")};
    const auto got = contextOver(take, 2, 4);
    REQUIRE(got.size() == 1);
    CHECK(got[0].scale == "nonesuch");
    CHECK(got[0].beat == 0);
}

TEST_CASE("contextOver: a tiny period over a long span is bounded") {
    Take take;
    take.context = {keyPt(0, "C", "major")};
    take.contextPeriod = 1.0 / 960;
    bool truncated = false;
    const auto got = contextOver(take, 0, 1e9, &truncated);
    CHECK(got.size() == kMaxContextPoints);
    CHECK(truncated);
    truncated = true;
    contextOver(take, 0, 1.0 / 96, &truncated);
    CHECK_FALSE(truncated);
}

// --- placeAt: the place of a note that does not exist yet ----------------------------------------

TEST_CASE("placeAt agrees with the place of every note that does exist") {
    RigSpec looped;
    looped.notes = {note(0, 1, 60), note(2, 1, 64)};
    looped.lengthBeats = 4;
    looped.loop = true;
    looped.length = 8;
    looped.startOffset = 5;  // a whole cycle and a beat in
    looped.context = {keyPt(0, "C", "major"), chordPt(0, "C"), keyPt(4, "Eb", "major"),
                      chordPt(4, "Ab")};
    looped.period = 8;
    looped.phase = 1;
    looped.follow = ClipFollow::Chords;
    looped.project = {keyPt(0, "F", "major"), chordPt(0, "F"), keyPt(4, "A", "minor"),
                      chordPt(6, "Am")};
    RigSpec plain = varied();
    plain.position = 2;
    plain.startOffset = 1;
    plain.playrate = 2;
    plain.length = 4;
    for (const RigSpec& spec : {looped, plain, varied()}) {
        const Rig r = rig(spec);
        const auto notes = soundingNotes(r.s, r.item, -100, 100);
        REQUIRE_FALSE(notes.empty());
        for (const SoundingNote& n : notes) {
            const auto at = placeAt(r.s, r.item, n.songBeat);
            REQUIRE(at);
            CHECK(at->contextBeat == doctest::Approx(n.contextBeat).epsilon(1e-9));
            CHECK(at->songBeat == n.songBeat);
            CHECK(soundsAt(*at, n.channel, n.written).pitch == n.sounds.pitch);
            CHECK(soundsAt(*at, n.channel, n.written).by == n.sounds.by);
        }
        CHECK(placeAt(r.s, r.item, r.take, notes[0].songBeat));
    }
}

TEST_CASE("placeAt: both sides' keys and chords, a place before the item, and no place") {
    RigSpec spec = varied();
    spec.position = 4;
    spec.context.push_back(chordPt(0, "C"));
    spec.context.push_back(chordPt(4, "Ab"));
    std::stable_sort(spec.context.begin(), spec.context.end(), [](const auto& a, const auto& b) {
        return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
    });
    spec.project.push_back(chordPt(0, "F"));
    const Rig r = rig(spec);
    const auto at = placeAt(r.s, r.item, 9);  // item beat 5: the take's second half
    REQUIRE(at);
    CHECK(at->sourceBeat == 5);
    CHECK(at->contextBeat == 5);
    CHECK(at->take.key == std::optional<harmony::Key>(key("Eb", harmony::Scale::Major)));
    CHECK(at->take.chord == std::optional<harmony::Chord>(chord("Ab")));
    CHECK(at->project.key == std::optional<harmony::Key>(key("A", harmony::Scale::Minor)));
    CHECK(at->project.chord == std::optional<harmony::Chord>(chord("F")));
    CHECK(at->follow == ClipFollow::Key);
    // A key follow ignores the chord parts, so the pitch is the key's.
    CHECK(soundsAt(*at, 0, 67).pitch == harmony::follow(67, {key("Eb", harmony::Scale::Major), {}},
                                                        {key("A", harmony::Scale::Minor), {}},
                                                        harmony::FollowMode::Key)
                                            .pitch);
    CHECK(soundsAt(*at, 9, 67).pitch == 67);  // channel 10 is never mapped
    // Before the item's start the context runs back (it wraps here, with a period of 8), and the
    // project is read where the song beat falls: before its first key, none (beat -3).
    const auto before = placeAt(r.s, r.item, 2);
    REQUIRE(before);
    CHECK(before->sourceBeat == -2);
    CHECK(before->contextBeat == 6);
    CHECK(before->project.key == std::optional<harmony::Key>(key("F", harmony::Scale::Major)));
    const auto early = placeAt(r.s, r.item, -3);
    REQUIRE(early);
    CHECK_FALSE(early->project.key);
    // A take that does not follow, and a missing item, give no mapping and no place.
    const Session none =
        edit<Take>(r.s, r.take, [](Take& t) { t.follow = ClipFollow::None; }).value();
    CHECK(soundsAt(*placeAt(none, r.item, 9), 0, 67).pitch == 67);
    CHECK_FALSE(placeAt(r.s, Id{Kind::Item, 99}, 0));
    CHECK_FALSE(placeAt(r.s, r.item, Id{Kind::Take, 99}, 0));
    // The project's key before its first point is none: as written.
    RigSpec late = spec;
    late.project = {keyPt(8, "F", "major")};
    const Rig l = rig(late);
    CHECK_FALSE(placeAt(l.s, l.item, 5)->project.key);
    CHECK(soundsAt(*placeAt(l.s, l.item, 5), 0, 67).pitch == 67);
}

// --- a take laid from a clip ---------------------------------------------------------------------

TEST_CASE(
    "takeContextFromClip: the clip's context in the take's beats, read as the launcher reads it") {
    Clip clip;
    clip.length = 8;
    clip.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major"), chordPt(2, "F")};
    std::stable_sort(clip.context.begin(), clip.context.end(), [](const auto& a, const auto& b) {
        return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
    });
    Take take;
    take.source = Id{Kind::Midi, 1};
    takeContextFromClip(take, clip, 0, ClipFollow::Key);
    CHECK(take.contextPeriod == std::optional<double>(8.0));
    CHECK(take.contextPhase == 0);
    CHECK(take.context.size() == 3);
    CHECK(take.follow == ClipFollow::Key);
    // Playrate 2 and a slip of 1.5: beats and period scale, the phase is c0*r - offset.
    Take fast;
    fast.source = Id{Kind::Midi, 1};
    fast.playrate = 2;
    fast.startOffset = 1.5;
    takeContextFromClip(fast, clip, 3, ClipFollow::Chords);
    CHECK(fast.contextPeriod == std::optional<double>(16.0));
    CHECK(fast.contextPhase == 3 * 2 - 1.5);
    CHECK(fast.context[1].beat == 4);  // the chord at clip beat 2
    CHECK(fast.context[2].beat == 8);  // Eb at clip beat 4
    CHECK(fast.follow == ClipFollow::Chords);
    // A clip with no context leaves the take as it was; an audio or empty take holds none.
    Take plain;
    plain.source = Id{Kind::Midi, 1};
    takeContextFromClip(plain, Clip{}, 0, ClipFollow::None);
    CHECK(plain.context.empty());
    CHECK_FALSE(plain.contextPeriod);
    Take empty;
    takeContextFromClip(empty, clip, 0, ClipFollow::Key);
    CHECK(empty.context.empty());
    // Follow other than none without a readable key is reset by the fit.
    Clip chordsOnly;
    chordsOnly.length = 4;
    chordsOnly.context = {chordPt(0, "C")};
    Take reset;
    reset.source = Id{Kind::Midi, 1};
    takeContextFromClip(reset, chordsOnly, 0, ClipFollow::Key);
    CHECK(reset.follow == ClipFollow::None);
}

TEST_CASE(
    "takeContextFromClip: a note heard d beats after the item's start reads clip beat (c0 + d) * "
    "r") {
    for (const double rate : {1.0, 2.0, 0.5}) {
        for (const double offset : {0.0, 1.0}) {
            for (const double c0 : {0.0, 2.0}) {
                // A clip of length 8 over 4 beats of looping content (C at 0, Eb at 4).
                Clip clip;
                clip.length = 8;
                clip.context = {keyPt(0, "C", "major"), keyPt(4, "Eb", "major")};
                RigSpec spec;
                spec.notes = {note(0, 1, 60), note(1, 1, 62), note(2, 1, 64), note(3, 1, 65)};
                spec.lengthBeats = 4;
                spec.loop = true;
                spec.length = 8 / rate;
                spec.playrate = rate;
                spec.startOffset = offset;
                spec.project = {keyPt(0, "F", "major")};
                Rig r = rig(spec);
                r.s = edit<Take>(r.s, r.take, [&](Take& t) {
                          takeContextFromClip(t, clip, c0, ClipFollow::Key);
                      }).value();
                const Item& item = *r.s.items.find(r.item);
                for (const SoundingNote& n : soundingNotes(r.s, r.item, -100, 100)) {
                    const double d = n.songBeat - item.position;
                    double want = std::fmod((c0 + d) * rate, 8 * rate);
                    if (want < 0) want += 8 * rate;
                    INFO("rate " << rate << " offset " << offset << " c0 " << c0 << " note "
                                 << n.note << " pass " << n.pass);
                    CHECK(n.contextBeat == doctest::Approx(want).epsilon(1e-9));
                }
            }
        }
    }
}

TEST_CASE("take context: a period under 1/960 or a huge phase cannot hang a reader (round 85)") {
    const Rig r = rig(varied());
    const auto set = [&](std::optional<double> period, double phase) {
        return edit<Take>(r.s, r.take, [&](Take& t) {
            t.context = {keyPt(0, "C", "major"), chordPt(0, "C")};
            t.contextPeriod = period;
            t.contextPhase = phase;
        });
    };
    CHECK(set(8.0, 0));
    CHECK(says(set(1e-12, 0), "at least 1/960"));
    CHECK(says(set(0.0005, 0), "at least 1/960"));
    CHECK(says(set(1.0 / 960.0 * 1.5, 0), "1/960 grid"));
    CHECK(says(set(8.0, 1e18), "within a billion beats"));
    CHECK(set(1.0 / 960.0, 0));

    SUBCASE("the load fit drops them, with a note") {
        Take take;
        take.source = Id{Kind::Midi, 1};
        take.context = {keyPt(0, "C", "major")};
        take.contextPeriod = 1e-12;
        take.contextPhase = 1e18;
        const auto notes = fitTakeContext(take, TakeHome::Item);
        CHECK(notes.size() == 2);
        CHECK_FALSE(take.contextPeriod);
        CHECK(take.contextPhase == 0);
    }
    SUBCASE("contextOver is bounded whatever a take holds") {
        Take take;
        take.context = {keyPt(0, "C", "major"), chordPt(0, "C")};
        take.contextPeriod = 1e-12;  // never read as a period
        bool truncated = false;
        CHECK_FALSE(contextOver(take, 0, 64, &truncated).empty());
        take.contextPeriod = 1.0 / 960.0;
        take.context = {keyPt(0, "C", "major")};
        // 64 beats of 1/960-beat repeats is 61440 of them: walked, and the points merge on the
        // grid, so the answer stays small.
        CHECK(contextOver(take, 0, 64, &truncated).size() <= kMaxContextPoints * 2);
        // A span past any double's step stops with truncated set instead of walking on.
        contextOver(take, 0, 1e18, &truncated);
        CHECK(truncated);
        take.contextPeriod = 8.0;
        contextOver(take, 1e18, 1e18 + 1e6, &truncated);
        CHECK(truncated);
    }
}
