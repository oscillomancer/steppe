// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "platform.h"
#include <cstdlib>
#include "supple/base/paths.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#endif
#endif
namespace supple::host::platform {
namespace fs = std::filesystem;
fs::path pathFromText(std::string_view text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string pathText(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path executablePath() {
#if defined(_WIN32)
    std::wstring text(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    if (size == 0 || size == text.size()) return {};
    text.resize(size);
    return text;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string text(size, '\0');
    if (_NSGetExecutablePath(text.data(), &size) != 0) return {};
    return fs::path(text.c_str());
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}
fs::path bundleExecutable(const fs::path& path) {
#if defined(__APPLE__)
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return path;
    const auto text = pathText(path);
    auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
                                                       reinterpret_cast<const UInt8*>(text.data()),
                                                       static_cast<CFIndex>(text.size()), true);
    auto bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    auto executable = bundle ? CFBundleCopyExecutableURL(bundle) : nullptr;
    std::vector<UInt8> buffer(32768);
    const bool ok =
        executable && CFURLGetFileSystemRepresentation(executable, true, buffer.data(),
                                                       static_cast<CFIndex>(buffer.size()));
    fs::path binary;
    if (ok) binary = reinterpret_cast<const char*>(buffer.data());
    if (executable) CFRelease(executable);
    if (bundle) CFRelease(bundle);
    if (url) CFRelease(url);
    return binary;
#else
    return path;  // a CLAP is the file itself (a shared library)
#endif
}
Result<void*> openLibrary(const fs::path& path) {
    const fs::path binary = bundleExecutable(path);
    if (binary.empty())
        return Error("host.load", "CLAP bundle has no executable", "/path",
                     "check Contents/Info.plist and Contents/MacOS");
#if defined(_WIN32)
    auto handle = LoadLibraryW(binary.c_str());
    if (handle) return static_cast<void*>(handle);
    const auto detail = "Windows error " + std::to_string(GetLastError());
#else
    auto handle = ::dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle) return handle;
    const std::string detail = ::dlerror();
#endif
    return Error("host.load", "cannot load " + pathText(path) + ": " + detail, "/path",
                 "check that the plugin and its dependencies are built for this platform");
}
void* symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return ::dlsym(library, name);
#endif
}
void closeLibrary(void* library) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    ::dlclose(library);
#endif
}
bool executable(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}
std::string_view scanHelperName() {
#if defined(__linux__)
    return "bridge";
#else
    return "scanner";
#endif
}
namespace {
std::string environmentText(const char* name) {
#if defined(_WIN32)
    const auto wideName = pathFromText(name).wstring();
    const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return pathText(fs::path(value));
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
}  // namespace
std::vector<fs::path> pluginPaths(bool vst3) {
    std::vector<fs::path> result;
    auto add = [&](const fs::path& path) {
        std::error_code ec;
        if (!path.empty() && fs::exists(path, ec)) result.push_back(path);
    };
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    const auto text = environmentText(vst3 ? "VST3_PATH" : "CLAP_PATH");
    if (!text.empty()) {
        for (std::size_t start = 0; start <= text.size();) {
            const auto end = text.find(separator, start);
            if (end > start) add(pathFromText(text.substr(start, end - start)));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

#if defined(_WIN32)
    const auto common = environmentText("COMMONPROGRAMFILES");
    if (!common.empty()) add(pathFromText(common) / (vst3 ? "VST3" : "CLAP"));
    const auto local = environmentText("LOCALAPPDATA");
    if (!local.empty()) add(pathFromText(local) / "Programs/Common" / (vst3 ? "VST3" : "CLAP"));
#elif defined(__APPLE__)
    const auto home = paths::home();
    const auto directory = fs::path("Library/Audio/Plug-Ins") / (vst3 ? "VST3" : "CLAP");
    if (!home.empty()) add(home / directory);
    add(fs::path("/") / directory);
#else
    const auto home = paths::home();
    if (!home.empty()) add(home / (vst3 ? ".vst3" : ".clap"));
    add(vst3 ? "/usr/lib/vst3" : "/usr/lib/clap");
    add(vst3 ? "/usr/local/lib/vst3" : "/usr/local/lib/clap");
#endif
    return result;
}
}  // namespace supple::host::platform
