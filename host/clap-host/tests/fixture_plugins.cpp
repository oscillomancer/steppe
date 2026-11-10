// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Test fixtures: two small, deterministic CLAP plugins built in-tree so the
// host tests are hermetic.
//
//   com.suppleaudio.test.gain  — stereo effect; params "gain" (dB, ADR-0010
//     normalized CLAP value, modulatable by param_mod) and "latency" (0/64 samples, stepped;
//     changing it while active requests a restart); versioned state; the semantic-params extension
//     implemented with core-params + core-patch.
//   com.suppleaudio.test.sine  — instrument; 0 inputs, stereo output, CLAP and
//     MIDI note dialects; one sine voice per key at velocity * 0.25, stops at
//     note off and reports NOTE_END.
//   com.suppleaudio.test.note-echo  — note effect with output note ports: echoes
//     notes a fifth up and emits note/MIDI output of every kind (ADR-0068).
//   com.suppleaudio.test.edge  — stereo pass-through for host edge cases. Its
//     4-byte state is a configuration: byte 0 = refuse start_processing,
//     bytes 1..3 = reported tail in samples (little endian; mode 20: twice
//     that once it has processed a block). Loading state
//     calls request_flush; param 7 ("Flushes") reads back how many times
//     params.flush has been called. Byte 0 = 49: stereo delay, bytes 1..3 =
//     latency in frames. Byte 0 = 50: an underflow probe, out = (in · 2^-100) · 2^100, exact for
//     |in| >= 2^-26 and zero below it when process() runs with FTZ/DAZ (ADR-0025).
#include <clap/clap.h>
#if defined(_WIN32)
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "supple/clap/semantic_params.h"
#include "supple/json/json.h"
#include "supple/params/params.h"
#include "supple/patch/patch.h"

namespace {
void fixtureWrite(int fd, const void* bytes, unsigned count) {
#if defined(_WIN32)
    (void)::_write(fd, bytes, count);
#else
    (void)::write(fd, bytes, count);
#endif
}

using namespace supple;

// --- gain -----------------------------------------------------------------------

constexpr params::Choice kLatencyChoices[] = {{"none", "0 samples"}, {"short", "64 samples"}};
constexpr params::ParamSpec kGainTable[] = {
    {.id = 0,
     .key = "gain",
     .name = "Gain",
     .type = params::Type::Float,
     .unit = units::Unit::dB,
     .min = -60,
     .max = 12,
     .def = 0,
     .digits = 2,
     .description = "Output gain.",
     .flags = params::kAutomatable | params::kModulatable | params::kOffAtMin},
    {.id = 1,
     .key = "latency",
     .name = "Latency",
     .type = params::Type::Enum,
     .max = 1,
     .def = 0,
     .choices = kLatencyChoices,
     .description = "Adds a fixed delay (restart required).",
     .flags = params::kAutomatable},
};
constexpr params::Table kGainT{kGainTable};

const patch::FlatPatchType& gainPatchType() {
    static const patch::FlatPatchType t("test-gain", 1, 0, kGainT, false);
    return t;
}

const clap_plugin_descriptor_t* gainDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.gain",
                                               "Test Gain",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Gain fixture",
                                               features};
    return &d;
}

// A third-party-style CLAP effect: ordinary params and state, no semantic keys.
const clap_plugin_descriptor_t* rawGainDescriptor() {
    static const auto descriptor = [] {
        auto value = *gainDescriptor();
        value.id = "com.suppleaudio.test.raw-gain";
        value.name = "Test Raw Gain";
        return value;
    }();
    return &descriptor;
}

const clap_plugin_descriptor_t* checkedGainDescriptor(bool reject) {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t checked = {CLAP_VERSION_INIT,
                                                     "com.suppleaudio.test.threaded-gain",
                                                     "Thread Checked Gain",
                                                     "Supple Audio",
                                                     "",
                                                     "",
                                                     "",
                                                     "0.1.0",
                                                     "Thread contract fixture",
                                                     features};
    static const clap_plugin_descriptor_t refusing = {CLAP_VERSION_INIT,
                                                      "com.suppleaudio.test.reject-gain",
                                                      "Reject Gain",
                                                      "Supple Audio",
                                                      "",
                                                      "",
                                                      "",
                                                      "0.1.0",
                                                      "Patch refusal fixture",
                                                      features};
    return reject ? &refusing : &checked;
}

const clap_plugin_descriptor_t* slowGainDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.slow-gain",
                                               "Slow Gain",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Slow patch fixture",
                                               features};
    return &d;
}

const clap_plugin_descriptor_t* badCurveDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.bad-curve",
                                               "Bad Curve",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Invalid semantic curve fixture",
                                               features};
    return &d;
}

const clap_plugin_descriptor_t* malformedSchemaDescriptor() {
    static clap_plugin_descriptor_t descriptor = *gainDescriptor();
    descriptor.id = "com.suppleaudio.test.malformed-schema";
    descriptor.name = "Malformed Schema";
    return &descriptor;
}

const clap_plugin_descriptor_t* contractDescriptor(bool missingValue) {
    static clap_plugin_descriptor_t d[2] = {*gainDescriptor(), *gainDescriptor()};
    d[0].id = "com.suppleaudio.test.semantic-id-mismatch";
    d[0].name = "Semantic ID Mismatch";
    d[1].id = "com.suppleaudio.test.missing-value";
    d[1].name = "Missing Value";
    return &d[missingValue ? 1 : 0];
}

const clap_plugin_descriptor_t* oversizedDescriptor(bool ports) {
    static clap_plugin_descriptor_t descriptors[2] = {*gainDescriptor(), *gainDescriptor()};
    descriptors[0].id = "com.suppleaudio.test.many-ports";
    descriptors[0].name = "Too Many Ports";
    descriptors[1].id = "com.suppleaudio.test.many-channels";
    descriptors[1].name = "Too Many Channels";
    return &descriptors[ports ? 0 : 1];
}

std::atomic<int> gThreadFailures{0};
std::atomic<int> gPatchStarts{0};
std::atomic<std::uint32_t> gGainCreates{0};

struct Gain {
    Gain() { ++gGainCreates; }
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    std::atomic<double> gainDb{0.0};
    std::atomic<int> latencyChoice{0};
    std::atomic<bool> editorPending{false};
    std::atomic<std::uint32_t> editorFlags{0}, hostDeliveries{0};
    int activeLatency = 0;  // latency in samples at activation
    double gain = 1.0;      // audio thread
    double gainMod = 0.0;   // audio thread: CLAP param_mod on gain, in CLAP units
    float delay[2][64] = {};
    int delayPos = 0;
    bool noSemantic = false;
    bool enforceThread = false;
    bool rejectPatch = false;
    bool slowPatch = false;
    bool mismatchedId = false, missingValue = false;
    bool malformedSchema = false;
    bool badCurve = false;

    void applyParam(clap_id id, double clapValue) {
        ++hostDeliveries;
        if (const auto* p = params::findById(kGainT, id)) {
            const double v = params::fromClap(*p, clapValue);
            if (id == 0) gainDb = v;
            if (id == 1 && static_cast<int>(v) != latencyChoice.load()) {
                latencyChoice = static_cast<int>(v);
                if (host && host->request_restart) host->request_restart(host);
            }
        }
    }
    static double dbToGain(double db) { return db <= -60 ? 0.0 : std::pow(10.0, db / 20.0); }

    patch::Patch<patch::FlatBody> current() const {
        auto p = gainPatchType().defaults();
        p.body.values[0] = gainDb.load();
        p.body.values[1] = latencyChoice.load();
        return p;
    }
};

Gain* G(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

bool checkGainMain(const clap_plugin_t* p) {
    if (!G(p)->enforceThread) return true;
    const auto* h = G(p)->host;
    const auto* check =
        static_cast<const clap_host_thread_check_t*>(h->get_extension(h, CLAP_EXT_THREAD_CHECK));
    const bool ok = check && check->is_main_thread(h);
    if (!ok) gThreadFailures.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

uint32_t CLAP_ABI gainParamCount(const clap_plugin_t*) { return 2; }
bool CLAP_ABI gainParamInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index >= 2) return false;
    const auto& p = kGainTable[index];
    std::memset(info, 0, sizeof(*info));
    info->id = p.id;
    info->flags = params::clapFlags(p);
    std::snprintf(info->name, sizeof(info->name), "%.*s", static_cast<int>(p.name.size()),
                  p.name.data());
    const auto r = params::clapRange(p);
    info->min_value = r.min;
    info->max_value = r.max;
    info->default_value = r.def;
    return true;
}
bool CLAP_ABI gainParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
    auto* g = G(pl);
    if (g->missingValue && id == 0) return false;
    if (id == 0)
        *out = params::toClap(kGainTable[0], g->gainDb);
    else if (id == 1)
        *out = g->latencyChoice;
    else
        return false;
    return true;
}
bool CLAP_ABI gainValueToText(const clap_plugin_t*, clap_id id, double v, char* out,
                              uint32_t size) {
    const auto* p = params::findById(kGainT, id);
    if (!p) return false;
    std::snprintf(out, size, "%s", params::format(*p, params::fromClap(*p, v)).c_str());
    return true;
}
bool CLAP_ABI gainTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    const auto* p = params::findById(kGainT, id);
    if (!p) return false;
    auto v = params::parse(*p, text);
    if (!v) return false;
    *out = params::toClap(*p, *v);
    return true;
}
void gainEditorOutput(const clap_plugin_t* pl, const clap_output_events_t* out) {
    if (G(pl)->editorPending.exchange(false)) {
        clap_event_param_gesture_t gesture{};
        gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID,
                          CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_IS_LIVE};
        gesture.param_id = 0;
        out->try_push(out, &gesture.header);
        clap_event_param_value_t value{};
        value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE,
                        G(pl)->editorFlags.load()};
        value.param_id = 0;
        value.value = params::toClap(kGainTable[0], G(pl)->gainDb);
        value.note_id = value.port_index = value.channel = value.key = -1;
        out->try_push(out, &value.header);
        gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &gesture.header);
    }
}
void CLAP_ABI gainFlush(const clap_plugin_t* pl, const clap_input_events_t* in,
                        const clap_output_events_t* out) {
    for (uint32_t i = 0; i < in->size(in); ++i) {
        const auto* h = in->get(in, i);
        if (h->type == CLAP_EVENT_PARAM_VALUE)
            G(pl)->applyParam(reinterpret_cast<const clap_event_param_value_t*>(h)->param_id,
                              reinterpret_cast<const clap_event_param_value_t*>(h)->value);
    }
    gainEditorOutput(pl, out);
}
const clap_plugin_params_t kGainParams = {gainParamCount,  gainParamInfo,   gainParamValue,
                                          gainValueToText, gainTextToValue, gainFlush};

