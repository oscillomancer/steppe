// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/schemas.h"
#include "migration.h"
#include "supple/model/audio_warp.h"
#include "supple/model/render_settings.h"
#include "supple/schema/document.h"

#include "supple/base/files.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>

namespace supple::model {

using schema::ObjectBuilder;
using schema::Schema;

namespace {

// Field helpers: `dflt(b, value)` marks the last field optional with that
// default and omits it from the output when equal.
template <class T, class M>
ObjectBuilder<T>& dflt(ObjectBuilder<T>& b, M value) {
    return b.defaultValue(std::move(value)).omitIfDefault();
}

Schema<int> intS() { return schema::integerOf<int>(); }
Schema<int> oversamplingSchema() {
    const auto integer = intS();
    const std::array allowed{1, 2, 4, 8};
    auto spec = integer.jsonSchema();
    spec["enum"] = json::Value::array();
    for (const int factor : allowed) spec["enum"].asArray().push_back(factor);
    return {"oversampling",
            [integer, allowed](const json::Value& v, int& out, schema::DecodeCtx& ctx,
                               const std::string& path) {
                if (!integer.decodeInto(v, out, ctx, path)) return false;
                if (std::find(allowed.begin(), allowed.end(), out) == allowed.end()) {
                    ctx.error("fx.oversampling", "oversampling must be 1, 2, 4 or 8", path,
                              "set oversampling to one of 1, 2, 4 or 8");
                    return false;
                }
                return true;
            },
            [integer](int v) { return integer.encode(v); }, spec};
}
Schema<double> num() { return schema::number(); }
Schema<std::string> str() { return schema::string(); }
Schema<bool> boolean() { return schema::boolean(); }
Schema<std::vector<Id>> ids() {
    auto array = schema::arrayOf(idSchema());
    return {"ids",
            [array](const json::Value& v, std::vector<Id>& out, schema::DecodeCtx& ctx,
                    const std::string& path) {
                out.clear();  // project.masters has a nonempty default
                return array.decodeInto(v, out, ctx, path);
            },
            [array](const std::vector<Id>& v) { return array.encode(v); }, array.jsonSchema()};
}

Schema<json::Value> jsonObject() {
    json::Value spec = json::Value::object();
    spec["type"] = "object";
    return {"JSON object",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& c, const std::string& p) {
                if (!v.isObject()) {
                    c.error("schema.type", "expected an object", p);
                    return false;
                }
                out = v;
                return true;
            },
            [](const json::Value& v) { return v; }, spec};
}

// project.render: the typed render settings (render_settings.h), stored as
// their canonical JSON. Members before schema 2.3 migrate first.
Schema<json::Value> renderSettings() {
    auto decode = [](const json::Value& v, json::Value& out, schema::DecodeCtx& c,
                     const std::string& p) {
        if (!jsonObject().decodeInto(v, out, c, p)) return false;
        if (auto migrated = render::migrateLegacy(out, p); !migrated) {
            c.error(migrated.error().code, migrated.error().message, migrated.error().path,
                    migrated.error().hint);
            return false;
        }
        render::Settings settings;
        if (!render::settingsSchema().decodeInto(out, settings, c, p)) return false;
        out = render::toJson(settings);
        return true;
    };
    return {"render settings", decode, [](const json::Value& v) { return v; },
            render::settingsSchema().jsonSchema()};
}

std::string hex2(unsigned v) {
    char b[3];
    std::snprintf(b, sizeof b, "%02X", v & 0xff);
    return b;
}

// MIDI sequences in the compact row form (session-model §5.1): notes
// [start, length, channel (1-based), pitch, velocity, off_velocity], controls
// [tick, kind, channel, number, value], sysex [tick, "F0 .. F7"].
Schema<midi::Sequence> sequenceSchema() {
    static constexpr std::pair<midi::ControlKind, const char*> kKinds[] = {
        {midi::ControlKind::CC, "cc"},
        {midi::ControlKind::ProgramChange, "program"},
        {midi::ControlKind::ChannelPressure, "pressure"},
        {midi::ControlKind::PolyPressure, "poly"},
        {midi::ControlKind::PitchBend, "bend"}};
    auto decode = [](const json::Value& v, midi::Sequence& out, schema::DecodeCtx& c,
                     const std::string& p) {
        if (!v.isObject()) {
            c.error("schema.type", "expected a MIDI sequence object", p);
            return false;
        }
        if (c.warnings)
            for (const auto& field : v.asObject())
                if (field.key != "ppq" && field.key != "length" && field.key != "notes" &&
                    field.key != "controls" && field.key != "sysex")
                    c.warnings->push_back({"schema.unknown_member", "ignored unknown member",
                                           json::pointerJoin(p, field.key)});
        auto intAt = [&](const json::Value& row, std::size_t i, std::int64_t lo, std::int64_t hi,
                         const std::string& at, std::int64_t& x) {
            if (i >= row.size()) {
                c.error("schema.type", "expected an integer", at + "/" + std::to_string(i));
                return false;
            }
            const auto path = at + "/" + std::to_string(i);
            if (!schema::integer().decodeInto(row.at(i), x, c, path)) return false;
            if (x < lo || x > hi) {
                c.error("schema.out_of_range", "value out of range", path,
                        std::to_string(lo) + " .. " + std::to_string(hi));
                return false;
            }
            return true;
        };
        bool ok = true;
        if (const auto* ppq = v.find("ppq"))
            ok &= schema::integerOf<int>().range(1, 32767).decodeInto(*ppq, out.ppq, c, p + "/ppq");
        if (const auto* len = v.find("length"))
            ok &= schema::integer().min(0).decodeInto(*len, out.length, c, p + "/length");
        for (const char* key : {"notes", "controls", "sysex"})
            if (const auto* rows = v.find(key); rows && !rows->isArray()) {
                c.error("schema.type", "expected an array", p + "/" + key);
                ok = false;
            }
        if (const auto* notes = v.find("notes"); notes && notes->isArray()) {
            for (std::size_t i = 0; i < notes->size(); ++i) {
                const auto& row = notes->at(i);
                const std::string at = p + "/notes/" + std::to_string(i);
                std::int64_t s = 0, l = 0, ch = 0, key = 0, vel = 0, off = 0;
                if (!row.isArray() || row.size() < 5 || row.size() > 6) {
                    c.error("schema.type",
                            "a note is [start, length, channel, pitch, velocity, off_velocity?]",
                            at);
                    ok = false;
                    continue;
                }
                if (!intAt(row, 0, 0, INT64_MAX / 4, at, s) ||
                    !intAt(row, 1, 0, INT64_MAX / 4, at, l) || !intAt(row, 2, 1, 16, at, ch) ||
                    !intAt(row, 3, 0, 127, at, key) || !intAt(row, 4, 1, 127, at, vel) ||
                    (row.size() > 5 && !intAt(row, 5, 0, 127, at, off))) {
                    ok = false;
                    continue;
                }
                out.notes.push_back({s, l, static_cast<std::uint8_t>(ch - 1),
                                     static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(vel),
                                     static_cast<std::uint8_t>(off)});
            }
        }
        if (const auto* controls = v.find("controls"); controls && controls->isArray()) {
            for (std::size_t i = 0; i < controls->size(); ++i) {
                const auto& row = controls->at(i);
                const std::string at = p + "/controls/" + std::to_string(i);
                if (!row.isArray() || row.size() != 5 || !row.at(1).isString()) {
                    c.error("schema.type", "a control is [tick, kind, channel, number, value]", at);
                    ok = false;
                    continue;
                }
                midi::Control ctl;
                bool known = false;
                for (auto [k, name] : kKinds)
                    if (row.at(1).asString() == name) {
                        ctl.kind = k;
                        known = true;
                    }
                if (!known) {
                    c.error("schema.enum", "unknown control kind", at + "/1",
                            "cc, program, pressure, poly, bend");
                    ok = false;
                    continue;
                }
                std::int64_t t = 0, ch = 0, n = 0, val = 0;
                if (!intAt(row, 0, 0, INT64_MAX / 4, at, t) || !intAt(row, 2, 1, 16, at, ch) ||
                    !intAt(row, 3, 0, 127, at, n) ||
                    !intAt(row, 4, ctl.kind == midi::ControlKind::PitchBend ? -8192 : 0,
                           ctl.kind == midi::ControlKind::PitchBend ? 8191 : 127, at, val)) {
                    ok = false;
                    continue;
                }
                ctl.tick = t;
                ctl.channel = static_cast<std::uint8_t>(ch - 1);
                ctl.number = static_cast<std::uint8_t>(n);
                ctl.value = static_cast<std::int32_t>(val);
                out.controls.push_back(ctl);
            }
        }
        if (const auto* sysex = v.find("sysex"); sysex && sysex->isArray()) {
            for (std::size_t i = 0; i < sysex->size(); ++i) {
                const auto& row = sysex->at(i);
                const std::string at = p + "/sysex/" + std::to_string(i);
                if (!row.isArray() || row.size() != 2 || !row.at(0).isNumber() ||
                    !row.at(1).isString()) {
                    c.error("schema.type", "sysex is [tick, \"F0 .. F7\"]", at);
                    ok = false;
                    continue;
                }
                midi::Sysex x;
                if (!intAt(row, 0, 0, INT64_MAX / 4, at, x.tick)) {
                    ok = false;
                    continue;
                }
                const std::string& hex = row.at(1).asString();
                bool valid = !hex.empty() && hex.size() % 3 == 2;
                for (std::size_t k = 0; valid && k < hex.size(); k += 3) {
                    unsigned byte = 0;
                    const auto [end, ec] =
                        std::from_chars(hex.data() + k, hex.data() + k + 2, byte, 16);
                    valid = ec == std::errc{} && end == hex.data() + k + 2 &&
                            (k + 2 == hex.size() || hex[k + 2] == ' ');
                    if (valid) x.bytes.push_back(static_cast<std::uint8_t>(byte));
                }
                if (!valid || x.bytes.size() < 2 || x.bytes.front() != 0xF0 ||
                    x.bytes.back() != 0xF7 ||
                    std::any_of(x.bytes.begin() + 1, x.bytes.end() - 1,
                                [](std::uint8_t byte) { return byte > 0x7F; })) {
                    c.error("schema.type", "expected a hexadecimal F0 .. F7 sysex message",
                            at + "/1", "e.g. F0 7E 7F 09 01 F7");
                    ok = false;
                    continue;
                }
                out.sysex.push_back(std::move(x));
            }
        }
        return ok;
    };
    auto encode = [](const midi::Sequence& s) {
        json::Value v = json::Value::object();
        v["ppq"] = s.ppq;
        v["length"] = s.length;
        json::Value notes = json::Value::array();
        for (const auto& n : s.notes) {
            json::Value row = json::Value::array();
            row.push_back(n.start);
            row.push_back(n.length);
            row.push_back(n.channel + 1);
            row.push_back(n.pitch);
            row.push_back(n.velocity);
            row.push_back(n.offVelocity);
            notes.push_back(std::move(row));
        }
        v["notes"] = std::move(notes);
        if (!s.controls.empty()) {
            json::Value controls = json::Value::array();
            for (const auto& c : s.controls) {
                json::Value row = json::Value::array();
                row.push_back(c.tick);
                const char* name = "cc";
                for (auto [k, nm] : kKinds)
                    if (k == c.kind) name = nm;
                row.push_back(name);
                row.push_back(c.channel + 1);
                row.push_back(c.number);
                row.push_back(c.value);
                controls.push_back(std::move(row));
            }
            v["controls"] = std::move(controls);
        }
        if (!s.sysex.empty()) {
            json::Value sysex = json::Value::array();
            for (const auto& x : s.sysex) {
                std::string hex;
                for (std::uint8_t b : x.bytes) hex += (hex.empty() ? "" : " ") + hex2(b);
                json::Value row = json::Value::array();
                row.push_back(x.tick);
                row.push_back(hex);
                sysex.push_back(std::move(row));
            }
            v["sysex"] = std::move(sysex);
        }
        return v;
    };
    json::Value ex = json::Value::object();
    ex["type"] = "object";
    ex["description"] =
        "MIDI item: ppq, length (ticks), notes [[start, length, channel, pitch, "
        "velocity, off_velocity]], controls [[tick, kind, channel, number, "
        "value]], sysex [[tick, \"F0 .. F7\"]]";
    return Schema<midi::Sequence>("midi sequence", decode, encode, ex);
}

// CLAP id -> value, as a JSON object keyed by the decimal id.
Schema<std::map<std::uint32_t, double>> paramValuesSchema() {
    auto inner = schema::mapOf(schema::number());
    auto decode = [inner](const json::Value& v, std::map<std::uint32_t, double>& out,
                          schema::DecodeCtx& c, const std::string& p) {
        std::map<std::string, double> tmp;
        if (!inner.decodeInto(v, tmp, c, p)) return false;
        for (const auto& [k, x] : tmp) {
            std::uint32_t id = 0;
            const auto [end, ec] = std::from_chars(k.data(), k.data() + k.size(), id);
            if (ec != std::errc{} || end != k.data() + k.size()) {
                c.error("schema.key", "parameter keys are unsigned 32-bit decimal ids",
                        json::pointerJoin(p, k), "use decimal digits from 0 to 4294967295");
                return false;
            }
            out[id] = x;
        }
        return true;
    };
    auto encode = [](const std::map<std::uint32_t, double>& m) {
        json::Value v = json::Value::object();
        for (const auto& [k, x] : m) v[std::to_string(k)] = x;
        return v;
    };
    return Schema<std::map<std::uint32_t, double>>("param values", decode, encode,
                                                   inner.jsonSchema());
}

template <class E>
Schema<E> en(std::initializer_list<std::pair<E, std::string_view>> choices) {
    return schema::enumeration<E>(choices);
}

auto timebaseS() {
    return en<Timebase>({{Timebase::Time, "time"},
                         {Timebase::Beats, "beats"},
                         {Timebase::BeatsPosition, "beats_position"}});
}

}  // namespace

