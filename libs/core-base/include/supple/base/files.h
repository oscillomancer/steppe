// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Small whole-file helpers (main/I-O threads only).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"

namespace supple {

// Error codes: "file.open" (missing/unreadable), "file.read", "file.too_large".
Result<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path,
                                           std::size_t maxBytes = std::size_t(1) << 30);
Result<std::string> readTextFile(const std::filesystem::path& path,
                                 std::size_t maxBytes = std::size_t(1) << 30);

// Non-RT durable checkpoints. The stream helper flushes buffered bytes first.
bool syncFileStream(std::FILE* stream);
Status syncFile(const std::filesystem::path& path);
// POSIX: parent/ancestor directory barriers, leaf first. Windows validates
// directories; publication relies on write-through renames, without claiming
// POSIX-equivalent namespace durability (ADR-0082). No elevation.
Status syncParentDirectories(const std::filesystem::path& file);

// Writes a uniquely named temporary file in the same directory, fsyncs it,
// renames it over `path` and fsyncs the directory: readers see the old or the
// new file, never a partial one, concurrent writers never share a temp file,
// and a successful return survives a crash. Creates missing parent
// directories. Error code: "file.write" before publication, or
// "file.sync_published" if the new file may already have been published;
// callers must retain anything it references. Windows has the namespace
// durability limit noted above. On Windows a replace blocked by
// another program that holds the destination open retries for about a second
// before failing with an error that names the cause.
Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
Status writeFileAtomic(const std::filesystem::path& path, std::string_view text);

// As writeFileAtomic, but never replaces: the synced temporary file is
// published under `path` only if nothing is there, atomically (renameat2
// RENAME_NOREPLACE or link() on Linux, renamex_np RENAME_EXCL or link() on
// macOS, a rename without replace on Windows). "file.exists" when `path` is
// taken, at any moment up to the publication; nothing is written then.
Status writeFileNew(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

// Tests only: called with the destination before each atomic write syncs its
// data; returning false fails that write as a failed sync would. An empty
// function removes the hook. Not for concurrent use with writes.
void setFileSyncHookForTesting(std::function<bool(const std::filesystem::path&)> hook);
// Separate observer/fault hook for path checkpoints (true means directory).
// Kept separate so atomic-write hooks never receive directory paths.
// Not for concurrent reconfiguration while checkpoints are running.
void setDurabilityHookForTesting(std::function<bool(const std::filesystem::path&, bool)> hook);

#ifdef _WIN32
// Windows only. Antivirus scanners, indexers, sync clients and viewers often
// hold a just-written file open without FILE_SHARE_DELETE, which fails a
// rename or replace for a moment. `attempt` performs one try and returns true
// on success; on failure it leaves the Win32 error in GetLastError(). While
// the failure is a sharing or lock violation (or access denied on a target
// that is not a directory) the attempt repeats with a short backoff, for about
// one second in all. Returns the last attempt's result; `winError` receives its
// error. Non-RT threads only: it sleeps.
bool retryWhileFileBusy(const std::filesystem::path& target, const std::function<bool()>& attempt,
                        unsigned long& winError);
// Whether `winError` (after retryWhileFileBusy gave up) means another program
// holds the file; `target` is the destination that was being replaced.
bool isFileBusyError(const std::filesystem::path& target, unsigned long winError);
// The structured "file.write" error for a replace that stayed blocked: it says
// another program has the file open and what to do about it.
Error fileBusyError(const std::filesystem::path& target, unsigned long winError);
#endif

// RFC 4648 base64 (standard alphabet, padded). Decoding rejects anything else
// (no whitespace, no URL alphabet): "base64.invalid".
std::string base64Encode(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace supple
