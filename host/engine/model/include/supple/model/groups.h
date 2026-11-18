// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <set>

#include "supple/model/session.h"

namespace supple::model {
// Model-thread helpers. Results are unique, sorted IDs; linked results include source.
std::vector<Id> linkedTracks(const Session&, Id track, std::string_view attribute);
std::vector<Id> linkedItems(const Session&, Id item);
std::vector<Id> trackGroups(const Session&, Id track);
// All unique controlling VCAs, excluding target itself for a group target.
std::vector<Id> vcaControllers(const Session&, Id target);
Status validateGroups(const Session&);
// Model curve value at project seconds, including automation items.
double groupParameterAt(const Session&, Id object, std::string_view param, double seconds,
                        double fallback);
struct EffectiveLevel {
    double requestedDb = 0, appliedDb = 0;
    bool mute = false, soloSilenced = false;
};
EffectiveLevel effectiveTrackLevel(const Session&, Id track, double seconds);
// The tracks a solo silences; none while nothing is soloed. A track stays
// audible if it is soloed (On, InPlace, or by a soloed VCA), solo-defeated, a
// master, or on a soloed track's signal path: what feeds it (a soloed bus or
// folder keeps its sources and children) and where it goes (a soloed source
// keeps its folders, buses and the master). A MIDI send floods downstream
// only (ADR-0067); a rack send is its home channel's send (ADR-0075). The
// engine's plans and the readouts both use this, so they cannot disagree.
std::set<Id> soloSilencedTracks(const Session&);
}  // namespace supple::model
