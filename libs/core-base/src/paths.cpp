// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/paths.h"
#include <cstdlib>
#include "supple/base/sha256.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <climits>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#endif

namespace supple::paths {
namespace {
std::filesystem::path env(const char* key) {
#if defined(_WIN32)
    // Use the wide environment API so non-ASCII user names retain their path.
    const std::wstring name(key, key + std::char_traits<char>::length(key));
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name.c_str(), value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return std::filesystem::path(value);
#else
    const char* value = std::getenv(key);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}
#if !defined(_WIN32)
std::filesystem::path belowHome(const std::filesystem::path& suffix) {
    auto root = home();
    return root.empty() ? root : root / suffix;
}
#endif
}  // namespace
std::filesystem::path home() {
#if defined(_WIN32)
    return env("USERPROFILE");
#else
    return env("HOME");
#endif
}
std::filesystem::path config() {
#if defined(_WIN32)
    return env("APPDATA");
#elif defined(__APPLE__)
    return belowHome("Library/Application Support");
#else
    auto root = env("XDG_CONFIG_HOME");
    return root.empty() ? belowHome(".config") : root;
#endif
}
std::filesystem::path data(bool allowTemporary) {
#if defined(_WIN32)
    auto root = env("APPDATA");
#elif defined(__APPLE__)
    auto root = belowHome("Library/Application Support");
#else
    auto root = env("XDG_DATA_HOME");
    if (root.empty() && allowTemporary) {
        const char* user = std::getenv("HOME");
        return std::filesystem::path(user ? user : "/tmp") / ".local/share";
    }
    if (root.empty()) root = belowHome(".local/share");
#endif
    return root.empty() && allowTemporary ? runtime() / ".local/share" : root;
}
std::filesystem::path localData() {
#if defined(_WIN32)
    return env("LOCALAPPDATA");
#else
    return data();
#endif
}
std::filesystem::path cache(bool allowTemporary) {
#if defined(_WIN32)
    auto root = env("LOCALAPPDATA");
#elif defined(__APPLE__)
    auto root = belowHome("Library/Caches");
#else
    auto root = env("XDG_CACHE_HOME");
    if (root.empty() && allowTemporary) {
        const char* user = std::getenv("HOME");
        return std::filesystem::path(user ? user : "/tmp") / ".cache";
    }
    if (root.empty()) root = belowHome(".cache");
#endif
    return root.empty() && allowTemporary ? runtime() / ".cache" : root;
}
std::filesystem::path runtime() {
#if defined(_WIN32)
    auto root = env("LOCALAPPDATA");
    if (!root.empty()) return root / "Temp";
#elif defined(__linux__)
    auto root = env("XDG_RUNTIME_DIR");
    if (!root.empty()) return root;
    // Some launchers (Codex's MCP client among them) drop XDG_RUNTIME_DIR; the
    // login's /run/user/<uid> is where it would have pointed.
    const auto uid = ::getuid();
    const std::string user = "/run/user/" + std::to_string(uid);
    struct stat info{};
    if (::stat(user.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == uid &&
        (info.st_mode & 077) == 0)
        return user;
    return "/tmp";  // Preserve the Linux control socket contract.
#elif defined(__APPLE__)
    // Some launchers (MCP clients on the official SDKs among them) drop
    // TMPDIR; confstr names the per-user directory it would have held.
    if (env("TMPDIR").empty()) {
        char user[PATH_MAX];
        const auto size = ::confstr(_CS_DARWIN_USER_TEMP_DIR, user, sizeof user);
        if (size > 0 && size <= sizeof user) return user;
    }
#endif
    std::error_code error;
    auto temporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : temporary;
}

std::string legacyIdFolder(std::string_view id) {
    std::string folder(id);
    for (char& c : folder)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_'))
            c = '_';
    if (folder.empty() || folder.front() == '.') folder.insert(folder.begin(), '_');
    return folder;
}

std::string idFolder(std::string_view id) {
    std::string folder = legacyIdFolder(id.substr(0, 100));
    return folder + "-" + sha256Hex(id).substr(0, 8);
}
}  // namespace supple::paths
