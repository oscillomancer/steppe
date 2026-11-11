// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// JSON forms of the session objects (core-schema) and the project file
// `supple.project/2.11` (session-model §5). Fields equal to their defaults are
// omitted, so files and diffs stay small; decoding fills them back in.
#pragma once

#include <string>
#include <string_view>

#include "supple/model/session.h"
#include "supple/schema/schema.h"

namespace supple::model {

schema::Schema<TrackFreeze> trackFreezeSchema();
schema::Schema<StripType> stripTypeSchema();
schema::Schema<RoutePan> routePanSchema();
schema::Schema<PanLaw> panLawSchema();    // {db: 0|-2.5|-3|-4.5|-6, boost}
schema::Schema<PanMode> panModeSchema();  // pan | dual | width
// The project's timecode (ADR-0071): {rate: "30", start: "00:00:00:00"},
// either member optional (30 fps, 00:00:00:00); the start is a label valid
// at the rate (";" before the frames in drop-frame).
schema::Schema<sync::TimecodeMapping> timecodeSchema();
schema::Schema<sync::FrameRate>
frameRateSchema();                          // "23.976" | "24" | "25" | "29.97df" | "29.97" | "30"
schema::Schema<Timebase> timebaseSchema();  // time | beats | beats_position
schema::Schema<PinMap> pinMapSchema();      // {in, out, key}: 1-based track channels
schema::Schema<MidiControl> midiControlSchema();  // {kind, channel, number, device}
schema::Schema<ControlMode> controlModeSchema();  // absolute | relative1..3 | toggle
// A ControlBinding's members on the schema of a type that extends it: midi,
// then mode and soft_takeover, omitted at their defaults.
template <class T>
schema::ObjectBuilder<T>& controlBindingFields(schema::ObjectBuilder<T>& b) {
    MidiControl T::* control = &T::control;
    ControlMode T::* mode = &T::mode;
    bool T::* softTakeover = &T::softTakeover;
    b.field("midi", control, midiControlSchema());
    b.field("mode", mode, controlModeSchema()).defaultValue(ControlMode::Absolute).omitIfDefault();
    b.field("soft_takeover", softTakeover, schema::boolean()).defaultValue(false).omitIfDefault();
    return b;
}
schema::Schema<ParamLearn> paramLearnSchema();        // {param, midi, mode, …} (ADR-0027)
schema::Schema<ControlLearn> controlLearnSchema();    // {param, midi, mode, …} (ADR-0058)
schema::Schema<TrackTimebase> trackTimebaseSchema();  // project | time | beats | beats_position
schema::Schema<ItemTimebase> itemTimebaseSchema();    // track | time | beats | beats_position
schema::Schema<Id> idSchema();                        // "tr_3"
// A cable end, "mo_3.out" (rack.md §2.3); merges remap the object's id in
// strings matching kCableEndPattern.
inline constexpr std::string_view kCableEndPattern =
    "^(mo_[0-9]+\\.[a-z][a-z0-9_]*|fx_[0-9]+\\.([a-z][a-z0-9_]*|[0-9]+))$";
schema::Schema<CableEnd> cableEndSchema();
template <class T>
const schema::Schema<T>& schemaOf();  // specialized for every object type

template <class T>
json::Value toJson(const T& object) {
    return schemaOf<T>().encode(object);
}

inline constexpr std::string_view kProjectSchema = "supple.project/2.11";

// The whole session as the canonical project document (members in the
// spec's order, objects keyed by ID in ID order).
json::Value projectToJson(const Session& s);
std::string serializeProject(const Session& s);  // canonical text + "\n"
// Decodes and validates (schema errors with paths, then invariants).
Result<Session> projectFromJson(const json::Value& doc,
                                std::vector<schema::Warning>* warnings = nullptr);
Result<Session> parseProject(std::string_view text,
                             std::vector<schema::Warning>* warnings = nullptr);

}  // namespace supple::model