uint32_t CLAP_ABI gainLatency(const clap_plugin_t* pl) {
    return static_cast<uint32_t>(G(pl)->activeLatency);
}
const clap_plugin_latency_t kGainLatency = {gainLatency};

bool CLAP_ABI gainStateSave(const clap_plugin_t* pl, const clap_ostream_t* os) {
    const std::string doc = gainPatchType().serialize(G(pl)->current());
    auto bytes = patch::encodeState(doc, 0x54474e31);  // "TGN1"
    if (!bytes) return false;
    return os->write(os, bytes->data(), bytes->size()) == static_cast<int64_t>(bytes->size());
}
bool CLAP_ABI gainStateLoad(const clap_plugin_t* pl, const clap_istream_t* is) {
    std::vector<std::uint8_t> buf;
    std::uint8_t tmp[512];
    for (;;) {
        const int64_t n = is->read(is, tmp, sizeof(tmp));
        if (n < 0) return false;
        if (n == 0) break;
        buf.insert(buf.end(), tmp, tmp + n);
    }
    auto text = patch::decodeState(buf, 0x54474e31);
    if (!text) return false;
    auto p = gainPatchType().parse(*text);
    if (!p) return false;
    G(pl)->gainDb = p->body.values[0];
    G(pl)->latencyChoice = static_cast<int>(p->body.values[1]);
    return true;
}
const clap_plugin_state_t kGainState = {gainStateSave, gainStateLoad};

bool checkedGain(const clap_plugin_t* p) {
    return !std::strcmp(p->desc->id, checkedGainDescriptor(false)->id) ||
           !std::strcmp(p->desc->id, checkedGainDescriptor(true)->id) ||
           !std::strcmp(p->desc->id, slowGainDescriptor()->id) ||
           !std::strcmp(p->desc->id, badCurveDescriptor()->id);
}
uint32_t CLAP_ABI gainPortCount(const clap_plugin_t* p, bool input) {
    if (!std::strcmp(p->desc->id, oversizedDescriptor(true)->id)) return 129;
    return input && checkedGain(p) ? 0 : 1;
}
bool CLAP_ABI gainPortGet(const clap_plugin_t* p, uint32_t index, bool isInput,
                          clap_audio_port_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = !std::strcmp(p->desc->id, oversizedDescriptor(false)->id) ? 65 : 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kGainPorts = {gainPortCount, gainPortGet};
extern const clap_plugin_note_ports_t kSineNotes;

// semantic extension
bool writeString(const clap_ostream_t* os, const std::string& s) {
    return os->write(os, s.data(), s.size()) == static_cast<int64_t>(s.size());
}
std::string errorsJson(const Error& e) {
    json::Value arr = json::Value::array();
    auto one = [](const Error& x) {
        json::Value v = json::Value::object();
        v["code"] = x.code;
        v["message"] = x.message;
        v["path"] = x.path;
        v["hint"] = x.hint;
        return v;
    };
    if (e.details.empty()) arr.push_back(one(e));
    for (const auto& d : e.details) arr.push_back(one(d));
    return json::writeCompact(arr);
}
bool CLAP_ABI semSchema(const clap_plugin_t* pl, const clap_ostream_t* out) {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.semantic-schema/1.0";
    doc["plugin"]["id"] = pl->desc->id;
    doc["patch_type"] = gainPatchType().schemaId();
    doc["params"] = *params::describeTable(kGainT).find("params");
    if (G(pl)->mismatchedId) doc["params"]["gain"]["id"] = 42;
    if (G(pl)->malformedSchema) doc["params"]["gain"]["min"] = "bad";
    if (G(pl)->badCurve) doc["params"]["gain"]["curve"] = "pow:invalid";
    doc["patch"] = gainPatchType().jsonSchema();
    return writeString(out, json::writeCompact(doc));
}
bool CLAP_ABI semGetPatch(const clap_plugin_t* pl, uint32_t detail, const clap_ostream_t* out) {
    if (G(pl)->missingValue) {
        auto doc = json::parse(gainPatchType().serialize(G(pl)->current())).value();
        doc["params"] = json::Value::object();
        return writeString(out, json::writeCompact(doc));
    }
    return writeString(
        out, gainPatchType().serialize(G(pl)->current(), detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED
                                                             ? patch::Detail::Changed
                                                             : patch::Detail::Full));
}
bool CLAP_ABI semSetPatch(const clap_plugin_t* pl, uint32_t mode, const char* text, uint32_t size,
                          const clap_ostream_t* errors) {
    if (!checkGainMain(pl)) return false;
    if (G(pl)->slowPatch) {
        gPatchStarts.fetch_add(1, std::memory_order_release);
        gPatchStarts.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    if (G(pl)->rejectPatch) {
        if (errors) writeString(errors, errorsJson(Error("fixture.reject", "patch rejected")));
        return false;
    }
    auto doc = json::parse(std::string_view(text, size));
    if (!doc) {
        if (errors) writeString(errors, errorsJson(doc.error()));
        return false;
    }
    auto result = mode == CLAP_SUPPLE_SET_REPLACE ? gainPatchType().fromJson(*doc)
                                                  : gainPatchType().merge(G(pl)->current(), *doc);
    if (!result) {
        if (errors) writeString(errors, errorsJson(result.error()));
        return false;
    }
    if (mode == CLAP_SUPPLE_SET_VALIDATE_ONLY) return true;
    ++G(pl)->hostDeliveries;
    G(pl)->gainDb = result->body.values[0];
    const int latency = static_cast<int>(result->body.values[1]);
    if (latency != G(pl)->latencyChoice.exchange(latency) && G(pl)->host->request_restart)
        G(pl)->host->request_restart(G(pl)->host);
    return true;
}
bool CLAP_ABI semDescribe(const clap_plugin_t* pl, uint32_t detail, const clap_ostream_t* out) {
    return writeString(out,
                       "Gain: " + patch::describeFlat(gainPatchType(), G(pl)->current(),
                                                      detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED
                                                          ? patch::Detail::Changed
                                                          : patch::Detail::Full));
}
const clap_plugin_supple_semantic_params_t kGainSemantic = {
    offsetof(clap_plugin_supple_semantic_params_t, get_factory_patches),
    semSchema,
    semGetPatch,
    semSetPatch,
    semDescribe,
    nullptr};

struct GainControl {
    void (*edit)(const clap_plugin_t*, double, std::uint32_t);
    std::uint32_t (*deliveries)(const clap_plugin_t*);
    void (*latency)(const clap_plugin_t*, bool);
    std::uint32_t (*creates)();
};
void gainEditorEdit(const clap_plugin_t* pl, double value, std::uint32_t flags) {
    G(pl)->gainDb = value;
    G(pl)->editorFlags = flags;
    G(pl)->editorPending = true;
    const auto* hp = static_cast<const clap_host_params_t*>(
        G(pl)->host->get_extension(G(pl)->host, CLAP_EXT_PARAMS));
    hp->request_flush(G(pl)->host);
}
std::uint32_t gainHostDeliveries(const clap_plugin_t* pl) { return G(pl)->hostDeliveries.load(); }
void gainRequestLatency(const clap_plugin_t* pl, bool delayed) {
    G(pl)->latencyChoice = delayed ? 1 : 0;
    G(pl)->host->request_restart(G(pl)->host);
}
std::uint32_t gainCreates() { return gGainCreates.load(); }
const GainControl kGainControl{gainEditorEdit, gainHostDeliveries, gainRequestLatency, gainCreates};
const void* CLAP_ABI gainExt(const clap_plugin_t* pl, const char* id) {
    if (!std::strcmp(id, "supple.test.gain-control")) return &kGainControl;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kGainParams;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kGainLatency;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kGainState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    if (checkedGain(pl) && !std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kSineNotes;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS))
        return G(pl)->noSemantic ? nullptr : &kGainSemantic;
    return nullptr;
}

bool CLAP_ABI gainActivate(const clap_plugin_t* pl, double, uint32_t, uint32_t) {
    if (!checkGainMain(pl)) return false;
    auto* g = G(pl);
    g->activeLatency = g->latencyChoice ? 64 : 0;
    std::memset(g->delay, 0, sizeof(g->delay));
    g->delayPos = 0;
    g->gain = Gain::dbToGain(g->gainDb);
    g->gainMod = 0.0;
    return true;
}

clap_process_status CLAP_ABI gainProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    auto* g = G(pl);
    gainEditorOutput(pl, p->out_events);
    const uint32_t n = p->frames_count;
    const uint32_t nev = p->in_events->size(p->in_events);
    uint32_t ev = 0;
    for (uint32_t i = 0; i < n; ++i) {
        while (ev < nev) {
            const auto* h = p->in_events->get(p->in_events, ev);
            if (h->time > i) break;
            if (h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
                g->applyParam(pv->param_id, pv->value);
            } else if (h->type == CLAP_EVENT_PARAM_MOD) {
                const auto* pm = reinterpret_cast<const clap_event_param_mod_t*>(h);
                if (pm->param_id == 0) g->gainMod = pm->amount;
            }
            ++ev;
        }
        // No smoothing: tests check sample-exact changes. Modulation adds in CLAP units.
        const double clap = params::toClap(kGainTable[0], g->gainDb) + g->gainMod;
        g->gain = Gain::dbToGain(params::fromClap(kGainTable[0], std::clamp(clap, 0.0, 1.0)));
        for (int c = 0; c < 2; ++c) {
            float x = p->audio_inputs_count > 0 ? p->audio_inputs[0].data32[c][i] : 0.0f;
            if (g->activeLatency) {
                float& slot = g->delay[c][g->delayPos];
                const float delayed = slot;
                slot = x;
                x = delayed;
            }
            p->audio_outputs[0].data32[c][i] = static_cast<float>(static_cast<double>(x) * g->gain);
        }
        if (g->activeLatency) g->delayPos = (g->delayPos + 1) % 64;
    }
    return CLAP_PROCESS_CONTINUE;
}

