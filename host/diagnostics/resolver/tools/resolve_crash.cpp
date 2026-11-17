// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Command line for the offline crash resolver: names the functions in a local crash report.
#include <supple/crash-resolver/resolve.h>
#include <supple/json/write.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace supple::diagnostics::resolver;

int usage(std::FILE* to, int code) {
    std::fputs(
        "usage: resolve-crash [--json] --symbols PATH [--symbols PATH ...] REPORT.json\n"
        "\n"
        "Names the functions in the thread traces of a local crash report. PATH is an\n"
        "executable, a separate symbol file or a folder searched for them (not through\n"
        "links, at most 8 levels and 20000 files). A module is matched to a file by its build\n"
        "id, so pass the files of the build that crashed. Nothing is written or downloaded.\n"
        "Windows images give exported function names only; their PDB is not read.\n"
        "  --json   print supple.crash-symbols/1.0 instead of text\n",
        to);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    bool asJson = false;
    std::vector<fs::path> roots;
    std::string report;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") return usage(stdout, 0);
        if (arg == "--json") {
            asJson = true;
        } else if (arg == "--symbols") {
            if (++i >= argc) return usage(stderr, 2);
            roots.emplace_back(argv[i]);
        } else if (!arg.empty() && arg[0] == '-') {
            return usage(stderr, 2);
        } else if (report.empty()) {
            report = std::string(arg);
        } else {
            return usage(stderr, 2);
        }
    }
    if (report.empty()) return usage(stderr, 2);
    if (roots.empty()) {
        std::fputs("resolve-crash: pass at least one --symbols file or folder\n", stderr);
        return 2;
    }
    const auto parsed = readReport(fs::path(report));
    if (!parsed) {
        std::fprintf(stderr, "resolve-crash: %s\n", parsed.error().toString().c_str());
        return 1;
    }
    ScanStats stats;
    const auto candidates = scan(roots, stats);
    const auto resolution = resolve(*parsed, candidates);
    if (stats.truncated)
        std::fputs(
            "resolve-crash: the search stopped at its limits; some files were not looked at\n",
            stderr);
    const std::string text =
        asJson ? supple::json::write(supple::diagnostics::resolver::formatJson(*parsed, resolution))
                         .value() +
                     "\n"
               : formatText(*parsed, resolution);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