Schema<Timebase> timebaseSchema() { return timebaseS(); }
Schema<TrackTimebase> trackTimebaseSchema() {
    return en<TrackTimebase>({{TrackTimebase::Project, "project"},
                              {TrackTimebase::Time, "time"},
                              {TrackTimebase::Beats, "beats"},
                              {TrackTimebase::BeatsPosition, "beats_position"}});
}
Schema<ItemTimebase> itemTimebaseSchema() {
    return en<ItemTimebase>({{ItemTimebase::Track, "track"},
                             {ItemTimebase::Time, "time"},
                             {ItemTimebase::Beats, "beats"},
                             {ItemTimebase::BeatsPosition, "beats_position"}});
}

Schema<Id> idSchema() {
    json::Value ex = json::Value::object();
    ex["type"] = "string";
    ex["pattern"] = "^[a-z]{2}_[0-9]+$";
    return Schema<Id>(
        "id",
        [](const json::Value& v, Id& out, schema::DecodeCtx& c, const std::string& p) {
            if (!v.isString()) {
                c.error("schema.type", "expected an id string", p, "e.g. \"tr_1\"");
                return false;
            }
            auto id = Id::parse(v.asString());
            if (!id) {
                c.error("schema.id", "\"" + v.asString() + "\" is not an object id", p,
                        "ids look like tr_1, it_4, fx_2");
                return false;
            }
            out = *id;
            return true;
        },
        [](const Id& id) { return json::Value(id.str()); }, ex);
}

// --- per-type schemas -------------------------------------------------------------