// --- sine -------------------------------------------------------------------------

const clap_plugin_descriptor_t* sineDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                     CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.sine",
                                               "Test Sine",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Sine fixture",
                                               features};
    return &d;
}

struct Voice {
    bool on = false;
    int key = 0, channel = 0;
    int32_t noteId = -1;
    double phase = 0, inc = 0, amp = 0;
};
struct Sine {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    double sampleRate = 48000;
    Voice voices[8];
};
Sine* S(const clap_plugin_t* p) { return static_cast<Sine*>(p->plugin_data); }

uint32_t CLAP_ABI sinePortCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
bool CLAP_ABI sinePortGet(const clap_plugin_t* p, uint32_t index, bool isInput,
                          clap_audio_port_info_t* info) {
    if (isInput) return false;
    return gainPortGet(p, index, false, info);
}
const clap_plugin_audio_ports_t kSinePorts = {sinePortCount, sinePortGet};
uint32_t CLAP_ABI sineNoteCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool CLAP_ABI sineNoteGet(const clap_plugin_t*, uint32_t index, bool isInput,
                          clap_note_port_info_t* info) {
    if (!isInput || index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}
const clap_plugin_note_ports_t kSineNotes = {sineNoteCount, sineNoteGet};
bool CLAP_ABI sineSave(const clap_plugin_t*, const clap_ostream_t* out) {
    const std::uint8_t version = 1;
    return out->write(out, &version, 1) == 1;
}
bool CLAP_ABI sineLoad(const clap_plugin_t*, const clap_istream_t* in) {
    std::uint8_t version = 0;
    return in->read(in, &version, 1) == 1 && version == 1;
}
const clap_plugin_state_t kSineState = {sineSave, sineLoad};
const void* CLAP_ABI sineExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kSineState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kSinePorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kSineNotes;
    return nullptr;
}
bool CLAP_ABI sineActivate(const clap_plugin_t* pl, double sr, uint32_t, uint32_t) {
    S(pl)->sampleRate = sr;
    for (auto& v : S(pl)->voices) v = {};
    return true;
}
void noteOn(Sine* s, int channel, int key, int32_t id, double velocity) {
    for (auto& v : s->voices) {
        if (v.on) continue;
        v = {true,
             key,
             channel,
             id,
             0.0,
             440.0 * std::pow(2.0, (key - 69) / 12.0) / s->sampleRate,
             0.25 * velocity};
        return;
    }
}
void noteOff(Sine* s, const clap_output_events_t* out, uint32_t time, int channel, int key) {
    for (auto& v : s->voices) {
        if (!v.on || v.key != key || v.channel != channel) continue;
        v.on = false;
        clap_event_note_t end{};
        end.header = {sizeof(end), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0};
        end.note_id = v.noteId;
        end.key = static_cast<int16_t>(key);
        end.channel = static_cast<int16_t>(channel);
        out->try_push(out, &end.header);
    }
}
clap_process_status CLAP_ABI sineProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    auto* s = S(pl);
    const uint32_t n = p->frames_count;
    const uint32_t nev = p->in_events->size(p->in_events);
    uint32_t ev = 0;
    float* L = p->audio_outputs[0].data32[0];
    float* R = p->audio_outputs[0].data32[1];
    for (uint32_t i = 0; i < n; ++i) {
        while (ev < nev) {
            const auto* h = p->in_events->get(p->in_events, ev);
            if (h->time > i) break;
            if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
                const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
                if (h->type == CLAP_EVENT_NOTE_ON)
                    noteOn(s, ne->channel, ne->key, ne->note_id, ne->velocity);
                else
                    noteOff(s, p->out_events, i, ne->channel, ne->key);
            } else if (h->type == CLAP_EVENT_MIDI) {
                const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                const int status = m->data[0] & 0xf0, ch = m->data[0] & 0x0f;
                if (status == 0x90 && m->data[2] > 0)
                    noteOn(s, ch, m->data[1], -1, m->data[2] / 127.0);
                else if (status == 0x80 || (status == 0x90 && m->data[2] == 0))
                    noteOff(s, p->out_events, i, ch, m->data[1]);
            }
            ++ev;
        }
        double y = 0;
        for (auto& v : s->voices) {
            if (!v.on) continue;
            y += v.amp * std::sin(2.0 * M_PI * v.phase);
            v.phase += v.inc;
            v.phase -= std::floor(v.phase);
        }
        L[i] = R[i] = static_cast<float>(y);
    }
    return CLAP_PROCESS_CONTINUE;
}

// --- edge -------------------------------------------------------------------------

const clap_plugin_descriptor_t* edgeDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {
        CLAP_VERSION_INIT, "com.suppleaudio.test.edge", "Test Edge", "Supple Audio", "", "", "",
        "0.1.0",           "Host edge-case fixture",    features};
    return &d;
}

const clap_host_t* gEditorHost = nullptr;

