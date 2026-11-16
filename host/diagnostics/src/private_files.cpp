// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "private_files.h"
#include <cerrno>
#include <cstddef>
#include <vector>
#include "native.h"
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
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace supple::diagnostics::detail {
namespace fs = std::filesystem;
namespace {
Error failure() {
    return Error("diagnostics.file", "private diagnostic file operation failed", {},
                 "check local diagnostic folder permissions and free space");
}
constexpr std::size_t kLimit = 4 * 1024 * 1024;
#if defined(_WIN32)
struct Security {
    std::vector<std::byte> token;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    bool ready = false;
    Security() {
        HANDLE process = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process)) return;
        DWORD size = 0;
        GetTokenInformation(process, TokenUser, nullptr, 0, &size);
        token.resize(size);
        const bool ok = GetTokenInformation(process, TokenUser, token.data(), size, &size) != FALSE;
        CloseHandle(process);
        if (!ok) return;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(user(), &sid)) return;
        const std::wstring sddl =
            L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        ready = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                     &descriptor, nullptr) != FALSE;
    }
    ~Security() {
        if (descriptor) LocalFree(descriptor);
    }
    PSID user() { return reinterpret_cast<TOKEN_USER*>(token.data())->User.Sid; }
};
bool owned(HANDLE file, bool directory = false) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1))
        return false;
    Security security;
    if (!security.ready) return false;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    bool ok = owner && EqualSid(owner, security.user()) && acl;
    BYTE system[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(system);
    ok = ok && CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size);
    for (DWORD i = 0; ok && i < acl->AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(acl, i, &raw)) {
            ok = false;
            break;
        }
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            ok = false;
            break;
        }
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        auto sid = const_cast<DWORD*>(&ace->SidStart);
        if (ace->Mask && !EqualSid(sid, security.user()) && !EqualSid(sid, system)) ok = false;
    }
    LocalFree(descriptor);
    return ok;
}
#else
bool owned(int fd, struct stat& info) {
    return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
           info.st_nlink == 1 && !(info.st_mode & 0077) && info.st_size >= 0 &&
           static_cast<std::uint64_t>(info.st_size) <= kLimit;
}
#endif
}  // namespace
#if defined(_WIN32)
bool privateDirectoryAcl(const fs::path& path) {
    HANDLE file =
        CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = owned(file, true);
    CloseHandle(file);
    return ok;
}
#endif
Status privatePublish(const fs::path& path, const std::string& text) {
    if (text.size() > kLimit) return failure();
    auto id = incidentId();
    if (!id) return id.error();
    const auto temporary = path.parent_path() / (".tmp-" + *id);
#if defined(_WIN32)
    Security security;
    if (!security.ready) return failure();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.descriptor, FALSE};
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return failure();
    DWORD written = 0;
    const bool writtenOk =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!writtenOk || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return failure();
    }
#else
    const int fd =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return failure();
    std::size_t at = 0;
    while (at < text.size()) {
        const auto n = ::write(fd, text.data() + at, text.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += static_cast<std::size_t>(n);
    }
    const bool written = at == text.size() && ::fsync(fd) == 0;
    ::close(fd);
    const bool linked = written && ::link(temporary.c_str(), path.c_str()) == 0;
    (void)::unlink(temporary.c_str());
    if (!linked) return failure();
    const int directory =
        ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return failure();
    const int sync = ::fsync(directory);
    ::close(directory);
    if (sync != 0) return failure();
#endif
    return okStatus();
}
Result<std::string> privateRead(const fs::path& path) {
#if defined(_WIN32)
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return failure();
    LARGE_INTEGER size{};
    if (!owned(file) || !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(kLimit)) {
        CloseHandle(file);
        return failure();
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) &&
                    read == text.size();
    CloseHandle(file);
    if (!ok) return failure();
    return text;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return failure();
    struct stat info{};
    if (!owned(fd, info)) {
        ::close(fd);
        return failure();
    }
    std::string text(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t at = 0;
    while (at < text.size()) {
        const auto n = ::read(fd, text.data() + at, text.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += static_cast<std::size_t>(n);
    }
    char extra;
    const auto tail = ::read(fd, &extra, 1);
    ::close(fd);
    if (at != text.size() || tail != 0) return failure();
    return text;
#endif
}
bool privateRemove(const fs::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool ok = owned(file) && SetFileInformationByHandle(file, FileDispositionInfo,
                                                              &disposition, sizeof(disposition));
    CloseHandle(file);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat opened{}, named{};
    const bool ok = owned(fd, opened) && ::lstat(path.c_str(), &named) == 0 &&
                    opened.st_dev == named.st_dev && opened.st_ino == named.st_ino &&
                    ::unlink(path.c_str()) == 0;
    ::close(fd);
    return ok;
#endif
}
}  // namespace supple::diagnostics::detail
