// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "media_files.h"

#include "file_platform.h"
#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstring>

#include "supple/base/files.h"
#include "supple/base/sha256.h"

namespace supple::engine::detail {
namespace {
namespace fs = std::filesystem;

Error copyError(const fs::path& from, const fs::path& to, const std::string& why) {
    return Error("media.copy", "cannot copy " + from.string() + " to " + to.string() + ": " + why,
                 {}, "check that the file is readable and the folder writable with free space");
}

// Whether two regular files hold the same bytes.
bool sameBytes(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const auto sa = fs::file_size(a, ea), sb = fs::file_size(b, eb);
    if (ea || eb || sa != sb) return false;
    auto ha = sha256File(a), hb = sha256File(b);
    return ha && hb && *ha == *hb;
}

// A whole copy at `to`, which must not exist: a clone (FICLONE) or a byte
// copy into a temporary name, published with link() so nothing is replaced.
Status copyNew(const fs::path& from, const fs::path& to, bool* raced = nullptr,
               bool* created = nullptr) {
    if (raced) *raced = false;
    if (created) *created = false;
    static std::atomic<unsigned> serial{0};
    const auto nextTemp = [&] {
        return to.parent_path() / (".copy-" + std::to_string(files::processId()) + "-" +
                                   std::to_string(serial.fetch_add(1)) + ".copy-tmp");
    };
    fs::path temp;
    // Check after reservation: native identity catches case folding and other
    // filesystem aliases too, including a final name equal to this candidate.
    const auto isPayload = [&] {
        std::error_code ignored;
        return fs::equivalent(temp, to, ignored);
    };
#if defined(__linux__)
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return copyError(from, to, std::strerror(errno));
    int out = -1;
    for (;;) {
        temp = nextTemp();
        out = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (out >= 0) {
            if (!isPayload()) break;
            ::close(out);
            std::error_code cleanup;
            fs::remove(temp, cleanup);
            if (!cleanup) continue;
            ::close(in);
            return copyError(from, to, cleanup.message());
        }
        const auto error = errno;
        if (error != EEXIST) {
            ::close(in);
            return copyError(from, to, std::strerror(error));
        }
    }
    const bool cloned = ::ioctl(out, FICLONE, in) == 0;
    ::close(in);
    ::close(out);
    std::error_code ec;
    if (!cloned) fs::copy_file(from, temp, fs::copy_options::overwrite_existing, ec);
#else
    // Reserve the staging file ourselves, so cleanup can never remove an
    // entry copy_file found already there (including after a PID is reused).
    std::error_code ec;
    for (;;) {
        temp = nextTemp();
        auto* reserved = files::createAppend(temp, ec);
        if (reserved) {
            if (std::fclose(reserved) == 0) {
                if (!isPayload()) break;
                fs::remove(temp, ec);
                if (!ec) continue;
                return copyError(from, to, ec.message());
            }
            const std::string reason = std::strerror(errno);
            fs::remove(temp, ec);
            return copyError(from, to, reason);
        }
        if (ec != std::errc::file_exists) return copyError(from, to, ec.message());
    }
    fs::copy_file(from, temp, fs::copy_options::overwrite_existing, ec);
#endif
    if (ec) {
        const auto reason = ec.message();
        fs::remove(temp, ec);
        return copyError(from, to, reason);
    }
#if defined(_WIN32)
    // CopyFile may copy the source's read-only attribute. This staging file
    // is ours; FlushFileBuffers needs a writable handle. The source stays as is.
    fs::permissions(temp, fs::perms::owner_write, fs::perm_options::add, ec);
    if (ec) {
        const auto reason = ec.message();
        fs::remove(temp, ec);
        return copyError(from, to, reason);
    }
#endif
    if (auto synced = syncFile(temp); !synced) {
        const auto reason = synced.error().message;
        fs::remove(temp, ec);
        return copyError(from, to, reason);
    }
    const int linked = files::publishNew(temp, to);
    const int error = errno;
    fs::remove(temp, ec);
    if (linked != 0) {
        if (raced && error == EEXIST) *raced = true;
        return copyError(from, to,
                         error == EBUSY ? "another program has the file open; close it and retry"
                                        : std::strerror(error));
    }
    if (created) *created = true;
    if (auto synced = syncParentDirectories(to); !synced)
        return copyError(from, to, synced.error().message);
    return {};
}

}  // namespace

Result<std::string> copyIntoFolder(const fs::path& file, const fs::path& dir, bool* created) {
    std::error_code ec;
    if (created) *created = false;
    if (!fs::is_regular_file(file, ec))
        return Error("media.missing", file.string() + " is missing", {},
                     "restore the file, or remove the items that use it");
    fs::create_directories(dir, ec);
    if (ec) return copyError(file, dir, ec.message());
    const std::string stem = file.stem().string(), ext = file.extension().string();
    for (int n = 1; n < 10000; ++n) {
        const std::string name = n == 1 ? stem + ext : stem + "-" + std::to_string(n) + ext;
        const fs::path target = dir / name;
        if (fs::equivalent(file, target, ec)) {
            SUPPLE_TRY(syncFile(target));
            SUPPLE_TRY(syncParentDirectories(target));
            return name;
        }
        if (fs::exists(target, ec)) {
            if (sameBytes(file, target)) {
                SUPPLE_TRY(syncFile(target));
                SUPPLE_TRY(syncParentDirectories(target));
                return name;
            }
            continue;
        }
        bool raced = false;
        if (auto copied = copyNew(file, target, &raced, created); !copied) {
            if (raced) continue;  // only an actual no-replace race, never a failed barrier
            return std::move(copied).error();
        }
        if (created) *created = true;
        return name;
    }
    return copyError(file, dir, "too many files with this name");
}

Status copyFolder(const fs::path& from, const fs::path& to, std::vector<fs::path>* created) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return {};
    if (fs::exists(to, ec) && fs::equivalent(from, to, ec)) return {};
    const auto source = fs::canonical(from, ec);
    if (ec) return copyError(from, to, ec.message());
    const auto absolute = fs::absolute(to, ec);
    if (ec) return copyError(from, to, ec.message());
    const auto destination = fs::weakly_canonical(absolute, ec);
    if (ec) return copyError(from, to, ec.message());
    const auto relativeDestination = destination.lexically_relative(source);
    const bool excludeDestination = !relativeDestination.empty() && relativeDestination != "." &&
                                    *relativeDestination.begin() != "..";
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        // Save As may put the destination inside the old media folder, including
        // through a symlink. Exclude it only then: an ancestor destination must
        // still receive every source file when saving back to the old project.
        if (excludeDestination) {
            const auto resolved = fs::weakly_canonical(it->path(), ec);
            if (ec) break;
            const auto inside = resolved.lexically_relative(destination);
            if (!inside.empty() && *inside.begin() != "..") {
                it.disable_recursion_pending();
                continue;
            }
        }
        // A freeze, consolidate or mix job has not published these files into history yet.
        if (it->is_directory(ec) && (it->path().filename() == ".freeze-pending" ||
                                     it->path().filename() == ".consolidate-pending" ||
                                     it->path().filename() == ".mix-pending")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = it->path().lexically_relative(from);
        const fs::path target = to / rel;
        if (fs::exists(target, ec)) {
            if (sameBytes(it->path(), target)) {
                SUPPLE_TRY(syncFile(target));
                SUPPLE_TRY(syncParentDirectories(target));
                continue;
            }
            return Error("media.conflict", target.string() + " already exists with other contents",
                         {}, "save into a folder of its own, without another project's media");
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) return copyError(it->path(), target, ec.message());
        bool made = false;
        auto copied = copyNew(it->path(), target, nullptr, &made);
        if (created && made) created->push_back(target);  // even if its directory barrier failed
        SUPPLE_TRY(std::move(copied));
    }
    if (ec) return copyError(from, to, ec.message());
    return {};
}