struct Edge {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
#if defined(_WIN32)
    bool dpiAwareOnCreate = AreDpiAwarenessContextsEqual(
        GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    // Loaded on the main thread while the audio thread may flush or process
    // (both allowed by CLAP), so each byte is atomic.
    std::atomic<std::uint8_t> config[4] = {0, 0, 0, 0};
    std::atomic<int> flushes{0};
    int timerCalls = 0, activations = 0, mainCalls = 0;
    std::atomic<int> processCalls{0};
    std::atomic<bool> removed{false};
    std::atomic<bool> processing{false};
    std::atomic<int> editorPending{0}, hostDeliveries{0};
    std::atomic<double> editorValue{0};
    std::vector<std::array<float, 2>> delay;
    std::size_t delayPos = 0;
    clap_id timers[2] = {CLAP_INVALID_ID, CLAP_INVALID_ID};
    // A display-free editor (gap log #39): it records the parent window it is
    // given (read back as parameter 8) and asks for a resize when shown.
    bool guiCreated = false, guiShown = false;
    std::uint32_t guiCreates = 0;
    std::uint64_t guiParent = 0;
    std::uint32_t guiWidth = 300, guiHeight = 200;
    // The last display scale the host set (parameter 11) and how many times
    // (12). Mode 7 rescales its own size in response, as a plug-in may.
    double guiScale = 0;
    std::uint32_t guiScaleCalls = 0;
};
Edge* E(const clap_plugin_t* p) { return static_cast<Edge*>(p->plugin_data); }

uint32_t CLAP_ABI edgeParamCount(const clap_plugin_t* pl) { return E(pl)->removed ? 1 : 2; }
bool CLAP_ABI edgeParamInfo(const clap_plugin_t* pl, uint32_t index, clap_param_info_t* info) {
    if (index > 1) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = index == 0 && !E(pl)->removed ? 7 : 8;
    // Mode 53: a state-dependent macro parameter, absent in the default state.
    if (E(pl)->config[0] == 53 && index == 0) info->id = 105;
    // Mode 33: an editor that emits outputs for a read-only (display) parameter.
    info->flags = E(pl)->config[0] >= 11 && E(pl)->config[0] != 33 ? CLAP_PARAM_IS_AUTOMATABLE
                                                                   : CLAP_PARAM_IS_READONLY;
    if (E(pl)->config[0] == 53 && index == 0) info->flags |= CLAP_PARAM_IS_MODULATABLE;
    std::snprintf(info->name, sizeof(info->name), index == 0 ? "Flushes" : "Editor parent");
    if (E(pl)->config[0] == 51) {
        std::memset(info->name, 'N', sizeof(info->name));
        std::memset(info->module, 'M', sizeof(info->module));
    }
    info->max_value = index == 0 ? 1e9 : 1e15;
    return true;
}
bool CLAP_ABI edgeParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
#if defined(_WIN32)
    if (id == 13) {
        *out = E(pl)->dpiAwareOnCreate ? 1 : 0;
        return true;
    }
#endif
    if (id == 8) {
        *out = E(pl)->config[0] >= 30 ? E(pl)->hostDeliveries.load()
                                      : static_cast<double>(E(pl)->guiParent);
        return true;
    }
    if (id == 10) {
        *out = E(pl)->mainCalls;
        return true;
    }
    if (id == 11 || id == 12) {
        *out = id == 11 ? E(pl)->guiScale : E(pl)->guiScaleCalls;
        return true;
    }
    if (id == 9) {
        *out = E(pl)->timerCalls;
        return true;
    }
    if (id != 7 && !(id == 105 && E(pl)->config[0] == 53)) return false;
    *out = E(pl)->config[0] >= 30 ? E(pl)->editorValue.load() : E(pl)->flushes.load();
    return true;
}
bool CLAP_ABI edgeValueToText(const clap_plugin_t* pl, clap_id, double v, char* out,
                              uint32_t size) {
    if (E(pl)->config[0] == 51) {
        std::memset(out, 'V', size);
        return true;
    }
    std::snprintf(out, size, "%d", static_cast<int>(v));
    return true;
}
bool CLAP_ABI edgeTextToValue(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void edgeEditorOutput(const clap_plugin_t* pl, const clap_output_events_t* out, bool idle) {
    auto& edge = *E(pl);
    const int phase = edge.editorPending.load();
    if (!phase) return;
    clap_event_param_gesture_t gesture{};
    gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN,
                      CLAP_EVENT_IS_LIVE};
    gesture.param_id = 7;
    if (phase == 1) out->try_push(out, &gesture.header);
    clap_event_param_value_t value{};
    value.header = {
        sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE,
        CLAP_EVENT_IS_LIVE | (edge.config[0] == 31 ? std::uint32_t{CLAP_EVENT_DONT_RECORD} : 0U)};
    value.param_id = 7;
    value.value = idle || phase == 2 ? 10 : 5;
    value.note_id = value.port_index = value.channel = value.key = -1;
    edge.editorValue = value.value;
    const int repeats = edge.config[0] == 32 ? 2000 : 1;
    for (int i = 0; i < repeats; ++i) out->try_push(out, &value.header);
    if (idle || phase == 2) {
        gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &gesture.header);
        edge.editorPending = 0;
    } else
        edge.editorPending = 2;
}
void edgeHostInput(const clap_plugin_t* pl, const clap_input_events_t* in) {
    if (E(pl)->config[0] < 30) return;
    for (uint32_t i = 0; i < in->size(in); ++i) {
        const auto* h = in->get(in, i);
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            ++E(pl)->hostDeliveries;
            if (reinterpret_cast<const clap_event_param_value_t*>(h)->param_id == 7)
                E(pl)->editorValue = reinterpret_cast<const clap_event_param_value_t*>(h)->value;
        }
    }
}
void CLAP_ABI edgeFlush(const clap_plugin_t* pl, const clap_input_events_t* in,
                        const clap_output_events_t* out) {
    const auto value = ++E(pl)->flushes;
    edgeHostInput(pl, in);
    if (E(pl)->config[0] >= 30 && !E(pl)->processing) edgeEditorOutput(pl, out, true);
    if (E(pl)->config[0] != 9 && E(pl)->config[0] != 10) return;
    const int repeats = E(pl)->config[0] == 10 ? 600 : 1;
    for (int i = 0; i < repeats; ++i) {
        clap_event_param_gesture_t gesture{};
        gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID,
                          CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_IS_LIVE};
        gesture.param_id = 7;
        out->try_push(out, &gesture.header);
        clap_event_param_value_t param{};
        param.header = {sizeof(param), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE,
                        CLAP_EVENT_IS_LIVE | CLAP_EVENT_DONT_RECORD};
        param.param_id = 7;
        param.value = value;
        param.note_id = param.port_index = param.channel = param.key = -1;
        out->try_push(out, &param.header);
        gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &gesture.header);
    }
}
const clap_plugin_params_t kEdgeParams = {edgeParamCount,  edgeParamInfo,   edgeParamValue,
                                          edgeValueToText, edgeTextToValue, edgeFlush};
uint32_t CLAP_ABI edgeTail(const clap_plugin_t* pl) {
    const auto& c = E(pl)->config;
    const auto tail = static_cast<uint32_t>(c[1].load()) |
                      (static_cast<uint32_t>(c[2].load()) << 8) |
                      (static_cast<uint32_t>(c[3].load()) << 16);
    // Mode 20: a tail learned while processing (a delay hearing the tempo).
    return c[0] == 49 ? 0 : c[0] == 20 && E(pl)->processCalls > 0 ? 2 * tail : tail;
}
const clap_plugin_tail_t kEdgeTail = {edgeTail};
bool CLAP_ABI edgeStateSave(const clap_plugin_t* pl, const clap_ostream_t* os) {
    std::array<std::uint8_t, 4> state;
    for (std::size_t i = 0; i < 4; ++i) state[i] = E(pl)->config[i].load();
    if (state[0] >= 30 && state[0] != 49)
        state[1] = static_cast<std::uint8_t>(E(pl)->editorValue.load());
    return state[0] != 7 && os->write(os, state.data(), 4) == 4;
}
bool CLAP_ABI edgeStateLoad(const clap_plugin_t* pl, const clap_istream_t* is) {
    if (E(pl)->config[0] == 8) return false;
    std::uint8_t buf[4];
    if (is->read(is, buf, 4) != 4) return false;
    const auto oldMode = E(pl)->config[0].load();
    for (std::size_t i = 0; i < 4; ++i) E(pl)->config[i].store(buf[i]);
    if (buf[0] >= 30) E(pl)->editorValue = buf[1];
    if (buf[0] == 2 || buf[0] == 43 || buf[0] == 47) {
        const auto* timer = static_cast<const clap_host_timer_support_t*>(
            E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_TIMER_SUPPORT));
        timer->register_timer(E(pl)->host, 0, &E(pl)->timers[0]);
        timer->register_timer(E(pl)->host, 0, &E(pl)->timers[1]);
    }
    if (buf[0] == 5 || buf[0] == 49) E(pl)->host->request_restart(E(pl)->host);
    const auto* hp = static_cast<const clap_host_params_t*>(
        E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_PARAMS));
    if (hp && (oldMode == 53 || buf[0] == 53)) hp->rescan(E(pl)->host, CLAP_PARAM_RESCAN_ALL);
    if (buf[0] == 20) {
        E(pl)->host->request_process(E(pl)->host);
        hp->clear(E(pl)->host, 7, CLAP_PARAM_CLEAR_AUTOMATIONS);
        hp->clear(E(pl)->host, 8, CLAP_PARAM_CLEAR_MODULATIONS);
    }
    if (buf[0] == 21) {
        hp->clear(E(pl)->host, 7, CLAP_PARAM_CLEAR_ALL);
        hp->rescan(E(pl)->host, CLAP_PARAM_RESCAN_ALL);
        hp->clear(E(pl)->host, 8, CLAP_PARAM_CLEAR_ALL);
    }
    if (hp) hp->request_flush(E(pl)->host);
    return true;
}
const clap_plugin_state_t kEdgeState = {edgeStateSave, edgeStateLoad};
bool CLAP_ABI edgeGuiApi(const clap_plugin_t*, const char* api, bool floating) {
#if defined(__APPLE__)
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_COCOA);
#elif defined(_WIN32)
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
#else
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_X11);
#endif
}
bool CLAP_ABI edgeGuiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
#if defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
#else
    *api = CLAP_WINDOW_API_X11;
