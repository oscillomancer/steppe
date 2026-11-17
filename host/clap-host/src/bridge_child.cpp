// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_child.h"
#include <string>
#include <vector>
#include "child_status.h"
#include "platform.h"
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/event.h>
#endif
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <poll.h>
#include <sys/syscall.h>
#endif
namespace supple::host::bridge {
std::filesystem::path ownExecutable() { return platform::executablePath(); }
Result<std::unique_ptr<NativeChild>> NativeChild::launch(const std::filesystem::path& exe,
                                                         Resource control) {
    std::unique_ptr<NativeChild> child(new NativeChild);
    ChildProcess::LaunchOptions options;
    options.control = control;
    options.captureOutput = false;  // plugin logging cannot fill an undrained pipe
    options.inheritStderr = true;   // retain plug-in diagnostics and assert/abort messages
#if defined(_WIN32)
    const std::vector<std::string> args{"--control-handle", std::to_string(control)};
#else
    const std::vector<std::string> args{"--control-fd", "3"};
#endif
    auto spawned = ChildProcess::spawn(exe, args, options);
    if (!spawned) return spawned.error();
    child->owner_ = std::move(*spawned);
    child->pid_ = child->owner_->pid();
#if defined(_WIN32)
    child->process_ = reinterpret_cast<HANDLE>(child->owner_->nativeProcess());
#elif defined(__linux__)
    // Socket HUP remains the death-watch fallback on kernels without pidfds.
    child->death_ = static_cast<int>(::syscall(SYS_pidfd_open, child->pid_, 0));
#else
    child->death_ = kqueue();
    if (child->death_ >= 0 && fcntl(child->death_, F_SETFD, FD_CLOEXEC) == 0) {
        struct kevent watch;
        EV_SET(&watch, static_cast<std::uintptr_t>(child->pid_), EVFILT_PROC, EV_ADD | EV_ENABLE,
               NOTE_EXIT, 0, nullptr);
        if (kevent(child->death_, &watch, 1, nullptr, 0, nullptr) == 0) return child;
    }
    return Error("host.bridge", "could not watch plugin bridge exit", exe.string(),
                 "free process/event resources");
#endif
    return child;
}
NativeChild::~NativeChild() {
    // ChildProcess keeps the leader unreaped until this final tree cleanup;
    // its PID cannot be recycled underneath an audio-side kill.
    owner_.reset();
#if !defined(_WIN32)
    if (death_ >= 0) ::close(death_);
#endif
}
Waitable NativeChild::waitable() const {
#if defined(_WIN32)
    return Waitable::handle(process_);
#else
    return Waitable(death_);
#endif
}
bool NativeChild::alive() const {
#if defined(_WIN32)
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
#if defined(__linux__)
    if (death_ >= 0) {
        pollfd descriptor{death_, POLLIN, 0};
        return ::poll(&descriptor, 1, 0) == 0;
    }
#endif
    siginfo_t status{};
    return pid_ > 0 &&
           waitid(P_PID, static_cast<id_t>(pid_), &status, WEXITED | WNOHANG | WNOWAIT) == 0 &&
           !childEnded(status);
#endif
}
void NativeChild::kill() const {
    if (owner_) owner_->killTree();
}
int NativeChild::exitStatus() const {
#if defined(_WIN32)
    DWORD status = 0;
    GetExitCodeProcess(process_, &status);
    return static_cast<int>(status);
#else
    siginfo_t status{};
    if (waitid(P_PID, static_cast<id_t>(pid_), &status, WEXITED | WNOHANG | WNOWAIT) != 0 ||
        !childEnded(status))
        return 0;
    return status.si_code == CLD_EXITED ? 0 : status.si_status;
#endif
}
#if defined(_WIN32)
Resource NativeChild::duplicate(Resource resource) const {
    HANDLE remote = nullptr;
    return DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(resource), process_,
                           &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)
               ? reinterpret_cast<Resource>(remote)
               : noResource;
}
void NativeChild::revoke(Resource resource) const {
    if (resource == noResource) return;
    HANDLE local = nullptr;
    if (DuplicateHandle(process_, reinterpret_cast<HANDLE>(resource), GetCurrentProcess(), &local,
                        0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
        CloseHandle(local);
}
#endif
}  // namespace supple::host::bridge
