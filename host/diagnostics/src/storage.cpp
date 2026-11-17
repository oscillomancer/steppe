// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "storage.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
                                         "attribution"};
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
                   : oneOf("kind", {"crash", "unexpected_exit"})))
        return false;
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
    }
    // Attribution is optional (reports of earlier builds have none); when present it must be
    // one the report's own plug-ins and commands support.
    if (const auto* attribution = value.find("attribution")) {
        const auto* level = attribution->isObject() ? attribution->find("confidence") : nullptr;
        const bool known =
            level && level->isString() &&
            (level->asString() == "fault_in_image" || level->asString() == "active_token" ||
             level->asString() == "concurrent_activity" || level->asString() == "recent_command" ||
             level->asString() == "unknown");
        if ((known || !tolerant || !level || !level->isString()) &&
            !attributionShape(*attribution, *value.find("plugins"), *value.find("commands"),
                              *threads, value.find("modules")))
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
Result<std::intptr_t> createMarkerFile(const fs::path& root, const Packet& boot) {
    auto value = base(boot);
    value["schema"] = "supple.crash-run/1.0";
    const std::string text = json::writeCanonical(value) + "\n";
    const auto path = root / (std::string(boot.incident.data(), 32) + ".run");
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
        return failure();
    }
    return reinterpret_cast<std::intptr_t>(file);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return failure();
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 ||
        ::write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size()) ||
        ::fsync(fd) != 0) {
        ::close(fd);
        return failure();
    }
    return static_cast<std::intptr_t>(fd);
#endif
}
}  // namespace
Result<std::intptr_t> createMarker(const fs::path& root, const Packet& boot) {
    auto marker = createMarkerFile(root, boot);
    // A run whose supervisor died leaves its marker and nothing prunes it; starting a run is the
    // one moment every platform's code passes through. Best effort: a failure here never stops
    // the run.
    if (marker) (void)pruneMarkers(root);
    return marker;
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
void removeOwnMarker(const fs::path& root, const Packet& boot, std::intptr_t file) {
#if defined(_WIN32)
    releaseMarker(file);
    removeMarker(root, boot);
#else
    removeMarker(root, boot);
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
        if (info.nFileSizeHigh == 0 && info.nFileSizeLow == 0) {
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
        if (info.st_size == 0)
            result = MarkerState::Held;
        else if (::flock(fd, LOCK_SH | LOCK_NB) == 0)
            result = MarkerState::Free;  // the lock goes with the descriptor, on close
        else if (errno == EWOULDBLOCK)
            result = MarkerState::Held;
    }
    ::close(fd);
    return result;
#endif
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
// Private overload lets storage tests control filesystem interleavings without
// clocks or platform-specific hooks. Ordinary callers supply no checkpoint.
Status pruneReports(const fs::path& root,
                    const std::function<void(const fs::path&, std::string_view)>& checkpoint) {
    struct Entry {
        fs::path path;
        fs::file_time_type time;
        std::uintmax_t bytes;
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
        reports.push_back({path, time, bytes});
        total += bytes;
    }
    if (ec) return failure();
    std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) {
        return a.time == b.time ? a.path < b.path : a.time < b.time;
    });
    std::size_t count = reports.size();
    for (const auto& report : reports) {
        if (count <= 20 && total <= 50 * 1024 * 1024) break;
        if (checkpoint) checkpoint(report.path, "remove");
        if (!privateRemove(report.path)) return failure();
        privateRemove(
            fs::path(report.path).replace_extension(".ack"));  // its acknowledgement, if any
        privateRemove(fs::path(report.path).replace_extension(".settled"));  // and settlement
        --count;
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
    std::vector<fs::path> markers, marks;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto path = it->path();
        const std::string stem = reportStem(path);
        if (stem.empty()) continue;
        if (path.extension() == ".json") reported.insert(stem);
        if (path.extension() == ".run") markers.push_back(path);
        if (path.extension() == ".ack" || path.extension() == ".settled") marks.push_back(path);
    }
    if (ec) return failure();
    std::set<std::string> running;
    for (const auto& path : markers) {
        running.insert(reportStem(path));
        // Only a run that ended and left no report counts: a held marker is a live run, and a
        // marker beside a report is that report's business.
        if (reported.count(reportStem(path)) || probeMarker(path) != MarkerState::Free) continue;
        std::error_code e;
        const auto time = fs::last_write_time(path, e);
        if (!e) stale.push_back({path, time});
    }
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
