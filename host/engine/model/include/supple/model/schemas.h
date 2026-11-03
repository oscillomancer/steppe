// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// JSON forms of the session objects (core-schema) and the project file
// `supple.project/2.5` (session-model §5). Fields equal to their defaults are
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
schema::Schema<Timebase> timebaseSchema();            // time | beats | beats_position
schema::Schema<PinMap> pinMapSchema();                // {in, out, key}: 1-based track channels
schema::Schema<MidiControl> midiControlSchema();      // {kind, channel, number, device}
schema::Schema<ControlMode> controlModeSchema();      // absolute | relative1..3 | toggle
schema::Schema<ParamLearn> paramLearnSchema();        // {param, midi, mode, …} (ADR-0027)
schema::Schema<TrackTimebase> trackTimebaseSchema();  // project | time | beats | beats_position
schema::Schema<ItemTimebase> itemTimebaseSchema();    // track | time | beats | beats_position
schema::Schema<Id> idSchema();                        // "tr_3"
template <class T>
const schema::Schema<T>& schemaOf();  // specialized for every object type

template <class T>
json::Value toJson(const T& object) {
    return schemaOf<T>().encode(object);
}

inline constexpr std::string_view kProjectSchema = "supple.project/2.5";

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
