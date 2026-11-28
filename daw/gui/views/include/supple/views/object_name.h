// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the GUI calls a model object where it refers to one (docs/specs/gui.md
// §4.1, "Names, not ids"): window and inspector titles, lists, menus,
// messages and the status line. No id reaches the user; commands and what
// agents see keep their ids.
#pragma once

#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/model/ids.h"
#include "supple/model/session.h"

namespace supple::views {

// An object's name, else words that say where it is, never its id:
// - a track: model::trackLabel ("Bass", else "Track 3", "Bus 2");
// - a scene: its name, else "Scene N" by its place in the launcher pane;
// - a clip: its name, else "<track>'s clip in <scene>";
// - an item: its label (Lanes::itemLabel), else "<track>'s item at bar N"
//   (", beat B" after it when another item of the track starts in that bar);
// - a take: its name ("Stabs, take 2" when another take of its item or
//   clip has that name), else as its item or clip;
// - an envelope: "<track or take>'s <param> automation", else "the <param>
//   automation";
// - an automation item: its name, else "<envelope> item at bar N";
// - a section or region: its name, else "the section (range) at bar N";
// - a marker: its name, else "the marker at bar N";
// - a tempo or meter change, a key or chord point: "the tempo change at bar
//   N", "the meter change at bar N", "the key (chord) at bar N";
// - a plug-in: its label, else the plug-in's name; a take's, then "on" its
//   item or clip ("Filter on Bass's item at bar 9", "Filter on Take 2 of
//   Riff" where it has several takes);
// - a send: "the send from <track> to <track>";
// - a group: its name, else "Group N" by its place among the groups;
// - a rack: model::rackLabel; a module: its label, else its plug-in's name
//   or its type's name; a cable: "the cable from <module> to <module>";
// - a modulation route: "the modulation of <target>";
// - a media file: its file name; a MIDI source: as the item that plays it.
// One that is gone is "a removed …".
std::string objectName(const model::Session& session, model::Id id);
// A take as its item's take list says it (the inspector, the take FX
// title, a take FX lane's note): its name ("Stabs, take 2" when another
// take of its item or clip has that name, as a copy does), else "Take N"
// by its place when its item or clip has more than one take, else
// objectName's words.
std::string takeLabel(const model::Session& session, model::Id take);
// `text` with each whole id in it (tr_3, it_4, sc_2, …) said by objectName.
// The other words are kept.
std::string objectNames(const model::Session& session, std::string_view text);
// The message, the hint and each listed error so; the code and path are kept.
Error objectNames(const model::Session& session, Error error);

}  // namespace supple::views
