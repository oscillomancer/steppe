// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A crash report in the words the viewer shows and the copied summary holds (gui.md, topic 7c):
// what happened first, in plain sentences, and the technical detail as sections. It reads the
// report's JSON as a newer or older build may have written it: a key it does not know is
// ignored, one it expected and cannot find reads as "not recorded", never as an error, and
// nothing a report holds is left out of the summary.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "supple/json/value.h"

namespace supple::gui {

struct ReportSection {
    std::string id;         // "plugins", "bridges", "threads", "commands", "limits"
    std::string title;      // "Plug-ins"
    std::size_t count = 0;  // shown after the title: "Plug-ins (3)"
    std::string note;       // one plain sentence of what the lines are; may be empty
    std::vector<std::string> lines;
};

struct ReportPage {
    std::string headline;                // what to read first
    std::string plugin;                  // the plug-in's name inside the headline, to set in bold
    std::string subtitle;                // when, which build, which app
    std::vector<std::string> sentences;  // plain: what stopped it, what the report points at
    std::vector<ReportSection> sections;
    // The whole page as plain text (every line of every section), for the clipboard.
    std::string text() const;
};

// `saved` is the time the report was written, already as text (the caller knows the clock and
// the zone). `report` is a valid report's parsed JSON, any kind.
ReportPage describeReport(const json::Value& report, const std::string& saved);
// A run that ended and left no report: its marker alone.
ReportPage describeUnreportedRun(const std::string& role, const std::string& started);
// "the app", "the headless engine", "a test run"; an unknown role as written.
std::string roleName(const std::string& role);
// A signal or exception code in words: "a segmentation fault (signal 11)". `platform` is the
// report's own ("linux", "macos", "windows").
std::string causeSentence(const std::string& platform, const std::string& kind, long long cause,
                          long long code, bool nativeFatal);

}  // namespace supple::gui
