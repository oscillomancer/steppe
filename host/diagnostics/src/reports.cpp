// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagnostics/reports.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <system_error>
#include <vector>
#include "inventory.h"
#include "native.h"
#include "private_files.h"
#include "storage.h"
#include "supple/base/files.h"
#include "supple/base/paths.h"
#include "supple/diagnostics/frontend.h"
#include "supple/json/json.h"
#include "supple/product.h"
#include "trace.h"
namespace supple::diagnostics {
namespace fs = std::filesystem;
using namespace detail;
namespace {
constexpr std::uintmax_t kReportLimit = 4 * 1024 * 1024;
Error noReport(const std::string& incident) {
    return Error("diagnostics.no_report", "there is no finished crash report with that incident id",
                 {},
                 "list the reports first; a report appears once its run has ended: " + incident);
}
Error storageFailure() {
    return Error("diagnostics.storage", "local diagnostic storage is unavailable", {},
                 "check permissions in the app local data folder");
}
fs::path pathFor(const fs::path& root, const std::string& incident, const char* extension) {
    return root / (incident + extension);
}
bool incidentName(const std::string& text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool isAcknowledged(const fs::path& root, const std::string& incident) {
    auto text = privateRead(pathFor(root, incident, ".ack"));
    if (!text) return false;
    auto value = json::parse(*text);
    if (!value || !value->isObject()) return false;
    const auto* schema = value->find("schema");
    const auto* id = value->find("incident");
    return schema && schema->isString() && schema->asString() == "supple.crash-ack/1.0" && id &&
           id->isString() && id->asString() == incident;
}
std::string textOf(const json::Value& object, const char* key) {
    const auto* v = object.find(key);
    return v && v->isString() ? v->asString() : std::string();
}
// A report that passed the tolerant shape check, summarised. The nested arrays were validated,
// so the lookups below find what they expect, but they still check rather than assume.
ReportSummary summarise(const json::Value& report, const fs::path& path,
                        fs::file_time_type written) {
    ReportSummary s;
    s.incident = textOf(report, "incident");
    s.role = textOf(report, "role");
    const std::string kind = textOf(report, "kind");
    s.kind = kind == "crash"             ? ReportKind::Crash
             : kind == "unexpected_exit" ? ReportKind::UnexpectedExit
                                         : ReportKind::Other;
    s.platform = textOf(report, "platform");
    if (const auto* build = report.find("build"); build && build->isObject()) {
        s.version = textOf(*build, "version");
        s.commit = textOf(*build, "commit");
    }
    s.written = written;
    s.path = path;
    const auto* plugins = report.find("plugins");
    if (plugins && plugins->isArray()) s.pluginCount = plugins->size();
    if (const auto* attribution = report.find("attribution");
        attribution && attribution->isObject()) {
        const std::string level = textOf(*attribution, "confidence");
        static constexpr std::string_view known[]{
            "fault_in_image", "active_token", "concurrent_activity", "recent_command", "unknown"};
        s.attribution = std::find(std::begin(known), std::end(known), level) != std::end(known)
                            ? level
                            : "other";
        const auto* ordinal = attribution->find("plugin");
        if (ordinal && ordinal->isInt() && plugins && plugins->isArray())
            for (const auto& item : plugins->asArray()) {
                const auto* id = item.find("id");
                if (!id || !id->isInt() || id->asInt() != ordinal->asInt()) continue;
                ReportPlugin plugin;
                plugin.name = textOf(item, "name");
                plugin.vendor = textOf(item, "vendor");
                plugin.version = textOf(item, "version");
                if (const auto* build = item.find("build"); build && build->isObject())
                    plugin.buildId = textOf(*build, "id");
                s.culprit = std::move(plugin);
                break;
            }
    }
    return s;
}
struct Loaded {
    json::Value value;
    fs::file_time_type written;
};
// A valid report file: the right name, the bounded private read, the tolerant shape, and its
// own incident id equal to its name. Reports of a newer build (a kind or field this one does
// not know) pass; reports whose known parts are damaged do not.
std::optional<Loaded> loadReport(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) return std::nullopt;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes > kReportLimit) return std::nullopt;
    auto text = privateRead(path);
    if (!text) return std::nullopt;
    auto value = json::parse(*text);
    if (!value || !reportShape(*value, true)) return std::nullopt;
    const auto* id = value->find("incident");
    if (!id || !id->isString() || id->asString() != reportStem(path)) return std::nullopt;
    const auto written = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return Loaded{std::move(*value), written};
}
}  // namespace
fs::path defaultDirectory() {
    const auto data = paths::localData();
    return data.empty() ? fs::path() : data / std::string(product::kSlug) / "diagnostics";
}
Result<ReportListing> listReports(const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    ReportListing listing;
    if (root.empty()) return listing;
    SUPPLE_TRY(checkDirectoryForReading(root));
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(root, ec))) return listing;
    // One pass over the folder, then the decisions: which incidents have a report, which a marker.
    std::map<std::string, fs::path> reportFiles, markerFiles;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto path = it->path();
        const std::string stem = reportStem(path);
        if (stem.empty()) continue;
        if (path.extension() == ".json") reportFiles.emplace(stem, path);
        if (path.extension() == ".run") markerFiles.emplace(stem, path);
    }
    if (ec) return storageFailure();
    std::map<std::string, MarkerState> markers;
    for (const auto& [incident, path] : markerFiles) markers.emplace(incident, probeMarker(path));
    for (const auto& [incident, state] : markers)
        if (state == MarkerState::Held) ++listing.liveRuns;
    for (const auto& [incident, path] : reportFiles) {
        const auto marker = markers.find(incident);
        if (marker != markers.end() && marker->second == MarkerState::Held) {
            ++listing.pending;
            continue;
        }
        auto loaded = loadReport(path);
        if (!loaded) {
            ++listing.ignored;
            continue;
        }
        auto summary = summarise(loaded->value, path, loaded->written);
        summary.acknowledged = isAcknowledged(root, incident);
        listing.reports.push_back(std::move(summary));
    }
    for (const auto& [incident, state] : markers) {
        if (state != MarkerState::Free || reportFiles.count(incident)) continue;
        UnreportedRun run;
        run.incident = incident;
        run.marker = markerFiles[incident];
        if (auto text = privateRead(run.marker))
            if (auto value = json::parse(*text); value && value->isObject())
                run.role = textOf(*value, "role");
        run.acknowledged = isAcknowledged(root, incident);
        std::error_code e;
        run.started = fs::last_write_time(run.marker, e);
        listing.unreported.push_back(std::move(run));
    }
    std::sort(listing.reports.begin(), listing.reports.end(), [](const auto& a, const auto& b) {
        return a.written == b.written ? a.incident > b.incident : a.written > b.written;
    });
    // Markers carry no time of their own that a test or a user could rely on: file times order
    // them.
    const auto markerTime = [](const UnreportedRun& r) {
        std::error_code e;
        return fs::last_write_time(r.marker, e);
    };
    std::sort(listing.unreported.begin(), listing.unreported.end(),
              [&](const auto& a, const auto& b) {
                  const auto ta = markerTime(a), tb = markerTime(b);
                  return ta == tb ? a.incident > b.incident : ta > tb;
              });
    return listing;
}
Result<std::string> readReport(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    if (probeMarker(pathFor(root, incident, ".run")) == MarkerState::Held)
        return noReport(incident);
    const auto path = pathFor(root, incident, ".json");
    auto loaded = loadReport(path);
    if (!loaded) return noReport(incident);
    return json::writeCanonical(loaded->value) + "\n";
}
Status acknowledge(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    const auto state = probeMarker(pathFor(root, incident, ".run"));
    if (state == MarkerState::Held) return noReport(incident);
    // Only an incident that exists can be acknowledged: a report that is valid, or a run that
    // ended and left its marker.
    if (!loadReport(pathFor(root, incident, ".json")) && state != MarkerState::Free)
        return noReport(incident);
    if (isAcknowledged(root, incident)) return okStatus();
    // A file that is there but is not an acknowledgement is replaced: publishing does not
    // overwrite on Windows.
    if (!privateRemove(pathFor(root, incident, ".ack"))) return storageFailure();
    auto value = json::Value::object();
    value["schema"] = "supple.crash-ack/1.0";
    value["incident"] = incident;
    return privatePublish(pathFor(root, incident, ".ack"), json::writeCanonical(value) + "\n");
}
namespace {
// The files of an incident whose run is over. The marker goes last: while it is there the
// incident is still found, so a deletion that stops half way can be repeated.
Status removeIncident(const fs::path& root, const std::string& incident) {
    if (!privateRemove(pathFor(root, incident, ".json")) ||
        !privateRemove(pathFor(root, incident, ".ack")))
        return storageFailure();
    if (probeMarker(pathFor(root, incident, ".run")) == MarkerState::Free &&
        !privateRemove(pathFor(root, incident, ".run")))
        return storageFailure();
    return okStatus();
}
}  // namespace
Status writeSampleReport(const std::string& incident, const fs::path& directory) {
    if (!incidentName(incident)) return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(diagnostics::Role::Fixture);
    std::copy(incident.begin(), incident.end(), boot.incident.begin());
    SUPPLE_TRY(prepareDirectory(directory));
    return publishReport(directory, boot, nullptr, 31, false);
}
Status deleteReport(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    const auto state = probeMarker(pathFor(root, incident, ".run"));
    if (state == MarkerState::Held) return noReport(incident);
    if (!loadReport(pathFor(root, incident, ".json")) && state != MarkerState::Free)
        return noReport(incident);
    return removeIncident(root, incident);
}
Result<std::size_t> deleteAllReports(const fs::path& directory) {
    auto listing = listReports(directory);
    if (!listing) return listing.error();
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    std::size_t removed = 0;
    for (const auto& report : listing->reports) {
        SUPPLE_TRY(removeIncident(root, report.incident));
        ++removed;
    }
    for (const auto& run : listing->unreported) {
        SUPPLE_TRY(removeIncident(root, run.incident));
        ++removed;
    }
    return removed;
}
Status writeSampleUnreportedRun(const std::string& incident, const fs::path& directory) {
    if (!incidentName(incident)) return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(diagnostics::Role::Fixture);
    std::copy(incident.begin(), incident.end(), boot.incident.begin());
    SUPPLE_TRY(prepareDirectory(directory));
    auto marker = createMarker(directory, boot);
    if (!marker) return marker.error();
    releaseMarker(*marker);  // the holder is gone, the marker stays: a run that ended
    return okStatus();
}
Status writeSampleCrash(const std::string& incident, const std::string& plugin,
                        const std::string& vendor, const fs::path& directory) {
    if (!incidentName(incident) || plugin.size() > 64 || vendor.size() > 64)
        return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(diagnostics::Role::Fixture);
    std::copy(incident.begin(), incident.end(), boot.incident.begin());
    Packet announce = boot;
    announce.kind = Message::Instance;
    auto& r = announce.instance;
    r.cell = 4;
    r.event = static_cast<std::uint8_t>(diagctx::EventKind::Add);
    r.kind = static_cast<std::uint8_t>(diagctx::InstanceKind::Clap);
    r.origin = static_cast<std::uint8_t>(diagctx::InstanceOrigin::Foreign);
    r.mode = static_cast<std::uint8_t>(diagctx::InstanceMode::InProcess);
    r.state = static_cast<std::uint8_t>(diagctx::InstanceState::Active);
    r.image = 0x5100;
    std::memcpy(r.text[0], plugin.data(), plugin.size());
    r.length[0] = static_cast<std::uint8_t>(plugin.size());
    std::memcpy(r.text[1], vendor.data(), vendor.size());
    r.length[1] = static_cast<std::uint8_t>(vendor.size());
    Inventory inventory;
    if (!inventory.apply(announce)) return noReport(incident);
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.modules.resize(1);
    trace.modules[0].id = "0123456789abcdef";
    trace.regions = {{0x5000, 0x6000, 0}};
    trace.threads.resize(1);
    trace.threads[0].role = detail::Role::Fault;
    trace.threads[0].missing = Missing::None;
    Frame frame;
    frame.module = 0;
    trace.threads[0].walk.frames.push_back(frame);
    Packet terminal = boot;
    terminal.code = 11;
    terminal.pc = 0x5100;
    SUPPLE_TRY(prepareDirectory(directory));
    return publishReport(directory, boot, &terminal, 11, true, &trace, &inventory);
}
namespace {
std::string who(const ReportPlugin& p) {
    return p.vendor.empty() ? p.name : p.name + " by " + p.vendor;
}
}  // namespace
std::optional<RestartNotice> restartNotice(const ReportListing& listing) {
    // The newest unacknowledged incident, report or run without one, and the rest behind it.
    struct Candidate {
        std::filesystem::file_time_type time;
        const ReportSummary* report;
        const UnreportedRun* run;
    };
    std::vector<Candidate> found;
    for (const auto& r : listing.reports)
        if (!r.acknowledged) found.push_back({r.written, &r, nullptr});
    for (const auto& r : listing.unreported)
        if (!r.acknowledged) found.push_back({r.started, nullptr, &r});
    if (found.empty()) return std::nullopt;
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.time > b.time; });
    RestartNotice notice;
    for (const auto& c : found)
        notice.incidents.push_back(c.report ? c.report->incident : c.run->incident);
    notice.earlier = found.size() - 1;
    const std::string app(product::kName);
    const Candidate& newest = found.front();
    if (newest.report) {
        notice.kind = RestartNotice::Kind::Report;
        const auto& r = *newest.report;
        const bool points =
            (r.attribution == "fault_in_image" || r.attribution == "active_token") && r.culprit &&
            !r.culprit->name.empty();
        notice.headline = points ? who(*r.culprit) + " was running when " + app +
                                       " closed unexpectedly last time."
                                 : app + " closed unexpectedly last time.";
        notice.detail = "A crash report was saved on this computer; it was not sent anywhere.";
        if (points) {
            notice.plugin = r.culprit->name;
            notice.detail += " It points at that plug-in but cannot prove it was the cause.";
        }
    } else {
        notice.kind = RestartNotice::Kind::NoReport;
        notice.headline = app + " closed unexpectedly last time, and no crash report was written.";
        notice.detail =
            "That happens when the program that watches it stopped too, or could not save one.";
    }
    if (notice.earlier == 1)
        notice.detail += " One earlier unexpected close was not looked at.";
    else if (notice.earlier > 1)
        notice.detail +=
            " " + std::to_string(notice.earlier) + " earlier unexpected closes were not looked at.";
    return notice;
}
std::string noticeText(const RestartNotice& notice) {
    return notice.headline + " " + notice.detail;
}
Status acknowledgeNotice(const RestartNotice& notice, const fs::path& directory) {
    Status first = okStatus();
    for (const auto& incident : notice.incidents)
        if (auto st = acknowledge(incident, directory); !st && first) first = st;
    return first;
}
}  // namespace supple::diagnostics
