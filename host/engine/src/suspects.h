// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Whether the suspicion a crash report left applies to the plug-in installed now (topic 7d, the
// design note's part (c); spec: docs/specs/diagnostics.md "Suspects and settling" and
// docs/specs/engine.md §5).
//
// The reader (`diagnostics::suspects`) names plug-ins from the reports alone. A plug-in that was
// reinstalled since is not the plug-in that crashed, so before anyone is asked about one, the
// installed file's build id is compared with the report's `build.id`: a string compare of ids
// first, and the file is read (headers only, at most `maxReads` files within `budget`) only for a
// suspect of the same format and id whose report has a build id. A report with no build id (an
// `active_token` attribution, every VST3) is matched on the version instead.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "supple/diagnostics/reports.h"
#include "supple/host/clap_host.h"

namespace supple::engine::detail {

struct SuspicionVerdict {
    // Same: the installed file holds the build the report names. Version: the report names no
    // build and the installed version is the one it names. Unreadable: the installed file could
    // not be checked (unreadable, not an image, the read budget spent), so it is asked about:
    // it may be the build that crashed. None: no suspicion applies (nothing names this plug-in, a
    // different build or version is installed, or the suspicion is settled).
    enum class Build { None, Same, Version, Unreadable } build = Build::None;
    std::vector<std::string> incidents;  // newest first, without repeats
    std::string attribution;             // the strongest level: fault_in_image over active_token
    bool applies() const { return build != Build::None; }
};

std::string_view buildName(SuspicionVerdict::Build build);

// The ids of the images in a file, or nothing when it cannot be read or holds no image.
using IdReader =
    std::function<std::optional<std::vector<std::string>>(const std::filesystem::path&)>;
struct SuspectOptions {
    std::chrono::milliseconds budget{250};
    std::size_t maxReads = 3;
    std::function<std::chrono::steady_clock::time_point()> now;  // injected by tests
    IdReader readIds;                                            // injected by tests
};

class SuspectCheck {
  public:
    explicit SuspectCheck(std::vector<diagnostics::Suspect> suspects, SuspectOptions options = {});
    bool any() const { return !suspects_.empty(); }
    // One installed plug-in, as the catalogue (or a running instance) describes it.
    SuspicionVerdict check(const host::PluginInfo& installed);

  private:
    std::optional<std::vector<std::string>> idsOf(const std::filesystem::path& file);
    std::vector<diagnostics::Suspect> suspects_;
    SuspectOptions options_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::size_t reads_ = 0;
    std::map<std::filesystem::path, std::optional<std::vector<std::string>>> read_;
};

}  // namespace supple::engine::detail
