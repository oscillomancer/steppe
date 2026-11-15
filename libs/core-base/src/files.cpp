// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/files.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>  // renamex_np
#endif
#endif

#include <algorithm>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace supple {

namespace fs = std::filesystem;
namespace {
std::string displayPath(const fs::path& path) {
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
#else
    return path.string();
#endif
}
}  // namespace

Result<std::vector<std::uint8_t>> readFile(const fs::path& path, std::size_t maxBytes) {
#ifdef _WIN32
    // Readers must permit delete sharing for atomic replacement on Windows.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return Error("file.open", "cannot open " + displayPath(path), {}, "check the path");
    const auto close = [](void* handle) { ::CloseHandle(handle); };
    const std::unique_ptr<void, decltype(close)> owner(file, close);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        return Error("file.read", "cannot read " + displayPath(path));
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        return Error("file.too_large",
                     displayPath(path) + " is larger than " + std::to_string(maxBytes) + " bytes");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t off = 0;
    while (off < bytes.size()) {
        DWORD read = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - off, 1 << 20));
        if (!::ReadFile(file, bytes.data() + off, count, &read, nullptr) || read == 0) {
            return Error("file.read", "short read from " + displayPath(path));
        }
        off += read;
    }
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error("file.open", "cannot open " + displayPath(path), {}, "check the path");
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) return Error("file.read", "cannot read " + displayPath(path));
    if (static_cast<std::uint64_t>(end) > maxBytes)
        return Error("file.too_large",
                     displayPath(path) + " is larger than " + std::to_string(maxBytes) + " bytes");
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) return Error("file.read", "short read from " + displayPath(path));
#endif
    return bytes;
}

Result<std::string> readTextFile(const fs::path& path, std::size_t maxBytes) {
    auto bytes = readFile(path, maxBytes);
    if (!bytes) return std::move(bytes).error();
    return std::string(bytes->begin(), bytes->end());
}

#ifdef _WIN32
namespace {
constexpr DWORD kRetryBudgetMs = 1000;
constexpr DWORD kFirstDelayMs = 2;
constexpr DWORD kMaxDelayMs = 64;

// Sharing and lock violations mean another handle is open. Access denied is
// ambiguous (permissions, a read-only file, a directory in the way), so it
// counts only for an existing regular file.
bool isBusyCode(const fs::path& target, DWORD code) {
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
        code == ERROR_USER_MAPPED_FILE || code == ERROR_DELETE_PENDING)
        return true;
    if (code != ERROR_ACCESS_DENIED) return false;
    std::error_code ec;
    return fs::is_regular_file(target, ec);
}
}  // namespace

bool isFileBusyError(const fs::path& target, unsigned long winError) {
    return isBusyCode(target, static_cast<DWORD>(winError));
}

bool retryWhileFileBusy(const fs::path& target, const std::function<bool()>& attempt,
                        unsigned long& winError) {
    const ULONGLONG start = ::GetTickCount64();
    DWORD delay = kFirstDelayMs;
    for (;;) {
        if (attempt()) {
            winError = 0;
            return true;
        }
        winError = ::GetLastError();
        if (!isBusyCode(target, static_cast<DWORD>(winError))) return false;
        const ULONGLONG elapsed = ::GetTickCount64() - start;
        if (elapsed >= kRetryBudgetMs) return false;
        ::Sleep(static_cast<DWORD>(std::min<ULONGLONG>(delay, kRetryBudgetMs - elapsed)));
        delay = std::min(delay * 2, kMaxDelayMs);
    }
}

Error fileBusyError(const fs::path& target, unsigned long winError) {
    return Error("file.write",
                 "cannot replace " + displayPath(target) +
                     ": another program has the file open (Windows error " +
                     std::to_string(winError) + ")",
                 displayPath(target),
                 "close the program using the file (a viewer, sync client, backup or antivirus "
                 "scan), or wait a moment, then save again; the previous file is unchanged");
}
#endif

