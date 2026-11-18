// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "storage.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <set>
#include <vector>
#include "diagnostics_build.h"
#include "inventory.h"
#include "private_files.h"
#include "supple/base/files.h"
#include "supple/product.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace supple::diagnostics::detail {
namespace fs = std::filesystem;
namespace {
Error failure() {
    return Error("diagnostics.storage", "local diagnostic storage is unavailable", {},
                 "check permissions and free space in the app local data folder");
}
#if defined(_WIN32)
struct PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    std::vector<std::byte> token;
    bool ready = false;
    PrivateSecurity() {
        HANDLE process = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process)) return;
        DWORD size = 0;
        GetTokenInformation(process, TokenUser, nullptr, 0, &size);
        token.resize(size);
        const bool ok = GetTokenInformation(process, TokenUser, token.data(), size, &size) != FALSE;
        CloseHandle(process);
        if (!ok) return;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token.data())->User.Sid, &sid))
            return;
        std::wstring text =
            L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        ready = ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
                                                                     &descriptor, nullptr) != FALSE;
    }
    ~PrivateSecurity() {
        if (descriptor) LocalFree(descriptor);
    }
    PSID user() { return reinterpret_cast<TOKEN_USER*>(token.data())->User.Sid; }
};
bool privateDirectory(const fs::path& path, bool strict = true) {
    PrivateSecurity security;
    if (!security.ready) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.descriptor, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) ||
        (flags & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR current = nullptr;
    auto name = path.wstring();
    const DWORD status =
        GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                              nullptr, nullptr, nullptr, &current);
    const bool ok = status == ERROR_SUCCESS && owner && EqualSid(owner, security.user());
    if (current) LocalFree(current);
    return ok && (!strict || privateDirectoryAcl(path));
}
#else
bool privateDirectory(const fs::path& path, bool strict = true) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info{};
    const bool ok = ::fstat(fd, &info) == 0 && info.st_uid == ::geteuid() &&
                    !(info.st_mode & (strict ? 0077 : 0022));
    ::close(fd);
    return ok;
}
#endif
std::string role(const Packet& p) {
    return p.role == 1 ? "gui" : p.role == 2 ? "engine" : "fixture";
}
json::Value base(const Packet& boot) {
    auto v = json::Value::object();
    v["incident"] = std::string(boot.incident.data(), 32);
    v["role"] = role(boot);
    v["build"] = json::Value::object();
    v["build"]["version"] = std::string(product::kVersion);
    v["build"]["commit"] = kCommit;
    v["build"]["dirty"] = kDirty;
#if defined(_WIN32)
    v["platform"] = "windows";
#elif defined(__APPLE__)
    v["platform"] = "macos";
#else
    v["platform"] = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    v["architecture"] = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    v["architecture"] = "x86_64";
#else
    v["architecture"] = "unknown";
#endif
    return v;
}
}  // namespace
bool reportName(std::string_view stem) {
    return stem.size() == 32 && std::all_of(stem.begin(), stem.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::string reportStem(const fs::path& path) {
    const auto native = path.stem().native();
    if (native.size() != 32) return {};
    std::string text;
    for (auto c : native) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        text += static_cast<char>(c);
    }
    return text;
}
bool reportShape(const json::Value& value, bool tolerant) {
    if (!value.isObject()) return false;
    for (const auto& field : value.asObject()) {
        if (tolerant) break;
        const std::string_view allowed[]{"schema",      "incident",
                                         "role",        "build",
                                         "platform",    "architecture",
                                         "kind",        "code",
                                         "cause",       "native_fatal_status",
                                         "complete",    "terminal_context",
                                         "unavailable", "modules",
                                         "threads",     "plugins",
                                         "commands",    "dropped",
                                         "attribution", "bridges",
                                         "bridge"};
        if (std::find(std::begin(allowed), std::end(allowed), field.key) == std::end(allowed))
            return false;
    }
    auto text = [&](const char* key, std::size_t max) {
        const auto* v = value.find(key);
        return v && v->isString() && v->asString().size() <= max;
    };
    auto oneOf = [&](const char* key, std::initializer_list<std::string_view> choices) {
        const auto* v = value.find(key);
        return v && v->isString() &&
               std::find(choices.begin(), choices.end(), v->asString()) != choices.end();
    };
    if (!text("incident", 32) || !oneOf("schema", {"supple.crash-report/1.0"}) ||
        !oneOf("role", {"gui", "engine", "fixture"}) ||
        !oneOf("platform", {"linux", "macos", "windows"}) ||
        !oneOf("architecture", {"arm64", "x86_64", "unknown"}) ||
        !(tolerant ? text("kind", 32) && !value.find("kind")->asString().empty()
                   : oneOf("kind", {"crash", "unexpected_exit", "bridge_exit"})))
        return false;
    // A bridge_exit report (topic 6a) is about one bridge process that died while the worker
    // lived; no other kind names one.
    const bool bridgeExit = value.find("kind")->asString() == "bridge_exit";
    std::int64_t bridgeOrdinal = 0;
    if (const auto* bridge = value.find("bridge")) {
        if (!bridgeExit || !bridge->isInt() || bridge->asInt() < 1 || bridge->asInt() > UINT16_MAX)
            return false;
        bridgeOrdinal = bridge->asInt();
    } else if (bridgeExit) {
        return false;
    }
    auto integer = [&](const char* key, std::int64_t min, std::int64_t max) {
        const auto* v = value.find(key);
        return v && v->isInt() && v->asInt() >= min && v->asInt() <= max;
    };
    // Normal returns are signed ints; native capsule codes/causes are uint32_t.
    if (!integer("code", INT32_MIN, UINT32_MAX) || !integer("cause", 0, UINT32_MAX) ||
        !value.find("dropped") || !droppedShape(*value.find("dropped")))
        return false;
    for (const auto* key : {"complete", "terminal_context", "native_fatal_status"}) {
        const auto* v = value.find(key);
        if (!v || !v->isBool()) return false;
    }
    if (value.find("complete")->asBool()) return false;
    // It has no capsule, no thread and no fault: only the inventory's account of the bridge.
    if (bridgeExit &&
        (value.find("terminal_context")->asBool() || value.find("native_fatal_status")->asBool() ||
         !value.find("bridges") || !value.find("attribution") ||
         !bridgeExitShape(*value.find("bridges"), bridgeOrdinal, value.find("code")->asInt())))
        return false;
    // Thread traces, the plug-in inventory and the command entries have a validated nested
    // schema: a report whose array has another shape is kept, never authority to delete.
    {
        const auto* v = value.find("commands");
        if (!v || !commandsShape(*v)) return false;
    }
    const auto* threads = value.find("threads");
    if (!threads || !traceShape(value.find("modules"), *threads)) return false;
    {
        const auto* plugins = value.find("plugins");
        if (!plugins || !pluginsShape(*plugins, threads->size())) return false;
        // The bridge processes (topic 6a) are optional: reports of earlier builds have none.
        if (const auto* bridges = value.find("bridges")) {
            const auto* dropped = value.find("dropped");
            const auto* lost = dropped && dropped->isObject() ? dropped->find("bridges") : nullptr;
            if (!bridgesShape(*bridges, *plugins, lost && lost->isInt() ? lost->asInt() : 0))
                return false;
        }
    }
    // Attribution is optional (reports of earlier builds have none); when present it must be
    // one the report's own plug-ins and commands support.
    if (const auto* attribution = value.find("attribution")) {
        const auto* level = attribution->isObject() ? attribution->find("confidence") : nullptr;
        const bool known =
            level && level->isString() &&
            (level->asString() == "fault_in_image" || level->asString() == "active_token" ||
             level->asString() == "concurrent_activity" || level->asString() == "recent_command" ||
             level->asString() == "unknown" || level->asString() == "bridge_exit");
        if ((known || !tolerant || !level || !level->isString()) &&
            !attributionShape(*attribution, *value.find("plugins"), *value.find("commands"),
                              *threads, value.find("modules"), bridgeOrdinal))
            return false;
        if (bridgeExit && (!level || !level->isString() || level->asString() != "bridge_exit"))
            return false;
    }
    const auto* unavailable = value.find("unavailable");
    if (!unavailable || !unavailable->isArray() || unavailable->size() == 0 ||
        unavailable->size() > 128)
        return false;
    for (const auto& reason : unavailable->asArray())
        if (!reason.isString() || reason.asString().empty() || reason.asString().size() > 32768)
            return false;
    const auto* build = value.find("build");
    if (!build || !build->isObject() || build->size() != 3) return false;
    for (const auto* key : {"version", "commit"}) {
        const auto* v = build->find(key);
        if (!v || !v->isString() || v->asString().size() > 64) return false;
    }
    const auto* dirty = build->find("dirty");
    return dirty && dirty->isBool();
}
Status prepareDirectory(const fs::path& root) {
    if (root.empty() || !root.is_absolute()) return failure();
    // The caller's application-local parent must already exist, or be created
    // using ordinary directory creation. The two owned levels are private.
    std::error_code ec;
    fs::create_directories(root.parent_path().parent_path(), ec);
    if (ec || !privateDirectory(root.parent_path(), false) || !privateDirectory(root))
        return failure();
    return okStatus();
}
namespace {
Result<std::intptr_t> createMarkerFile(const fs::path& root, const Packet& boot,
                                       const char* extension) {
    auto value = base(boot);
    value["schema"] = "supple.crash-run/1.0";
    const std::string text = json::writeCanonical(value) + "\n";
    const auto path = root / (std::string(boot.incident.data(), 32) + extension);
#if defined(_WIN32)
    PrivateSecurity security;
    if (!security.ready) return failure();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.descriptor, FALSE};
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return failure();
    OVERLAPPED lock{};
    DWORD written = 0;
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock) ||
        !WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !FlushFileBuffers(file)) {
        CloseHandle(file);
        DeleteFileW(path.c_str());  // not left empty: an empty marker reads as one being created
        return failure();
    }
    return reinterpret_cast<std::intptr_t>(file);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return failure();
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 ||
        ::write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size()) ||
        ::fsync(fd) != 0) {
        ::unlink(path.c_str());  // not left empty: an empty marker reads as one being created
        ::close(fd);
        return failure();
    }
    return static_cast<std::intptr_t>(fd);
