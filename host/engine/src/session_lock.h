// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// An unsaved project's folder belongs to the run holding the OS lock on its
// session.lock (ADR-0078). The system releases the lock when the process
// ends however it ends, so a folder whose lock can be taken was left by a
// run that is gone: crashed, or killed. Process IDs are not used: they are
// reused.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>

namespace supple::engine::detail {

inline constexpr const char* kSessionLockName = "session.lock";

class SessionLock {
  public:
    // Takes the lock on `folder`/session.lock without waiting, creating the
    // file (not the folder). Empty when another process holds it or the file
    // cannot be opened.
    static std::optional<SessionLock> tryAcquire(const std::filesystem::path& folder);
    SessionLock(SessionLock&&) noexcept;
    SessionLock& operator=(SessionLock&&) noexcept;
    ~SessionLock();

    // Releases the lock and deletes the lock file (Windows refuses to delete
    // an open file).
    void releaseAndRemove();

  private:
    struct Handle;
    explicit SessionLock(std::unique_ptr<Handle> handle);
    std::unique_ptr<Handle> handle_;
};

}  // namespace supple::engine::detail
