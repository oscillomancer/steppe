// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <fcntl.h>
// Puts a descriptor in non-blocking mode for the duration of a send or receive and puts
// the flags back. MSG_DONTWAIT alone is not enough everywhere: where the kernel looks at
// the socket's own non-blocking state to decide whether to wait for buffer space (macOS
// did, in CI), a full socket would block the caller past its deadline, and the caller can
// be a crash handler or a thread that must never wait. fcntl is async-signal-safe.
// Failure to set the flag is not fatal: the per-call flag still applies where it works.
// If another thread has already made the descriptor non-blocking, nothing is changed and
// nothing is restored.
namespace supple::diagnostics::detail {
class NonBlocking {
  public:
    explicit NonBlocking(int fd) noexcept : fd_(fd) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0 && !(flags & O_NONBLOCK) && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0)
            restore_ = flags;
    }
    ~NonBlocking() {
        if (restore_ >= 0) (void)::fcntl(fd_, F_SETFL, restore_);
    }
    NonBlocking(const NonBlocking&) = delete;
    NonBlocking& operator=(const NonBlocking&) = delete;

  private:
    int fd_;
    int restore_ = -1;  // the flags to put back; -1 when nothing was changed
};
}  // namespace supple::diagnostics::detail
