// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#if !defined(_WIN32)
#include <sys/wait.h>
namespace supple::host {
// Whether a waitid() record says the child is gone: it exited, or a signal
// ended it. macOS's waitid also returns a record for a stopped child when
// only WEXITED was asked for (native CI, review round 34: si_code
// CLD_STOPPED, si_status SIGSTOP). A stopped or continued child is alive: a
// debugger, job control or a test may stop a healthy one.
inline bool childEnded(const siginfo_t& status) {
    return status.si_pid != 0 && (status.si_code == CLD_EXITED || status.si_code == CLD_KILLED ||
                                  status.si_code == CLD_DUMPED);
}
}  // namespace supple::host
#endif