bool resolvedInside(const fs::path& root, const fs::path& path, std::vector<fs::path>* missing) {
    std::error_code ec;
    const fs::path base = fs::canonical(root, ec);
    if (ec) return false;
    fs::path existing = path;
    std::vector<fs::path> absent;
    while (!existing.empty() && !fs::exists(fs::symlink_status(existing, ec))) {
        absent.push_back(existing);
        if (existing == existing.parent_path()) return false;
        existing = existing.parent_path();
    }
    for (const auto& part : path.lexically_relative(existing))
        if (part == "..") return false;
    const fs::path rel = fs::canonical(existing, ec).lexically_relative(base);
    if (ec || rel.empty() || *rel.begin() == "..") return false;
    if (missing) *missing = std::move(absent);
    return true;
}

Status recoveryFileInside(const fs::path& root, const fs::path& file) {
    if (root.empty() || resolvedInside(root, file)) return okStatus();
    return Error("recover.outside",
                 file.string() + " leads outside " + root.string() + "; recovery does not open it",
                 {}, "copy the file into " + root.string() + " if it belongs there");
}

Result<std::vector<std::uint8_t>> readRecoveryFile(const fs::path& root, const fs::path& file,
                                                   std::size_t limit) {
    SUPPLE_TRY(recoveryFileInside(root, file));
    std::vector<std::uint8_t> bytes;
    switch (files::readRegular(file, limit, bytes)) {
        case files::ReadRegular::Read:
            return bytes;
        case files::ReadRegular::Missing:
            return Error("file.open", "cannot open " + file.string() + ": it does not exist", {},
                         "check the path");
        case files::ReadRegular::NotRegular:
            return Error("recover.not_file",
                         file.string() + " is not a regular file; recovery does not open it", {},
                         "move it out of the folder: recovery reads only the regular files a "
                         "recording or an autosave leaves");
        case files::ReadRegular::TooLarge:
            return Error("file.too_large",
                         file.string() + " is larger than " + std::to_string(limit) + " bytes", {},
                         "this is not a file recovery wrote; move it out of the folder");
        case files::ReadRegular::Failed:
            break;
    }
    return Error("file.read", "cannot read " + file.string(), {},
                 "check that the file is readable");
}

Result<std::string> readRecoveryText(const fs::path& root, const fs::path& file,
                                     std::size_t limit) {
    auto bytes = readRecoveryFile(root, file, limit);
    if (!bytes) return std::move(bytes).error();
    return std::string(bytes->begin(), bytes->end());
}

}  // namespace supple::engine::detail