#endif
}
}  // namespace
Result<std::intptr_t> createMarker(const fs::path& root, const Packet& boot) {
    auto marker = createMarkerFile(root, boot, ".run");
    // A run whose supervisor died leaves its marker and nothing prunes it; starting a run is the
    // one moment every platform's code passes through. Best effort: a failure here never stops
    // the run.
    if (marker) (void)pruneMarkers(root);
    return marker;
}
Result<std::intptr_t> createPublishHold(const fs::path& root, const Packet& boot) {
    return createMarkerFile(root, boot, ".pub");
}
void releaseMarker(std::intptr_t file) {
    if (file < 0) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(file));
#else
    ::close(static_cast<int>(file));
#endif
}
void removeMarker(const fs::path& root, const Packet& boot) {
    std::error_code ec;
    fs::remove(root / (std::string(boot.incident.data(), 32) + ".run"), ec);
}
namespace {
void removeHoldFile(const fs::path& root, const Packet& boot) {
    std::error_code ec;
    fs::remove(root / (std::string(boot.incident.data(), 32) + ".pub"), ec);
}
}  // namespace
void removeOwnMarker(const fs::path& root, const Packet& boot, std::intptr_t file) {
#if defined(_WIN32)
    releaseMarker(file);
    removeMarker(root, boot);
#else
    removeMarker(root, boot);
    releaseMarker(file);
#endif
    // Only called when no supervisor is left (ADR-0091): its hold, if it made one, has no holder.
    removeHoldFile(root, boot);
}
void removePublishHold(const fs::path& root, const Packet& boot, std::intptr_t file) {
#if defined(_WIN32)
    releaseMarker(file);
    removeHoldFile(root, boot);
#else
    removeHoldFile(root, boot);
    releaseMarker(file);
#endif
}
Status checkDirectoryForReading(const fs::path& root) {
    std::error_code ec;
    const auto status = fs::symlink_status(root, ec);
    if (status.type() == fs::file_type::not_found) return okStatus();
    if (ec) return failure();
    if (!privateDirectory(root.parent_path(), false) || !privateDirectory(root)) return failure();
    return okStatus();
}
namespace {
// How long an empty marker is taken to be one being created. Creation takes microseconds; an
// older empty marker is left by a creator that failed between its creation and its first write,
// and is judged by its lock like any other (held only if someone really holds it).
constexpr std::int64_t kEmptyMarkerGraceSeconds = 60;
}  // namespace
MarkerState probeMarker(const fs::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return MarkerState::Gone;
        // Another handle denies sharing: something holds the marker.
        return error == ERROR_SHARING_VIOLATION ? MarkerState::Held : MarkerState::Unreadable;
    }
    MarkerState result = MarkerState::Unreadable;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        FILETIME nowTime{};
        GetSystemTimeAsFileTime(&nowTime);
        const auto ticks = [](const FILETIME& t) {
            return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        // 100 ns units; a time in the future (the clock was set back) is not young: the lock
        // decides, as for any older empty marker.
        const bool young =
            ticks(info.ftLastWriteTime) <= ticks(nowTime) &&
            ticks(info.ftLastWriteTime) + kEmptyMarkerGraceSeconds * 10'000'000ULL > ticks(nowTime);
        if (info.nFileSizeHigh == 0 && info.nFileSizeLow == 0 && young) {
            result = MarkerState::Held;
        } else {
            OVERLAPPED probe{};
            if (LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &probe)) {
                UnlockFileEx(file, 0, 1, 0, &probe);
                result = MarkerState::Free;
            } else if (GetLastError() == ERROR_LOCK_VIOLATION) {
                result = MarkerState::Held;
            }
        }
    }
    CloseHandle(file);
    return result;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? MarkerState::Gone : MarkerState::Unreadable;
    MarkerState result = MarkerState::Unreadable;
    struct stat info{};
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid()) {
        // A modification time ahead of the clock (it was set back) is not young either.
        const std::int64_t age = static_cast<std::int64_t>(::time(nullptr)) - info.st_mtime;
        if (info.st_size == 0 && age >= 0 && age < kEmptyMarkerGraceSeconds)
            result = MarkerState::Held;  // being created
        else if (::flock(fd, LOCK_SH | LOCK_NB) == 0)
            result = MarkerState::Free;  // the lock goes with the descriptor, on close
        else if (errno == EWOULDBLOCK)
            result = MarkerState::Held;
    }
    ::close(fd);
    return result;