namespace {
std::function<bool(const fs::path&)>& syncHook() {
    static std::function<bool(const fs::path&)> hook;
    return hook;
}
bool syncAllowed(const fs::path& path) { return !syncHook() || syncHook()(path); }
std::function<bool(const fs::path&, bool)>& durabilityHook() {
    static std::function<bool(const fs::path&, bool)> hook;
    return hook;
}
bool durabilityAllowed(const fs::path& path, bool directory) {
    return !durabilityHook() || durabilityHook()(path, directory);
}

Status writeFile(const fs::path& path, std::span<const std::uint8_t> bytes, bool replace);
}  // namespace

void setFileSyncHookForTesting(std::function<bool(const fs::path&)> hook) {
    syncHook() = std::move(hook);
}
void setDurabilityHookForTesting(std::function<bool(const fs::path&, bool)> hook) {
    durabilityHook() = std::move(hook);
}

namespace {
#ifndef _WIN32
bool syncDescriptor(int fd) {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
    if (errno != EINVAL && errno != ENOTSUP && errno != ENOTTY) return false;
#endif
    return ::fsync(fd) == 0;
}
#endif
Error syncError(const fs::path& path, bool directory, const std::string& reason) {
    return Error(directory ? "file.sync_directory" : "file.sync",
                 "cannot sync " + displayPath(path) + ": " + reason, displayPath(path),
                 "check free space, permissions and storage health; keep the original media");
}
}  // namespace

bool syncFileStream(std::FILE* stream) {
    if (!stream || std::fflush(stream) != 0) return false;
#ifdef _WIN32
    return ::FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)))) != 0;
#else
    return syncDescriptor(::fileno(stream));
#endif
}

Status syncFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return syncError(path, false, ec ? ec.message() : "not a regular file");
    if (!durabilityAllowed(path, false)) return syncError(path, false, "injected sync failure");
#ifdef _WIN32
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return syncError(path, false, std::system_category().message(::GetLastError()));
    const bool synced = ::FlushFileBuffers(file) != 0;
    const auto error = ::GetLastError();
    ::CloseHandle(file);
    if (!synced) return syncError(path, false, std::system_category().message(error));
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file < 0) return syncError(path, false, std::strerror(errno));
    struct stat info{};
    const bool regular = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode);
    const bool synced = regular && syncDescriptor(file);
    const int error = errno;
    ::close(file);
    if (!synced)
        return syncError(path, false, regular ? std::strerror(error) : "not a regular file");
#endif
    return okStatus();
}

Status syncParentDirectories(const fs::path& file) {
    std::error_code ec;
    const auto absolute = fs::absolute(file, ec);
    if (ec) return syncError(file, true, ec.message());
    // Resolve the real parent before processing '..': lexical normalization
    // can select another directory when a preceding component is a symlink.
    auto directory = fs::canonical(absolute.parent_path(), ec);
    if (ec) return syncError(file, true, ec.message());
    for (; !directory.empty();) {
        if (!durabilityAllowed(directory, true))
            return syncError(directory, true, "injected sync failure");
#ifdef _WIN32
        // No documented unprivileged POSIX directory-barrier equivalent.
        // Media publication uses MOVEFILE_WRITE_THROUGH; validate paths here
        // without claiming a namespace power-loss bound or elevating.
        if (!fs::is_directory(directory, ec))
            return syncError(directory, true, ec ? ec.message() : "not a directory");
#else
        const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return syncError(directory, true, std::strerror(errno));
        const bool synced = syncDescriptor(fd);
        const int error = errno;
        ::close(fd);
        // A read-only ancestor has no new writable namespace entries from
        // this operation. Lower writable directories were checkpointed first.
        if (!synced && error == EROFS) return okStatus();
        if (!synced) return syncError(directory, true, std::strerror(error));
#endif
        auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = std::move(parent);
    }
    return okStatus();
}

Status writeFileAtomic(const fs::path& path, std::span<const std::uint8_t> bytes) {
    return writeFile(path, bytes, true);
}

