// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The crash report viewer (gui.md, ADR-0083 topic 7c): Help > Crash reports… lists what earlier
// runs left in the diagnostics folder and shows one in plain words, with the technical detail
// folded away. It reads the folder with the diagnostics library's reader and never uploads a
// thing: the actions are mark as seen, delete one or all, open the folder and copy a text
// summary.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "app_actions.h"
#include "crash_report_text.h"
#include "supple/diagnostics/reports.h"
#include "supple/ui/root.h"

namespace supple::gui {

class CrashViewer {
  public:
    // `directory` is the diagnostics folder; empty reads nothing (no folder, no reports).
    CrashViewer(ui::Root& root, AppActions& actions, std::filesystem::path directory);
    // Opens the viewer on `incident` (or the newest report when it is empty or not listed).
    void open(const std::string& incident = {});
    // The clipboard and the file manager, replaceable by tests.
    std::function<void(const std::string&)> copyText;
    std::function<void(const std::filesystem::path&)> openFolder;

  private:
    struct Entry {
        std::string incident;
        bool unreported = false;
        bool seen = false;
        diagnostics::ReportSummary report;
        diagnostics::UnreportedRun run;
        std::string when;  // the file time as text
        std::filesystem::file_time_type time;
    };
    void load();                     // the folder's listing, newest first
    void rebuild();                  // the whole popup's body from `entries_`
    void select(std::size_t index);  // shows one entry's page
    void showPage(const Entry& entry);
    void status(const std::string& text);
    void armDelete(bool all);
    void doDelete();
    void deleteAll();
    ui::Root& root_;
    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    std::size_t selected_ = 0;
    ReportPage page_;
    bool deleteArmed_ = false, deleteAllArmed_ = false;
    std::string pendingIncident_;
    ui::Node* popup_ = nullptr;
    std::vector<ui::Node*> rows_;  // the list's rows, ReportRow in the .cpp
    float bodyHeight_ = 300;
};

// "17 Nov 2026, 14:32" in the local time zone.
std::string timeText(std::filesystem::file_time_type time);

}  // namespace supple::gui