template <>
const Schema<Playrate>& schemaOf<Playrate>() {
    static const Schema<Playrate> s = [] {
        ObjectBuilder<Playrate> b = schema::object<Playrate>("playrate");
        b.field("rate", &Playrate::rate, num().range(kMinPlayrate, kMaxPlayrate));
        dflt(b, 1.0);
        b.field("preserve_pitch", &Playrate::preservePitch, boolean());
        dflt(b, false);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Project>& schemaOf<Project>() {
    static const Schema<Project> s = [] {
        const Project d;
        ObjectBuilder<Project> b = schema::object<Project>("project");
        b.field("name", &Project::name, str());
        dflt(b, d.name);
        b.field("title", &Project::title, str());
        dflt(b, d.title);
        b.field("author", &Project::author, str());
        dflt(b, d.author);
        b.field("notes", &Project::notes, str());
        dflt(b, d.notes);
        b.field("sample_rate", &Project::sampleRate, num().range(8000, 384000));
        dflt(b, d.sampleRate);
        b.field("ppq", &Project::ppq, intS().range(1, 32767));
        dflt(b, d.ppq);
        b.field("timebase", &Project::timebase, timebaseS());
        dflt(b, d.timebase);
        b.field("media_dir", &Project::mediaDir, str());
        dflt(b, d.mediaDir);
        b.field("console9", &Project::console9, boolean());
        dflt(b, d.console9);
        b.field("pan_law", &Project::panLaw, panLawSchema());
        dflt(b, d.panLaw);
        b.field("playrate", &Project::playrate, schemaOf<Playrate>());
        dflt(b, d.playrate);
        b.field("masters", &Project::masters, ids());
        dflt(b, d.masters);
        b.field("seed", &Project::seed, schema::integerOf<std::uint64_t>());
        dflt(b, d.seed);
        b.field("history_budget_bytes", &Project::historyBudgetBytes,
                schema::integerOf<std::uint64_t>().min(1));
        dflt(b, d.historyBudgetBytes);
        b.field("render", &Project::render, renderSettings());
        dflt(b, d.render);
        return b.build();
    }();
    return s;
}

template <>
const Schema<TempoPointObj>& schemaOf<TempoPointObj>() {
    static const Schema<TempoPointObj> s = [] {
        ObjectBuilder<TempoPointObj> b = schema::object<TempoPointObj>("tempo_point");
        b.field("beat", &TempoPointObj::beat, num().min(0));
        b.field("bpm", &TempoPointObj::bpm, num().range(1, 1000));
        b.field("ramp", &TempoPointObj::ramp, boolean());
        dflt(b, false);
        return b.build();
    }();
    return s;
}

template <>
const Schema<MeterObj>& schemaOf<MeterObj>() {
    static const Schema<MeterObj> s = [] {
        ObjectBuilder<MeterObj> b = schema::object<MeterObj>("meter_change");
        b.field("bar", &MeterObj::bar, intS().min(1));
        b.field("numerator", &MeterObj::numerator, intS().range(1, 64));
        b.field("denominator", &MeterObj::denominator, intS().range(1, 64));
        return b.build();
    }();
    return s;
}

Schema<TrackFreeze> trackFreezeSchema() {
    return schema::object<TrackFreeze>("track_freeze")
        .field("source", &TrackFreeze::source, idSchema())
        .field("content_frames", &TrackFreeze::contentFrames, schema::integer().range(1, 1LL << 28))
        .field("tail_frames", &TrackFreeze::tailFrames, schema::integer().range(0, 1LL << 28))
        .field("latency_frames", &TrackFreeze::latencyFrames, schema::integer().range(0, 1LL << 28))
        .field("dependency_hash", &TrackFreeze::dependencyHash,
               str().pattern("^sha256:[0-9a-f]{64}$", "a SHA-256 dependency hash"))
        .build();
}

template <>
const Schema<Vca>& schemaOf<Vca>() {
    static const auto s = [] {
        auto b = schema::object<Vca>("vca");
        b.field("volume_db", &Vca::volumeDb, num().range(kSilenceDb, 12));
        dflt(b, 0.0);
        b.field("mute", &Vca::mute, boolean());
        dflt(b, false);
        b.field("solo", &Vca::solo, boolean());
        dflt(b, false);
        b.field("followers", &Vca::followers, ids());
        dflt(b, std::vector<Id>{});
        b.field("learn", &Vca::learn, schema::arrayOf(controlLearnSchema()));
        dflt(b, std::vector<ControlLearn>{});
        return b.build();
    }();
    return s;
}
template <>
const Schema<TrackGroup>& schemaOf<TrackGroup>() {
    static const auto s = [] {
        const TrackGroup d;
        auto b = schema::object<TrackGroup>("track_group");
        b.field("name", &TrackGroup::name, str());
        dflt(b, d.name);
        b.field("color", &TrackGroup::color, str());
        dflt(b, d.color);
        b.field("active", &TrackGroup::active, boolean());
        dflt(b, d.active);
        b.field("members", &TrackGroup::members, ids());
        dflt(b, d.members);
        b.field(
            "links", &TrackGroup::links,
            schema::arrayOf(str().pattern("^(volume|pan|mute|solo|armed|monitor|selection|edit)$",
                                          "a track group link")));
        dflt(b, d.links);
        b.field("vca", &TrackGroup::vca, schemaOf<Vca>());
        return b.build();
    }();
    return s;
}

template <>
const Schema<Track>& schemaOf<Track>() {
    static const Schema<Track> s = [] {
        const Track d;
        auto parentSend = schema::object<ParentSend>("parent_send")
                              .field("enabled", &ParentSend::enabled, boolean())
                              .build();
        auto record =
            schema::object<RecordSettings>("record")
                .field("armed", &RecordSettings::armed, boolean())
                .field("input", &RecordSettings::input, idSchema())
                .field("midi_input", &RecordSettings::midiInput, idSchema())
                .field("monitor", &RecordSettings::monitor,
                       en<Monitor>(
                           {{Monitor::Off, "off"}, {Monitor::On, "on"}, {Monitor::Auto, "auto"}}))
                .field("mode", &RecordSettings::mode,
                       en<RecordMode>({{RecordMode::Input, "input"},
                                       {RecordMode::Output, "output"},
                                       {RecordMode::MidiOverdub, "midi_overdub"},
                                       {RecordMode::MidiReplace, "midi_replace"}}))
                .build();
        ObjectBuilder<Track> b = schema::object<Track>("track");
        b.field("type", &Track::type, stripTypeSchema());
        dflt(b, d.type);
        b.field("name", &Track::name, str());
        dflt(b, d.name);
        b.field("color", &Track::color, str());
        dflt(b, d.color);
        b.field("notes", &Track::notes, str());
        dflt(b, d.notes);
        b.field("parent", &Track::parent, idSchema());
        b.field("children", &Track::children, ids());
        dflt(b, d.children);
        b.field("channels", &Track::channels, intS().range(1, kMaxTrackChannels));
        dflt(b, d.channels);
        b.field("volume_db", &Track::volumeDb, num().range(kSilenceDb, 12));
        dflt(b, d.volumeDb);
        b.field("pan", &Track::pan, num().range(-100, 100));
        dflt(b, d.pan);
        b.field("pan_mode", &Track::panMode, panModeSchema());
        dflt(b, d.panMode);
        b.field("width", &Track::width, num().range(-100, 100));
        dflt(b, d.width);
        b.field("pan_left", &Track::panLeft, num().range(-100, 100));
        dflt(b, d.panLeft);
        b.field("pan_right", &Track::panRight, num().range(-100, 100));
        dflt(b, d.panRight);
        b.field("pan_law", &Track::panLaw, panLawSchema());
        b.field("mute", &Track::mute, boolean());
        dflt(b, d.mute);
        b.field("phase_invert", &Track::phaseInvert, boolean());
        dflt(b, d.phaseInvert);
        b.field("mono", &Track::mono, boolean());
        dflt(b, d.mono);
        b.field("solo", &Track::solo,
                en<Solo>({{Solo::Off, "off"},
                          {Solo::On, "on"},
                          {Solo::InPlace, "in_place"},
                          {Solo::Defeat, "defeat"}}));
        dflt(b, d.solo);
        b.field("parent_send", &Track::parentSend, parentSend);
        dflt(b, d.parentSend);
        b.field("hw_output", &Track::hwOutput, schema::arrayOf(intS().range(1, kMaxDeviceOutputs)));
        b.field("lane_mute", &Track::laneMute, boolean());
        dflt(b, d.laneMute);
        b.field("record", &Track::record, record);
        dflt(b, d.record);
        b.field("timebase", &Track::timebase, trackTimebaseSchema());
        dflt(b, d.timebase);
        b.field("chain", &Track::chain, ids());
        dflt(b, d.chain);
        b.field("input_chain", &Track::inputChain, ids());
        dflt(b, d.inputChain);
        b.field("locked", &Track::locked, boolean());
        dflt(b, d.locked);
        b.field("hidden_tcp", &Track::hiddenTcp, boolean());
        dflt(b, d.hiddenTcp);
        b.field("hidden_mixer", &Track::hiddenMixer, boolean());
        dflt(b, d.hiddenMixer);
        b.field("freeze", &Track::freeze, trackFreezeSchema());
        b.field("folder_compact", &Track::folderCompact, intS().range(0, 2));
        dflt(b, d.folderCompact);
        b.field("learn", &Track::learn, schema::arrayOf(controlLearnSchema()));
        dflt(b, d.learn);
        return b.build();
    }();
    return s;
}

namespace {
Schema<Fade> fadeSchema() {
    return schema::object<Fade>("fade")
        .field("length", &Fade::length, num().min(0))
        .field("shape", &Fade::shape,
               en<FadeShape>({{FadeShape::Linear, "linear"},
                              {FadeShape::Slow, "slow"},
                              {FadeShape::Fast, "fast"},
                              {FadeShape::SlowFast, "slow_fast"},
                              {FadeShape::FastSlow, "fast_slow"},
                              {FadeShape::Cos, "cos"},
                              {FadeShape::Exp, "exp"}}))
        .defaultValue(FadeShape::Linear)
        .omitIfDefault()
        .field("curve", &Fade::curve, num().range(-100, 100))
        .defaultValue(0.0)
        .omitIfDefault()
        .build();
}
}  // namespace

template <>
const Schema<Item>& schemaOf<Item>() {
    static const Schema<Item> s = [] {
        const Item d;
        ObjectBuilder<Item> b = schema::object<Item>("item");
        b.field("track", &Item::track, idSchema());
        b.field("lane", &Item::lane, idSchema());
        b.field("position", &Item::position, num());
        b.field("length", &Item::length, num().min(0));
        b.field("timebase", &Item::timebase, itemTimebaseSchema());
        dflt(b, d.timebase);
        b.field("loop", &Item::loop, boolean());
        dflt(b, d.loop);
        b.field("mute", &Item::mute, boolean());
        dflt(b, d.mute);
        b.field("locked", &Item::locked, boolean());
        dflt(b, d.locked);
        b.field("gain_db", &Item::gainDb, num().range(kSilenceDb, 24));
        dflt(b, d.gainDb);
        b.field("fade_in", &Item::fadeIn, fadeSchema());
        dflt(b, d.fadeIn);
        b.field("fade_out", &Item::fadeOut, fadeSchema());
        dflt(b, d.fadeOut);
        b.field("takes", &Item::takes, ids());
        dflt(b, d.takes);
        b.field("active_take", &Item::activeTake, idSchema());
        b.field("snap_offset", &Item::snapOffset, num());
        dflt(b, d.snapOffset);
        b.field("group", &Item::group, idSchema());
        b.field("color", &Item::color, str());
        dflt(b, d.color);
        b.field("notes", &Item::notes, str());
        dflt(b, d.notes);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Take>& schemaOf<Take>() {
    static const Schema<Take> s = [] {
        const Take d;
        auto stretch = schema::object<StretchMarker>("stretch_marker")
                           .field("source_pos", &StretchMarker::sourcePos, num())
                           .field("item_pos", &StretchMarker::itemPos, num())
                           .field("slope", &StretchMarker::slope, num())
                           .defaultValue(0.0)
                           .omitIfDefault()
                           .build();
        auto marker = schema::object<TakeMarker>("take_marker")
                          .field("pos", &TakeMarker::pos, num())
                          .field("name", &TakeMarker::name, str())
                          .defaultValue(std::string())
                          .omitIfDefault()
                          .field("color", &TakeMarker::color, str())
                          .defaultValue(std::string())
                          .omitIfDefault()
                          .build();
        ObjectBuilder<Take> b = schema::object<Take>("take");
        b.field("name", &Take::name, str());
        dflt(b, d.name);
        b.field("source", &Take::source, idSchema());
        b.field("start_offset", &Take::startOffset, num());
        dflt(b, d.startOffset);
        b.field("playrate", &Take::playrate, num().range(0.01, 100));
        dflt(b, d.playrate);
        b.field("preserve_pitch", &Take::preservePitch, boolean());
        dflt(b, d.preservePitch);
        b.field("pitch", &Take::pitch, num().range(-48, 48));
        dflt(b, d.pitch);
        b.field("pitch_mode", &Take::pitchMode, str());
        dflt(b, d.pitchMode);
        b.field("gain_db", &Take::gainDb, num().range(kSilenceDb, 24));
        dflt(b, d.gainDb);
        b.field("pan", &Take::pan, num().range(-100, 100));
        dflt(b, d.pan);
        b.field("phase_invert", &Take::phaseInvert, boolean());
        dflt(b, d.phaseInvert);
        b.field("channel_mode", &Take::channelMode,
                en<ChannelMode>({{ChannelMode::Normal, "normal"},
                                 {ChannelMode::ReverseStereo, "reverse_stereo"},
                                 {ChannelMode::MonoMix, "mono_mix"},
                                 {ChannelMode::Left, "left"},
                                 {ChannelMode::Right, "right"}}));
        dflt(b, d.channelMode);
        b.field("stretch_markers", &Take::stretchMarkers,
                schema::arrayOf(stretch).maxItems(kMaxAudioMarkers));
        dflt(b, d.stretchMarkers);
        const auto hits = schema::object<Transients>("transients")
                              .field("algorithm", &Transients::algorithm, str())
                              .defaultValue(std::string("flux-v1"))
                              .field("source_hash", &Transients::sourceHash, str())
                              .field("sensitivity", &Transients::sensitivity, num().range(0, 1))
                              .defaultValue(0.5)
                              .field("positions", &Transients::positions,
                                     schema::arrayOf(num().min(0)).maxItems(kMaxAudioMarkers))
                              .build();
        b.field("transients", &Take::transients, hits);
        b.field("warp_peers", &Take::warpPeers, ids().maxItems(128));
        dflt(b, d.warpPeers);
        b.field("take_markers", &Take::takeMarkers, schema::arrayOf(marker));
        dflt(b, d.takeMarkers);
        b.field("chain", &Take::chain, ids());
        dflt(b, d.chain);
        return b.build();
    }();
    return s;
}

template <>
const Schema<MediaSource>& schemaOf<MediaSource>() {
    static const Schema<MediaSource> s = [] {
        ObjectBuilder<MediaSource> b = schema::object<MediaSource>("media");
        b.field("path", &MediaSource::path, str());
        b.field("hash", &MediaSource::hash, str());
        b.field("format", &MediaSource::format, str());
        b.field("sample_rate", &MediaSource::sampleRate, num().min(0));
        b.field("channels", &MediaSource::channels, intS().min(0));
        b.field("frames", &MediaSource::frames, schema::integer().min(0));
        return b.build();
    }();
    return s;
}

template <>
const Schema<MidiSource>& schemaOf<MidiSource>() {
    static const Schema<MidiSource> s = [] {
        ObjectBuilder<MidiSource> b = schema::object<MidiSource>("midi");
        b.field("sequence", &MidiSource::sequence, sequenceSchema());
        b.field("pooled", &MidiSource::pooled, boolean());
        dflt(b, false);
        return b.build();
    }();
    return s;
}

Schema<PinMap> pinMapSchema() {
    const PinMap d;
    auto b = schema::object<PinMap>("pin_map");
    auto pins = schema::arrayOf(intS().range(1, kMaxTrackChannels));
    b.field("in", &PinMap::in, pins);
    dflt(b, d.in);
    // Past the main pair, 0 leaves an auxiliary output unconnected (ADR-0021).
    b.field("out", &PinMap::out, schema::arrayOf(intS().range(0, kMaxTrackChannels)));
    dflt(b, d.out);
    b.field("key", &PinMap::key, pins);
    dflt(b, d.key);
    return b.build();
}

Schema<MidiControl> midiControlSchema() {
    const MidiControl d;
    auto b = schema::object<MidiControl>("midi_control");
    b.field("kind", &MidiControl::kind,
            en<ControlKind>({{ControlKind::Cc, "cc"},
                             {ControlKind::Note, "note"},
                             {ControlKind::PitchBend, "pitch_bend"},
                             {ControlKind::Program, "program"}}));
    b.field("channel", &MidiControl::channel, intS().range(1, 16));
    b.field("number", &MidiControl::number, intS().range(0, 127));
    dflt(b, d.number);
    b.field("device", &MidiControl::device, str());
    dflt(b, d.device);
    return b.build();
}

Schema<ControlMode> controlModeSchema() {
    return en<ControlMode>({{ControlMode::Absolute, "absolute"},
                            {ControlMode::Relative1, "relative1"},
                            {ControlMode::Relative2, "relative2"},
                            {ControlMode::Relative3, "relative3"},
                            {ControlMode::Toggle, "toggle"}});
}

Schema<ParamLearn> paramLearnSchema() {
    const ParamLearn d;
    auto b = schema::object<ParamLearn>("param_learn");
    b.field("param", &ParamLearn::param, str());
    controlBindingFields(b);
    b.field("when_selected", &ParamLearn::whenSelected, boolean());
    dflt(b, d.whenSelected);
    b.field("when_focused", &ParamLearn::whenFocused, boolean());
    dflt(b, d.whenFocused);
    b.field("when_visible", &ParamLearn::whenVisible, boolean());
    dflt(b, d.whenVisible);
    return b.build();
}

Schema<ControlLearn> controlLearnSchema() {
    auto b = schema::object<ControlLearn>("control_learn");
    b.field("param", &ControlLearn::param, str());
    controlBindingFields(b);
    return b.build();
}

template <>
const Schema<FxInstance>& schemaOf<FxInstance>() {
    static const Schema<FxInstance> s = [] {
        const FxInstance d;
        auto plugin = schema::object<PluginRef>("plugin_ref")
                          .field("format", &PluginRef::format, str())
                          .field("id", &PluginRef::id, str())
                          .field("name", &PluginRef::name, str())
                          .defaultValue(std::string())
                          .omitIfDefault()
                          .field("vendor", &PluginRef::vendor, str())
                          .defaultValue(std::string())
                          .omitIfDefault()
                          .field("version", &PluginRef::version, str())
                          .defaultValue(std::string())
                          .omitIfDefault()
                          .build();
        ObjectBuilder<FxInstance> b = schema::object<FxInstance>("fx");
        b.field("plugin", &FxInstance::plugin, plugin);
        b.field("pins", &FxInstance::pins, pinMapSchema());
        dflt(b, d.pins);
        b.field("label", &FxInstance::label, str());
        dflt(b, d.label);
        b.field("bypass", &FxInstance::bypass, boolean());
        dflt(b, d.bypass);
        b.field("offline", &FxInstance::offline, boolean());
        dflt(b, d.offline);
        b.field("wet", &FxInstance::wet, num().range(0, 100));
        dflt(b, d.wet);
        b.field("isolation", &FxInstance::isolation,
                en<Isolation>({{Isolation::Inherit, "inherit"},
                               {Isolation::InProcess, "in_process"},
                               {Isolation::SharedBridge, "shared_bridge"},
                               {Isolation::Dedicated, "dedicated"}}));
        dflt(b, d.isolation);
        b.field("oversampling", &FxInstance::oversampling, oversamplingSchema());
        dflt(b, d.oversampling);
        b.field("parallel", &FxInstance::parallel, boolean());
        dflt(b, d.parallel);
        b.field("pdc", &FxInstance::pdc,
                en<PdcMode>({{PdcMode::Compensate, "compensate"}, {PdcMode::Ignore, "ignore"}}));
        dflt(b, d.pdc);
        b.field("auto_bypass_on_silence", &FxInstance::autoBypassOnSilence, boolean());
        dflt(b, d.autoBypassOnSilence);
        b.field("patch", &FxInstance::patch, jsonObject());
        b.field("state", &FxInstance::state, str());
        b.field("params", &FxInstance::params, paramValuesSchema());
        dflt(b, d.params);
        b.field("learn", &FxInstance::learn, schema::arrayOf(paramLearnSchema()));
        dflt(b, d.learn);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Route>& schemaOf<Route>() {
    static const Schema<Route> s = [] {
        const Route d;
        auto end =
            schema::object<RouteEnd>("route_end")
                .field("track", &RouteEnd::track, idSchema())
                .field("fx", &RouteEnd::fx, idSchema())
                .field("hw_channels", &RouteEnd::hwChannels, schema::arrayOf(intS().range(1, 1024)))
                .field("midi_device", &RouteEnd::midiDevice, str())
                .field("module", &RouteEnd::module, idSchema())
                .build();
        auto channels =
            schema::object<ChannelMap>("channel_map")
                .field("from", &ChannelMap::from,
                       schema::arrayOf(intS().range(1, kMaxTrackChannels)))
                .field("to", &ChannelMap::to, schema::arrayOf(intS().range(1, kMaxTrackChannels)))
                .build();
        auto midiR = schema::object<MidiRouting>("midi_routing")
                         .field("from_channel", &MidiRouting::fromChannel, intS().range(0, 16))
                         .field("to_channel", &MidiRouting::toChannel, intS().range(0, 16))
                         .build();
        ObjectBuilder<Route> b = schema::object<Route>("route");
        b.field("from", &Route::from, end);
        b.field("to", &Route::to, end);
        b.field("tap", &Route::tap,
                en<Tap>({{Tap::PostFader, "post_fader"},
                         {Tap::PreFader, "pre_fader"},
                         {Tap::PreFx, "pre_fx"}}));
        dflt(b, d.tap);
        b.field("volume_db", &Route::volumeDb, num().range(kSilenceDb, 12));
        dflt(b, d.volumeDb);
        b.field("pan", &Route::pan, routePanSchema());
        dflt(b, d.pan);
        b.field("mute", &Route::mute, boolean());
        dflt(b, d.mute);
        b.field("phase_invert", &Route::phaseInvert, boolean());
        dflt(b, d.phaseInvert);
        b.field("mono", &Route::mono, boolean());
        dflt(b, d.mono);
        b.field("channels", &Route::channels, channels);
        dflt(b, d.channels);
        b.field("midi", &Route::midi, midiR);
        b.field("learn", &Route::learn, schema::arrayOf(controlLearnSchema()));
        dflt(b, d.learn);
        return b.build();
    }();
    return s;
}

namespace {
Schema<EnvPoint> envPointSchema() {
    return schema::object<EnvPoint>("env_point")
        .field("pos", &EnvPoint::pos, num())
        .field("value", &EnvPoint::value, num())
        .field("shape", &EnvPoint::shape,
               en<PointShape>({{PointShape::Linear, "linear"},
                               {PointShape::Square, "square"},
                               {PointShape::Slow, "slow"},
                               {PointShape::FastStart, "fast_start"},
                               {PointShape::FastEnd, "fast_end"},
                               {PointShape::Bezier, "bezier"}}))
        .defaultValue(PointShape::Linear)
        .omitIfDefault()
        .field("tension", &EnvPoint::tension, num().range(-1, 1))
        .defaultValue(0.0)
        .omitIfDefault()
        .build();
}
}  // namespace

template <>
const Schema<Envelope>& schemaOf<Envelope>() {
    static const Schema<Envelope> s = [] {
        const Envelope d;
        auto target = schema::object<EnvTarget>("env_target")
                          .field("object", &EnvTarget::object, idSchema())
                          .field("param", &EnvTarget::param, str().minLength(1))
                          .build();
        auto point = envPointSchema();
        ObjectBuilder<Envelope> b = schema::object<Envelope>("envelope");
        b.field("target", &Envelope::target, target);
        b.field("mode", &Envelope::mode,
                en<EnvMode>({{EnvMode::Trim, "trim"},
                             {EnvMode::Read, "read"},
                             {EnvMode::Touch, "touch"},
                             {EnvMode::Latch, "latch"},
                             {EnvMode::LatchPreview, "latch_preview"},
                             {EnvMode::Write, "write"}}));
        dflt(b, d.mode);
        b.field("active", &Envelope::active, boolean());
        dflt(b, d.active);
        b.field("armed", &Envelope::armed, boolean());
        dflt(b, d.armed);
        b.field("visible", &Envelope::visible, boolean());
        dflt(b, d.visible);
        b.field("timebase", &Envelope::timebase, timebaseS());
        dflt(b, d.timebase);
        b.field("points", &Envelope::points, schema::arrayOf(point));
        dflt(b, d.points);
        return b.build();
    }();
    return s;
}

template <>
const Schema<AutomationItem>& schemaOf<AutomationItem>() {
    static const Schema<AutomationItem> s = [] {
        const AutomationItem d;
        ObjectBuilder<AutomationItem> b = schema::object<AutomationItem>("automation_item");
        b.field("envelope", &AutomationItem::envelope, idSchema());
        b.field("position", &AutomationItem::position, num().min(0));
        b.field("length", &AutomationItem::length, num().exclusiveMin(0));
        b.field("source_length", &AutomationItem::sourceLength, num().exclusiveMin(0));
        b.field("loop", &AutomationItem::loop, boolean());
        dflt(b, d.loop);
        b.field("pool", &AutomationItem::pool, intS().min(0));
        dflt(b, d.pool);
        b.field("points", &AutomationItem::points, schema::arrayOf(envPointSchema()));
        dflt(b, d.points);
        b.field("baseline", &AutomationItem::baseline, num());
        dflt(b, d.baseline);
        b.field("amplitude", &AutomationItem::amplitude, num());
        dflt(b, d.amplitude);
        b.field("name", &AutomationItem::name, str());
        dflt(b, d.name);
        return b.build();
    }();
    return s;
}

Schema<CableEnd> cableEndSchema() {
    json::Value ex = json::Value::object();
    ex["type"] = "string";
    ex["pattern"] = std::string(kCableEndPattern);
    return Schema<CableEnd>(
        "cable end",
        [](const json::Value& v, CableEnd& out, schema::DecodeCtx& c, const std::string& p) {
            auto end = v.isString() ? CableEnd::parse(v.asString()) : std::nullopt;
            if (!end) {
                c.error("schema.cable_end", "expected \"<module>.<key>\"", p,
                        "e.g. \"mo_3.out\", \"mo_4.cutoff\" or \"fx_9.12\"");
                return false;
            }
            out = std::move(*end);
            return true;
        },
        [](const CableEnd& end) { return json::Value(end.str()); }, ex);
}

namespace {
// A module's knobs by key: numbers, choices' keys and toggles' bools.
Schema<json::Value> moduleParamsSchema() {
    json::Value spec = json::Value::object();
    spec["type"] = "object";
    json::Value values = json::Value::object();
    values["type"] =
        json::Array{json::Value("number"), json::Value("string"), json::Value("boolean")};
    spec["additionalProperties"] = std::move(values);
    return {"module params",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& c, const std::string& p) {
                if (!v.isObject()) {
                    c.error("schema.type", "expected an object of parameter values", p);
                    return false;
                }
                for (const auto& m : v.asObject())
                    if (!(m.value.isNumber() && std::isfinite(m.value.asDouble())) &&
                        !m.value.isString() && !m.value.isBool()) {
                        c.error("schema.type", "a parameter is a number, a choice's key or a bool",
                                json::pointerJoin(p, m.key), "e.g. {\"cutoff\": 800}");
                        return false;
                    }
                out = v;
                return true;
            },
            [](const json::Value& v) { return v; }, spec};
}
}  // namespace

template <>
const Schema<Rack>& schemaOf<Rack>() {
    static const Schema<Rack> s = [] {
        const Rack d;
        ObjectBuilder<Rack> b = schema::object<Rack>("rack");
        b.field("track", &Rack::track, idSchema());
        b.field("name", &Rack::name, str());
        dflt(b, d.name);
        return b.build();
    }();
    return s;
}

template <>
const Schema<RackModule>& schemaOf<RackModule>() {
    static const Schema<RackModule> s = [] {
        const RackModule d;
        ObjectBuilder<RackModule> b = schema::object<RackModule>("module");
        b.field("rack", &RackModule::rack, idSchema());
        b.field("type", &RackModule::type, str());
        b.field("version", &RackModule::version, str());
        dflt(b, d.version);
        b.field("label", &RackModule::label, str());
        dflt(b, d.label);
        b.field("x", &RackModule::x, num().range(-kMaxRackCoordinate, kMaxRackCoordinate));
        dflt(b, d.x);
        b.field("y", &RackModule::y, num().range(-kMaxRackCoordinate, kMaxRackCoordinate));
        dflt(b, d.y);
        b.field("params", &RackModule::params, moduleParamsSchema());
        dflt(b, d.params);
        b.field("bypass", &RackModule::bypass, boolean());
        dflt(b, d.bypass);
        b.field("source", &RackModule::source, idSchema());
        b.field("source_off", &RackModule::sourceOff, boolean());
        dflt(b, d.sourceOff);
        b.field("device", &RackModule::device, str());
        b.field("fx", &RackModule::fx, idSchema());
        b.field("learn", &RackModule::learn, schema::arrayOf(paramLearnSchema()));
        dflt(b, d.learn);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Cable>& schemaOf<Cable>() {
    static const Schema<Cable> s = [] {
        const Cable d;
        ObjectBuilder<Cable> b = schema::object<Cable>("cable");
        b.field("from", &Cable::from, cableEndSchema());
        b.field("to", &Cable::to, cableEndSchema());
        b.field("amount", &Cable::amount, num());
        b.field("color", &Cable::color, str());
        dflt(b, d.color);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Marker>& schemaOf<Marker>() {
    static const Schema<Marker> s = [] {
        const Marker d;
        ObjectBuilder<Marker> b = schema::object<Marker>("marker");
        b.field("pos", &Marker::pos, num());
        b.field("timebase", &Marker::timebase, timebaseS());
        dflt(b, d.timebase);
        b.field("name", &Marker::name, str());
        dflt(b, d.name);
        b.field("color", &Marker::color, str());
        dflt(b, d.color);
        return b.build();
    }();
    return s;
}

template <>
const Schema<Region>& schemaOf<Region>() {
    static const Schema<Region> s = [] {
        const Region d;
        ObjectBuilder<Region> b = schema::object<Region>("region");
        b.field("start", &Region::start, num());
        b.field("end", &Region::end, num());
        b.field("timebase", &Region::timebase, timebaseS());
        dflt(b, d.timebase);
        b.field("name", &Region::name, str());
        dflt(b, d.name);
        b.field("color", &Region::color, str());
        dflt(b, d.color);
        return b.build();
    }();
    return s;
}

// --- the project file ---------------------------------------------------------------

namespace {

template <class T>
json::Value mapToJson(const IdMap<T>& m) {
    json::Value v = json::Value::object();
    for (const auto& e : m) v[e.id.str()] = schemaOf<T>().encode(*e.value);
    return v;
}

// Decodes {id: object} of one kind into the builder, collecting errors.
template <class T>
void mapFromJson(const json::Value* v, const std::string& path, Kind kind,
                 typename IdMap<T>::Builder& out, std::vector<Error>& errors,
                 const schema::DecodeOptions& options, std::vector<schema::Warning>* warnings) {
    if (!v) return;
    if (!v->isObject()) {
        errors.emplace_back("schema.type", "expected an object keyed by id", path);
        return;
    }
    for (const auto& m : v->asObject()) {
        const std::string at = json::pointerJoin(path, m.key);
        auto id = Id::parse(m.key);
        if (!id || id->kind != kind) {
            errors.emplace_back("project.id",
                                "\"" + m.key + "\" is not a " + std::string(prefix(kind)) + "_ id",
                                at, "keys here look like " + std::string(prefix(kind)) + "_1");
            continue;
        }
        std::vector<schema::Warning> localWarnings;
        auto obj = schemaOf<T>().decode(m.value, options, &localWarnings);
        if (warnings)
            for (auto& warning : localWarnings) {
                warning.path = at + warning.path;
                warnings->push_back(std::move(warning));
            }
        if (!obj) {
            Error e = std::move(obj).error();
            auto fix = [&](Error& x) { x.path = at + x.path; };
            fix(e);
            for (auto& d : e.details) fix(d);
            errors.push_back(std::move(e));
            continue;
        }
        out.put(*id, 0, std::make_shared<const T>(std::move(*obj)));
    }
}

}  // namespace

json::Value projectToJson(const Session& s) {
    json::Value doc = json::Value::object();
    doc["schema"] = std::string(kProjectSchema);
    json::Value project = schemaOf<Project>().encode(*s.project);
    json::Value next = json::Value::object();
    for (std::size_t k = 0; k < kKindCount; ++k) {
        const auto kind = static_cast<Kind>(k);
        // The rack kinds (2.7) are written once used: older projects stay
        // byte-identical but for the schema.
        const bool rack = kind == Kind::Rack || kind == Kind::Module || kind == Kind::Cable;
        if (rack && s.next.next[k] <= 1) continue;
        next[std::string(prefix(kind))] = s.next.next[k];
    }
    project["next_ids"] = std::move(next);
    doc["project"] = std::move(project);
    doc["tempo"]["points"] = mapToJson(s.tempo);
    doc["tempo"]["meters"] = mapToJson(s.meters);
    doc["rulers"]["markers"] = mapToJson(s.markers);
    doc["rulers"]["regions"] = mapToJson(s.regions);
    doc["tracks"] = mapToJson(s.tracks);
    doc["items"] = mapToJson(s.items);
    doc["takes"] = mapToJson(s.takes);
    doc["fx"] = mapToJson(s.fx);
    doc["sources"]["media"] = mapToJson(s.media);
    doc["sources"]["midi"] = mapToJson(s.midi);
    doc["routes"] = mapToJson(s.routes);
    doc["envelopes"] = mapToJson(s.envelopes);
    for (const auto& e : s.envelopes) {
        auto list = json::Value::array();
        for (const auto& item : s.autoItems)
            if (item.value->envelope == e.id) list.push_back(item.id.str());
        doc["envelopes"][e.id.str()]["items"] = std::move(list);
    }
    // Written only when there are some: projects without them stay byte-identical.
    if (!s.autoItems.empty()) doc["automation_items"] = mapToJson(s.autoItems);
    if (!s.groups.empty()) doc["groups"] = mapToJson(s.groups);
    if (!s.racks.empty()) doc["racks"] = mapToJson(s.racks);
    if (!s.modules.empty()) doc["modules"] = mapToJson(s.modules);
    if (!s.cables.empty()) doc["cables"] = mapToJson(s.cables);
    json::Value blobs = json::Value::object();
    for (const auto& [hash, blob] : s.blobs) {
        json::Value b = json::Value::object();
        b["size"] = blob->data.size();
        b["encoding"] = "base64";
        b["data"] = base64Encode(blob->data);
        blobs[hash] = std::move(b);
    }
    doc["blobs"] = std::move(blobs);
    for (const auto& section : s.extraSections.asObject()) doc[section.key] = section.value;
    return doc;
}

std::string serializeProject(const Session& s) {
    return json::writeCanonical(projectToJson(s)) + "\n";
}

Result<Session> projectFromJson(const json::Value& input, std::vector<schema::Warning>* warnings) {
    json::Value doc = input;
    if (!doc.isObject()) return Error("project.type", "a project is a JSON object");
    std::map<std::string, json::Value> derivedItems;
    if (auto* envs = doc.asObject().find("envelopes"); envs && envs->isObject())
        for (auto& entry : envs->asObject())
            if (entry.value.isObject())
                if (auto* items = entry.value.find("items")) {
                    const auto path = json::pointerJoin("/envelopes", entry.key) + "/items";
                    if (!items->isArray()) return Error("schema.type", "expected an array", path);
                    for (std::size_t i = 0; i < items->size(); ++i)
                        if (!items->at(i).isString())
                            return Error("schema.type", "expected an item id string",
                                         path + "/" + std::to_string(i));
                    derivedItems.emplace(entry.key, *items);
                    entry.value.asObject().erase("items");
                }
    const json::Value* schemaId = doc.find("schema");
    if (!schemaId || !schemaId->isString())
        return Error("project.schema", "missing \"schema\"", "/schema",
                     "\"schema\": \"" + std::string(kProjectSchema) + "\"");
    auto version = schema::parseDocumentId(schemaId->asString());
    if (!version || version->ns != "supple" || version->type != "project" || version->major < 1 ||
        version->major > 2)
        return Error("project.version", "unsupported project format " + schemaId->asString(),
                     "/schema",
                     "this build reads supple.project/1.x and " + std::string(kProjectSchema));
    schema::DecodeOptions options;
    if (version->major == 1) {
        auto migrated = migrateProject1(std::move(doc), warnings);
        if (!migrated) return std::move(migrated).error();
        doc = std::move(*migrated);
    } else if (version->minor > 10 && warnings)
        warnings->push_back(
            {"project.newer_minor", "loading a newer project minor version", "/schema"});
    // Before 2.4 a master's hardware output could be written as a route to
    // hw_channels, which nothing played (ADR-0056): it becomes the master's
    // hw_output (its first such route), and the route goes.
    if (version->major == 1 || version->minor < 4)
        if (auto* routes = doc.asObject().find("routes"); routes && routes->isObject()) {
            auto* tracks = doc.asObject().find("tracks");
            std::vector<std::string> moved;
            for (const auto& entry : routes->asObject()) {
                const json::Value* from = entry.value.find("from");
                const json::Value* to = entry.value.find("to");
                const json::Value* channels = to ? to->find("hw_channels") : nullptr;
                const json::Value* source = from ? from->find("track") : nullptr;
                if (!channels || !channels->isArray() || !source || !source->isString() ||
                    !tracks || !tracks->isObject())
                    continue;
                json::Value* track = tracks->asObject().find(source->asString());
                const json::Value* type = track ? track->find("type") : nullptr;
                if (!track || !track->isObject() || !type || !type->isString() ||
                    type->asString() != "master")
                    continue;
                if (!track->find("hw_output")) {
                    json::Value out = json::Value::array();
                    for (std::size_t i = 0; i < channels->size() && i < 2; ++i)
                        out.push_back(channels->at(i));
                    (*track)["hw_output"] = std::move(out);
                }
                moved.push_back(entry.key);
            }
            for (const auto& id : moved) {
                routes->asObject().erase(id);
                if (warnings)
                    warnings->push_back({"project.hw_output_route",
                                         "moved a master's hardware-output route into its "
                                         "hw_output",
                                         json::pointerJoin("/routes", id)});
            }
        }
    std::uint32_t legacyGroupNext = 1;
    // Previously opaque groups never become active merely by opening an old file.
    if ((version->major == 1 || version->minor < 2) && doc.find("groups")) {
        auto legacy = doc["groups"];
        if (legacy.isObject())
            for (const auto& entry : legacy.asObject())
                if (auto id = Id::parse(entry.key); id && id->kind == Kind::Group) {
                    if (id->n == UINT32_MAX)
                        return Error("project.id", "legacy group id exhausted",
                                     "/groups/" + entry.key, "use a smaller group token");
                    legacyGroupNext = std::max(legacyGroupNext, id->n + 1);
                }
        if (auto* ext = doc.find("extensions"); ext && !ext->isObject())
            return Error("schema.type", "expected an extensions object", "/extensions",
                         "keep extensions as an object");
        std::string key = "legacy_track_groups";
        for (int suffix = 1; doc["extensions"].find(key); ++suffix)
            key = "legacy_track_groups_" + std::to_string(suffix);
        doc["extensions"][key] = std::move(legacy);
        doc.asObject().erase("groups");
        if (warnings)
            warnings->push_back({"project.groups_legacy",
                                 "preserved inactive legacy groups in extensions." + key,
                                 "/groups"});
    }
    options.unknownMembers = schema::UnknownMembers::Ignore;
    std::vector<Error> errors;
    Session s;
    s.version = 0;
    auto warn = [&](std::string code, std::string message, std::string path) {
        if (warnings) warnings->push_back({std::move(code), std::move(message), std::move(path)});
    };
    const std::set<std::string_view> known{
        "schema", "project", "tempo",   "rulers",  "tracks",    "items",
        "takes",  "fx",      "sources", "routes",  "envelopes", "automation_items",
        "blobs",  "groups",  "racks",   "modules", "cables"};
    for (const auto& section : doc.asObject()) {
        const auto path = json::pointerJoin("", section.key);
        if (section.key == "ui") {
            warn("project.ui_sidecar", "ignored UI state; it lives in <name>.ui.json", path);
        } else if (!known.count(section.key)) {
            if (section.key == "extensions" && !section.value.isObject())
                errors.emplace_back("schema.type", "expected an extensions object", path);
            else {
                s.extraSections[section.key] = section.value;
                if (section.key == "groups")
                    warn("project.groups_unsupported", "track groups are not yet implemented",
                         path);
            }
        }
    }
    auto unknown = [&](const json::Value& value, const std::set<std::string_view>& names,
                       const std::string& path) {
        if (value.isObject())
            for (const auto& field : value.asObject())
                if (!names.count(field.key))
                    warn("schema.unknown_member", "ignored unknown member",
                         json::pointerJoin(path, field.key));
    };
    for (const auto& [name, fields] :
         std::vector<std::pair<std::string_view, std::set<std::string_view>>>{
             {"tempo", {"points", "meters"}},
             {"rulers", {"markers", "regions"}},
             {"sources", {"media", "midi"}}})
        if (const auto* value = doc.find(name)) {
            if (!value->isObject())
                errors.emplace_back("schema.type", "expected an object", "/" + std::string(name));
            else
                unknown(*value, fields, "/" + std::string(name));
        }
    if (const json::Value* p = doc.find("project")) {
        json::Value copy = *p;
        const json::Value* next = p->find("next_ids");
        if (copy.isObject()) copy.asObject().erase("next_ids");
        std::vector<schema::Warning> localWarnings;
        auto project = schemaOf<Project>().decode(copy, options, &localWarnings);
        if (warnings)
            for (auto& warning : localWarnings) {
                warning.path = "/project" + warning.path;
                warnings->push_back(std::move(warning));
            }
        if (!project) {
            Error e = std::move(project).error();
            e.path = "/project" + e.path;
            for (auto& d : e.details) d.path = "/project" + d.path;
            errors.push_back(std::move(e));
        } else {
            s.project = std::make_shared<const Project>(std::move(*project));
        }
        if (next) {
            if (!next->isObject())
                errors.emplace_back("schema.type", "expected next_ids object", "/project/next_ids");
            else
                for (const auto& m : next->asObject()) {
                    const auto k = kindFromPrefix(m.key);
                    if (!k) {
                        warn("schema.unknown_member", "ignored unknown member",
                             json::pointerJoin("/project/next_ids", m.key));
                        continue;
                    }
                    auto counter = schema::integerOf<std::uint32_t>().decode(m.value);
                    if (!counter) {
                        auto error = std::move(counter).error();
                        error.path = json::pointerJoin("/project/next_ids", m.key) + error.path;
                        errors.push_back(std::move(error));
                    } else
                        s.next[*k] = *counter;
                }
        }
    }
    // The rack kinds' counters are written once used (2.7); before, they
    // start where a new session's do.
    for (Kind k : {Kind::Rack, Kind::Module, Kind::Cable})
        if (s.next[k] == 0) s.next[k] = 1;
    auto sub = [&](const char* a, const char* b) -> const json::Value* {
        const json::Value* x = doc.find(a);
        return x && b ? x->find(b) : x;
    };
    IdMap<TempoPointObj>::Builder tempo(s.tempo);
    IdMap<MeterObj>::Builder meters(s.meters);
    IdMap<Marker>::Builder markers(s.markers);
    IdMap<Region>::Builder regions(s.regions);
    IdMap<Track>::Builder tracks(s.tracks);
    IdMap<TrackGroup>::Builder groups(s.groups);
    IdMap<Item>::Builder items(s.items);
    IdMap<Take>::Builder takes(s.takes);
    IdMap<MediaSource>::Builder media(s.media);
    IdMap<MidiSource>::Builder midis(s.midi);
    IdMap<Route>::Builder routes(s.routes);
    IdMap<Envelope>::Builder envelopes(s.envelopes);
    IdMap<AutomationItem>::Builder autoItems(s.autoItems);
    IdMap<Rack>::Builder racks(s.racks);
    IdMap<RackModule>::Builder modules(s.modules);
    IdMap<Cable>::Builder cables(s.cables);
    mapFromJson<TempoPointObj>(sub("tempo", "points"), "/tempo/points", Kind::TempoPoint, tempo,
                               errors, options, warnings);
    mapFromJson<MeterObj>(sub("tempo", "meters"), "/tempo/meters", Kind::Meter, meters, errors,
                          options, warnings);
    mapFromJson<Marker>(sub("rulers", "markers"), "/rulers/markers", Kind::Marker, markers, errors,
                        options, warnings);
    mapFromJson<Region>(sub("rulers", "regions"), "/rulers/regions", Kind::Region, regions, errors,
                        options, warnings);
    mapFromJson<Track>(sub("tracks", nullptr), "/tracks", Kind::Track, tracks, errors, options,
                       warnings);
    mapFromJson<TrackGroup>(sub("groups", nullptr), "/groups", Kind::Group, groups, errors, options,
                            warnings);
    mapFromJson<Item>(sub("items", nullptr), "/items", Kind::Item, items, errors, options,
                      warnings);
    mapFromJson<Take>(sub("takes", nullptr), "/takes", Kind::Take, takes, errors, options,
                      warnings);
    mapFromJson<MediaSource>(sub("sources", "media"), "/sources/media", Kind::Media, media, errors,
                             options, warnings);
    mapFromJson<MidiSource>(sub("sources", "midi"), "/sources/midi", Kind::Midi, midis, errors,
                            options, warnings);
    mapFromJson<Route>(sub("routes", nullptr), "/routes", Kind::Route, routes, errors, options,
                       warnings);
    mapFromJson<Envelope>(sub("envelopes", nullptr), "/envelopes", Kind::Envelope, envelopes,
                          errors, options, warnings);
    mapFromJson<AutomationItem>(sub("automation_items", nullptr), "/automation_items",
                                Kind::AutoItem, autoItems, errors, options, warnings);
    mapFromJson<Rack>(sub("racks", nullptr), "/racks", Kind::Rack, racks, errors, options,
                      warnings);
    mapFromJson<RackModule>(sub("modules", nullptr), "/modules", Kind::Module, modules, errors,
                            options, warnings);
    mapFromJson<Cable>(sub("cables", nullptr), "/cables", Kind::Cable, cables, errors, options,
                       warnings);
    // fx live under "fx" (a top-level collection keyed by fx id).
    IdMap<FxInstance>::Builder fx(s.fx);
    mapFromJson<FxInstance>(sub("fx", nullptr), "/fx", Kind::Fx, fx, errors, options, warnings);
    if (const json::Value* blobs = doc.find("blobs"); blobs && blobs->isObject()) {
        for (const auto& m : blobs->asObject()) {
            const auto path = json::pointerJoin("/blobs", m.key);
            if (!m.value.isObject()) {
                errors.emplace_back("schema.type", "expected a blob object", path);
                continue;
            }
            unknown(m.value, {"size", "encoding", "data"}, path);
            if (const auto* encoding = m.value.find("encoding");
                !encoding || !encoding->isString() || *encoding != "base64") {
                errors.emplace_back("project.blob", "encoding must be base64", path + "/encoding");
                continue;
            }
            const json::Value* data = m.value.find("data");
            auto bytes =
                data && data->isString()
                    ? base64Decode(data->asString())
                    : Result<std::vector<std::uint8_t>>(Error("base64.invalid", "missing data"));
            if (!bytes) {
                errors.emplace_back("project.blob", "blob " + m.key + " is not valid base64",
                                    json::pointerJoin("/blobs", m.key) + "/data");
                continue;
            }
            auto blob = std::make_shared<Blob>();
            blob->data = std::move(*bytes);
            const auto* size = m.value.find("size");
            auto count = size ? schema::integerOf<std::uint64_t>().decode(*size)
                              : Result<std::uint64_t>(Error("schema.required", "missing size"));
            if (!count || *count != blob->data.size()) {
                errors.emplace_back("project.blob", "size must match decoded blob bytes",
                                    path + "/size");
                continue;
            }
            s.blobs[m.key] = std::move(blob);
        }
    } else if (doc.find("blobs")) {
        errors.emplace_back("schema.type", "expected a blobs object", "/blobs");
    }
    if (!errors.empty()) {
        if (errors.size() == 1) return errors.front();
        Error e("schema.invalid", std::to_string(errors.size()) + " problems in the project");
        e.details = std::move(errors);
        return e;
    }
    s.tempo = tempo.build();
    s.meters = meters.build();
    s.markers = markers.build();
    s.regions = regions.build();
    s.tracks = tracks.build();
    s.groups = groups.build();
    s.items = items.build();
    s.takes = takes.build();
    s.media = media.build();
    s.midi = midis.build();
    s.fx = fx.build();
    s.routes = routes.build();
    s.envelopes = envelopes.build();
    s.autoItems = autoItems.build();
    s.racks = racks.build();
    s.modules = modules.build();
    s.cables = cables.build();
    if (warnings)
        for (const auto& [envelope, given] : derivedItems) {
            auto expected = json::Value::array();
            for (const auto& item : s.autoItems)
                if (item.value->envelope.str() == envelope) expected.push_back(item.id.str());
            if (given != expected)
                warnings->push_back({"project.derived_items",
                                     "ignored stale envelope items; derived from automation_items",
                                     json::pointerJoin("/envelopes", envelope) + "/items"});
        }
    for (const auto& e : s.items)
        if (e.value->group && e.value->group->kind == Kind::Group) {
            if (e.value->group->n == UINT32_MAX)
                return Error("project.id", "legacy group id exhausted",
                             "/items/" + e.id.str() + "/group", "use a smaller group token");
            s.next[Kind::Group] = std::max(s.next[Kind::Group], e.value->group->n + 1);
        }
    s.next[Kind::Group] = std::max(s.next[Kind::Group], legacyGroupNext);
    for (const auto& entry : s.takes) {
        if (auto map = validateWarpMap(*entry.value); !map)
            warn("warp.inactive", "preserved malformed stretch map; warping is inactive",
                 "/takes/" + entry.id.str() + "/stretch_markers");
        for (const auto& marker : entry.value->stretchMarkers)
            if (marker.slope != 0) {
                warn("warp.slope", "preserved slope; only linear interpolation is supported",
                     "/takes/" + entry.id.str() + "/stretch_markers");
                break;
            }
    }
    // Before independent inputs (ADR-0065), choosing a keyboard on a rack's
    // home channel turned its Out route into the MIDI one, silencing the
    // rack. Where that is unambiguous (the rack's one Out has no route), the
    // MIDI route moves to midi_input and the Out feeds its channel again.
    struct Repair {
        Id track, midi, out;
    };
    std::vector<Repair> repairs;
    if (version->major == 1 || version->minor <= 8)
        for (const auto& rk : s.racks) {
            const Track* home = s.tracks.find(rk.value->track);
            if (!home || home->type != StripType::Channel || home->record.midiInput ||
                !home->record.input)
                continue;
            const Route* midiRoute = s.routes.find(*home->record.input);
            if (!midiRoute || !midiRoute->from.midiDevice || midiRoute->to.track != rk.value->track)
                continue;
            std::vector<Id> outs;
            for (const auto& m : s.modules)
                if (m.value->rack == rk.id && m.value->type == kOutModule) outs.push_back(m.id);
            if (outs.size() != 1) continue;
            const bool routed = std::any_of(s.routes.begin(), s.routes.end(), [&](const auto& r) {
                return r.value->from.module == outs.front();
            });
            if (!routed) repairs.push_back({rk.value->track, *home->record.input, outs.front()});
        }
    if (!repairs.empty()) {
        EditTxn fixed(s, 0);
        for (const Repair& r : repairs) {
            Route route;
            route.from.module = r.out;
            route.to.track = r.track;
            const Id made = fixed.create(route);
            fixed.modify<Track>(r.track, [&](Track& t) {
                t.record.midiInput = r.midi;
                t.record.input = made;
            });
        }
        if (auto repaired = fixed.commit()) {
            for (const Repair& r : repairs)
                warn("project.rack_input_repaired",
                     "reconnected " + r.out.str() + " as the channel's input; its MIDI input " +
                         r.midi.str() + " stays beside it",
                     "/tracks/" + r.track.str() + "/record");
            return repaired;
        }
        for (const Repair& r : repairs)
            warn("project.rack_input_unrepaired",
                 r.out.str() + " has no route to its channel; choose it as the input again",
                 "/tracks/" + r.track.str() + "/record");
    }
    // Derive the time map and validate everything through a no-op transaction.
    EditTxn txn(s, 0);
    return txn.commit();
}

Result<Session> parseProject(std::string_view text, std::vector<schema::Warning>* warnings) {
    auto doc = json::parse(text);
    if (!doc) return std::move(doc).error();
    return projectFromJson(*doc, warnings);
}

}  // namespace supple::model