Status writeFileNew(const fs::path& path, std::span<const std::uint8_t> bytes) {
    return writeFile(path, bytes, false);
}

namespace {
Status writeFile(const fs::path& path, std::span<const std::uint8_t> bytes, bool replace) {
    std::error_code ec;
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
    fs::create_directories(dir, ec);
    // A unique temporary name per writer (pid + counter), created exclusively,
    // so concurrent writers of the same target never share a temp file.
    static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
    fs::path tmp = path;
    tmp += "." + std::to_string(::GetCurrentProcessId()) + "." +
           std::to_string(counter.fetch_add(1)) + ".tmp";
    bool published = false;
    auto error = [&](const std::string& what, DWORD code) {
        if (!published) fs::remove(tmp, ec);
        return Error(published ? "file.sync_published" : "file.write",
                     what + " " + displayPath(path) + ": Windows error " + std::to_string(code), {},
                     published ? "the new file may already exist; check storage before retrying"
                               : "check free disk space and permissions");
    };
    const auto destination = fs::absolute(path, ec).native();
    if (ec) {
        fs::remove(tmp, ec);
        return Error("file.write", "cannot resolve the destination path", {}, "check the path");
    }
    // Rename the open file with POSIX replacement semantics (Windows 10+).
    // Concurrent writers may still hold the file they just published; they
    // must not prevent the next writer from atomically replacing that name.
    FILE_RENAME_INFO rename{};
    constexpr DWORD kReplaceIfExists = 0x1, kPosixSemantics = 0x2;
    rename.Flags = replace ? kReplaceIfExists | kPosixSemantics : kPosixSemantics;
    rename.FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::vector<std::uint8_t> renameBytes(sizeof(rename) + rename.FileNameLength + sizeof(wchar_t));
    std::memcpy(renameBytes.data(), &rename, sizeof(rename));
    std::memcpy(renameBytes.data() + offsetof(FILE_RENAME_INFO, FileName), destination.data(),
                rename.FileNameLength);
    const HANDLE file =
        ::CreateFileW(tmp.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return Error("file.write",
                     "cannot create " + displayPath(tmp) + ": Windows error " +
                         std::to_string(::GetLastError()),
                     {}, "check that the directory exists and is writable");
    std::size_t off = 0;
    while (off < bytes.size()) {
        DWORD written = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - off, 1 << 20));
        if (!::WriteFile(file, bytes.data() + off, count, &written, nullptr) || written == 0) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(file);
            return error("cannot write", code);
        }
        off += written;
    }
    if (!syncAllowed(path) || !::FlushFileBuffers(file)) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(file);
        return error("cannot sync", code);
    }
    unsigned long renameError = 0;
    if (!retryWhileFileBusy(
            path,
            [&] {
                return ::SetFileInformationByHandle(file, FileRenameInfoEx, renameBytes.data(),
                                                    static_cast<DWORD>(renameBytes.size())) != 0;
            },
            renameError)) {
        ::CloseHandle(file);
        if (!replace && (renameError == ERROR_ALREADY_EXISTS || renameError == ERROR_FILE_EXISTS)) {
            fs::remove(tmp, ec);
            return Error("file.exists", displayPath(path) + " exists", {}, "choose another name");
        }
        if (isFileBusyError(path, renameError)) {
            fs::remove(tmp, ec);
            return fileBusyError(path, renameError);
        }
        return error("cannot rename temporary file into place", renameError);
    }
    published = true;
    const bool allowed = durabilityAllowed(path, false);
    if (!allowed || !::FlushFileBuffers(file)) {
        const DWORD code = allowed ? ::GetLastError() : ERROR_WRITE_FAULT;
        ::CloseHandle(file);
        return error("cannot sync renamed file", code);
    }
    if (!::CloseHandle(file)) return error("cannot close", ::GetLastError());