#endif
    *floating = false;
    return true;
}
bool CLAP_ABI edgeGuiCreate(const clap_plugin_t* pl, const char* api, bool floating) {
    if (!edgeGuiApi(pl, api, floating) || E(pl)->guiCreated) return false;
    E(pl)->guiCreated = true;
    ++E(pl)->guiCreates;
    return true;
}
void CLAP_ABI edgeGuiDestroy(const clap_plugin_t* pl) {
    E(pl)->guiCreated = E(pl)->guiShown = false;
    E(pl)->guiParent = 0;
}
bool CLAP_ABI edgeGuiScale(const clap_plugin_t* pl, double scale) {
    E(pl)->guiScale = scale;
    ++E(pl)->guiScaleCalls;
    if (E(pl)->config[0] == 7) {
        E(pl)->guiWidth = static_cast<std::uint32_t>(std::lround(300 * scale));
        E(pl)->guiHeight = static_cast<std::uint32_t>(std::lround(200 * scale));
    }
    return true;
}
bool CLAP_ABI edgeGuiGetSize(const clap_plugin_t* pl, uint32_t* w, uint32_t* h) {
    if (E(pl)->config[0] == 3) return false;
    *w = E(pl)->guiWidth;
    *h = E(pl)->guiHeight;
    return true;
}
bool CLAP_ABI edgeGuiCanResize(const clap_plugin_t*) { return true; }
bool CLAP_ABI edgeGuiHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    *hints = {true, true, false, 0, 0};
    return true;
}
bool CLAP_ABI edgeGuiAdjust(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    *w = std::max<uint32_t>(*w, 100);
    *h = std::max<uint32_t>(*h, 100);
    return true;
}
bool CLAP_ABI edgeGuiSetSize(const clap_plugin_t* pl, uint32_t w, uint32_t h) {
    E(pl)->guiWidth = w;
    E(pl)->guiHeight = h;
    return true;
}
bool CLAP_ABI edgeGuiParent(const clap_plugin_t* pl, const clap_window_t* window) {
    if (!E(pl)->guiCreated || !edgeGuiApi(pl, window->api, false)) return false;
    E(pl)->guiParent = !std::strcmp(window->api, CLAP_WINDOW_API_X11)
                           ? window->x11
                           : reinterpret_cast<std::uintptr_t>(window->ptr);
    return true;
}
bool CLAP_ABI edgeGuiTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI edgeGuiTitle(const clap_plugin_t*, const char*) {}
bool CLAP_ABI edgeGuiShow(const clap_plugin_t* pl) {
    if (E(pl)->config[0] == 4) return false;
    E(pl)->guiShown = true;
    if (E(pl)->config[0] >= 30) E(pl)->editorPending = 1;
    if (E(pl)->config[0] == 9 || E(pl)->config[0] == 10 || E(pl)->config[0] >= 30) {
        const auto* params = static_cast<const clap_host_params_t*>(
            E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_PARAMS));
        params->request_flush(E(pl)->host);
    }
    gEditorHost = E(pl)->host;
    const auto* gui =
        static_cast<const clap_host_gui_t*>(E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_GUI));
    if (gui) {
        gui->request_resize(E(pl)->host, 320, 240);
        if (E(pl)->config[0] == 47 && E(pl)->guiCreates == 1) gui->closed(E(pl)->host, false);
        if (E(pl)->config[0] == 6) {
            gui->request_show(E(pl)->host);
            gui->request_hide(E(pl)->host);
        }
    }
    return true;
}
bool CLAP_ABI edgeGuiHide(const clap_plugin_t* pl) {
    E(pl)->guiShown = false;
    return true;
}
const clap_plugin_gui_t kEdgeGui = {
    edgeGuiApi,     edgeGuiPreferred, edgeGuiCreate, edgeGuiDestroy, edgeGuiScale,
    edgeGuiGetSize, edgeGuiCanResize, edgeGuiHints,  edgeGuiAdjust,  edgeGuiSetSize,
    edgeGuiParent,  edgeGuiTransient, edgeGuiTitle,  edgeGuiShow,    edgeGuiHide};
void CLAP_ABI edgeTimer(const clap_plugin_t* pl, clap_id id) {
    auto& edge = *E(pl);
    const auto* timer = static_cast<const clap_host_timer_support_t*>(
        edge.host->get_extension(edge.host, CLAP_EXT_TIMER_SUPPORT));
    ++edge.timerCalls;
    if (edge.config[0] == 47) {
        const auto* state = static_cast<const clap_host_state_t*>(
            edge.host->get_extension(edge.host, CLAP_EXT_STATE));
        state->mark_dirty(edge.host);
    }
    if (edge.config[0] == 43) {
        const char done = 't';
        (void)fixtureWrite(edge.config[1], &done, 1);  // main-thread timer receipt
    }
    timer->unregister_timer(edge.host, id);
    if (edge.timerCalls == 1) {
        timer->unregister_timer(edge.host, edge.timers[1]);
        timer->register_timer(edge.host, 0, &edge.timers[0]);
    }
}
const clap_plugin_timer_support_t kEdgeTimer = {edgeTimer};
struct EdgeControl {
    void (*requestProcess)(const clap_plugin_t*);
    void (*rescan)(const clap_plugin_t*, bool);
    void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
    void (*restart)(const clap_plugin_t*);
    // State beyond the parameters (a sampler's file): its last byte, then
    // clap.state mark_dirty, as an editor would.
    void (*markDirty)(const clap_plugin_t*, std::uint8_t);
};
void edgeRequestProcess(const clap_plugin_t* pl) { E(pl)->host->request_process(E(pl)->host); }
void edgeRescan(const clap_plugin_t* pl, bool removed) {
    E(pl)->removed = removed;
    const auto* hp = static_cast<const clap_host_params_t*>(
        E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_PARAMS));
    hp->rescan(E(pl)->host, CLAP_PARAM_RESCAN_ALL);
}
void edgeClear(const clap_plugin_t* pl, clap_id id, clap_param_clear_flags flags) {
    const auto* hp = static_cast<const clap_host_params_t*>(
        E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_PARAMS));
    hp->clear(E(pl)->host, id, flags);
}
void edgeRestart(const clap_plugin_t* pl) { E(pl)->host->request_restart(E(pl)->host); }
void edgeMarkDirty(const clap_plugin_t* pl, std::uint8_t value) {
    E(pl)->config[3] = value;
    const auto* hs = static_cast<const clap_host_state_t*>(
        E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_STATE));
    hs->mark_dirty(E(pl)->host);
}
const EdgeControl kEdgeControl = {edgeRequestProcess, edgeRescan, edgeClear, edgeRestart,
                                  edgeMarkDirty};
void CLAP_ABI edgeMain(const clap_plugin_t* pl) {
    ++E(pl)->mainCalls;
    if (E(pl)->config[0] == 44) {
        E(pl)->config[3] = 17;
        const auto* state = static_cast<const clap_host_state_t*>(
            E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_STATE));
        state->mark_dirty(E(pl)->host);
        const auto* gui = static_cast<const clap_host_gui_t*>(
            E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_GUI));
        gui->request_resize(E(pl)->host, 400 + E(pl)->mainCalls, 300);
        const auto* tail = static_cast<const clap_host_tail_t*>(
            E(pl)->host->get_extension(E(pl)->host, CLAP_EXT_TAIL));
        tail->changed(E(pl)->host);
        if (E(pl)->mainCalls == 1) E(pl)->host->request_callback(E(pl)->host);
    }
    if (E(pl)->config[0] == 42) {
        const char done = 'm';
        (void)fixtureWrite(E(pl)->config[1], &done, 1);  // main-thread callback receipt
    }
}
uint32_t CLAP_ABI edgeLatency(const clap_plugin_t* pl) {
    const auto& c = E(pl)->config;
    if (c[0] == 49)
        return c[1].load() | (uint32_t(c[2].load()) << 8) | (uint32_t(c[3].load()) << 16);
    return c[0] == 5 ? 64 : 0;
}
const clap_plugin_latency_t kEdgeLatency = {edgeLatency};

void CLAP_ABI edgeFd(const clap_plugin_t* pl, int, clap_posix_fd_flags_t) { ++E(pl)->mainCalls; }
const clap_plugin_posix_fd_support_t kEdgeFd = {edgeFd};

