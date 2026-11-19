// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "suspects.h"

#include <algorithm>
#include <numeric>

#include "supple/imageid/image.h"

namespace supple::engine::detail {

namespace {
namespace imageid = supple::diagnostics::resolver;

std::optional<std::vector<std::string>> readImageIds(const std::filesystem::path& file) {
    imageid::FileSource source(file);
    if (!source.ok()) return std::nullopt;
    const auto images = imageid::identify(source);
    if (images.empty()) return std::nullopt;
    std::vector<std::string> ids;
    for (const auto& image : images)
        if (!image.id.empty()) ids.push_back(image.id);
    return ids;
}

int strength(std::string_view attribution) {
    return attribution == "fault_in_image" ? 2 : attribution == "active_token" ? 1 : 0;
}
}  // namespace

std::string_view buildName(SuspicionVerdict::Build build) {
    switch (build) {
        case SuspicionVerdict::Build::Same:
            return "same";
        case SuspicionVerdict::Build::Version:
            return "version";
        case SuspicionVerdict::Build::Unreadable:
            return "unreadable";
        case SuspicionVerdict::Build::None:
            break;
    }
    return "none";
}

SuspectCheck::SuspectCheck(std::vector<diagnostics::Suspect> suspects, SuspectOptions options)
    : suspects_(std::move(suspects)), options_(std::move(options)) {
    if (!options_.now) options_.now = [] { return std::chrono::steady_clock::now(); };
    if (!options_.readIds) options_.readIds = readImageIds;
}

std::optional<std::vector<std::string>> SuspectCheck::idsOf(const std::filesystem::path& file) {
    if (const auto it = read_.find(file); it != read_.end()) return it->second;
    if (!deadline_) deadline_ = options_.now() + options_.budget;
    // Spent: not read at all, so it is asked about rather than guessed (E11). Not remembered,
    // so a later check with budget left may still read it (a fresh SuspectCheck per run).
    if (reads_ >= options_.maxReads || options_.now() >= *deadline_) return std::nullopt;
    ++reads_;
    auto ids = options_.readIds(file);
    read_.emplace(file, ids);
    return ids;
}

SuspicionVerdict SuspectCheck::check(const host::PluginInfo& installed) {
    SuspicionVerdict out;
    std::vector<const diagnostics::Suspect*> byBuild, byVersion;
    for (const auto& suspect : suspects_) {
        if (suspect.format != installed.format || suspect.pluginId != installed.id) continue;
        (suspect.buildId.empty() ? byVersion : byBuild).push_back(&suspect);
    }
    auto take = [&](const diagnostics::Suspect& suspect) {
        for (std::size_t i = 0; i < suspect.incidents.size(); ++i)
            if (std::ranges::find(out.incidents, suspect.incidents[i]) == out.incidents.end()) {
                out.incidents.push_back(suspect.incidents[i]);
                out.incidentTimes.push_back(
                    i < suspect.incidentTimes.size() ? suspect.incidentTimes[i] : suspect.newest);
            }
        if (strength(suspect.attribution) > strength(out.attribution))
            out.attribution = suspect.attribution;
    };
    for (const auto* suspect : byVersion)
        if (suspect->version == installed.version) {
            take(*suspect);
            out.build = SuspicionVerdict::Build::Version;
        }
    if (!byBuild.empty()) {
        const auto file = host::pluginImageFile(installed);
        const auto ids = file.empty() ? std::nullopt : idsOf(file);
        if (!ids) {
            // Which build is installed is unknown: any of them may be.
            for (const auto* suspect : byBuild) take(*suspect);
            if (out.build == SuspicionVerdict::Build::None)
                out.build = SuspicionVerdict::Build::Unreadable;
        } else {
            bool same = false;
            for (const auto* suspect : byBuild)
                if (std::ranges::find(*ids, suspect->buildId) != ids->end()) {
                    take(*suspect);
                    same = true;
                }
            if (same) out.build = SuspicionVerdict::Build::Same;
        }
    }
    // Newest first across the suspects merged. Each suspect's own list is newest first, but a
    // build suspect and a version suspect interleave, so the merge sorts by report time (stable:
    // equal times keep the order they were taken in).
    std::vector<std::size_t> order(out.incidents.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return out.incidentTimes[a] > out.incidentTimes[b];
    });
    SuspicionVerdict sorted = out;
    for (std::size_t i = 0; i < order.size(); ++i) {
        sorted.incidents[i] = out.incidents[order[i]];
        sorted.incidentTimes[i] = out.incidentTimes[order[i]];
    }
    return sorted;
}

}  // namespace supple::engine::detail
