// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagnostics/reports.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <system_error>
#include <vector>
#include "inventory.h"
#include "private_files.h"
#include "storage.h"
#include "supple/base/files.h"
#include "supple/base/paths.h"
#include "supple/diagcap/native.h"
#include "supple/diagcap/trace.h"
#include "supple/diagnostics/frontend.h"
#include "supple/json/json.h"
#include "supple/product.h"
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
// The small files beside an incident (`<incident>.ack`, `<incident>.settled`): canonical JSON
// with a schema and the incident id. One that is there but is not that does not count.
constexpr const char* kAckSchema = "supple.crash-ack/1.0";
constexpr const char* kSettledSchema = "supple.crash-settled/1.0";
bool hasMark(const fs::path& root, const std::string& incident, const char* extension,
             const char* wanted) {
    auto text = privateRead(pathFor(root, incident, extension));
    if (!text) return false;
    auto value = json::parse(*text);
    if (!value || !value->isObject()) return false;
    const auto* schema = value->find("schema");
    const auto* id = value->find("incident");
    return schema && schema->isString() && schema->asString() == wanted && id && id->isString() &&
           id->asString() == incident;
}
bool isAcknowledged(const fs::path& root, const std::string& incident) {
    return hasMark(root, incident, ".ack", kAckSchema);
}
bool isSettled(const fs::path& root, const std::string& incident) {
    return hasMark(root, incident, ".settled", kSettledSchema);
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
             : kind == "bridge_exit"     ? ReportKind::BridgeExit
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
            "fault_in_image", "active_token", "concurrent_activity",
            "recent_command", "unknown",      "bridge_exit"};
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
                plugin.pluginId = textOf(item, "plugin_id");
                plugin.kind = textOf(item, "kind");
                plugin.origin = textOf(item, "origin");
                if (plugin.kind == "clap" || plugin.kind == "clap_library")
                    plugin.format = "clap";
                else if (plugin.kind == "vst3" || plugin.kind == "vst3_library")
                    plugin.format = "vst3";
                if (const auto* build = item.find("build"); build && build->isObject())
                    plugin.buildId = textOf(*build, "id");
                s.culprit = std::move(plugin);
                break;
            }
    }
    return s;
}
// The role a run's marker recorded; empty when it cannot be read.
std::string markerRole(const fs::path& marker) {
    if (auto text = privateRead(marker))
        if (auto value = json::parse(*text); value && value->isObject()) {
            const auto* role = value->find("role");
            return role && role->isString() ? role->asString() : std::string();
        }
    return {};
}
const char* roleName(diagnostics::Role role) {
    return role == diagnostics::Role::Gui      ? "gui"
           : role == diagnostics::Role::Engine ? "engine"
                                               : "fixture";
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
// One pass over the folder: which incidents have a report file, which a marker, and what each
// marker says. Nothing is parsed.
struct Scan {
    std::map<std::string, fs::path> reportFiles, markerFiles;
    std::map<std::string, MarkerState> markers;
};
Result<Scan> scanFolder(const fs::path& root) {
    Scan scan;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto path = it->path();
        const std::string stem = reportStem(path);
        if (stem.empty()) continue;
        if (path.extension() == ".json") scan.reportFiles.emplace(stem, path);
        if (path.extension() == ".run") scan.markerFiles.emplace(stem, path);
    }
    if (ec) return storageFailure();
    for (const auto& [incident, path] : scan.markerFiles)
        scan.markers.emplace(incident, probeRun(root, incident));
    return scan;
}
bool isHeld(const Scan& scan, const std::string& incident) {
    const auto marker = scan.markers.find(incident);
    return marker != scan.markers.end() && marker->second == MarkerState::Held;
}
// Writes the small file `<incident><extension>`. One that is there but is not what it should
// be is replaced: publishing does not overwrite on Windows.
Status writeMark(const fs::path& root, const std::string& incident, const char* extension,
                 const char* schema) {
    if (hasMark(root, incident, extension, schema)) return okStatus();
    if (!privateRemove(pathFor(root, incident, extension))) return storageFailure();
    auto value = json::Value::object();
    value["schema"] = schema;
    value["incident"] = incident;
    Status published =
        privatePublish(pathFor(root, incident, extension), json::writeCanonical(value) + "\n");
    // Publishing never replaces, so a second writer that removed the same stray file meets the
    // first one's new file here (the GUI's Dismiss and an agent's crash.acknowledge at once). The
    // mark is there and valid: that is what was asked for.
    if (!published && hasMark(root, incident, extension, schema)) return okStatus();
    return published;
}
// Records the sighting.
Status writeAck(const fs::path& root, const std::string& incident) {
    return writeMark(root, incident, ".ack", kAckSchema);
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
    auto scanned = scanFolder(root);
    if (!scanned) return scanned.error();
    const auto& reportFiles = scanned->reportFiles;
    const auto& markerFiles = scanned->markerFiles;
    const auto& markers = scanned->markers;
    for (const auto& [incident, state] : markers)
        if (state == MarkerState::Held) ++listing.liveRuns;
    for (const auto& [incident, path] : reportFiles) {
        if (isHeld(*scanned, incident)) {
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
        run.marker = markerFiles.at(incident);
        run.role = markerRole(run.marker);
        run.acknowledged = isAcknowledged(root, incident);
        std::error_code e;
        run.started = fs::last_write_time(run.marker, e);
        listing.unreported.push_back(std::move(run));
    }
    std::sort(listing.reports.begin(), listing.reports.end(), [](const auto& a, const auto& b) {
        return a.written == b.written ? a.incident > b.incident : a.written > b.written;
    });
    // Markers carry no time of their own that a test or a user could rely on: file times order
    // them, as read once above (a comparator that read them again could meet a marker removed or
    // rewritten between two comparisons, which is not an ordering).
    std::sort(listing.unreported.begin(), listing.unreported.end(),
              [](const auto& a, const auto& b) {
                  return a.started == b.started ? a.incident > b.incident : a.started > b.started;
              });
    return listing;
}
Result<std::string> readReport(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    if (probeRun(root, incident) == MarkerState::Held) return noReport(incident);
    const auto path = pathFor(root, incident, ".json");
    auto loaded = loadReport(path);
    if (!loaded) return noReport(incident);
    return json::writeCanonical(loaded->value) + "\n";
}
namespace {
// Whether `incident` is something that has ended: a valid report, or, when there is no report file
// at all, a run that ended and left its marker (`state` is its run's, probeRun). A report-named
// file this build cannot read is neither: it is not this build's to acknowledge or remove, whatever
// its marker says.
bool finishedIncident(const fs::path& root, const std::string& incident, MarkerState state) {
    const auto json = pathFor(root, incident, ".json");
    if (loadReport(json)) return true;
    std::error_code ec;
    const auto status = fs::symlink_status(json, ec);
    // A missing file reports not_found, with an error code; any other error leaves it unknown,
    // which is treated as present.
    const bool present = status.type() != fs::file_type::not_found &&
                         !(ec && ec == std::errc::no_such_file_or_directory);
    return !present && state == MarkerState::Free;
}
}  // namespace
Status acknowledge(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    const auto state = probeRun(root, incident);
    if (state == MarkerState::Held) return noReport(incident);
    // Only an incident that exists can be acknowledged: a report that is valid, or a run that
    // ended and left its marker.
    if (!finishedIncident(root, incident, state)) return noReport(incident);
    return writeAck(root, incident);
}
Status settle(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    // Only a report that exists and whose run is over (T8): a settlement is about what a report
    // said, so there is nothing to settle for a run that left no report.
    if (probeRun(root, incident) == MarkerState::Held) return noReport(incident);
    if (!loadReport(pathFor(root, incident, ".json"))) return noReport(incident);
    return writeMark(root, incident, ".settled", kSettledSchema);
}
Status settle(const Suspect& suspect, const fs::path& directory) {
    Status first = okStatus();
    for (const auto& incident : suspect.incidents) {
        Status st = settle(incident, directory);
        if (!st && first) first = st;
    }
    return first;
}
Result<std::vector<Suspect>> suspects(const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty()) return std::vector<Suspect>();
    SUPPLE_TRY(checkDirectoryForReading(root));
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(root, ec))) return std::vector<Suspect>();
    auto scanned = scanFolder(root);
    if (!scanned) return scanned.error();
    // Names and file times first, newest first, as the notice orders them; only then are the
    // newest few opened. A report whose run is still live is not part of the window.
    struct Candidate {
        std::string incident;
        fs::path path;
        fs::file_time_type time;
    };
    std::vector<Candidate> found;
    for (const auto& [incident, path] : scanned->reportFiles) {
        if (isHeld(*scanned, incident)) continue;
        std::error_code e;
        const auto time = fs::last_write_time(path, e);
        if (!e) found.push_back({incident, path, time});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.time == b.time ? a.incident > b.incident : a.time > b.time;
    });
    std::vector<Suspect> out;
    std::size_t valid = 0, attempts = 0;
    for (const auto& c : found) {
        if (valid >= kSuspectReports || attempts >= kSuspectAttempts) break;
        ++attempts;
        auto loaded = loadReport(c.path);
        if (!loaded) continue;
        // A bridge's death is not a run that closed: it neither names a suspect nor takes a
        // place in the window of the last few reports.
        if (textOf(loaded->value, "kind") == "bridge_exit") continue;
        ++valid;
        if (isSettled(root, c.incident)) continue;
        const auto summary = summarise(loaded->value, c.path, loaded->written);
        const bool named =
            summary.attribution == "fault_in_image" || summary.attribution == "active_token";
        if (!named || !summary.culprit) continue;
        const ReportPlugin& plugin = *summary.culprit;
        if (plugin.format.empty() || plugin.pluginId.empty()) continue;
        // Without a build id the version stands in for it, so two versions are two suspects.
        const auto same = [&](const Suspect& s) {
            return s.format == plugin.format && s.pluginId == plugin.pluginId &&
                   s.buildId == plugin.buildId &&
                   (!s.buildId.empty() || s.version == plugin.version);
        };
        auto it = std::find_if(out.begin(), out.end(), same);
        if (it == out.end()) {
            Suspect s;
            s.format = plugin.format;
            s.pluginId = plugin.pluginId;
            s.buildId = plugin.buildId;
            s.name = plugin.name;
            s.vendor = plugin.vendor;
            s.version = plugin.version;
            s.origin = plugin.origin;
            s.attribution = summary.attribution;
            s.newest = c.time;
            out.push_back(std::move(s));
            it = out.end() - 1;
        }
        if (summary.attribution == "fault_in_image") it->attribution = "fault_in_image";
        it->incidents.push_back(c.incident);
        it->incidentTimes.push_back(c.time);
    }
    return out;
}