const void* CLAP_ABI edgeExt(const clap_plugin_t* pl, const char* id) {
    if (!std::strcmp(id, "supple.test.edge-host")) return E(pl)->host;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kSineNotes;
    if (!std::strcmp(id, "supple.test.edge-control")) return &kEdgeControl;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kEdgeLatency;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kEdgeTimer;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kEdgeFd;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kEdgeGui;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kEdgeParams;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kEdgeTail;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kEdgeState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    return nullptr;
}
bool CLAP_ABI edgeActivate(const clap_plugin_t* pl, double, uint32_t, uint32_t) {
    if (E(pl)->config[0] == 49) {
        E(pl)->delay.assign(edgeLatency(pl), {});
        E(pl)->delayPos = 0;
    }
    return ++E(pl)->activations == 1 || E(pl)->config[0] != 5;
}
bool CLAP_ABI edgeStart(const clap_plugin_t* pl) {
    E(pl)->processing = E(pl)->config[0] == 0 || E(pl)->config[0] >= 11;
    return E(pl)->processing;
}
void CLAP_ABI edgeStop(const clap_plugin_t* pl) { E(pl)->processing = false; }
clap_process_status CLAP_ABI edgeProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    const auto calls = ++E(pl)->processCalls;
    if (!E(pl)->delay.empty()) {
        auto& e = *E(pl);
        for (uint32_t i = 0; i < p->frames_count; ++i) {
            for (uint32_t c = 0; c < 2; ++c) {
                const float input = p->audio_inputs[0].data32[c][i];
                p->audio_outputs[0].data32[c][i] = e.delay[e.delayPos][c];
                e.delay[e.delayPos][c] = input;
            }
            e.delayPos = (e.delayPos + 1) % e.delay.size();
        }
        return CLAP_PROCESS_CONTINUE;
    }
    if (E(pl)->config[0] == 42 || E(pl)->config[0] == 44)
        E(pl)->host->request_callback(E(pl)->host);
    edgeHostInput(pl, p->in_events);
    if (E(pl)->config[0] >= 30) edgeEditorOutput(pl, p->out_events, false);
    clap_event_param_value_t param{};
    param.header = {sizeof(param), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE,
                    CLAP_EVENT_DONT_RECORD | CLAP_EVENT_IS_LIVE};
    param.param_id = 7;
    param.value = 42;
    param.note_id = param.port_index = param.channel = param.key = -1;
    if (E(pl)->config[0] == 40) {
        // Payload storage is present, but the declared event ends at its
        // header. A host must reject it before reading the payload.
        param.header.size = sizeof(clap_event_header_t);
        p->out_events->try_push(p->out_events, &param.header);
        clap_event_param_gesture_t gesture{};
        gesture.header = {sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                          CLAP_EVENT_PARAM_GESTURE_BEGIN, 0};
        p->out_events->try_push(p->out_events, &gesture.header);
        gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        p->out_events->try_push(p->out_events, &gesture.header);
        clap_event_note_t end{};
        end.header = {sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END,
                      0};
        p->out_events->try_push(p->out_events, &end.header);
        param.header.size = sizeof(param);
        p->out_events->try_push(p->out_events, &param.header);
    }
    if (E(pl)->config[0] == 11) param.value = calls;
    if (E(pl)->config[0] == 13) {
        param.note_id = 4;
        param.port_index = 1;
        param.channel = 2;
        param.key = 3;
        for (int repeat = 0; repeat < 300; ++repeat) {
            clap_event_param_gesture_t gesture{};
            gesture.header = {sizeof(gesture), 1, CLAP_CORE_EVENT_SPACE_ID,
                              CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_IS_LIVE};
            gesture.param_id = 7;
            p->out_events->try_push(p->out_events, &gesture.header);
            param.header.time = 1;
            p->out_events->try_push(p->out_events, &param.header);
            gesture.header.time = 1;
            gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
            p->out_events->try_push(p->out_events, &gesture.header);
        }
    } else if (E(pl)->config[0] < 30)
        p->out_events->try_push(p->out_events, &param.header);
    for (uint32_t i = 0; i < p->in_events->size(p->in_events); ++i) {
        const auto* event = p->in_events->get(p->in_events, i);
        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            (event->type != CLAP_EVENT_NOTE_OFF &&
             !(E(pl)->config[0] == 14 && event->type == CLAP_EVENT_NOTE_ON &&
               reinterpret_cast<const clap_event_note_t*>(event)->note_id == E(pl)->config[1])))
            continue;
        auto end = *reinterpret_cast<const clap_event_note_t*>(event);
        end.header.type = CLAP_EVENT_NOTE_END;
        if (E(pl)->config[0] == 14) {
            const std::uint8_t mask = E(pl)->config[2];
            if (mask & 1) end.note_id = -1;
            if (mask & 2) end.port_index = -1;
            if (mask & 4) end.channel = -1;
            if (mask & 8) end.key = -1;
        }
        p->out_events->try_push(p->out_events, &end.header);
    }
    // Mode 50: the scaled-down intermediate is subnormal for |x| < 2^-26, so
    // the output shows this thread's FP environment in normal-range audio.
    const bool probe = E(pl)->config[0] == 50;
    const bool tempoProbe = E(pl)->config[0] == 48;
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < p->frames_count; ++i) {
            const float x = p->audio_inputs[0].data32[c][i];
            p->audio_outputs[0].data32[c][i] =
                tempoProbe ? static_cast<float>(p->transport ? p->transport->tempo / 1000. : 0.)
                : probe    ? x * 0x1p-100f * 0x1p100f
                           : x;
        }
    return E(pl)->config[0] == 11 ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
}

// --- key mix (sidechain fixture, ADR-0018) --------------------------------------------------
// Inputs: main (stereo), key (stereo), an extra mono port; outputs: main and
// an auxiliary pair. out = main + key, exactly, so a key's alignment shows in
// the output. A host that does not give every declared port a buffer gets
// CLAP_PROCESS_ERROR. A key port the host deactivated (audio-ports-activation)
// is unrouted: it keys from its main input instead, out = 2 * main.

const clap_plugin_descriptor_t* keyMixDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.keymix",
                                               "Test Key Mix",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Main plus sidechain key (sidechain tests)",
                                               features};
    return &d;
}
const clap_plugin_descriptor_t* legacyKeyMixDescriptor() {
    static clap_plugin_descriptor_t d = *keyMixDescriptor();
    d.id = "com.suppleaudio.test.legacy-keymix";
    d.name = "Legacy Key Mix";
    return &d;
}

struct KeyMix {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    bool keyActive = true;
};
KeyMix* K(const clap_plugin_t* p) { return static_cast<KeyMix*>(p->plugin_data); }
uint32_t CLAP_ABI keyMixPortCount(const clap_plugin_t*, bool input) { return input ? 3 : 2; }
bool CLAP_ABI keyMixPortGet(const clap_plugin_t*, uint32_t index, bool isInput,
                            clap_audio_port_info_t* info) {
    if (index >= (isInput ? 3u : 2u)) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    const char* names[2][3] = {{"Out", "Aux Out", ""}, {"In", "Key", "Extra"}};
    std::snprintf(info->name, sizeof(info->name), "%s", names[isInput ? 1 : 0][index]);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = isInput && index == 2 ? 1 : 2;
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kKeyMixPorts = {keyMixPortCount, keyMixPortGet};
bool CLAP_ABI keyMixSetActive(const clap_plugin_t* p, bool isInput, uint32_t index, bool active,
                              uint32_t) {
    if (isInput && index == 1) K(p)->keyActive = active;
    return true;
}
const clap_plugin_audio_ports_activation_t kKeyMixActivation = {
    [](const clap_plugin_t*) { return false; }, keyMixSetActive};
const void* CLAP_ABI keyMixExt(const clap_plugin_t* p, const char* id) {
    if (p->desc != legacyKeyMixDescriptor() && !std::strcmp(id, CLAP_EXT_AUDIO_PORTS_ACTIVATION))
        return &kKeyMixActivation;
    return !std::strcmp(id, CLAP_EXT_AUDIO_PORTS) ? &kKeyMixPorts : nullptr;
}
bool CLAP_ABI keyMixActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
clap_process_status CLAP_ABI keyMixProcess(const clap_plugin_t* plugin, const clap_process_t* p) {
    if (p->audio_inputs_count != 3 || p->audio_outputs_count != 2 ||
        p->audio_inputs[0].channel_count != 2 || p->audio_inputs[1].channel_count != 2 ||
        p->audio_inputs[2].channel_count != 1 || p->audio_outputs[1].channel_count != 2)
        return CLAP_PROCESS_ERROR;
    const clap_audio_buffer_t& key = p->audio_inputs[K(plugin)->keyActive ? 1 : 0];
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < p->frames_count; ++i) {
            p->audio_outputs[0].data32[c][i] = p->audio_inputs[0].data32[c][i] + key.data32[c][i];
            p->audio_outputs[1].data32[c][i] = 0.0f;
        }
    return CLAP_PROCESS_CONTINUE;
}

// --- multi-out (a multi-output instrument, ADR-0021) ---------------------------------------
// No inputs; outputs: "Main" (stereo), "Kick" (stereo) and "Snare" (stereo),
// each a constant level (0.125, 0.25, 0.5 on both channels) so where each
// lands shows in a render. A host that does not give every declared port a
// buffer gets CLAP_PROCESS_ERROR.

const clap_plugin_descriptor_t* multiOutDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.multiout",
                                               "Test Multi-Out",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Three stereo outputs at fixed levels",
                                               features};
    return &d;
}
// "Test Multi-Out x48" has 48 stereo outputs past the main one ("Out 2" to
// "Out 49", 0.5 each), more ports than a 16-port host takes (LSP's x48
// DirectOut multi-sampler has 49).
const clap_plugin_descriptor_t* multiOut48Descriptor() {
    static clap_plugin_descriptor_t d = *multiOutDescriptor();
    d.id = "com.suppleaudio.test.multiout-x48";
    d.name = "Test Multi-Out x48";
    return &d;
}
struct MultiOut {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
};
constexpr float kMultiOutLevels[3] = {0.125f, 0.25f, 0.5f};
uint32_t multiOutPorts(const clap_plugin_t* p) {
    return !std::strcmp(p->desc->id, multiOut48Descriptor()->id) ? 49 : 3;
}
uint32_t CLAP_ABI multiOutPortCount(const clap_plugin_t* p, bool input) {
    return input ? 0 : multiOutPorts(p);
}
bool CLAP_ABI multiOutPortGet(const clap_plugin_t* p, uint32_t index, bool isInput,
                              clap_audio_port_info_t* info) {
    if (isInput || index >= multiOutPorts(p)) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    const char* names[3] = {"Main", "Kick", "Snare"};
    if (multiOutPorts(p) == 3)
        std::snprintf(info->name, sizeof(info->name), "%s", names[index]);
    else
        std::snprintf(info->name, sizeof(info->name), index ? "Out %u" : "Main", index + 1);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kMultiOutPorts = {multiOutPortCount, multiOutPortGet};
const void* CLAP_ABI multiOutExt(const clap_plugin_t*, const char* id) {
    return !std::strcmp(id, CLAP_EXT_AUDIO_PORTS) ? &kMultiOutPorts : nullptr;
}
bool CLAP_ABI multiOutActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
clap_process_status CLAP_ABI multiOutProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    if (p->audio_outputs_count != multiOutPorts(pl)) return CLAP_PROCESS_ERROR;
    for (uint32_t port = 0; port < p->audio_outputs_count; ++port) {
        if (p->audio_outputs[port].channel_count != 2) return CLAP_PROCESS_ERROR;
        for (uint32_t c = 0; c < 2; ++c)
            std::fill_n(p->audio_outputs[port].data32[c], p->frames_count,
                        kMultiOutLevels[std::min(port, 2u)]);
    }
    return CLAP_PROCESS_CONTINUE;
}

