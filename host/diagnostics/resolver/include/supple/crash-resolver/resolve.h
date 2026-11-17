// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Reads a local crash report (`supple.crash-report/1.0`), finds the symbol files its modules
// came from by build id, and names the functions in its frames. Offline and read-only: it
// opens only the report and the files and directories it is told to search, writes nothing
// and uses no network, symbol server or environment symbol path.
#pragma once
#include <supple/base/result.h>
#include <supple/crash-resolver/image.h>
#include <supple/json/value.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supple::diagnostics::resolver {

struct ReportModule {
    Format format = Format::Elf;
    std::string id;  // empty: the image had no identity, so no file can be matched to it
};
struct ReportFrame {
    int module = -1;  // index into modules; -1 for generated code or an unknown image
    std::uint64_t offset = 0;
    bool isReturn = false;
};
struct ReportThread {
    // fault | model | audio_render | audio_live | audio_background | audio_offline |
    // recorder | midi_output | other, or main in reports written before thread roles
    std::string role;
    std::string registered;  // the fault thread's own registered role, else empty
    std::string entry;       // the plug-in or module entry the thread was inside, else empty
    unsigned worker = 0;     // the N of audio-live-N and audio-bg-N, else 0
    std::string state;       // walked | unavailable
    std::string why;         // `stop` when walked, `reason` when unavailable
    std::vector<ReportFrame> frames;
};
struct Report {
    std::string incident, role, platform, kind;
    std::vector<ReportModule> modules;
    std::vector<ReportThread> threads;
};

constexpr std::size_t kMaxReportBytes = 4u << 20;
// Parses and strictly validates the parts of a report the resolver uses; anything else in
// it is ignored. Errors carry `report.*` codes with a JSON pointer to the offending value.
Result<Report> parseReport(std::string_view text);
Result<Report> readReport(const std::filesystem::path& path);

// A file that holds an image, found by `scan`.
struct Candidate {
    std::filesystem::path path;
    Identity image;
};
struct ScanLimits {
    std::size_t maxFiles = 20000;  // files examined across all roots
    std::size_t maxDepth = 8;
    std::size_t maxDirectoryEntries = 100000;  // a folder with more is skipped, whole
};
struct ScanStats {
    std::size_t examined = 0;
    bool truncated = false;  // a limit stopped the scan
};
// Images in the given files and directories (directories are walked in sorted order, links
// are not followed). Only headers are read.
std::vector<Candidate> scan(const std::vector<std::filesystem::path>& roots, ScanStats& stats,
                            const ScanLimits& limits = {}, const Limits& reader = {});

enum class ModuleState { Resolved, NoSymbols, Unreadable, NoIdentity, NotFound };
std::string_view stateName(ModuleState state);
struct ModuleResolution {
    ModuleState state = ModuleState::NotFound;
    std::filesystem::path file;  // the matched file, when there is one
    std::string error;           // why NoSymbols or Unreadable
    // Shared by every module of the report that has this id: a table is loaded once.
    std::shared_ptr<const SymbolTable> table;
};
struct Resolution {
    std::vector<ModuleResolution> modules;
    // The function for a frame, or nothing: no module, no match, no symbols.
    std::optional<std::string> name(const ReportFrame& frame) const;
    // Same, with the distance from the function's start.
    std::optional<std::pair<std::string, std::uint64_t>> locate(const ReportFrame& frame) const;
};
// Matches each module to the candidates with its format and id (in the order given, at most
// eight files per id) and loads the symbols of the one that names the most of the report's
// frames in that module, the earlier on a tie. A module is Resolved only if it named a frame
// (or none is in it): a stripped binary's few exported names do not hide its debug file.
Resolution resolve(const Report& report, const std::vector<Candidate>& candidates,
                   const Limits& limits = {});

std::string formatText(const Report& report, const Resolution& resolution);
json::Value formatJson(const Report& report, const Resolution& resolution);

}  // namespace supple::diagnostics::resolver