#else
    const fs::path tmp = path.string() + "." + std::to_string(::getpid()) + "." +
                         std::to_string(counter.fetch_add(1)) + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return Error("file.write",
                     "cannot create " + displayPath(tmp) + ": " + std::strerror(errno), {},
                     "check that the directory exists and is writable");
    auto fail = [&](const std::string& what) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        fs::remove(tmp, ec);
        return Error("file.write", what + " " + displayPath(tmp) + ": " + reason, {},
                     "check free disk space and permissions");
    };
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail("cannot write");
        off += static_cast<std::size_t>(n);
    }
    // Durability: the data reaches the disk before the rename makes it visible.
    if (!syncAllowed(path)) {
        errno = EIO;
        return fail("cannot sync");
    }
    if (::fsync(fd) != 0) return fail("cannot sync");
    if (::close(fd) != 0) {
        fs::remove(tmp, ec);
        return Error("file.write", "cannot close " + displayPath(tmp));
    }
    if (replace) {
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            const std::string reason = std::strerror(errno);
            fs::remove(tmp, ec);
            return Error("file.write",
                         "cannot move " + displayPath(tmp) + " into place: " + reason);
        }
    } else {
        // Publish only if the name is free, in one step: a rename that refuses
        // to replace where the system has one, else a hard link (which fails
        // on an existing name) and removing the temporary name.
        int published = -1;
        errno = ENOSYS;  // no such rename here: link below
#if defined(__linux__) && defined(SYS_renameat2)
        published = static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, tmp.c_str(), AT_FDCWD,
                                               path.c_str(), 1U /* RENAME_NOREPLACE */));
#elif defined(__APPLE__)
        published = ::renamex_np(tmp.c_str(), path.c_str(), RENAME_EXCL);
#endif
        if (published != 0 && errno != EEXIST) {
            published = ::link(tmp.c_str(), path.c_str());
            if (published == 0) ::unlink(tmp.c_str());
        }
        if (published != 0) {
            const int code = errno;
            const std::string reason = std::strerror(code);
            fs::remove(tmp, ec);
            if (code == EEXIST)
                return Error("file.exists", displayPath(path) + " exists", {},
                             "choose another name");
            return Error("file.write",
                         "cannot move " + displayPath(tmp) + " into place: " + reason);
        }
    }
    // Publication already happened. Distinguish an uncertain checkpoint from
    // a failure before publication, so callers retain any referenced media.
    if (auto synced = syncParentDirectories(path); !synced)
        return Error(
            "file.sync_published", synced.error().message, displayPath(path),
            "the new file may already exist; keep its media and check storage before retrying");
#endif
    return okStatus();
}
}  // namespace

Status writeFileAtomic(const fs::path& path, std::string_view text) {
    return writeFileAtomic(
        path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v =
            (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest) {
        std::uint32_t v = std::uint32_t(bytes[i]) << 16;
        if (rest == 2) v |= std::uint32_t(bytes[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

Result<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    auto bad = [&](std::size_t at) {
        return Error("base64.invalid", "invalid base64 at offset " + std::to_string(at), {},
                     "standard alphabet (A-Z a-z 0-9 + /), '=' padding, no whitespace");
    };
    if (text.size() % 4 != 0) return bad(text.size());
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        std::array<int, 4> d{};
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && last && k >= 2) {
                d[static_cast<std::size_t>(k)] = 0;
                ++pad;
                continue;
            }
            if (pad) return bad(i + static_cast<std::size_t>(k));  // data after padding
            d[static_cast<std::size_t>(k)] = decodeChar(c);
            if (d[static_cast<std::size_t>(k)] < 0) return bad(i + static_cast<std::size_t>(k));
        }
        const std::uint32_t v = (std::uint32_t(d[0]) << 18) | (std::uint32_t(d[1]) << 12) |
                                (std::uint32_t(d[2]) << 6) | std::uint32_t(d[3]);
        // Non-canonical encodings (unused bits set) are rejected too.
        if ((pad == 1 && (v & 0xff)) || (pad == 2 && (v & 0xffff))) return bad(i + 2);
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        if (pad < 2) out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        if (pad < 1) out.push_back(static_cast<std::uint8_t>(v & 0xff));
    }
    return out;
}

}  // namespace supple