// --- crash (the isolation fixture) -------------------------------------------------------
// An effect that passes audio through until its Mode parameter says otherwise:
// 1 crashes the process in process() (SIGSEGV), 2 hangs it there.

const clap_plugin_descriptor_t* crashDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {
        CLAP_VERSION_INIT,
        "com.suppleaudio.test.crash",
        "Test Crash",
        "Supple Audio",
        "",
        "",
        "",
        "0.1.0",
        "Crashes or hangs on request (plugin isolation tests)",
        features};
    return &d;
}

struct Crash {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    std::atomic<int> mode{0};
};
Crash* C(const clap_plugin_t* p) { return static_cast<Crash*>(p->plugin_data); }

uint32_t CLAP_ABI crashParamCount(const clap_plugin_t*) { return 1; }
bool CLAP_ABI crashParamInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 1;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "Mode");
    info->max_value = 2;
    return true;
}
bool CLAP_ABI crashParamValue(const clap_plugin_t* pl, clap_id id, double* out) {
    if (id != 1) return false;
    *out = C(pl)->mode.load();
    return true;
}
bool CLAP_ABI crashValueToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
    static const char* kNames[] = {"pass", "crash", "hang"};
    std::snprintf(out, size, "%s", kNames[std::clamp(static_cast<int>(v), 0, 2)]);
    return true;
}
bool CLAP_ABI crashTextToValue(const clap_plugin_t*, clap_id, const char*, double*) {
    return false;
}
void crashEvents(const clap_plugin_t* pl, const clap_input_events_t* in) {
    for (uint32_t i = 0, n = in->size(in); i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (ev->param_id == 1) C(pl)->mode = static_cast<int>(ev->value + 0.5);
        }
    }
}
void CLAP_ABI crashFlush(const clap_plugin_t* pl, const clap_input_events_t* in,
                         const clap_output_events_t*) {
    crashEvents(pl, in);
}
const clap_plugin_params_t kCrashParams = {crashParamCount,  crashParamInfo,   crashParamValue,
                                           crashValueToText, crashTextToValue, crashFlush};
const void* CLAP_ABI crashExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kCrashParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kGainPorts;
    return nullptr;
}
bool CLAP_ABI crashActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
clap_process_status CLAP_ABI crashProcess(const clap_plugin_t* pl, const clap_process_t* p) {
    crashEvents(pl, p->in_events);
    if (C(pl)->mode.load() == 1) std::raise(SIGSEGV);
    while (C(pl)->mode.load() == 2) {  // spin (no system call: sanitizers see a hang, not a call)
    }
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < p->frames_count; ++i)
            p->audio_outputs[0].data32[c][i] = p->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}

// --- shared lifecycle ------------------------------------------------------------------

bool CLAP_ABI checkedGainInit(const clap_plugin_t* p) { return checkGainMain(p); }
void CLAP_ABI checkedGainDeactivate(const clap_plugin_t* p) { (void)checkGainMain(p); }
void CLAP_ABI checkedGainDestroy(const clap_plugin_t* p) {
    (void)checkGainMain(p);
    delete G(p);
}

bool CLAP_ABI initOk(const clap_plugin_t*) { return true; }
void CLAP_ABI deactivateNop(const clap_plugin_t*) {}
bool CLAP_ABI startOk(const clap_plugin_t*) { return true; }
void CLAP_ABI stopNop(const clap_plugin_t*) {}
void CLAP_ABI resetNop(const clap_plugin_t*) {}
void CLAP_ABI mainNop(const clap_plugin_t*) {}

template <class T>
void CLAP_ABI destroyT(const clap_plugin_t* p) {
    delete static_cast<T*>(p->plugin_data);
}

template <class T>
const clap_plugin_t* make(const clap_host_t* host, const clap_plugin_descriptor_t* desc,
                          bool(CLAP_ABI* activate)(const clap_plugin_t*, double, uint32_t,
                                                   uint32_t),
                          clap_process_status(CLAP_ABI* process)(const clap_plugin_t*,
                                                                 const clap_process_t*),
                          const void*(CLAP_ABI* ext)(const clap_plugin_t*, const char*)) {
    auto* self = new T();
    self->host = host;
    self->plugin = {desc,    self,    initOk,   destroyT<T>, activate, deactivateNop,
                    startOk, stopNop, resetNop, process,     ext,      mainNop};
    return &self->plugin;
}

// --- note echo (plug-in note and MIDI output, ADR-0068) ----------------------------
// A note effect with one input and one output note port and no audio ports
// (its block outputs are cleared). Each input note below key 100 comes out a
// fifth up (CLAP note on/off, same channel, note id and velocity) and a note
// on also emits CC 74 = velocity (CLAP_EVENT_MIDI) at the same time, after the
// note. A MIDI 1.0 note in comes out a fifth up as MIDI. Key 120 emits a burst
// of every kind, out of time order (see noteEchoStress); key 121 emits 300
// CC 1 events, one per frame from the note's time (wrapping).

const clap_plugin_descriptor_t* noteEchoDescriptor() {
    static const char* features[] = {CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr};
    static const clap_plugin_descriptor_t d = {CLAP_VERSION_INIT,
                                               "com.suppleaudio.test.note-echo",
                                               "Test Note Echo",
                                               "Supple Audio",
                                               "",
                                               "",
                                               "",
                                               "0.1.0",
                                               "Echoes notes a fifth up (note output tests)",
                                               features};
    return &d;
}

struct NoteEcho {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
};

uint32_t CLAP_ABI noteEchoNoteCount(const clap_plugin_t*, bool) { return 1; }
bool CLAP_ABI noteEchoNoteGet(const clap_plugin_t*, uint32_t index, bool isInput,
                              clap_note_port_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = isInput ? 0 : 1;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), isInput ? "Notes In" : "Notes Out");
    return true;
}
const clap_plugin_note_ports_t kNoteEchoNotes = {noteEchoNoteCount, noteEchoNoteGet};
const void* CLAP_ABI noteEchoExt(const clap_plugin_t*, const char* id) {
    return !std::strcmp(id, CLAP_EXT_NOTE_PORTS) ? &kNoteEchoNotes : nullptr;
}
bool CLAP_ABI noteEchoActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }

