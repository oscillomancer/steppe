// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/paths.h>
#include <cstdlib>
#include <optional>
#include <string>
#if defined(__APPLE__)
#include <unistd.h>
#include <climits>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
struct Environment {
    std::string key;
    std::optional<std::string> old;
    Environment(const char* name, const char* value) : key(name) {
        if (const char* previous = std::getenv(name)) old = previous;
        set(value);
    }
    void set(const char* value) {
#if defined(_WIN32)
        REQUIRE(_putenv_s(key.c_str(), value ? value : "") == 0);
#else
        REQUIRE((value ? setenv(key.c_str(), value, 1) : unsetenv(key.c_str())) == 0);
#endif
    }
    ~Environment() {
#if defined(_WIN32)
        _putenv_s(key.c_str(), old ? old->c_str() : "");
#else
        if (old)
            setenv(key.c_str(), old->c_str(), 1);
        else
            unsetenv(key.c_str());
#endif
    }
};
}  // namespace
TEST_CASE("platform paths select native user directories without creating them") {
    using namespace supple::paths;
#if defined(_WIN32)
    Environment user("USERPROFILE", "C:/test-user");
    Environment roaming("APPDATA", "C:/roaming");
    Environment local("LOCALAPPDATA", "C:/local");
    CHECK(home() == "C:/test-user");
    CHECK(config() == "C:/roaming");
    CHECK(data() == config());
    CHECK(cache() == "C:/local");
    CHECK(localData() == "C:/local");
    local.set(nullptr);
    CHECK(localData().empty());
    local.set("C:/local");
    CHECK(runtime() == "C:/local/Temp");
#else
    Environment user("HOME", "/tmp/test-user");
    Environment configEnv("XDG_CONFIG_HOME", "/tmp/config");
    Environment dataEnv("XDG_DATA_HOME", "/tmp/data");
    Environment cacheEnv("XDG_CACHE_HOME", "/tmp/cache");
    Environment runtimeEnv("XDG_RUNTIME_DIR", "/tmp/runtime");
#if defined(__APPLE__)
    CHECK(config() == "/tmp/test-user/Library/Application Support");
    CHECK(data() == config());
    CHECK(localData() == data());
    CHECK(cache() == "/tmp/test-user/Library/Caches");
    // Without TMPDIR: the per-user temporary directory TMPDIR normally names.
    Environment temporary("TMPDIR", nullptr);
    char perUser[PATH_MAX];
    REQUIRE(::confstr(_CS_DARWIN_USER_TEMP_DIR, perUser, sizeof perUser) > 0);
    CHECK(runtime() == std::filesystem::path(perUser));
    CHECK(runtime() != "/tmp");
#else
    CHECK(config() == "/tmp/config");
    CHECK(data() == "/tmp/data");
    CHECK(localData() == data());
    CHECK(cache() == "/tmp/cache");
    CHECK(runtime() == "/tmp/runtime");
    configEnv.set(nullptr);
    dataEnv.set("");
    cacheEnv.set(nullptr);
    runtimeEnv.set(nullptr);
    CHECK(config() == "/tmp/test-user/.config");
    CHECK(data() == "/tmp/test-user/.local/share");
    CHECK(cache() == "/tmp/test-user/.cache");
    // Without XDG_RUNTIME_DIR: the login's private /run/user/<uid>, else /tmp.
    const std::string login = "/run/user/" + std::to_string(::getuid());
    struct stat info{};
    const bool loginDir = ::stat(login.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                          info.st_uid == ::getuid() && (info.st_mode & 077) == 0;
    CHECK(runtime() == (loginDir ? login : std::string("/tmp")));
    user.set("");
    CHECK(cache().empty());
    CHECK(data().empty());
    CHECK(localData().empty());
    CHECK(cache(true) == ".cache");
    CHECK(data(true) == ".local/share");
    user.set(nullptr);
    CHECK(cache(true) == "/tmp/.cache");
    CHECK(data(true) == "/tmp/.local/share");
#endif
#endif
}

#if defined(_WIN32)
#include <windows.h>

// Every executable carries the UTF-8 code-page manifest (ADR-0044): narrow
// text is UTF-8 whatever the system locale, so a user folder outside the
// legacy code page survives std::filesystem, the CRT environment (how the
// script library reaches the player) and child processes.
TEST_CASE("Windows processes use UTF-8 for narrow paths and environment variables") {
    CHECK(GetACP() == CP_UTF8);
    const std::string folder = "C:/Zo\xc3\xab \xe6\x97\xa5\xe6\x9c\xac";  // "C:/Zoë 日本"
    const std::wstring wide = L"C:/Zo\u00eb \u65e5\u672c";
    Environment user("USERPROFILE", folder.c_str());
    CHECK(supple::paths::home().wstring() == wide);
    CHECK(supple::paths::home().string() == folder);
    CHECK(std::string(std::getenv("USERPROFILE")) == folder);
    wchar_t inherited[64] = {};
    REQUIRE(GetEnvironmentVariableW(L"USERPROFILE", inherited, 64) > 0);
    CHECK(std::wstring(inherited) == wide);
}
#endif

TEST_CASE("an ID's folder is portable, distinct and stable") {
    using supple::paths::idFolder;
    using supple::paths::legacyIdFolder;
    // Kept: presets already live under these names.
    CHECK(idFolder("com.example/a") == "com.example_a-a2dc87e2");
    CHECK(idFolder("com.example_a") == "com.example_a-1c9f81be");
    CHECK(legacyIdFolder("com.example/a") == legacyIdFolder("com.example_a"));
    CHECK(idFolder("com.Example.A") != idFolder("com.example.a"));
    CHECK(idFolder(".hidden").front() == '_');
    CHECK(idFolder("").front() == '_');
    CHECK(idFolder(std::string(1000, 'x')).size() == 109);
    CHECK(idFolder("caf\xc3\xa9/../x").find_first_of("/\\\x80\xc3") == std::string::npos);
}