namespace {
// The files of an incident whose run is over. The marker goes last: while it is there the
// incident is still found, so a deletion that stops half way can be repeated.
Status removeIncident(const fs::path& root, const std::string& incident) {
    if (!privateRemove(pathFor(root, incident, ".json")) ||
        !privateRemove(pathFor(root, incident, ".ack")) ||
        !privateRemove(pathFor(root, incident, ".settled")))
        return storageFailure();
    // The supervisor's hold, which a supervisor that is gone left, then the marker.
    if (probeMarker(pathFor(root, incident, ".pub")) == MarkerState::Free &&
        !privateRemove(pathFor(root, incident, ".pub")))
        return storageFailure();
    if (probeMarker(pathFor(root, incident, ".run")) == MarkerState::Free &&
        !privateRemove(pathFor(root, incident, ".run")))
        return storageFailure();
    return okStatus();
}
}  // namespace
Status writeSampleReport(const std::string& incident, const fs::path& directory,
                         diagnostics::Role role) {
    if (!incidentName(incident)) return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(role);
    std::copy(incident.begin(), incident.end(), boot.incident.begin());
    SUPPLE_TRY(prepareDirectory(directory));
    return publishReport(directory, boot, nullptr, 31, false);
}
Status deleteReport(const std::string& incident, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty() || !incidentName(incident)) return noReport(incident);
    SUPPLE_TRY(checkDirectoryForReading(root));
    const auto state = probeRun(root, incident);
    if (state == MarkerState::Held) return noReport(incident);
    if (!finishedIncident(root, incident, state)) return noReport(incident);
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
Status writeSampleUnreportedRun(const std::string& incident, const fs::path& directory,
                                diagnostics::Role role) {
    if (!incidentName(incident)) return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(role);
    std::copy(incident.begin(), incident.end(), boot.incident.begin());
    SUPPLE_TRY(prepareDirectory(directory));
    auto marker = createMarker(directory, boot);
    if (!marker) return marker.error();
    releaseMarker(*marker);  // the holder is gone, the marker stays: a run that ended
    return okStatus();
}
Status writeSampleCrash(const std::string& incident, const std::string& plugin,
                        const std::string& vendor, const fs::path& directory,
                        const SampleCrashPlugin& extra, diagnostics::Role role) {
    if (!incidentName(incident) || plugin.size() > 64 || vendor.size() > 64 ||
        extra.id.size() > 64 || extra.version.size() > 64)
        return noReport(incident);
    Packet boot;
    boot.role = static_cast<std::uint32_t>(role);
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
    std::memcpy(r.text[2], extra.version.data(), extra.version.size());
    r.length[2] = static_cast<std::uint8_t>(extra.version.size());
    std::memcpy(r.text[3], extra.id.data(), extra.id.size());
    r.length[3] = static_cast<std::uint8_t>(extra.id.size());
    Inventory inventory;
    if (!inventory.apply(announce)) return noReport(incident);
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.modules.resize(1);
    trace.modules[0].id = extra.buildId;
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
namespace {
RestartNotice composeNotice(const ReportSummary* report, bool noReport,
                            std::vector<std::string> incidents,
                            std::optional<diagnostics::Role> role) {
    RestartNotice notice;
    notice.earlier = incidents.size() - 1;
    notice.incidents = std::move(incidents);
    // Which app closed: the product's name, and for the headless engine (whose line a person may
    // read beside the app's own) that it was the engine.
    std::string app(product::kName);
    if (role) {
        notice.role = roleName(*role);
        if (*role == diagnostics::Role::Engine) app += " engine";
    }
    if (noReport) {
        notice.kind = RestartNotice::Kind::NoReport;
        notice.headline = app + " closed unexpectedly last time, and no crash report was written.";
        notice.detail =
            "That happens when the program that watches it stopped too, or could not save one.";
    } else {
        notice.kind = RestartNotice::Kind::Report;
        const bool points =
            report &&
            (report->attribution == "fault_in_image" || report->attribution == "active_token") &&
            report->culprit && !report->culprit->name.empty();
        notice.headline = points ? who(*report->culprit) + " was running when " + app +
                                       " closed unexpectedly last time."
                                 : app + " closed unexpectedly last time.";
        notice.detail = "A crash report was saved on this computer; it was not sent anywhere.";
        if (points) {
            notice.plugin = report->culprit->name;
            notice.detail +=
                " The report points at that plug-in but cannot prove it was the cause.";
        }
    }
    if (notice.earlier == 1)
        notice.earlierNote = app + " also closed unexpectedly once before that.";
    else if (notice.earlier > 1)
        notice.earlierNote = app + " also closed unexpectedly " + std::to_string(notice.earlier) +
                             " times before that.";
    return notice;
}
}  // namespace
Result<std::optional<RestartNotice>> lastRunNotice(const fs::path& directory,
                                                   std::optional<diagnostics::Role> role) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty()) return std::optional<RestartNotice>();
    SUPPLE_TRY(checkDirectoryForReading(root));
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(root, ec))) return std::optional<RestartNotice>();
    auto scanned = scanFolder(root);
    if (!scanned) return scanned.error();
    // The incidents nobody has acknowledged whose run is over, newest first by file time (a
    // run's marker time is when it started). Nothing is parsed to decide that.
    struct Candidate {
        std::string incident;
        fs::file_time_type time;
        bool report;
    };
    const std::string wanted = role ? roleName(*role) : std::string();
    std::vector<Candidate> found;
    for (const auto& [incident, path] : scanned->reportFiles) {
        if (isHeld(*scanned, incident) || isAcknowledged(root, incident)) continue;
        std::error_code e;
        const auto time = fs::last_write_time(path, e);
        if (!e) found.push_back({incident, time, true});
    }
    for (const auto& [incident, path] : scanned->markerFiles) {
        if (scanned->markers.at(incident) != MarkerState::Free ||
            scanned->reportFiles.count(incident) || isAcknowledged(root, incident))
            continue;
        if (role && markerRole(path) != wanted) continue;  // another app's run, or unreadable
        std::error_code e;
        const auto time = fs::last_write_time(path, e);
        if (!e) found.push_back({incident, time, false});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.time == b.time ? a.incident > b.incident : a.time > b.time;
    });
    // Only the newest few reports are read; the rest are counted by name (P4). A report that
    // cannot be read is not announced. An unreported run costs no read.
    std::optional<ReportSummary> head;
    bool headIsRun = false, anyHead = false;
    std::vector<std::string> incidents;
    std::size_t parsed = 0, reads = 0;
    for (const auto& c : found) {
        if (!c.report) {
            if (!anyHead) headIsRun = anyHead = true;
            incidents.push_back(c.incident);
            continue;
        }
        if (role) {
            // A report's role is known by reading it: the newest few are read, and the ones
            // that are another app's, or cannot be read, are nobody's here.
            if (reads >= kNoticeRoleReads) continue;
            const auto path = scanned->reportFiles.at(c.incident);
            auto loaded = loadReport(path);
            // A bridge's death is not the application closing, and does not use up a read: the
            // folder holds at most 20 reports, so the parsing it costs is bounded anyway.
            if (loaded && textOf(loaded->value, "kind") == "bridge_exit") continue;
            ++reads;
            if (!loaded || textOf(loaded->value, "role") != wanted) continue;
            if (!anyHead) {
                anyHead = true;
                head = summarise(loaded->value, path, loaded->written);
            }
            incidents.push_back(c.incident);
            continue;
        }
        if (parsed >= kNoticeReportsRead) {
            incidents.push_back(c.incident);
            continue;
        }
        ++parsed;
        const auto path = scanned->reportFiles.at(c.incident);
        auto loaded = loadReport(path);
        if (!loaded || textOf(loaded->value, "kind") == "bridge_exit") continue;
        if (!anyHead) {
            anyHead = true;
            head = summarise(loaded->value, path, loaded->written);
        }
        incidents.push_back(c.incident);
    }
    if (incidents.empty()) return std::optional<RestartNotice>();
    // No report read was readable but older ones were not read: the plain sentence (no head).
    return std::optional<RestartNotice>(
        composeNotice(head ? &*head : nullptr, headIsRun, std::move(incidents), role));
}
std::string noticeText(const RestartNotice& notice) {
    std::string text = notice.headline + " " + notice.detail;
    if (!notice.earlierNote.empty()) text += " " + notice.earlierNote;
    return text;
}
Status acknowledgeNotice(const RestartNotice& notice, const fs::path& directory) {
    const fs::path root = directory.empty() ? defaultDirectory() : directory;
    if (root.empty()) return storageFailure();
    SUPPLE_TRY(checkDirectoryForReading(root));
    // The listing established that these exist; confirming each by name keeps a click from
    // parsing the reports the notice did not read.
    Status first = okStatus();
    for (const auto& incident : notice.incidents) {
        Status st = okStatus();
        const auto state = probeRun(root, incident);
        std::error_code ec;
        const bool hasReport =
            fs::is_regular_file(fs::symlink_status(pathFor(root, incident, ".json"), ec));
        if (!incidentName(incident) || state == MarkerState::Held ||
            (!hasReport && state != MarkerState::Free))
            st = noReport(incident);
        else
            st = writeAck(root, incident);
        if (!st && first) first = st;
    }
    return first;
}
}  // namespace supple::diagnostics