void pushNote(const clap_output_events_t* out, uint16_t type, uint32_t time, int16_t channel,
              int16_t key, int32_t id, double velocity) {
    clap_event_note_t n{};
    n.header = {sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    n.note_id = id;
    n.port_index = 0;
    n.channel = channel;
    n.key = key;
    n.velocity = velocity;
    out->try_push(out, &n.header);
}
void pushMidi(const clap_output_events_t* out, uint32_t time, uint8_t status, uint8_t d1,
              uint8_t d2) {
    clap_event_midi_t m{};
    m.header = {sizeof(m), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    m.port_index = 0;
    m.data[0] = status;
    m.data[1] = d1;
    m.data[2] = d2;
    out->try_push(out, &m.header);
}
void pushUmp(const clap_output_events_t* out, uint32_t time, uint32_t w0, uint32_t w1) {
    clap_event_midi2_t m{};
    m.header = {sizeof(m), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0};
    m.data[0] = w0;
    m.data[1] = w1;
    out->try_push(out, &m.header);
}
void pushExpression(const clap_output_events_t* out, uint32_t time, clap_note_expression id,
                    int16_t key, double value) {
    clap_event_note_expression_t x{};
    x.header = {sizeof(x), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
    x.expression_id = id;
    x.note_id = -1;
    x.port_index = 0;
    x.channel = 2;
    x.key = key;
    x.value = value;
    out->try_push(out, &x.header);
}

// The burst, in this push order (times relative to the block of n frames):
//   note on 64 at n-1 · CC 7=100 at 0 · MIDI2 CC 10 (top bits 0x40) at 3 ·
//   MIDI2 per-note pitch bend at 3 (unsupported) · SysEx at 3 (unsupported) ·
//   pressure expression key 64 = 0.5 at 2 (poly pressure 64) · tuning
//   expression at 2 (unsupported) · MIDI data byte 0x80 at 2 (unsupported) ·
//   choke channel -1 key -1 at 1 · MIDI2 pitch bend (top 14 bits 0x2000) at 1 ·
//   note on at frame n + 50 (placed at n-1) · MIDI clock at 1.
void noteEchoStress(const clap_output_events_t* out, uint32_t n) {
    pushNote(out, CLAP_EVENT_NOTE_ON, n - 1, 0, 64, 5, 0.75);
    pushMidi(out, 0, 0xb0, 7, 100);
    pushUmp(out, 3, 0x40b00a00u, 0x80000000u);  // group 0, channel 0, CC 10
    pushUmp(out, 3, 0x40604000u, 0x80000000u);  // per-note pitch bend, key 64
    static const uint8_t sysex[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
    clap_event_midi_sysex_t x{};
    x.header = {sizeof(x), 3, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
    x.buffer = sysex;
    x.size = sizeof(sysex);
    out->try_push(out, &x.header);
    pushExpression(out, 2, CLAP_NOTE_EXPRESSION_PRESSURE, 64, 0.5);
    pushExpression(out, 2, CLAP_NOTE_EXPRESSION_TUNING, 64, 0.5);
    pushMidi(out, 2, 0xb0, 0x80, 1);
    pushNote(out, CLAP_EVENT_NOTE_CHOKE, 1, -1, -1, -1, 0);
    pushUmp(out, 1, 0x40e30000u, 0x80000000u);  // channel 3 pitch bend, centre
    pushNote(out, CLAP_EVENT_NOTE_ON, n + 50, 1, 65, 6, 1.0);
    pushMidi(out, 1, 0xf8, 0, 0);
}

clap_process_status CLAP_ABI noteEchoProcess(const clap_plugin_t*, const clap_process_t* p) {
    for (uint32_t port = 0; port < p->audio_outputs_count; ++port)
        for (uint32_t c = 0; c < p->audio_outputs[port].channel_count; ++c)
            std::fill_n(p->audio_outputs[port].data32[c], p->frames_count, 0.0f);
    const auto* out = p->out_events;
    const uint32_t count = p->in_events->size(p->in_events);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* h = p->in_events->get(p->in_events, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
            const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
            const bool on = h->type == CLAP_EVENT_NOTE_ON;
            if (on && n->key == 120) {
                noteEchoStress(out, p->frames_count);
                continue;
            }
            if (on && n->key == 121) {
                for (uint32_t k = 0; k < 300; ++k)
                    pushMidi(out, (h->time + k) % p->frames_count, 0xb0, 1,
                             static_cast<uint8_t>(k & 0x7f));
                continue;
            }
            if (n->key < 0 || n->key >= 100) continue;
            pushNote(out, h->type, h->time, n->channel, static_cast<int16_t>(n->key + 7),
                     n->note_id, n->velocity);
            if (on)
                pushMidi(out, h->time, static_cast<uint8_t>(0xb0 | (n->channel & 15)), 74,
                         static_cast<uint8_t>(std::lround(n->velocity * 127)));
        } else if (h->type == CLAP_EVENT_MIDI) {
            const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
            const int status = m->data[0] & 0xf0;
            if ((status == 0x80 || status == 0x90) && m->data[1] < 100)
                pushMidi(out, h->time, m->data[0], static_cast<uint8_t>(m->data[1] + 7),
                         m->data[2]);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 19; }
const clap_plugin_descriptor_t* CLAP_ABI factoryDesc(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0    ? gainDescriptor()
           : i == 1  ? sineDescriptor()
           : i == 2  ? edgeDescriptor()
           : i == 3  ? crashDescriptor()
           : i == 4  ? checkedGainDescriptor(false)
           : i == 5  ? checkedGainDescriptor(true)
           : i == 6  ? slowGainDescriptor()
           : i == 7  ? badCurveDescriptor()
           : i == 8  ? keyMixDescriptor()
           : i == 9  ? malformedSchemaDescriptor()
           : i == 10 ? oversizedDescriptor(true)
           : i == 11 ? oversizedDescriptor(false)
           : i == 12 ? multiOutDescriptor()
           : i == 14 ? contractDescriptor(false)
           : i == 15 ? contractDescriptor(true)
           : i == 17 ? rawGainDescriptor()
           : i == 16 ? legacyKeyMixDescriptor()
           : i == 13 ? multiOut48Descriptor()
           : i == 18 ? noteEchoDescriptor()
                     : nullptr;
}
const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                            const char* id) {
    if (!std::strcmp(id, rawGainDescriptor()->id)) {
        const auto* plugin =
            make<Gain>(host, rawGainDescriptor(), gainActivate, gainProcess, gainExt);
        G(plugin)->noSemantic = true;
        return plugin;
    }
    if (!std::strcmp(id, gainDescriptor()->id)) {
        auto* plugin = const_cast<clap_plugin_t*>(
            make<Gain>(host, gainDescriptor(), gainActivate, gainProcess, gainExt));
        plugin->reset = [](const clap_plugin_t* pl) {
            // A seek must clear latency history just as activation does.
            std::memset(G(pl)->delay, 0, sizeof(G(pl)->delay));
            G(pl)->delayPos = 0;
        };
        return plugin;
    }
    if (!std::strcmp(id, checkedGainDescriptor(false)->id) ||
        !std::strcmp(id, checkedGainDescriptor(true)->id) ||
        !std::strcmp(id, slowGainDescriptor()->id) || !std::strcmp(id, badCurveDescriptor()->id)) {
        const bool reject = !std::strcmp(id, checkedGainDescriptor(true)->id);
        const bool slow = !std::strcmp(id, slowGainDescriptor()->id);
        const bool badCurve = !std::strcmp(id, badCurveDescriptor()->id);
        const clap_plugin_t* p = make<Gain>(host,
                                            badCurve ? badCurveDescriptor()
                                            : slow   ? slowGainDescriptor()
                                                     : checkedGainDescriptor(reject),
                                            gainActivate, gainProcess, gainExt);
        G(p)->enforceThread = true;
        G(p)->rejectPatch = reject;
        G(p)->slowPatch = slow;
        G(p)->badCurve = badCurve;
        auto* mutablePlugin = const_cast<clap_plugin_t*>(p);
        mutablePlugin->init = checkedGainInit;
        mutablePlugin->deactivate = checkedGainDeactivate;
        mutablePlugin->destroy = checkedGainDestroy;
        return p;
    }
    for (bool ports : {true, false})
        if (!std::strcmp(id, oversizedDescriptor(ports)->id))
            return make<Gain>(host, oversizedDescriptor(ports), gainActivate, gainProcess, gainExt);
    for (bool missing : {false, true})
        if (!std::strcmp(id, contractDescriptor(missing)->id)) {
            const auto* plugin =
                make<Gain>(host, contractDescriptor(missing), gainActivate, gainProcess, gainExt);
            G(plugin)->mismatchedId = !missing;
            G(plugin)->missingValue = missing;
            return plugin;
        }
    if (!std::strcmp(id, malformedSchemaDescriptor()->id)) {
        const auto* plugin =
            make<Gain>(host, malformedSchemaDescriptor(), gainActivate, gainProcess, gainExt);
        G(plugin)->malformedSchema = true;
        return plugin;
    }
    if (!std::strcmp(id, sineDescriptor()->id)) {
        const clap_plugin_t* p =
            make<Sine>(host, sineDescriptor(), sineActivate, sineProcess, sineExt);
        // clap_plugin.reset: silence every voice (a reused instance must not
        // carry a hanging note into the next render).
        const_cast<clap_plugin_t*>(p)->reset = [](const clap_plugin_t* pl) {
            for (auto& v : S(pl)->voices) v = {};
        };
        return p;
    }
    if (!std::strcmp(id, edgeDescriptor()->id)) {
        const clap_plugin_t* p =
            make<Edge>(host, edgeDescriptor(), edgeActivate, edgeProcess, edgeExt);
        const_cast<clap_plugin_t*>(p)->reset = [](const clap_plugin_t* pl) {
            std::fill(E(pl)->delay.begin(), E(pl)->delay.end(), std::array<float, 2>{});
            E(pl)->delayPos = 0;
        };
        const_cast<clap_plugin_t*>(p)->on_main_thread = edgeMain;
        const_cast<clap_plugin_t*>(p)->start_processing = edgeStart;
        const_cast<clap_plugin_t*>(p)->stop_processing = edgeStop;
        return p;
    }
    if (!std::strcmp(id, noteEchoDescriptor()->id))
        return make<NoteEcho>(host, noteEchoDescriptor(), noteEchoActivate, noteEchoProcess,
                              noteEchoExt);
    if (!std::strcmp(id, crashDescriptor()->id))
        return make<Crash>(host, crashDescriptor(), crashActivate, crashProcess, crashExt);
    for (const auto* desc : {keyMixDescriptor(), legacyKeyMixDescriptor()})
        if (!std::strcmp(id, desc->id))
            return make<KeyMix>(host, desc, keyMixActivate, keyMixProcess, keyMixExt);
    for (const auto* desc : {multiOutDescriptor(), multiOut48Descriptor()})
        if (!std::strcmp(id, desc->id))
            return make<MultiOut>(host, desc, multiOutActivate, multiOutProcess, multiOutExt);
    return nullptr;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDesc, factoryCreate};

bool CLAP_ABI entryInit(const char*) { return true; }
void CLAP_ABI entryDeinit() {}
const void* CLAP_ABI entryFactory(const char* id) {
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT int supple_fixture_thread_failures() {
    return gThreadFailures.load(std::memory_order_relaxed);
}
extern "C" CLAP_EXPORT int supple_fixture_patch_starts() {
    return gPatchStarts.load(std::memory_order_acquire);
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                               entryDeinit, entryFactory};

extern "C" CLAP_EXPORT void supple_fixture_wait_patch_start(int previous) {
    while (gPatchStarts.load(std::memory_order_acquire) == previous) gPatchStarts.wait(previous);
}

extern "C" void supple_fixture_resize_stress() {
    const auto* gui =
        static_cast<const clap_host_gui_t*>(gEditorHost->get_extension(gEditorHost, CLAP_EXT_GUI));
    for (std::uint32_t width = 1; width <= 100000; ++width)
        gui->request_resize(gEditorHost, width, width ^ UINT32_MAX);
}