#endif
}
MarkerState probeRun(const fs::path& root, const std::string& incident) {
    const auto hold = probeMarker(root / (incident + ".pub"));
    const auto run = probeMarker(root / (incident + ".run"));
    if (run == MarkerState::Gone) return run;
    return hold == MarkerState::Held ? MarkerState::Held : run;
}
Status publishReport(const fs::path& root, const Packet& boot, const Packet* terminal, int code,
                     bool fatal, const TraceSnapshot* trace, const Inventory* inventory) {
    auto v = base(boot);
    v["schema"] = "supple.crash-report/1.0";
    v["kind"] = terminal ? "crash" : "unexpected_exit";
    v["native_fatal_status"] = fatal;
    v["code"] = terminal ? static_cast<long long>(terminal->code) : code;
    v["cause"] = terminal ? static_cast<long long>(terminal->cause) : 0;
    v["complete"] = false;
    v["terminal_context"] = terminal && terminal->pc != 0;
    v["unavailable"] = json::Value::array();
    if (!terminal)
        v["unavailable"].asArray().push_back("no terminal capsule; cause may be unknown");
    if (trace) {
        for (auto& limit : traceLimits(*trace)) v["unavailable"].asArray().push_back(limit);
        v["modules"] = modulesJson(*trace);
        v["threads"] = threadsJson(*trace);
    } else {
        v["threads"] = json::Value::array();
    }
    {
        // Without an inventory (a launcher's report, a test) nothing was announced: the same
        // code says so, with empty arrays.
        const Inventory none;
        const Inventory& known = inventory ? *inventory : none;
        for (auto& limit : known.limits()) v["unavailable"].asArray().push_back(limit);
        for (auto& limit : known.commandLimits(trace)) v["unavailable"].asArray().push_back(limit);
        v["plugins"] = known.pluginsJson(trace);
        v["bridges"] = known.bridgesJson();
        v["commands"] = known.commandsJson(trace);
        v["attribution"] = known.attributionJson(trace);
        v["dropped"] = known.droppedJson(trace);
    }
    // Deliberately never serialize pc/frame/sp, thread or process ids, paths, args,
    // exception or console text. Frames are module identity plus relative offsets.
    const auto text = json::writeCanonical(v) + "\n";
    if (text.size() > 4 * 1024 * 1024) return failure();
    SUPPLE_TRY(privatePublish(root / (std::string(boot.incident.data(), 32) + ".json"), text));
    return pruneReports(root);
}
std::optional<PreparedBridgeReport> prepareBridgeReport(const Packet& boot,
                                                        const Inventory& inventory,
                                                        std::uint16_t bridge) {
    const BridgeEntry* entry = inventory.bridge(bridge);
    if (!entry || !entry->ended) return std::nullopt;
    // Its own incident: the worker's run goes on under the id it has, and no marker belongs to
    // this one, so a reader counts it the moment it is there. The id is the one the worker minted
    // when it announced the end, so that the application can name this report; a worker that sent
    // none (an older one, a test) gets one made here.
    std::string id = entry->incident;
    if (id.size() != 32) {
        auto made = incidentId();
        if (!made) return std::nullopt;
        id = std::move(*made);
    }
    Packet own = boot;
    std::copy(id.begin(), id.end(), own.incident.begin());
    auto v = base(own);
    v["schema"] = "supple.crash-report/1.0";
    v["kind"] = "bridge_exit";
    v["bridge"] = static_cast<long long>(bridge);
    v["native_fatal_status"] = false;
    v["code"] = static_cast<long long>(entry->status);
    v["cause"] = 0;
    v["complete"] = false;
    v["terminal_context"] = false;
    v["unavailable"] = json::Value::array();
    v["unavailable"].asArray().push_back(
        "the bridge process's threads were not collected: the report is the host's account of it");
    for (auto& limit : inventory.limits()) v["unavailable"].asArray().push_back(limit);
    v["threads"] = json::Value::array();
    v["plugins"] = inventory.pluginsJson(nullptr);
    v["bridges"] = inventory.bridgesJson();
    v["commands"] = json::Value::array();
    v["attribution"] = inventory.bridgeExitAttributionJson(bridge);
    v["dropped"] = inventory.droppedJson(nullptr);
    auto text = json::writeCanonical(v) + "\n";
    if (text.size() > 4 * 1024 * 1024) return std::nullopt;
    return PreparedBridgeReport{std::move(id), std::move(text)};
}
Status writeBridgeReport(const fs::path& root, const PreparedBridgeReport& report) {
    SUPPLE_TRY(privatePublish(root / (report.id + ".json"), report.text));
    return pruneReports(root);
}
Status publishBridgeReport(const fs::path& root, const Packet& boot, const Inventory& inventory,
                           std::uint16_t bridge) {
    const auto prepared = prepareBridgeReport(boot, inventory, bridge);
    if (!prepared) return failure();
    return writeBridgeReport(root, *prepared);
}
// Private overload lets storage tests control filesystem interleavings without
// clocks or platform-specific hooks. Ordinary callers supply no checkpoint.
Status pruneReports(const fs::path& root,
                    const std::function<void(const fs::path&, std::string_view)>& checkpoint) {
    struct Entry {
        fs::path path;
        fs::file_time_type time;
        std::uintmax_t bytes;
        bool bridge;  // a bridge_exit report: kept on its own budget
    };
    std::vector<Entry> reports;
    std::error_code ec;
    std::uintmax_t total = 0;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        const auto path = entry.path();
        if (path.extension() != ".json" || !reportName(reportStem(path))) continue;
        if (checkpoint) checkpoint(path, "status");
        const auto status = entry.symlink_status(ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            continue;
        }
        if (ec) return failure();
        if (!fs::is_regular_file(status)) continue;
        if (checkpoint) checkpoint(path, "size");
        const auto bytes = entry.file_size(ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            continue;
        }
        if (ec) return failure();
        if (bytes > 4 * 1024 * 1024) continue;
        auto text = privateRead(path);
        if (!text) continue;
        auto value = json::parse(*text);
        if (!value || !reportShape(*value)) continue;
        const auto* schema = value->find("schema");
        const auto* id = value->find("incident");
        if (!schema || !schema->isString() || schema->asString() != "supple.crash-report/1.0" ||
            !id || !id->isString() || id->asString() != reportStem(path))
            continue;
        if (checkpoint) checkpoint(path, "time");
        const auto time = entry.last_write_time(ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            continue;
        }
        if (ec) return failure();
        const auto* kind = value->find("kind");
        reports.push_back(
            {path, time, bytes, kind && kind->isString() && kind->asString() == "bridge_exit"});
        total += bytes;
    }
    if (ec) return failure();
    std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) {
        return a.time == b.time ? a.path < b.path : a.time < b.time;
    });
    // A bridge's death is the host's note about a plug-in's process, not a crash of the app. It
    // has a budget of its own, so a plug-in whose bridge dies in every run cannot push out the
    // report that makes a plug-in a suspect; the byte cap covers both kinds.
    std::size_t count = 0, bridges = 0;
    for (const auto& report : reports) (report.bridge ? bridges : count) += 1;
    for (const auto& report : reports) {
        const bool overBytes = total > 50 * 1024 * 1024;
        if (!(report.bridge ? bridges > kKeepBridgeReports : count > 20) && !overBytes) continue;
        if (checkpoint) checkpoint(report.path, "remove");
        if (!privateRemove(report.path)) return failure();
        privateRemove(
            fs::path(report.path).replace_extension(".ack"));  // its acknowledgement, if any
        privateRemove(fs::path(report.path).replace_extension(".settled"));  // and settlement
        --(report.bridge ? bridges : count);
        total -= report.bytes;
    }
    return okStatus();
}
Status pruneMarkers(const fs::path& root) {
    struct Entry {
        fs::path path;
        fs::file_time_type time;
    };
    std::vector<Entry> stale;
    std::error_code ec;
    std::set<std::string> reported;
    std::vector<fs::path> markers, marks, holds;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto path = it->path();
        const std::string stem = reportStem(path);
        if (stem.empty()) continue;
        if (path.extension() == ".json") reported.insert(stem);
        if (path.extension() == ".run") markers.push_back(path);
        if (path.extension() == ".pub") holds.push_back(path);
        if (path.extension() == ".ack" || path.extension() == ".settled") marks.push_back(path);
    }
    if (ec) return failure();
    std::set<std::string> running;
    for (const auto& path : markers) {
        running.insert(reportStem(path));
        // Only a run that ended and left no report counts: a held marker is a live run, and a
        // marker beside a report is that report's business.
        if (reported.count(reportStem(path)) ||
            probeRun(root, reportStem(path)) != MarkerState::Free)
            continue;
        std::error_code e;
        const auto time = fs::last_write_time(path, e);
        if (!e) stale.push_back({path, time});
    }
    // A supervisor's hold with no run marker beside it (the worker never made one, or the
    // supervisor died) says nothing: removed once nobody holds it.
    for (const auto& path : holds)
        if (!running.count(reportStem(path)) && probeMarker(path) == MarkerState::Free)
            privateRemove(path);
    // An acknowledgement or a settlement whose report and run marker are both gone belongs to
    // nothing: a deletion or retention raced with the write. It is removed.
    for (const auto& path : marks)
        if (!reported.count(reportStem(path)) && !running.count(reportStem(path)))
            privateRemove(path);
    if (stale.size() <= kKeepUnreportedRuns) return okStatus();
    std::sort(stale.begin(), stale.end(), [](const auto& a, const auto& b) {
        return a.time == b.time ? a.path > b.path : a.time > b.time;
    });
    for (std::size_t i = kKeepUnreportedRuns; i < stale.size(); ++i) {
        if (!privateRemove(stale[i].path)) return failure();
        privateRemove(fs::path(stale[i].path).replace_extension(".ack"));
        privateRemove(fs::path(stale[i].path).replace_extension(".settled"));
        privateRemove(fs::path(stale[i].path).replace_extension(".pub"));
    }
    return okStatus();
}
Status pruneReports(const fs::path& root) {
    auto status = pruneReports(root, {});
    // Both are bounded and neither depends on the other: markers are pruned whatever reports did.
    auto markers = pruneMarkers(root);
    return status ? markers : status;
}
}  // namespace supple::diagnostics::detail
